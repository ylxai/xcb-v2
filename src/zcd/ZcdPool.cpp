#include "ZcdPool.hpp"

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
#include "ZcdJson.hpp"

namespace zcd {

namespace {

constexpr int kIoTimeoutSec = 30;

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
            n = static_cast<int>(::send(m_fd, payload.data() + off, payload.size() - off, 0));
        }
        if (n <= 0) {
            err = "tulis pool gagal";
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool Pool::recvLine(std::string& line, int timeoutMs, std::string& err) {
    // Pemanggil sudah memegang m_ioMu.
    const long long deadline = timeoutMs <= 0 ? 0 : timeoutMs;
    for (int tries = 0; tries < 64; tries++) {
        const size_t nl = m_buf.find('\n');
        if (nl != std::string::npos) {
            line = m_buf.substr(0, nl);
            m_buf.erase(0, nl + 1);
            return true;
        }

        struct pollfd pfd;
        pfd.fd = m_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int waitMs = deadline > 0 ? static_cast<int>(deadline) : (kIoTimeoutSec * 1000);
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

            // Verifikasi hostname/IP selalu aktif: pool ZCD publik memakai
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
        m_loginId = std::to_string(m_msgId);
        const std::string loginMsg =
            "{\"id\":" + std::to_string(m_msgId++) + ",\"jsonrpc\":\"2.0\",\"method\":\"login\"," +
            "\"params\":{\"login\":\"" + jsonEscape(who) + "\",\"pass\":\"x\",\"agent\":\"" +
            jsonEscape(m_cfg.agent) + "\",\"algo\":[\"rx/2\"]}}";
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
        {
            std::lock_guard<std::mutex> lock(m_jobMu);
            m_job = job;
            m_haveJob = true;
        }
        m_ready.store(true, std::memory_order_release);
        lg::info("zcd", "login pool OK (" + m_cfg.host + ":" + std::to_string(m_cfg.port) +
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
    {
        std::lock_guard<std::mutex> lock(m_ioMu);
        if (m_fd < 0) {
            err = "tidak terkoneksi";
            return false;
        }
        if (!recvLine(line, timeoutMs, err)) {
            if (err != "timeout menunggu data pool") m_ready.store(false, std::memory_order_release);
            return false;
        }
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
            if (m_haveJob && m_job.id == job.id) return false;
            m_job = job;
            m_haveJob = true;
        }
        out = job;
        return true;
    }

    if (line.find("\"status\"") != std::string::npos) {
        const bool ok = line.find("\"status\":\"OK\"") != std::string::npos ||
                        line.find("\"status\": \"OK\"") != std::string::npos;
        if (ok) {
            m_accepted.fetch_add(1, std::memory_order_relaxed);
        } else {
            m_rejected.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(m_rejectMu);
            m_lastReject = line.substr(0, 300);
        }
    }
    return false;
}

std::string Pool::lastReject() const {
    std::lock_guard<std::mutex> lock(m_rejectMu);
    return m_lastReject;
}

bool Pool::submit(const Job& job, const uint8_t digest[32], const uint8_t commitmentBytes[32],
                  std::string& err) {
    if (!m_ready.load(std::memory_order_acquire)) {
        err = "tidak terkoneksi";
        return false;
    }
    // XMRig: field "result" = commitment, field "commitment" = digest mentah.
    // Namanya terbalik relatif isinya (bug nyata di XMRig v6.26.0 yang
    // didokumentasikan zycord-node core/pow/pow.go).
    const std::string msg =
        "{\"id\":" + std::to_string(m_msgId++) + ",\"jsonrpc\":\"2.0\",\"method\":\"submit\"," +
        "\"params\":{\"id\":\"" + jsonEscape(m_loginId) + "\",\"job_id\":\"" +
        jsonEscape(job.id) + "\",\"nonce\":\"" + job.nonceHex(0) + "\",\"result\":\"" +
        toHex(commitmentBytes, 32) + "\",\"commitment\":\"" + toHex(digest, 32) +
        "\",\"algo\":\"" + jsonEscape(job.algo) + "\"}}";
    std::lock_guard<std::mutex> lock(m_ioMu);
    return sendLine(msg, err);
}

}  // namespace zcd
