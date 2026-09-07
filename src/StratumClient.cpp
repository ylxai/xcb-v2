#include "StratumClient.hpp"

#include "Log.hpp"

#include <algorithm>
#include <cstring>
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
// Handles  {"error": {"code":..,"message":".."}}  and  {"error":"str"}.
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
    }
    return "";
}

static std::string strip_0x(const std::string& s) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return s.substr(2);
    return s;
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

// ------------------------------------------------------------
StratumClient::StratumClient(std::vector<PoolConfig> pools) : m_pools(std::move(pools)) {
    if (m_pools.empty()) {
        PoolConfig fallback;
        fallback.host = "localhost";
        fallback.port = 8008;
        m_pools.push_back(fallback);
    }
}

StratumClient::~StratumClient() { disconnect(); }

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
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int ms = static_cast<int>(timeoutSec * 1000);
        int ret = ::poll(&pfd, 1, ms);
        if (ret <= 0) return "";
        char buf[8192];
        ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
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
    m_sock.store(fd);
    m_recvBuf.clear();
    return true;
}

void StratumClient::closeSocket() {
    std::lock_guard<std::mutex> lock(m_sendMutex);
    int fd = m_sock.exchange(-1);
    if (fd >= 0) {
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
        lg::info("stratum", "Connected to " + p.host + ":" + std::to_string(p.port));
        notifyPoolChange();
        m_failCount = 0;
        m_reconnectDelay = 2;
        m_msgId.store(1);

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
                                "ditolak semua sampai miner di-restart");

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

void StratumClient::handleResponse(const std::string& line) {
    uint64_t id = json_id_val(line);

    if (id == 0) {
        // Pool notification (rare in ETHPROXY mode).
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
        if (reason.empty()) reason = "rejected";
        if (m_onResult) m_onResult(false, reason, delayMs, ps.workerIdx);
    }
}

bool StratumClient::submitShare(const std::string& headerHex, const std::string& nonceHex,
                                const std::string& mixHashHex, int workerIdx) {
    if (m_sock.load() < 0) return false;

    uint64_t id = m_msgId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
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
