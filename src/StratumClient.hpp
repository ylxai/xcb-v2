#ifndef STRATUMCLIENT_HPP
#define STRATUMCLIENT_HPP

#include "Config.hpp"

#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct Job {
    std::string jobId;
    std::vector<uint8_t> header;  // EthProxy: 32 bytes (dijamin doEthGetWork());
                                  // Classic: 80 bytes header blok siap hash.
    std::string seedHex;
    std::string targetHex;         // hex target
    uint64_t targetInt = 0x00ffffffffffffffULL;
    bool clean = true;
    // Pool mengirim seed hash lain dari job sebelumnya. Cache/dataset RandomY
    // dibangun dari key tetap, jadi ini berarti hasil hash tidak lagi cocok
    // dengan yang divalidasi pool.
    bool seedChanged = false;

    // --- Hanya untuk jalur Classic (stratum mining.notify) ---
    // target share 256-bit, urutan big-endian w0 = paling
    // signifikan. Rumus = diff_to_hash() cpuminer-opt:
    //   target = 2^224 / diff   (low128 = 0xFFFF..., diff 1 = 2^32 hash)
    // mining.submit butuh extranonce2 & ntime persis seperti yang dipakai
    // saat merakit header ini.
    std::string extranonce2Hex;
    std::string ntimeHex;
    uint64_t target256[4] = {0, 0, 0, 0};
};

// Protokol stratum yang dipakai pool:
//  - EthProxy: eth_submitLogin / eth_getWork / eth_submitWork
//    (catchthatrabbit, koin ETHPROXY)
//  - Classic: mining.subscribe / mining.authorize / mining.notify /
//    mining.submit (MiningCore — Ratatoskr/RATR dan koin Bitcoin-like lain)
enum class StratumMode { EthProxy, Classic };

// ETHPROXY stratum client with:
//  - response/id correlation for share results (a failed getWork is never
//    miscounted as a rejected share)
//  - multi-pool failover (rotate on repeated failures, backoff)
//  - eth_submitHashrate reporting
//  - atomic msg id / socket so worker threads can submit concurrently
class StratumClient {
public:
    StratumClient(std::vector<PoolConfig> pools, StratumMode mode = StratumMode::EthProxy);
    ~StratumClient();

    void connect();
    void disconnect();
    bool isConnected() const;
    bool submitShare(const std::string& headerHex, const std::string& nonceHex,
                     const std::string& mixHashHex, int workerIdx);
    // Jalur Classic: params mining.submit = [user, jobId, extranonce2, ntime,
    // nonce]. nonceHex = 8 hex char byte nonce LE (persis byte nonce di
    // header 80-byte), ntimeHex = hex ntime dari pool, extranonce2Hex = hex
    // extranonce2 yang dipakai saat merakit header.
    bool submitShareClassic(const std::string& jobId, const std::string& extranonce2Hex,
                            const std::string& ntimeHex, const std::string& nonceHex,
                            int workerIdx);
    void submitHashrate(uint64_t rate);

    void setJobCallback(std::function<void(const Job&)> cb) { m_onJob = std::move(cb); }
    void setResultCallback(std::function<void(bool, const std::string&, int, int)> cb) {
        m_onResult = std::move(cb);
    }
    void setPoolChangeCallback(std::function<void(int, const std::string&)> cb) {
        m_onPoolChange = std::move(cb);
    }

    int poolIndex() const { return m_poolIdx.load(); }
    std::string poolHost() const;
    int poolCount() const { return static_cast<int>(m_pools.size()); }
    void setPollMs(int ms) { m_pollMs = ms > 0 ? ms : 1000; }

private:
    void run();
    void sendLine(const std::string& line);
    bool sendFrame(const std::string& json);
    std::string recvLine(double timeoutSec);
    std::string recvResponse(uint64_t wantId, double timeoutSec);
    bool openSocket(const PoolConfig& p);
    void closeSocket();
    void handleFail(const std::string& reason);
    void advancePool();
    bool doEthLogin();
    bool doEthGetWork();
    // --- Jalur Classic (MiningCore) ---
    bool doClassicSubscribe();    // mining.subscribe -> extranonce1 + size
    bool doClassicAuthorize();    // mining.authorize
    void handleClassicLine(const std::string& line);  // notify/set_difficulty/dll
    bool buildClassicJob(const std::vector<std::string>& params);
    void handleResponse(const std::string& line);
    void notifyPoolChange();

    StratumMode m_mode = StratumMode::EthProxy;
    std::vector<PoolConfig> m_pools;
    std::atomic<int> m_poolIdx{0};
    int m_failCount = 0;
    int m_reconnectDelay = 2;
    std::string m_workerName;

    std::atomic<int> m_sock{-1};
    // TLS (pool stratum+ssl, mis. rplant). m_sslCtx dibuat sekali dan dipakai
    // ulang antar reconnect; m_ssl berlaku selagi m_sock >= 0. m_ssl hanya
    // disentuh di bawah m_sendMutex/m_recvMutex (openSocket men-assign-nya
    // sebelum m_sock.store(fd), jadi pembaca fd>=0 selalu melihat ssl siap).
    SSL_CTX* m_sslCtx = nullptr;
    SSL* m_ssl = nullptr;
    int m_pollMs = 1000;  // eth_getWork poll interval (ms)
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::mutex m_sendMutex;
    std::mutex m_recvMutex;
    std::atomic<uint64_t> m_msgId{1};
    std::string m_recvBuf;
    std::string m_currentHeader;  // hex header of current job (no 0x)
    std::string m_currentSeed;
    std::string m_currentTarget;
    std::string m_currentJobId;

    // --- Classic state (MiningCore) ---
    double m_diff = 0;              // dari mining.set_difficulty; 0 = belum ada
    std::string m_extranonce1Hex;   // dari mining.subscribe
    int m_extranonce2Size = 4;      // byte
    uint32_t m_extranonce2Counter = 0;  // naik per job
    std::string m_targetOverrideHex;    // dari mining.set_target (64 hex), kosong = pakai diff

    // Pending share submissions: request id -> (submit time, worker index).
    struct PendingShare {
        std::chrono::steady_clock::time_point t;
        int workerIdx;
    };
    std::mutex m_pendingMutex;
    std::unordered_map<uint64_t, PendingShare> m_pending;

    // Callbacks
    std::function<void(const Job&)> m_onJob;
    std::function<void(bool, const std::string&, int, int)> m_onResult;  // ok, reason, delayMs, worker
    std::function<void(int, const std::string&)> m_onPoolChange;
};

#endif
