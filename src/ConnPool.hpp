#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Klien stratum rplant untuk algo RandomX v2 ("rx/2b" / randomx2b).
//
// Protokolnya bukan stratum Bitcoin-style dan bukan JSON-RPC node: rplant
// memakai varian xmrig/CryptoNote:
//   -> {"method":"login","params":{"login":WALLET,"pass":"x","algo":["rx/2b",...]}}
//   <- {"result":{"id":"1","job":{job_id, algo, blob, height, seed_hash, target}}}
//   <- {"method":"job","params":{...}}                 (job baru)
//   -> {"method":"submit","params":{"id":"1","job_id":..,"nonce":..,"result":..}}
//
// blob = 76 byte header (tanpa nonce); miner menambahkan nonce 4 byte LE
// menjadi header 80 byte, lalu RandomX v2 dengan key = seed_hash (32 byte).
// target = 8 byte little-endian: share valid bila uint64_le(hash) < target.
struct ConnPoolJob {
    std::string id;
    std::string algo;
    std::vector<uint8_t> blob;  // 76 byte
    std::string seedHex;        // 32 byte (hex) = RandomX key
    uint32_t height = 0;
    uint64_t target = 0;        // LE 64-bit
};

class ConnPool {
public:
    enum class Event { None, Job, Accepted, Rejected, Error };

    ConnPool(std::string host, int port, std::string wallet, std::string worker, bool tls);
    ~ConnPool();

    // DNS + TCP + TLS + login. Blocking (timeout socket 30 s).
    bool connectAndLogin(ConnPoolJob& first, std::string& err);

    // Baca satu event. timeoutMs <= 0 = non-blocking.
    Event poll(ConnPoolJob& job, int timeoutMs, std::string& detail);

    bool submit(const ConnPoolJob& job, uint32_t nonce, const uint8_t hash[32],
                std::string& err);

    bool alive() const { return m_ssl != nullptr; }
    void close();
    const std::string& wallet() const { return m_wallet; }

private:
    bool sendLine(const std::string& line, std::string& err);
    bool recvLine(std::string& line, int timeoutMs, std::string& err);
    bool parseJobJson(const std::string& line, ConnPoolJob& out, std::string& err);

    std::string m_host;
    int m_port;
    std::string m_wallet;
    std::string m_worker;
    bool m_tls;

    void* m_ctx = nullptr;  // SSL_CTX*
    void* m_ssl = nullptr;  // SSL*
    int m_fd = -1;
    std::string m_buf;
    std::string m_loginId;
    uint64_t m_msgId = 1;
};
