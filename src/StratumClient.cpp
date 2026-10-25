#include "StratumClient.hpp"

#include "Log.hpp"

#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

// ------------------------------------------------------------
// Minimal JSON helpers (no external deps).
// ------------------------------------------------------------
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

static std::string json_find_str(const std::string& json, const std::string& key) {
    auto pos = json.find('"' + key + '"');
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + key.size() + 2);
    if (pos == std::string::npos) return "";
    size_t end = pos + 1;
    std::string val;
    while (end < json.size()) {
        if (json[end] == '\\') {
            val += json[end + 1];
            end += 2;
            continue;
        }
        if (json[end] == '"') break;
        val += json[end++];
    }
    return val;
}

static std::string json_result_array(const std::string& json, int idx) {
    auto pos = json.find("\"result\"");
    if (pos == std::string::npos) return "";
    pos = json.find('[', pos);
    if (pos == std::string::npos) return "";
    pos++;
    int depth = 0, cur = 0;
    while (pos < json.size() && cur <= idx) {
        if (json[pos] == '[') depth++;
        if (json[pos] == ']') {
            if (--depth < 0) break;
        }
        if (json[pos] == ',' && depth == 0)
            cur++;
        else if (cur == idx) {
            if (json[pos] == '"') {
                std::string val;
                pos++;
                while (pos < json.size()) {
                    if (json[pos] == '\\') {
                        val += json[pos + 1];
                        pos += 2;
                        continue;
                    }
                    if (json[pos] == '"') break;
                    val += json[pos++];
                }
                return val;
            } else if (json[pos] == '-' || json[pos] == '+' ||
                       (json[pos] >= '0' && json[pos] <= '9')) {
                std::string num;
                while (pos < json.size() &&
                       (json[pos] == '-' || json[pos] == '+' || json[pos] == '.' ||
                        (json[pos] >= '0' && json[pos] <= '9') || json[pos] == 'e' ||
                        json[pos] == 'E'))
                    num += json[pos++];
                return num;
            } else if (json.substr(pos, 4) == "true") return "true";
            else if (json.substr(pos, 5) == "false") return "false";
        }
        pos++;
    }
    return "";
}

static bool json_is_true(const std::string& json) {
    auto pos = json.find("\"result\"");
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + 7);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    return json.substr(pos, 4) == "true";
}

static uint64_t json_id_val(const std::string& json) {
    auto pos = json.find("\"id\":");
    if (pos == std::string::npos) return 0;
    pos += 5;
    while (pos < json.size() && json[pos] == ' ') pos++;
    uint64_t id = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
        id = id * 10 + (json[pos++] - '0');
    return id;
}

// Extract a human-readable reason from a stratum error response.
// Handles  {"error": {"code":..,"message":".."}},  {"error":"str"}, and the
// classic stratum array form  {"error":[21,"above target",null]}.
static std::string json_error_reason(const std::string& json) {
    auto pos = json.find("\"error\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + 6);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (pos < json.size() && json[pos] == '"') {
        // {"error":"some string"}
        size_t end = pos + 1;
        std::string val;
        while (end < json.size()) {
            if (json[end] == '\\') {
                val += json[end + 1];
                end += 2;
                continue;
            }
            if (json[end] == '"') break;
            val += json[end++];
        }
        if (!val.empty()) return val;
    } else if (pos < json.size() && json[pos] == '{') {
        std::string msg = json_find_str(json.substr(pos), "message");
        if (!msg.empty()) return msg;
        std::string code = json_find_str(json.substr(pos), "code");
        if (!code.empty()) return "code " + code;
    } else if (pos < json.size() && json[pos] == '[') {
        // [code, "message", null] — ambil string pertama di dalam array.
        size_t end = pos + 1;
        while (end < json.size() && json[end] != '"') {
            if (json[end] == ']') return "";
            end++;
        }
        end++;  // lewati kutip pembuka
        std::string val;
        while (end < json.size()) {
            if (json[end] == '\\') {
                val += json[end + 1];
                end += 2;
                continue;
            }
            if (json[end] == '"') break;
            val += json[end++];
        }
        if (!val.empty()) return val;
    }
    return "";
}

static std::string strip_0x(const std::string& s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return s.substr(2);
    return s;
}

// Kumpulkan seluruh elemen array "params" (string, angka, true/false) secara
// berurutan. Dipakai untuk mining.notify yang param-nya panjang variabel.
// MiningCore membungkus merkle branch sebagai nested array di tengah params
// (mis. params[4] = ["hash1","hash2"] atau [] saat tanpa transaksi) — nested
// array satu level dibuka ratakan, jadi item branch muncul sebagai elemen
// tersendiri dan indexing [jobId, prevhash, coinb1, coinb2, branch...,
// version, nbits, ntime, clean] tetap lurus.
static void json_params_vec(const std::string& json, std::vector<std::string>& out) {
    out.clear();
    auto pos = json.find("\"params\"");
    if (pos == std::string::npos) return;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return;
    pos++;
    int depth = 0;
    std::string cur;
    bool inStr = false, hasElem = false;
    auto flush = [&]() {
        if (hasElem || !cur.empty()) {
            out.push_back(cur);
            cur.clear();
            hasElem = false;
        }
    };
    while (pos < json.size()) {
        char c = json[pos];
        if (inStr) {
            if (c == '\\') {
                cur += json[pos + 1];
                pos += 2;
                continue;
            }
            if (c == '"') {
                inStr = false;
                hasElem = true;
                pos++;
                continue;
            }
            cur += c;
            pos++;
            continue;
        }
        if (c == '"') {
            inStr = true;
            cur.clear();
            pos++;
            continue;
        }
        if (c == '[') {
            depth++;
            pos++;
            continue;
        }
        if (c == ']') {
            if (depth == 0) break;  // tutup array params luar
            depth--;                // tutup nested array: ratakan
            flush();
            pos++;
            continue;
        }
        if (c == ',') {
            flush();
            pos++;
            continue;
        }
        if (c == ' ' || c == '\t') {
            pos++;
            continue;
        }
        // angka / true / false / null
        cur += c;
        hasElem = true;
        pos++;
    }
    flush();
}

// ------------------------------------------------------------
// Hex decoding yang tidak boleh mematikan proses.
// Semua angka di bawah datang dari pool. std::stoul/std::stoull melempar
// std::invalid_argument untuk input non-hex, dan tidak ada satu pun try/catch
// di jalur ini — satu respons sampah dari pool dulu berujung std::terminate.
// ------------------------------------------------------------
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_is_valid(const std::string& hex) {
    if (hex.empty()) return false;
    for (char c : hex)
        if (hex_nibble(c) < 0) return false;
    return true;
}

// hex -> byte. false untuk panjang ganjil atau karakter non-hex.
static bool hex_to_bytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (!hex_is_valid(hex) || (hex.size() % 2) != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>((hex_nibble(hex[i]) << 4) | hex_nibble(hex[i + 1])));
    return true;
}

// hex (maksimal 16 char) -> uint64.
static bool hex_to_u64(const std::string& hex, uint64_t& out) {
    if (!hex_is_valid(hex) || hex.size() > 16) return false;
    uint64_t v = 0;
    for (char c : hex) v = (v << 4) | static_cast<uint64_t>(hex_nibble(c));
    out = v;
    return true;
}

// Target share 256-bit dari difficulty stratum, persis diff_to_hash()
// cpuminer-opt (util.c): target = (1/diff) * 2^224, low128 = 0xFF..FF.
// Skala diff standar: diff 1 = 2^32 hash. Faktor 65536 yang pernah dipakai
// di sini salah — opt_target_factor yespower di cpuminer-opt = 1.0 (yang
// di-set 128/256 hanya algo lyra2/allium), jadi pool memakai skala standar
// dan target kita yang 65536x terlalu mudah membuat SEMUA share ditolak
// ("low difficulty share" di MiningCore, ditolak semua di rplant).
// out[] urutan big-endian: out[0] = 64-bit paling signifikan.
static void diff_to_target256(double diff, uint64_t out[4]) {
    const long double exp96 = 79228162514264337593543950336.0L;  // 2^96
    if (!(diff > 0)) {
        out[0] = out[1] = out[2] = out[3] = ~0ULL;
        return;
    }
    long double m = (1.0L / diff) * exp96;
    __uint128_t hi;
    if (m >= (long double)((__uint128_t)0 - 1)) {
        hi = ~((__uint128_t)0);
    } else {
        hi = (__uint128_t)m;
    }
    out[0] = static_cast<uint64_t>(hi >> 64);
    out[1] = static_cast<uint64_t>(hi);
    out[2] = ~0ULL;
    out[3] = ~0ULL;
}

// Hex target 64-char (big-endian) -> 4 word BE.
static bool hex_to_target256(const std::string& hex, uint64_t out[4]) {
    if (hex.size() != 64 || !hex_is_valid(hex)) return false;
    for (int w = 0; w < 4; w++) {
        uint64_t v = 0;
        for (int i = 0; i < 16; i++) v = (v << 4) | (uint64_t)hex_nibble(hex[w * 16 + i]);
        out[w] = v;
    }
    return true;
}

// double SHA-256 (Bitcoin-style). out = 32 byte digest natural.
static void sha256d(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint8_t mid[32];
    SHA256(data, len, mid);
    SHA256(mid, sizeof(mid), out);
}

// ------------------------------------------------------------
// TLS (stratum+ssl). Context OpenSSL dibuat sekali per client dan dipakai
// ulang antar reconnect. Sertifikat pool tidak diverifikasi: pool mining
// umumnya memakai sertifikat sendiri yang berganti-ganti, dan yang dibutuhkan
// di sini adalah jalur terenkripsi, bukan autentikasi server.
// ------------------------------------------------------------
static int tls_write_all(SSL* ssl, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = SSL_write(ssl, data + off, static_cast<int>(len - off));
        if (n > 0) {
            off += static_cast<size_t>(n);
            continue;
        }
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_WRITE) continue;
        return -1;
    }
    return 0;
}

// SSL_read dengan retry WANT_READ via poll pada fd.
static int tls_read_retry(SSL* ssl, int fd, char* buf, int len, int timeoutMs) {
    while (true) {
        int n = SSL_read(ssl, buf, len);
        if (n > 0) return n;
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, timeoutMs) <= 0) return -1;
            continue;
        }
        if (err == SSL_ERROR_WANT_WRITE) continue;
        return -1;
    }
}

// ------------------------------------------------------------
StratumClient::StratumClient(std::vector<PoolConfig> pools, StratumMode mode)
    : m_mode(mode), m_pools(std::move(pools)) {
    if (m_pools.empty()) {
        PoolConfig fallback;
        fallback.host = "localhost";
        fallback.port = 8008;
        m_pools.push_back(fallback);
    }
}

StratumClient::~StratumClient() {
    disconnect();
    if (m_sslCtx) {
        SSL_CTX_free(m_sslCtx);
        m_sslCtx = nullptr;
    }
}

bool StratumClient::isConnected() const { return m_sock.load() >= 0; }

std::string StratumClient::poolHost() const {
    int idx = m_poolIdx.load();
    if (idx < 0 || idx >= (int)m_pools.size()) return "";
    return m_pools[idx].host + ":" + std::to_string(m_pools[idx].port);
}

void StratumClient::notifyPoolChange() {
    if (m_onPoolChange) m_onPoolChange(m_poolIdx.load(), poolHost());
}

void StratumClient::sendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    int fd = m_sock.load();
    if (fd < 0) return;
    std::string payload = line + "\n";
    if (m_ssl) {
        tls_write_all(m_ssl, payload.data(), payload.size());
        return;
    }
    ::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL);
}

std::string StratumClient::recvLine(double timeoutSec) {
    std::lock_guard<std::mutex> lock(m_recvMutex);
    while (true) {
        auto nl = m_recvBuf.find('\n');
        if (nl != std::string::npos) {
            std::string line = m_recvBuf.substr(0, nl);
            m_recvBuf.erase(0, nl + 1);
            return line;
        }
        int fd = m_sock.load();
        if (fd < 0) return "";
        int ms = static_cast<int>(timeoutSec * 1000);
        SSL* ssl = m_ssl;
        // Poll hanya untuk jalur plaintext, atau TLS saat SSL_pending()==0.
        // Data bisa masih tertahan di buffer SSL walau fd sudah kosong —
        // tanpa cek ini, recvLine menganggap timeout padahal baris lengkap
        // sudah siap dibaca.
        if (!ssl || SSL_pending(ssl) == 0) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            int ret = ::poll(&pfd, 1, ms);
            if (ret <= 0) return "";
        }
        char buf[8192];
        ssize_t n;
        if (ssl)
            n = tls_read_retry(ssl, fd, buf, sizeof(buf) - 1, ms);
        else
            n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return "";
        buf[n] = 0;
        if (m_recvBuf.size() + (size_t)n > (1u << 20)) m_recvBuf.clear();  // cap buffer
        m_recvBuf += std::string(buf, n);
    }
}

// Read lines until the response matching wantId arrives. Any other line
// (share results, notifications) is dispatched to handleResponse().
std::string StratumClient::recvResponse(uint64_t wantId, double timeoutSec) {
    while (true) {
        std::string line = recvLine(timeoutSec);
        if (line.empty()) return "";
        if (json_id_val(line) == wantId) return line;
        handleResponse(line);
    }
}

bool StratumClient::openSocket(const PoolConfig& p) {
    std::string portStr = std::to_string(p.port);
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(p.host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        lg::warn("stratum", "DNS fail: " + p.host);
        if (res) ::freeaddrinfo(res);
        return false;
    }
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return false;
    }
    ::fcntl(fd, F_SETFL, O_NONBLOCK);
    ::connect(fd, res->ai_addr, res->ai_addrlen);
    ::freeaddrinfo(res);
    struct pollfd pfd = {fd, POLLOUT, 0};
    if (::poll(&pfd, 1, 10000) <= 0) {
        ::close(fd);
        return false;
    }
    int soError = 0;
    socklen_t errLen = sizeof(soError);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &errLen);
    if (soError != 0) {
        ::close(fd);
        return false;
    }
    ::fcntl(fd, F_SETFL, 0);
    m_ssl = nullptr;
    if (p.tls) {
        if (!m_sslCtx) {
            m_sslCtx = SSL_CTX_new(TLS_client_method());
            if (!m_sslCtx) {
                lg::error("stratum", "SSL_CTX_new gagal");
                ::close(fd);
                return false;
            }
            SSL_CTX_set_verify(m_sslCtx, SSL_VERIFY_NONE, nullptr);
        }
        SSL* ssl = SSL_new(m_sslCtx);
        SSL_set_fd(ssl, fd);
        // SNI — beberapa pool (mis. rplant) menyajikan sertifikat per-hostname.
        SSL_set_tlsext_host_name(ssl, p.host.c_str());
        // Handshake blocking dengan batas waktu lewat socket timeout.
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (SSL_connect(ssl) != 1) {
            unsigned long e = ERR_peek_last_error();
            char errbuf[256] = {0};
            if (e) ERR_error_string_n(e, errbuf, sizeof(errbuf));
            lg::warn("stratum", "TLS handshake gagal (" + p.host + ":" + portStr + "): " +
                                    (e ? std::string(errbuf) : "timeout/EOF"));
            SSL_free(ssl);
            ::close(fd);
            return false;
        }
        // Lepas socket timeout: recvLine mengatur waktunya sendiri via poll.
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        // Sebelum m_sock.store(fd): pembaca yang melihat fd >= 0 pasti
        // menemukan m_ssl sudah siap.
        m_ssl = ssl;
    }
    m_sock.store(fd);
    m_recvBuf.clear();
    return true;
}

void StratumClient::closeSocket() {
    // Dua mutex sekaligus: recvLine memegang m_recvMutex dan bisa sedang
    // memakai m_ssl — SSL_free tanpa menunggunya berisiko use-after-free.
    std::lock_guard<std::mutex> slock(m_sendMutex);
    std::lock_guard<std::mutex> rlock(m_recvMutex);
    int fd = m_sock.exchange(-1);
    if (fd >= 0) {
        if (m_ssl) {
            SSL_shutdown(m_ssl);  // best effort
            SSL_free(m_ssl);
            m_ssl = nullptr;
        }
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    {
        std::lock_guard<std::mutex> plock(m_pendingMutex);
        m_pending.clear();
    }
}

void StratumClient::advancePool() {
    if (m_pools.size() <= 1) return;
    int next = (m_poolIdx.load() + 1) % (int)m_pools.size();
    m_poolIdx.store(next);
    lg::warn("stratum", "Switching to pool " + poolHost());
    notifyPoolChange();
}

void StratumClient::handleFail(const std::string& reason) {
    lg::warn("stratum", reason + " (pool " + poolHost() + "), reconnecting in " +
                              std::to_string(m_reconnectDelay) + "s");
    m_failCount++;
    if (m_failCount >= 3) {
        m_failCount = 0;
        advancePool();
    }
    std::this_thread::sleep_for(std::chrono::seconds(m_reconnectDelay));
    m_reconnectDelay = std::min(m_reconnectDelay * 2, 30);
}

void StratumClient::disconnect() {
    if (!m_running.exchange(false)) {
        // Make sure a stale thread is joined (defensive).
        if (m_thread.joinable()) m_thread.join();
        closeSocket();
        return;
    }
    closeSocket();
    if (m_thread.joinable()) m_thread.join();
}

void StratumClient::connect() {
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&StratumClient::run, this);
}

void StratumClient::run() {
    m_reconnectDelay = 2;
    while (m_running.load()) {
        const PoolConfig& p = m_pools[m_poolIdx.load()];

        if (!openSocket(p)) {
            handleFail("connect failed");
            continue;
        }
        lg::info("stratum", "Connected to " + p.host + ":" + std::to_string(p.port) +
                                " (" + (p.tls ? "tls, " : "") +
                                (m_mode == StratumMode::Classic ? "classic" : "ethproxy") + ")");
        notifyPoolChange();
        m_failCount = 0;
        m_reconnectDelay = 2;
        m_msgId.store(1);
        m_diff = 0;
        m_extranonce1Hex.clear();
        m_extranonce2Size = 4;
        m_extranonce2Counter = 0;
        m_targetOverrideHex.clear();

        if (m_mode == StratumMode::Classic) {
            // --- MiningCore / stratum klasik: subscribe -> authorize, lalu
            // job di-push oleh pool (tidak ada polling). ---
            if (!doClassicSubscribe()) {
                closeSocket();
                handleFail("subscribe failed");
                continue;
            }
            if (!doClassicAuthorize()) {
                closeSocket();
                handleFail("authorize failed");
                continue;
            }
            lg::info("stratum", "Authorized as " + m_workerName);

            int idle = 0;  // recvLine kosong berturut-turut (10s masing-masing)
            while (m_running.load()) {
                std::string line = recvLine(10.0);
                if (line.empty()) {
                    if (m_sock.load() < 0) break;      // socket ditutup dari luar
                    if (++idle >= 12) {                // ~2 menit tanpa data
                        lg::warn("stratum", "Tidak ada data dari pool 2 menit — reconnect");
                        break;
                    }
                    continue;
                }
                idle = 0;
                handleResponse(line);
            }
            closeSocket();
            if (m_running.load()) handleFail("disconnected");
            continue;
        }

        if (!doEthLogin()) {
            closeSocket();
            handleFail("login failed");
            continue;
        }
        lg::info("stratum", "Authorized as " + m_workerName);

        // --- Polling loop: eth_getWork every POLL_MS, handle inbound lines ---
        auto lastPoll = std::chrono::steady_clock::now();
        bool pollOk = true;
        while (m_running.load()) {
            std::string line = recvLine(0.2);
            if (line.empty()) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPoll).count();
                if (elapsed >= m_pollMs) {
                    if (!doEthGetWork()) {
                        pollOk = false;
                        break;
                    }
                    lastPoll = now;
                }
                continue;
            }
            handleResponse(line);
        }
        closeSocket();
        if (pollOk) handleFail("disconnected");
    }
    m_running.store(false);
    closeSocket();
}

bool StratumClient::doEthLogin() {
    m_workerName = m_pools[m_poolIdx.load()].wallet;
    if (!m_pools[m_poolIdx.load()].worker.empty()) m_workerName += "." + m_pools[m_poolIdx.load()].worker;

    uint64_t id = m_msgId.fetch_add(1);
    std::string msg = "{\"id\":" + std::to_string(id) +
                      ",\"method\":\"eth_submitLogin\",\"params\":[\"" +
                      json_escape(m_pools[m_poolIdx.load()].wallet) + "\"]";
    const std::string& worker = m_pools[m_poolIdx.load()].worker;
    if (!worker.empty()) msg += ",\"worker\":\"" + json_escape(worker) + "\"";
    msg += "}";
    sendLine(msg);

    std::string resp = recvResponse(id, 15.0);
    if (resp.empty()) return false;

    // result:true | result:"0x..." | result:[...] (non-null) => success
    bool ok = (resp.find("\"result\":true") != std::string::npos);
    if (!ok) ok = (resp.find("\"result\":\"0x") != std::string::npos);
    if (!ok) {
        auto rPos = resp.find("\"result\"");
        if (rPos != std::string::npos) {
            auto colon = resp.find(':', rPos + 7);
            if (colon != std::string::npos) {
                colon++;
                while (colon < resp.size() && resp[colon] == ' ') colon++;
                ok = (colon < resp.size() && resp[colon] != 'n');  // not null
            }
        }
    }
    if (!ok) {
        std::string err = json_error_reason(resp);
        lg::error("stratum", "Auth error: '" + (err.empty() ? std::string("unknown") : err) + "'");
    }
    return ok;
}

bool StratumClient::doEthGetWork() {
    uint64_t id = m_msgId.fetch_add(1);
    std::string msg = "{\"id\":" + std::to_string(id) +
                      ",\"method\":\"eth_getWork\",\"params\":[]}";
    sendLine(msg);

    std::string resp = recvResponse(id, 10.0);
    if (resp.empty()) return false;

    if (resp.find("\"error\"") != std::string::npos && resp.find("\"result\"") == std::string::npos)
        return false;

    std::string header = strip_0x(json_result_array(resp, 0));
    std::string seed = strip_0x(json_result_array(resp, 1));
    std::string target = strip_0x(json_result_array(resp, 2));

    if (header.empty() || target.empty()) return false;

    if (header == m_currentHeader && seed == m_currentSeed && target == m_currentTarget)
        return true;  // same job, skip

    // --- Validasi sebelum apa pun disimpan ---
    // Miner selalu mem-baca 32 byte header (memcpy di workerLoop), jadi header
    // yang lebih pendek dari itu adalah out-of-bounds read, bukan cuma job aneh.
    std::vector<uint8_t> headerBytes;
    if (header.size() != 64 || !hex_to_bytes(header, headerBytes)) {
        lg::warn("stratum", "Job ditolak: header harus 64 hex char, dapat " +
                                std::to_string(header.size()) + " ('" + header.substr(0, 24) +
                                (header.size() > 24 ? "..." : "") + "')");
        return true;  // koneksi masih sehat — tunggu poll berikutnya
    }
    // Target < 16 hex char dulu diam-diam memakai default 0x00ffffff...ff,
    // yang jauh lebih mudah dari target sebenarnya => banjir share invalid.
    uint64_t targetInt = 0;
    std::string targetHead = target.substr(0, 16);
    if (targetHead.size() != 16 || !hex_to_u64(targetHead, targetInt)) {
        lg::warn("stratum", "Job ditolak: target tidak valid ('" + target.substr(0, 24) + "')");
        return true;
    }
    if (!seed.empty() && !hex_is_valid(seed)) {
        lg::warn("stratum", "Job ditolak: seed hash bukan hex ('" + seed.substr(0, 24) + "')");
        return true;
    }

    // --- Perubahan seed = key RandomY berubah ---
    // Cache/dataset dibangun sekali dari key tetap. Kalau pool mulai mengirim
    // seed lain, seluruh share akan ditolak tanpa gejala lain, jadi ini harus
    // terlihat di log.
    const bool seedChanged = !m_currentSeed.empty() && seed != m_currentSeed;
    if (seedChanged)
        lg::warn("stratum", "SEED HASH BERUBAH: " + m_currentSeed.substr(0, 16) + "... -> " +
                                seed.substr(0, 16) +
                                "... — cache RandomY dibangun dari key tetap, share bisa "
                                "ditolak semua sampai service di-restart");

    m_currentHeader = header;
    m_currentSeed = seed;
    m_currentTarget = target;
    m_currentJobId = header.substr(0, 16);

    Job job;
    job.jobId = m_currentJobId;
    job.header = std::move(headerBytes);
    job.seedHex = seed;
    job.targetHex = target;
    job.targetInt = targetInt;
    job.seedChanged = seedChanged;

    lg::debug("stratum", "New job " + job.jobId + " target " + target.substr(0, 8) + "...");

    if (m_onJob) m_onJob(job);
    return true;
}

// ============================================================
// Jalur Classic (MiningCore): mining.subscribe / authorize / notify / submit
// ============================================================
bool StratumClient::doClassicSubscribe() {
    uint64_t id = m_msgId.fetch_add(1);
    std::string msg = "{\"id\":" + std::to_string(id) +
                      ",\"method\":\"mining.subscribe\",\"params\":[\"xcb/2.0\"]}";
    sendLine(msg);

    std::string resp = recvResponse(id, 15.0);
    if (resp.empty()) return false;

    // result: [[[...metode...]], "extranonce1", extranonce2_size]
    std::string en1 = strip_0x(json_result_array(resp, 1));
    std::string sz = json_result_array(resp, 2);
    if (en1.empty() || !hex_is_valid(en1)) {
        lg::error("stratum", "Subscribe: extranonce1 tidak valid ('" + en1.substr(0, 24) + "')");
        return false;
    }
    int size = 4;
    if (!sz.empty()) size = std::atoi(sz.c_str());
    if (size < 1 || size > 16) size = 4;
    m_extranonce1Hex = en1;
    m_extranonce2Size = size;
    lg::debug("stratum", "Subscribed: extranonce1=" + en1 +
                             " extranonce2_size=" + std::to_string(size));
    return true;
}

bool StratumClient::doClassicAuthorize() {
    const PoolConfig& p = m_pools[m_poolIdx.load()];
    m_workerName = p.wallet;
    if (!p.worker.empty()) m_workerName += "." + p.worker;

    uint64_t id = m_msgId.fetch_add(1);
    std::string msg = "{\"id\":" + std::to_string(id) +
                      ",\"method\":\"mining.authorize\",\"params\":[\"" +
                      json_escape(m_workerName) + "\",\"" + json_escape(p.password) + "\"]}";
    sendLine(msg);

    std::string resp = recvResponse(id, 15.0);
    if (resp.empty()) return false;
    bool ok = json_is_true(resp);
    if (!ok) {
        std::string err = json_error_reason(resp);
        lg::error("stratum", "Auth error: '" + (err.empty() ? std::string("unknown") : err) + "'");
    }
    return ok;
}

void StratumClient::handleClassicLine(const std::string& line) {
    std::string method = json_find_str(line, "method");
    if (method.empty()) return;

    if (method == "mining.notify") {
        std::vector<std::string> params;
        json_params_vec(line, params);
        // [jobId, prevhash, coinb1, coinb2, (branch dibuka ratakan)...,
        //  version, nbits, ntime, clean] -> minimal 8 saat branch kosong.
        if (params.size() < 8) {
            lg::warn("stratum", "mining.notify dengan " + std::to_string(params.size()) +
                                    " param (butuh >=8) — diabaikan");
            return;
        }
        buildClassicJob(params);
        return;
    }

    if (method == "mining.set_difficulty") {
        std::vector<std::string> params;
        json_params_vec(line, params);
        double diff = params.empty() ? 0.0 : std::atof(params[0].c_str());
        if (diff > 0) {
            bool first = (m_diff == 0);
            m_diff = diff;
            if (first)
                lg::info("stratum", "Difficulty: " + std::to_string(diff));
            else
                lg::debug("stratum", "Difficulty: " + std::to_string(diff));
        }
        return;
    }

    if (method == "mining.set_extranonce") {
        std::vector<std::string> params;
        json_params_vec(line, params);
        if (params.size() >= 2) {
            std::string en1 = strip_0x(params[0]);
            int sz = std::atoi(params[1].c_str());
            if (!en1.empty() && hex_is_valid(en1)) m_extranonce1Hex = en1;
            if (sz >= 1 && sz <= 16) m_extranonce2Size = sz;
            lg::info("stratum", "Extranonce berubah: " + m_extranonce1Hex.substr(0, 16) +
                                    " size=" + std::to_string(m_extranonce2Size));
        }
        return;
    }

    if (method == "mining.set_target") {
        std::vector<std::string> params;
        json_params_vec(line, params);
        if (!params.empty()) {
            std::string t = strip_0x(params[0]);
            if (t.size() == 64 && hex_is_valid(t)) {
                m_targetOverrideHex = t;
                lg::info("stratum", "Target dari pool: " + t.substr(0, 16) + "...");
            }
        }
        return;
    }

    lg::debug("stratum", "Notification: " + method);
}

bool StratumClient::buildClassicJob(const std::vector<std::string>& params) {
    const std::string& jobId = params[0];
    const std::string& prevhashHex = params[1];
    const std::string& coinb1Hex = params[2];
    const std::string& coinb2Hex = params[3];
    const size_t branchCount = params.size() - 8;
    const std::string& versionHex = params[4 + branchCount];
    const std::string& nbitsHex = params[5 + branchCount];
    const std::string& ntimeHex = params[6 + branchCount];
    const std::string& cleanStr = params[7 + branchCount];

    // --- Validasi input pool sebelum apa pun dipakai ---
    if (!hex_is_valid(prevhashHex) || prevhashHex.size() != 64) {
        lg::warn("stratum", "Job ditolak: prevhash bukan 64 hex ('" +
                                prevhashHex.substr(0, 24) + "')");
        return false;
    }
    if (!hex_is_valid(coinb1Hex) || !hex_is_valid(coinb2Hex)) {
        lg::warn("stratum", "Job ditolak: coinbase bukan hex");
        return false;
    }
    if (!hex_is_valid(versionHex) || versionHex.size() != 8 || !hex_is_valid(nbitsHex) ||
        nbitsHex.size() != 8 || !hex_is_valid(ntimeHex) || ntimeHex.size() != 8) {
        lg::warn("stratum", "Job ditolak: version/nbits/ntime bukan 8 hex");
        return false;
    }
    if (m_diff <= 0) {
        // Tanpa difficulty, target share tak terdefinisi — jangan buat job
        // (kalau target dibuat mudah, banjir share invalid; susah, share valid
        // tak pernah terkirim).
        lg::warn("stratum", "Job " + jobId + " diabaikan: set_difficulty belum diterima");
        return false;
    }

    // --- Extranonce2 untuk job ini (counter naik per job, ala cpuminer) ---
    std::vector<uint8_t> xn2(m_extranonce2Size, 0);
    uint32_t c = m_extranonce2Counter++;
    for (int i = 0; i < m_extranonce2Size && i < 4; i++) {
        size_t idx = m_extranonce2Size - 1 - static_cast<size_t>(i);
        xn2[idx] = static_cast<uint8_t>((c >> (8 * i)) & 0xff);
    }

    // --- Coinbase: coinb1 || extranonce1 || extranonce2 || coinb2 ---
    std::vector<uint8_t> coinbase;
    if (!hex_to_bytes(coinb1Hex, coinbase)) return false;
    std::vector<uint8_t> xn1;
    if (!hex_to_bytes(m_extranonce1Hex, xn1)) return false;
    coinbase.insert(coinbase.end(), xn1.begin(), xn1.end());
    coinbase.insert(coinbase.end(), xn2.begin(), xn2.end());
    std::vector<uint8_t> cb2;
    if (!hex_to_bytes(coinb2Hex, cb2)) return false;
    coinbase.insert(coinbase.end(), cb2.begin(), cb2.end());

    // --- Merkle root: dSHA256(coinbase), lalu dSHA256(root || branch) ---
    uint8_t merkle[32];
    sha256d(coinbase.data(), coinbase.size(), merkle);
    std::vector<uint8_t> buf(64);
    for (size_t i = 0; i < branchCount; i++) {
        std::vector<uint8_t> br;
        if (!hex_to_bytes(params[4 + i], br) || br.size() != 32) {
            lg::warn("stratum", "Job ditolak: merkle branch tidak valid");
            return false;
        }
        memcpy(buf.data(), merkle, 32);
        memcpy(buf.data() + 32, br.data(), 32);
        sha256d(buf.data(), buf.size(), merkle);
    }

    // --- Header 80 byte ---
    // version/ntime/nbits: hex dari pool adalah tampilan big-endian -> nilai
    // u32 ditulis little-endian ke header (urutan byte blok Bitcoin).
    // prevhash: hex2bin langsung (pool mengirim dalam urutan byte header).
    std::vector<uint8_t> header(80, 0);
    uint64_t v = 0, nb = 0, nt = 0;
    hex_to_u64(versionHex, v);
    hex_to_u64(nbitsHex, nb);
    hex_to_u64(ntimeHex, nt);
    header[0] = (uint8_t)(v & 0xff);
    header[1] = (uint8_t)((v >> 8) & 0xff);
    header[2] = (uint8_t)((v >> 16) & 0xff);
    header[3] = (uint8_t)((v >> 24) & 0xff);
    std::vector<uint8_t> prev;
    hex_to_bytes(prevhashHex, prev);
    memcpy(header.data() + 4, prev.data(), 32);
    memcpy(header.data() + 36, merkle, 32);
    header[68] = (uint8_t)(nt & 0xff);
    header[69] = (uint8_t)((nt >> 8) & 0xff);
    header[70] = (uint8_t)((nt >> 16) & 0xff);
    header[71] = (uint8_t)((nt >> 24) & 0xff);
    header[72] = (uint8_t)(nb & 0xff);
    header[73] = (uint8_t)((nb >> 8) & 0xff);
    header[74] = (uint8_t)((nb >> 16) & 0xff);
    header[75] = (uint8_t)((nb >> 24) & 0xff);
    // header[76..79] = nonce, diisi worker.

    // --- Target share: set_target dari pool kalau ada, kalau tidak dari diff ---
    uint64_t target256[4] = {0, 0, 0, 0};
    if (!m_targetOverrideHex.empty() && hex_to_target256(m_targetOverrideHex, target256)) {
        // pakai override
    } else {
        diff_to_target256(m_diff, target256);
    }

    // --- Extranonce2 hex (untuk mining.submit) ---
    static const char hx[] = "0123456789abcdef";
    std::string xn2Hex;
    xn2Hex.reserve(xn2.size() * 2);
    for (uint8_t b : xn2) {
        xn2Hex += hx[b >> 4];
        xn2Hex += hx[b & 0xf];
    }

    Job job;
    job.jobId = jobId;
    job.header = std::move(header);
    job.extranonce2Hex = xn2Hex;
    job.ntimeHex = ntimeHex;
    job.clean = (cleanStr == "true");
    job.target256[0] = target256[0];
    job.target256[1] = target256[1];
    job.target256[2] = target256[2];
    job.target256[3] = target256[3];
    job.targetHex = m_targetOverrideHex;

    m_currentJobId = jobId;
    lg::debug("stratum", "New job " + jobId + " diff " + std::to_string(m_diff) + " clean=" +
                             (job.clean ? "1" : "0"));

    if (m_onJob) m_onJob(job);
    return true;
}

void StratumClient::handleResponse(const std::string& line) {
    uint64_t id = json_id_val(line);

    if (id == 0) {
        // Pool notification. EthProxy: hanya log. Classic: job & parameter
        // mining.* datang lewat sini.
        if (m_mode == StratumMode::Classic) {
            handleClassicLine(line);
            return;
        }
        std::string method = json_find_str(line, "method");
        if (!method.empty()) lg::debug("stratum", "Notification: " + method);
        return;
    }

    // Only responses to our own share submissions are accounted here.
    PendingShare ps;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        auto it = m_pending.find(id);
        if (it == m_pending.end()) return;  // getWork/login response — already consumed
        ps = it->second;
        m_pending.erase(it);
    }

    int delayMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              ps.t)
            .count());

    bool ok = json_is_true(line);
    if (ok) {
        if (m_onResult) m_onResult(true, "", delayMs, ps.workerIdx);
    } else {
        std::string reason = json_error_reason(line);
        if (reason.empty()) {
            // Format tak dikenal: tampilkan responsnya apa adanya (dipotong)
            // supaya reject tidak jadi misteri.
            reason = "rejected: " + line.substr(0, 140);
        }
        if (m_onResult) m_onResult(false, reason, delayMs, ps.workerIdx);
    }
}

bool StratumClient::submitShare(const std::string& headerHex, const std::string& nonceHex,
                                const std::string& mixHashHex, int workerIdx) {
    if (m_sock.load() < 0) return false;

    uint64_t id = m_msgId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        // Pool lambat / diff rendah = ratusan share in-flight. Batasi supaya
        // map tidak tumbuh tanpa batas bila respons tidak pernah datang.
        if (m_pending.size() >= 4096) m_pending.erase(m_pending.begin());
        m_pending[id] = {std::chrono::steady_clock::now(), workerIdx};
    }

    std::string h = (headerHex.substr(0, 2) == "0x") ? headerHex : "0x" + headerHex;
    std::string n = (nonceHex.substr(0, 2) == "0x") ? nonceHex : "0x" + nonceHex;
    std::string m = (mixHashHex.substr(0, 2) == "0x") ? mixHashHex : "0x" + mixHashHex;

    std::string params = "[\"" + n + "\",\"" + h + "\",\"" + m + "\"]";
    std::string msg = "{\"id\":" + std::to_string(id) +
                      ",\"method\":\"eth_submitWork\",\"params\":" + params + "}";

    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        int fd = m_sock.load();
        if (fd >= 0) {
            std::string payload = msg + "\n";
            if (m_ssl)
                sent = tls_write_all(m_ssl, payload.data(), payload.size()) == 0;
            else
                sent = ::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL) ==
                       (ssize_t)payload.size();
        }
    }
    if (!sent) {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.erase(id);
    }
    return sent;
}

bool StratumClient::submitShareClassic(const std::string& jobId, const std::string& extranonce2Hex,
                                       const std::string& ntimeHex, const std::string& nonceHex,
                                       int workerIdx) {
    if (m_sock.load() < 0) return false;

    uint64_t id = m_msgId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        if (m_pending.size() >= 4096) m_pending.erase(m_pending.begin());
        m_pending[id] = {std::chrono::steady_clock::now(), workerIdx};
    }

    // mining.submit params: [user, jobId, extranonce2, ntime, nonce]
    // (urutan & byte-order persis seperti cpuminer std_le_build_stratum_request).
    std::string msg = "{\"id\":" + std::to_string(id) +
                      ",\"method\":\"mining.submit\",\"params\":[\"" +
                      json_escape(m_workerName) + "\",\"" + json_escape(jobId) + "\",\"" +
                      extranonce2Hex + "\",\"" + ntimeHex + "\",\"" + nonceHex + "\"]}";

    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        int fd = m_sock.load();
        if (fd >= 0) {
            std::string payload = msg + "\n";
            if (m_ssl)
                sent = tls_write_all(m_ssl, payload.data(), payload.size()) == 0;
            else
                sent = ::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL) ==
                       (ssize_t)payload.size();
        }
    }
    if (!sent) {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        m_pending.erase(id);
    }
    return sent;
}

void StratumClient::submitHashrate(uint64_t rate) {
    // eth_submitHashrate hanya ada di protokol ETHPROXY. MiningCore klasik
    // tidak memakainya — hening agar tidak memicu error "unknown method".
    if (m_mode != StratumMode::EthProxy) return;
    if (m_sock.load() < 0) return;

    // eth_submitHashrate: params ["0x<32-byte BE rate>", "0x<worker-id>"]
    static const char h[] = "0123456789abcdef";
    std::string rateHex(64, '0');
    for (int nib = 0; nib < 16; nib++) rateHex[48 + nib] = h[(rate >> ((15 - nib) * 4)) & 0xf];
    std::string workerId = "0x" + m_pools[m_poolIdx.load()].wallet;

    uint64_t id = m_msgId.fetch_add(1);
    std::string msg = "{\"id\":" + std::to_string(id) +
                      ",\"jsonrpc\":\"2.0\",\"method\":\"eth_submitHashrate\",\"params\":[\"" +
                      rateHex + "\",\"" + workerId + "\"]}";
    sendLine(msg);
}
