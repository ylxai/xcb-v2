#include "BlozPool.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "../Log.hpp"
#include "BlozJson.hpp"

#include <chrono>

namespace bloz {

namespace {

constexpr int kIoTimeoutSec = 30;

// Dua job dianggap sama bila seluruh isinya sama. job_id saja tidak cukup:
// pool boleh memakai ulang id untuk pekerjaan yang berbeda.
bool sameJobContent(const Job& a, const Job& b) {
    return a.id == b.id && a.target == b.target && a.height == b.height &&
           a.blob == b.blob && std::memcmp(a.key, b.key, sizeof(a.key)) == 0;
}

std::string toHex(const uint8_t* p, size_t n) {
    static const char hx[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += hx[p[i] >> 4];
        s += hx[p[i] & 0x0f];
    }
    return s;
}

}  // namespace

Pool::Pool(PoolConfig cfg) : m_cfg(std::move(cfg)) {}

Pool::~Pool() { disconnect(); }

void Pool::disconnect() {
    std::lock_guard<std::mutex> lock(m_ioMu);
    m_ready.store(false, std::memory_order_release);
    if (m_ssl) {
        SSL_shutdown(static_cast<SSL*>(m_ssl));
        SSL_free(static_cast<SSL*>(m_ssl));
        m_ssl = nullptr;
    }
    if (m_sslCtx) {
        SSL_CTX_free(static_cast<SSL_CTX*>(m_sslCtx));
        m_sslCtx = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_buf.clear();
    {
        // Share yang belum sempat terkirim milik koneksi lama; jangan dibawa
        // ke sesi berikutnya (job-nya sudah basi).
        std::lock_guard<std::mutex> out(m_outMu);
        m_outBuf.clear();
    }
}

bool Pool::sendLine(const std::string& line, std::string& err) {
    // Pemanggil sudah memegang m_ioMu.
    const std::string payload = line + "\n";
    size_t off = 0;
    while (off < payload.size()) {
        int n = 0;
        if (m_ssl) {
            n = SSL_write(static_cast<SSL*>(m_ssl), payload.data() + off,
                          static_cast<int>(payload.size() - off));
        } else {
            n = static_cast<int>(::send(m_fd, payload.data() + off, payload.size() - off,
                                        MSG_NOSIGNAL));
        }
        if (n <= 0) {
            err = "tulis pool gagal";
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool Pool::flushOutLocked(std::string& err) {
    std::string pending;
    {
        std::lock_guard<std::mutex> lock(m_outMu);
        if (m_outBuf.empty()) return true;
        pending.swap(m_outBuf);
    }
    // Pesan JSON tidak pernah berisi newline (di-escape), jadi memecah per
    // baris aman dan sendLine tetap menambahkan newline-nya sendiri.
    size_t start = 0;
    while (start < pending.size()) {
        const size_t nl = pending.find('\n', start);
        const size_t end = (nl == std::string::npos) ? pending.size() : nl;
        if (end > start && !sendLine(pending.substr(start, end - start), err)) return false;
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return true;
}

bool Pool::recvLine(std::string& line, int timeoutMs, std::string& err) {
    // Pemanggil sudah memegang m_ioMu.
    //
    // Satu deadline absolut untuk seluruh panggilan. Sebelumnya setiap poll
    // memakai timeout penuh, jadi reply yang datang bertahap bisa menahan
    // m_ioMu sampai 64 kali timeout — dan selama itu submit() dari worker
    // ikut tertahan.
    const auto start = std::chrono::steady_clock::now();
    const int totalMs = timeoutMs > 0 ? timeoutMs : (kIoTimeoutSec * 1000);
    const auto deadline = start + std::chrono::milliseconds(totalMs);

    for (int tries = 0; tries < 256; tries++) {
        const size_t nl = m_buf.find('\n');
        if (nl != std::string::npos) {
            line = m_buf.substr(0, nl);
            m_buf.erase(0, nl + 1);
            return true;
        }

        // Data yang sudah terdekripsi di buffer OpenSSL hanya bisa dibaca lewat
        // SSL_read; poll() pada socket tidak akan menyala karena byte itu tidak
        // lagi berada di kernel. Tanpa cek ini, notifikasi job bisa tertahan
        // sampai paket berikutnya datang.
        bool buffered = false;
        if (m_ssl) {
            buffered = SSL_pending(static_cast<SSL*>(m_ssl)) > 0;
        }

        if (!buffered) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                err = "timeout menunggu data pool";
                return false;
            }
            const long long left = static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            const int waitMs = static_cast<int>(left > 0 ? left : 1);

            struct pollfd pfd;
            pfd.fd = m_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            const int pr = ::poll(&pfd, 1, waitMs);
            if (pr == 0) {
                err = "timeout menunggu data pool";
                return false;
            }
            if (pr < 0) {
                if (errno == EINTR) continue;
                err = std::string("poll gagal: ") + std::strerror(errno);
                return false;
            }
        }

        char buf[8192];
        int n = 0;
        if (m_ssl) {
            n = SSL_read(static_cast<SSL*>(m_ssl), buf, sizeof(buf) - 1);
            if (n <= 0) {
                const int e = SSL_get_error(static_cast<SSL*>(m_ssl), n);
                if (e == SSL_ERROR_WANT_READ) continue;
                err = (n == 0 || e == SSL_ERROR_ZERO_RETURN) ? "pool menutup koneksi"
                                                             : "SSL_read gagal";
                return false;
            }
        } else {
            n = static_cast<int>(::recv(m_fd, buf, sizeof(buf) - 1, 0));
            if (n == 0) {
                err = "pool menutup koneksi";
                return false;
            }
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                err = std::string("recv gagal: ") + std::strerror(errno);
                return false;
            }
        }
        m_buf.append(buf, static_cast<size_t>(n));
    }
    err = "baris pool terlalu panjang";
    return false;
}

bool Pool::connect(std::string& err) {
    disconnect();

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string port = std::to_string(m_cfg.port);
    if (::getaddrinfo(m_cfg.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        err = "resolve gagal: " + m_cfg.host;
        return false;
    }

    int fd = -1;
    int lastErr = 0;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            lastErr = errno;
            continue;
        }
        struct timeval tv;
        tv.tv_sec = kIoTimeoutSec;
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
        err = std::string("connect gagal: ") + std::strerror(lastErr);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_ioMu);
        m_fd = fd;
        m_buf.clear();

        if (m_cfg.tls) {
            SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
            if (!ctx) {
                err = "SSL_CTX_new gagal";
                return false;
            }
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            // Tanpa verify mode PEER, OpenSSL tidak memverifikasi chain dan
            // SSL_get_verify_result() setelah handshake bisa melaporkan status
            // sisa handshake sebelumnya. Set eksplisit: chain harus valid.
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(ctx);
            SSL* ssl = SSL_new(ctx);
            if (!ssl) {
                SSL_CTX_free(ctx);
                err = "SSL_new gagal";
                return false;
            }
            if (SSL_set_fd(ssl, fd) != 1) {
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                err = "SSL_set_fd gagal";
                return false;
            }

            // Verifikasi hostname/IP selalu aktif: pool BLOZ publik memakai
            // sertifikat valid, dan menerima sertifikat apa pun akan membuat
            // login (berisi alamat wallet) bisa disadap.
            unsigned char abuf[sizeof(struct in6_addr)];
            const bool isIp = ::inet_pton(AF_INET, m_cfg.host.c_str(), abuf) == 1 ||
                              ::inet_pton(AF_INET6, m_cfg.host.c_str(), abuf) == 1;
            const int hostOk = isIp ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl),
                                                                   m_cfg.host.c_str())
                                    : SSL_set1_host(ssl, m_cfg.host.c_str());
            if (hostOk != 1) {
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                err = "setup verifikasi hostname TLS gagal";
                return false;
            }
            // SNI: tanpa ini pool yang dilayani virtual host/CDN bisa
            // mempersembahkan sertifikat default dan handshake ditolak. Hanya
            // untuk nama host; alamat IP literal tidak boleh dikirim sebagai
            // SNI (RFC 6066).
            if (!isIp) {
                if (SSL_set_tlsext_host_name(ssl, m_cfg.host.c_str()) != 1) {
                    SSL_free(ssl);
                    SSL_CTX_free(ctx);
                    err = "gagal set SNI TLS";
                    return false;
                }
            }
            if (SSL_connect(ssl) != 1) {
                char eb[256] = {0};
                ERR_error_string_n(ERR_peek_last_error(), eb, sizeof(eb));
                const long vr = SSL_get_verify_result(ssl);
                err = (vr != X509_V_OK)
                          ? std::string("TLS verifikasi gagal: ") +
                                X509_verify_cert_error_string(vr)
                          : std::string("TLS handshake gagal: ") + eb;
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                return false;
            }
            if (SSL_get_verify_result(ssl) != X509_V_OK) {
                err = "TLS verifikasi gagal setelah handshake";
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                return false;
            }
            m_ssl = ssl;
            m_sslCtx = ctx;
        }

        // Login: dialek Monero/XMRig. Wallet di "login", worker di "worker".
        // Wallet saja bila worker kosong; kalau tidak pool memperlakukan
        // bagian setelah titik sebagai label.
        const std::string who = m_cfg.worker.empty()
                                    ? m_cfg.login
                                    : (m_cfg.login + "." + m_cfg.worker);
        // m_msgId atomik: submit() di worker juga memakainya.
        {
            std::lock_guard<std::mutex> lock(m_loginMu);
            m_loginId.clear();
        }
        const std::string loginMsg =
            "{\"id\":" + std::to_string(m_msgId.fetch_add(1)) +
            ",\"jsonrpc\":\"2.0\",\"method\":\"login\"," +
            "\"params\":{\"login\":\"" + jsonEscape(who) + "\",\"pass\":\"x\",\"agent\":\"" +
            jsonEscape(m_cfg.agent) + "\",\"algo\":[\"rx/blockzero\"]}}";
        if (!sendLine(loginMsg, err)) return false;
    }

    // Tunggu balasan login yang berisi job pertama. Pesan login dikirim di atas
    // dengan `who` masih dalam scope; nama worker disimpan supaya log di bawah
    // bisa menyebutnya tanpa mengulang konstruksi string.
    const std::string loginName =
        m_cfg.worker.empty() ? m_cfg.login : (m_cfg.login + "." + m_cfg.worker);
    for (int tries = 0; tries < 8; tries++) {
        std::string line;
        std::string rerr;
        {
            std::lock_guard<std::mutex> lock(m_ioMu);
            if (!recvLine(line, kIoTimeoutSec * 1000, rerr)) {
                err = rerr;
                return false;
            }
        }
        if (line.find("\"job_id\"") == std::string::npos) {
            if (line.find("\"error\"") != std::string::npos &&
                line.find("\"error\":null") == std::string::npos) {
                err = "login ditolak: " + line.substr(0, 200);
                return false;
            }
            continue;
        }
        Job job;
        std::string perr;
        if (!parseJob(line, job, perr)) {
            err = "job login tidak valid: " + perr;
            return false;
        }

        // Session id untuk submit adalah `result.id`, bukan id request JSON-RPC.
        // rplant kebetulan mengembalikan "1" (sama dengan request id) sehingga
        // kesalahan ini tidak terlihat, tetapi pool lain memberi id berbeda dan
        // semua share akan ditolak. Fallback ke request id bila tidak ada.
        {
            const Json doc = parseJson(line, nullptr);
            std::string session;
            if (const Json* r = doc.get("result")) {
                if (r->isObj() && r->getStr("id", session) && !session.empty()) {
                    std::lock_guard<std::mutex> lock(m_loginMu);
                    m_loginId = session;
                }
            }
            std::lock_guard<std::mutex> lock(m_loginMu);
            if (m_loginId.empty()) m_loginId = std::to_string(m_msgId.load() - 1);
        }
        {
            std::lock_guard<std::mutex> lock(m_jobMu);
            m_job = job;
            m_haveJob = true;
        }
        m_ready.store(true, std::memory_order_release);
        lg::info("bloz", "login pool OK (" + m_cfg.host + ":" + std::to_string(m_cfg.port) +
                            (m_cfg.tls ? ", tls)" : ")") + " worker " + loginName +
                            ", tinggi " + std::to_string(job.height) + ", diff " +
                            std::to_string(static_cast<long long>(job.difficultyValue())));
        return true;
    }
    err = "login tidak menghasilkan job";
    return false;
}

bool Pool::ensureConnected(std::string& err) {
    if (m_ready.load(std::memory_order_acquire) && m_fd >= 0) return true;
    return connect(err);
}

bool Pool::currentJob(Job& out) {
    std::lock_guard<std::mutex> lock(m_jobMu);
    if (!m_haveJob) return false;
    out = m_job;
    return true;
}

bool Pool::nextJob(Job& out, int timeoutMs, std::string& err) {
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : kIoTimeoutSec * 1000);
    // Tunggu dalam irisan pendek sambil mengirim antrean submit. Kunci socket
    // dilepas tiap <=100 ms supaya disconnect() tidak tertahan, dan worker
    // tidak pernah berebut kunci ini: mereka hanya menulis ke m_outBuf.
    for (;;) {
        bool got = false;
        {
            std::lock_guard<std::mutex> lock(m_ioMu);
            if (m_fd < 0) {
                err = "tidak terkoneksi";
                return false;
            }
            if (!flushOutLocked(err)) {
                m_ready.store(false, std::memory_order_release);
                return false;
            }
            got = recvLine(line, 100, err);
            if (!got && err != "timeout menunggu data pool") {
                m_ready.store(false, std::memory_order_release);
                return false;
            }
        }
        if (got) break;
        if (std::chrono::steady_clock::now() >= deadline) return false;
    }

    // Notifikasi job atau balasan submit. Balasan submit dihitung di sini
    // supaya statistik accepted/rejected terisi tanpa jalur terpisah.
    if (line.find("\"job_id\"") != std::string::npos) {
        Job job;
        std::string perr;
        if (!parseJob(line, job, perr)) {
            err = "job tidak valid: " + perr;
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_jobMu);
            // Bandingkan isi, bukan hanya id: pool boleh memakai ulang job_id
            // untuk pekerjaan yang berbeda (termasuk setelah reconnect), dan
            // mengabaikannya membuat worker menambang blob lama.
            if (m_haveJob && sameJobContent(m_job, job)) return false;
            m_job = job;
            m_haveJob = true;
        }
        out = job;
        return true;
    }

    // Balasan submit: parse JSON-nya. Pencarian substring untuk beberapa
    // ejaan "status" melewatkan error JSON-RPC (tanpa field status) dan
    // salah menghitung status yang memakai whitespace JSON lain.
    {
        const Json doc = parseJson(line, nullptr);
        if (doc.isObj()) {
            bool counted = false;
            if (const Json* r = doc.get("result")) {
                if (r->isObj()) {
                    std::string status;
                    if (r->getStr("status", status)) {
                        counted = true;
                        if (status == "OK") {
                            m_accepted.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            m_rejected.fetch_add(1, std::memory_order_relaxed);
                            std::lock_guard<std::mutex> lock(m_rejectMu);
                            m_lastReject = line.substr(0, 300);
                        }
                    }
                }
            }
            // Error JSON-RPC (mis. "Invalid job id") tidak selalu membawa
            // status, tetapi tetap penolakan share.
            if (!counted && doc.get("error")) {
                m_rejected.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(m_rejectMu);
                m_lastReject = line.substr(0, 300);
            }
        }
    }
    return false;
}

std::string Pool::lastReject() const {
    std::lock_guard<std::mutex> lock(m_rejectMu);
    return m_lastReject;
}

bool Pool::submit(const Job& job, const uint8_t digest[32], std::string& err) {
    if (!m_ready.load(std::memory_order_acquire)) {
        err = "tidak terkoneksi";
        return false;
    }
    // Bentuk submit yang dipakai cpuminer-rplant untuk rx/blockzero, direkam
    // dari koneksi nyata:
    //   {"method":"submit","params":{"id":"<sesi>","job_id":"…",
    //    "nonce":"55030000","result":"<64 hex>","algo":"rx/blockzero"}}
    // `result` adalah digest RandomX mentah; tidak ada field "commitment".
    //
    // Pesan hanya diantrekan: socket dimiliki io thread, jadi worker tidak
    // pernah memegang kunci I/O. Versi lama menulis langsung di bawah m_ioMu
    // dan bisa kelaparan puluhan detik saat io thread menunggu job — share
    // jadi terlambat sampai melewati batas job.
    // Salin session id di bawah kuncinya: io thread bisa menggantinya saat
    // reconnect, dan membaca std::string sambil ditulis adalah data race.
    std::string loginId;
    {
        std::lock_guard<std::mutex> lock(m_loginMu);
        loginId = m_loginId;
    }
    const std::string msg =
        "{\"id\":" + std::to_string(m_msgId.fetch_add(1)) +
        ",\"jsonrpc\":\"2.0\",\"method\":\"submit\"," +
        "\"params\":{\"id\":\"" + jsonEscape(loginId) + "\",\"job_id\":\"" +
        jsonEscape(job.id) + "\",\"nonce\":\"" + job.nonceHex(0) + "\",\"result\":\"" +
        toHex(digest, 32) + "\",\"algo\":\"" + jsonEscape(job.algo) + "\"}}";
    {
        std::lock_guard<std::mutex> lock(m_outMu);
        m_outBuf += msg;
        m_outBuf += '\n';
    }
    return true;
}

}  // namespace bloz
