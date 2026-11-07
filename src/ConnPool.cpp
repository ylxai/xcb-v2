#include "ConnPool.hpp"

#include "ConnRpc.hpp"  // Json + parseJson
#include "JsonUtil.hpp"
#include "Log.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <arpa/inet.h>
#include <cstdlib>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

std::string toHex(const uint8_t* p, size_t n) {
    static const char hx[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += hx[p[i] >> 4];
        s += hx[p[i] & 0xf];
    }
    return s;
}

bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() % 2) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// Didefinisikan di bawah; connectAndLogin() memakainya lebih dulu.
bool parseJobDoc(const Json& doc, ConnPoolJob& out, std::string& err);

}  // namespace

ConnPool::ConnPool(std::string host, int port, std::string wallet, std::string worker,
                   bool tls)
    : m_host(std::move(host)),
      m_port(port),
      m_wallet(std::move(wallet)),
      m_worker(std::move(worker)),
      m_tls(tls) {}

ConnPool::~ConnPool() { close(); }

void ConnPool::close() {
    std::lock_guard<std::mutex> lock(m_ioMu);
    closeLocked();
}

void ConnPool::markReady() {
    std::lock_guard<std::mutex> lock(m_ioMu);
    m_ready = true;
}

bool ConnPool::isCurrentJob(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_ioMu);
    return !m_jobId.empty() && m_jobId == id;
}

void ConnPool::closeLocked() {
    m_ready = false;
    if (m_ssl) {
        SSL_shutdown(static_cast<SSL*>(m_ssl));
        SSL_free(static_cast<SSL*>(m_ssl));
        m_ssl = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    if (m_ctx) {
        SSL_CTX_free(static_cast<SSL_CTX*>(m_ctx));
        m_ctx = nullptr;
    }
    m_buf.clear();
    m_loginId.clear();
    m_jobId.clear();
}

bool ConnPool::connectAndLogin(ConnPoolJob& first, std::string& err) {
    close();

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string port = std::to_string(m_port);
    if (::getaddrinfo(m_host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        err = "resolve gagal: " + m_host;
        return false;
    }

    // Coba semua hasil DNS (IPv6 lalu IPv4, atau sebaliknya). Dulu hanya
    // hasil pertama yang dipakai: host dengan AAAA tidak reachable gagal
    // connect padahal A-nya sehat.
    int fd = -1;
    int lastErr = 0;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastErr = errno;
            continue;
        }
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        lastErr = errno;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        err = "connect gagal: " + std::string(std::strerror(lastErr));
        return false;
    }

    if (m_tls) {
        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            ::close(fd);
            err = "SSL_CTX_new gagal";
            return false;
        }

        // Verifikasi sertifikat + hostname secara default. Pool dengan
        // sertifikat self-signed bisa memakai CONN_TLS_INSECURE=1, dengan
        // peringatan eksplisit di log.
        const char* insecureEnv = std::getenv("CONN_TLS_INSECURE");
        const bool insecure = insecureEnv && insecureEnv[0] != '\0' &&
                              std::strcmp(insecureEnv, "0") != 0 &&
                              std::strcmp(insecureEnv, "false") != 0 &&
                              std::strcmp(insecureEnv, "no") != 0;
        if (insecure) {
            lg::warn("conn", "CONN_TLS_INSECURE=1: verifikasi sertifikat pool dimatikan");
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        } else {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
                SSL_CTX_free(ctx);
                ::close(fd);
                err = "CA bundle untuk verifikasi TLS tidak bisa dibaca";
                return false;
            }
        }

        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            SSL_CTX_free(ctx);
            ::close(fd);
            err = "SSL_new gagal";
            return false;
        }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, m_host.c_str());
        if (!insecure) {
            // Hostname bisa berupa DNS atau IP literal.
            unsigned char abuf[sizeof(struct in6_addr)];
            const bool isIp = ::inet_pton(AF_INET, m_host.c_str(), abuf) == 1 ||
                              ::inet_pton(AF_INET6, m_host.c_str(), abuf) == 1;
            const int hostOk = isIp
                ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), m_host.c_str())
                : SSL_set1_host(ssl, m_host.c_str());
            if (hostOk != 1) {
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                ::close(fd);
                err = "setup verifikasi hostname TLS gagal";
                return false;
            }
        }
        if (SSL_connect(ssl) != 1) {
            char eb[256] = {0};
            ERR_error_string_n(ERR_peek_last_error(), eb, sizeof(eb));
            const long vr = SSL_get_verify_result(ssl);
            if (!insecure && vr != X509_V_OK) {
                err = std::string("TLS verifikasi gagal: ") + X509_verify_cert_error_string(vr);
            } else {
                err = std::string("TLS handshake gagal: ") + eb;
            }
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            ::close(fd);
            return false;
        }
        if (!insecure) {
            const long vr = SSL_get_verify_result(ssl);
            if (vr != X509_V_OK) {
                err = std::string("TLS verifikasi gagal: ") + X509_verify_cert_error_string(vr);
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                ::close(fd);
                return false;
            }
        }
        // SO_RCVTIMEO 30 s dari socket() dibiarkan aktif. SSL_read pada socket
        // blocking bisa menunggu sisa record TLS yang tidak pernah datang, dan
        // karena m_ioMu juga dipakai submit(), stall itu akan membekukan semua
        // worker. Timeout membuat kegagalan seperti itu berakhir sebagai error
        // koneksi, bukan hang.
        {
            std::lock_guard<std::mutex> lock(m_ioMu);
            m_ctx = ctx;
            m_ssl = ssl;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_ioMu);
        m_fd = fd;
    }

    // --- login ---
    const std::string who = m_worker.empty() ? m_wallet : (m_wallet + "." + m_worker);
    std::string login;
    {
        std::lock_guard<std::mutex> lock(m_ioMu);
        login = "{\"id\":" + std::to_string(m_msgId++) +
                ",\"jsonrpc\":\"2.0\",\"method\":\"login\",\"params\":{"
                "\"login\":\"" + json_escape(who) + "\",\"pass\":\"x\",\"agent\":"
                "\"xcb-v2/1.0\",\"algo\":[\"rx/2b\"]}}";
    }
    if (!sendLine(login, err)) return false;

    // Tunggu respons login (punya "result" berisi job).
    for (int tries = 0; tries < 5; tries++) {
        std::string line;
        if (!recvLine(line, 30000, err)) return false;
        if (line.find("\"job_id\"") == std::string::npos) {
            if (line.find("\"error\"") != std::string::npos) {
                err = "login ditolak: " + line.substr(0, 200);
                return false;
            }
            continue;
        }
        std::string perr;
        Json doc = parseJson(line, &perr);
        if (!doc.isObj()) {
            err = "respons login bukan JSON: " + perr;
            return false;
        }
        std::string loginId;
        if (const Json* result = doc.get("result"); result && result->isObj()) {
            result->getStr("id", loginId);
        }
        if (!parseJobDoc(doc, first, err)) return false;
        {
            std::lock_guard<std::mutex> lock(m_ioMu);
            if (!loginId.empty()) m_loginId = loginId;
            m_jobId = first.id;
        }
        return true;
    }
    err = "login: tidak ada job dalam respons";
    return false;
}

bool ConnPool::sendLine(const std::string& line, std::string& err) {
    std::lock_guard<std::mutex> lock(m_ioMu);
    return sendLineLocked(line, err);
}

bool ConnPool::sendLineLocked(const std::string& line, std::string& err) {
    const std::string payload = line + "\n";
    if (m_ssl) {
        size_t off = 0;
        while (off < payload.size()) {
            const int n = SSL_write(static_cast<SSL*>(m_ssl), payload.data() + off,
                                    static_cast<int>(payload.size() - off));
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            err = "SSL_write gagal";
            return false;
        }
        return true;
    }
    size_t off = 0;
    while (off < payload.size()) {
        const ssize_t n = ::send(m_fd, payload.data() + off, payload.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            err = "send gagal";
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool ConnPool::recvLine(std::string& line, int timeoutMs, std::string& err) {
    line.clear();
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
    for (;;) {
        const auto nl = m_buf.find('\n');
        if (nl != std::string::npos) {
            line = m_buf.substr(0, nl);
            m_buf.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r')) line.pop_back();
            return true;
        }
        int waitMs = timeoutMs;
        if (timeoutMs > 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0) {
                err = "timeout";
                return false;
            }
            waitMs = static_cast<int>(left);
        }
        // Menunggu data tidak perlu lock: hanya poolLoop yang membaca, dan
        // menahan m_ioMu selama poll akan memblokir submit sampai timeout.
        int fd;
        bool pending = false;
        {
            std::lock_guard<std::mutex> lock(m_ioMu);
            fd = m_fd;
            pending = m_ssl && SSL_pending(static_cast<SSL*>(m_ssl)) > 0;
        }
        if (!pending) {
            struct pollfd pfd = {fd, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, waitMs);
            if (pr == 0) {
                err = "timeout";
                return false;
            }
            if (pr < 0) {
                if (errno == EINTR) continue;
                err = "poll gagal";
                return false;
            }
        }

        char buf[8192];
        int n = 0;
        {
            std::lock_guard<std::mutex> lock(m_ioMu);
            if (m_ssl) {
                const int r = SSL_read(static_cast<SSL*>(m_ssl), buf, sizeof(buf) - 1);
                if (r <= 0) {
                    const int e = SSL_get_error(static_cast<SSL*>(m_ssl), r);
                    // SSL_read hanya dipanggil setelah poll menyatakan data
                    // siap (atau SSL_pending > 0), jadi WANT_READ di sini
                    // berarti record TLS parsial yang tidak kunjung lengkap dan
                    // dihentikan SO_RCVTIMEO. Itu kegagalan koneksi, bukan
                    // timeout idle: kalau di-continue, poll() memetakan
                    // "timeout" ke Event::None dan sesi setengah mati dianggap
                    // sehat.
                    err = e == SSL_ERROR_WANT_READ ? "SSL_read timeout"
                                                   : "SSL_read EOF/error";
                    return false;
                }
                n = r;
            } else {
                const ssize_t r = ::recv(m_fd, buf, sizeof(buf) - 1, 0);
                if (r <= 0) {
                    err = "recv EOF";
                    return false;
                }
                n = static_cast<int>(r);
            }
        }
        buf[n] = '\0';
        if (m_buf.size() + static_cast<size_t>(n) > (1u << 20)) m_buf.clear();
        m_buf.append(buf, static_cast<size_t>(n));
    }
}

namespace {

// Dipakai poll() dan connectAndLogin() supaya satu baris hanya diparse sekali.
bool parseJobDoc(const Json& doc, ConnPoolJob& out, std::string& err) {
    // Job bisa berada di top-level params (notifikasi) atau result.job (login).
    const Json* job = doc.get("params");
    if (const Json* result = doc.get("result"); result && result->isObj()) {
        if (const Json* j = result->get("job")) job = j;
    }
    if (!job || !job->isObj()) {
        err = "job: objek job tidak ada";
        return false;
    }

    std::string id, algo, blobHex, seedHex, targetHex;
    double height = 0;
    if (!job->getStr("job_id", id)) {
        err = "job: job_id hilang";
        return false;
    }
    job->getStr("algo", algo);
    if (!job->getStr("blob", blobHex) || !job->getStr("seed_hash", seedHex) ||
        !job->getStr("target", targetHex) || !job->getNum("height", height)) {
        err = "job: field wajib hilang (blob/seed_hash/target/height)";
        return false;
    }
    if (!hexToBytes(blobHex, out.blob) ||
        (out.blob.size() != 80 && out.blob.size() != 76)) {
        err = "job: blob harus 80 (atau 76) byte, dapat " + std::to_string(out.blob.size());
        return false;
    }
    if (seedHex.size() != 64) {
        err = "job: seed_hash harus 64 hex";
        return false;
    }
    std::vector<uint8_t> tgt;
    if (!hexToBytes(targetHex, tgt) || tgt.size() != 8) {
        err = "job: target harus 8 byte";
        return false;
    }
    out.id = id;
    out.algo = algo;
    out.seedHex = seedHex;
    out.height = static_cast<uint32_t>(height);
    uint64_t t = 0;
    for (int i = 0; i < 8; i++)
        t |= static_cast<uint64_t>(tgt[static_cast<size_t>(i)]) << (8 * i);
    out.target = t;
    return true;
}

}  // namespace

ConnPool::Event ConnPool::poll(ConnPoolJob& job, int timeoutMs, std::string& detail) {
    std::string line;
    detail.clear();
    if (!recvLine(line, timeoutMs, detail)) {
        // Timeout idle bukan error koneksi: kembalikan None supaya loop
        // pemanggil bisa lanjut (dan tetap mengirim keepalive bila perlu).
        if (detail == "timeout") return Event::None;
        return Event::Error;
    }

    // Klasifikasi lewat JSON, bukan pencarian substring: pool boleh mengirim
    // spasi atau urutan key berbeda, dan teks "method":"job" bisa muncul di
    // dalam pesan lain. Satu baris cukup diparse sekali.
    std::string perr;
    const Json doc = parseJson(line, &perr);
    if (!doc.isObj()) {
        // Baris kosong bukan kesalahan; baris rusak berarti notifikasi job
        // bisa hilang tanpa jejak, jadi catat agar tidak silent.
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) return Event::None;
        lg::warn("conn", "baris pool bukan JSON: " + perr + " | " + line.substr(0, 160));
        return Event::None;
    }

    std::string method;
    if (doc.getStr("method", method)) {
        if (method == "job") {
            if (!parseJobDoc(doc, job, detail)) return Event::Error;
            {
                std::lock_guard<std::mutex> lock(m_ioMu);
                m_jobId = job.id;
            }
            return Event::Job;
        }
        if (method == "keepalived") return Event::None;
    }

    if (const Json* errv = doc.get("error"); errv && !errv->isNull()) {
        // Sertakan respons mentah (dipotong): pesan error rplant kadang
        // berupa objek/array dan versi lama hanya menampilkan "error".
        detail = "submit ditolak: " + line.substr(0, 220);
        return Event::Rejected;
    }
    if (const Json* result = doc.get("result")) {
        if (result->isObj()) {
            std::string status;
            if (result->getStr("status", status)) {
                if (status == "OK") return Event::Accepted;
                detail = "status=" + status;
                return Event::Rejected;
            }
        } else if (result->t == Json::T::Str && result->str == "OK") {
            return Event::Accepted;
        } else if (result->t == Json::T::Bool && result->b) {
            return Event::Accepted;
        }
    }
    return Event::None;
}

bool ConnPool::submit(const ConnPoolJob& job, uint32_t nonce, const uint8_t hash[32],
                      std::string& err) {
    std::lock_guard<std::mutex> lock(m_ioMu);
    if (!m_ready) {
        err = "tidak terkoneksi";
        return false;
    }
    uint8_t nonceLE[4] = {static_cast<uint8_t>(nonce), static_cast<uint8_t>(nonce >> 8),
                          static_cast<uint8_t>(nonce >> 16),
                          static_cast<uint8_t>(nonce >> 24)};
    const std::string nonceHex = toHex(nonceLE, 4);
    const std::string resultHex = toHex(hash, 32);
    const std::string msg = "{\"id\":" + std::to_string(m_msgId++) +
                            ",\"jsonrpc\":\"2.0\",\"method\":\"submit\",\"params\":{"
                            "\"id\":\"" + json_escape(m_loginId) + "\",\"job_id\":\"" +
                            json_escape(job.id) +
                            "\",\"nonce\":\"" + nonceHex + "\",\"result\":\"" + resultHex +
                            "\",\"algo\":\"" + json_escape(job.algo.empty() ? "rx/2b" : job.algo) +
                            "\"}}";
    return sendLineLocked(msg, err);
}
