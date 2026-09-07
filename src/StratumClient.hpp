#ifndef STRATUMCLIENT_HPP
#define STRATUMCLIENT_HPP

#include "Config.hpp"

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
    std::vector<uint8_t> header;  // 32 bytes binary — dijamin oleh doEthGetWork()
    std::string seedHex;
    std::string targetHex;         // hex target
    uint64_t targetInt = 0x00ffffffffffffffULL;
    bool clean = true;
    // Pool mengirim seed hash lain dari job sebelumnya. Cache/dataset RandomY
    // dibangun dari key tetap, jadi ini berarti hasil hash tidak lagi cocok
    // dengan yang divalidasi pool.
    bool seedChanged = false;
};

// ETHPROXY stratum client with:
//  - response/id correlation for share results (a failed getWork is never
//    miscounted as a rejected share)
//  - multi-pool failover (rotate on repeated failures, backoff)
//  - eth_submitHashrate reporting
//  - atomic msg id / socket so worker threads can submit concurrently
class StratumClient {
public:
    StratumClient(std::vector<PoolConfig> pools);
    ~StratumClient();

    void connect();
    void disconnect();
    bool isConnected() const;
    bool submitShare(const std::string& headerHex, const std::string& nonceHex,
                     const std::string& mixHashHex, int workerIdx);
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
    void handleResponse(const std::string& line);
    void notifyPoolChange();

    std::vector<PoolConfig> m_pools;
    std::atomic<int> m_poolIdx{0};
    int m_failCount = 0;
    int m_reconnectDelay = 2;
    std::string m_workerName;

    std::atomic<int> m_sock{-1};
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
