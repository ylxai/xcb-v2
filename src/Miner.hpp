#pragma once
#include "Config.hpp"
#include "Dashboard.hpp"
#include "Stats.hpp"
#include "StratumClient.hpp"
#include "Submitter.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <randomx.h>
#include <string>
#include <thread>
#include <vector>

struct Worker {
    int index = 0;     // logical index (may have gaps if a VM failed)
    int statsIdx = 0;  // dense index into Stats::workers
    std::thread thread;
    randomx_vm* vm = nullptr;
};

class Miner {
public:
    Miner();
    ~Miner();
    void start(const MinerConfig& cfg);
    void stop();
    bool isRunning() const { return m_running.load(); }
    std::string finalSummary() const;

private:
    // Display backends.
    enum class UiMode { Auto, Ftxui, Ansi, Log };

    void initDataset();
    void workerLoop(Worker* w);
    void statsLoop();
    void runFtxuiLoop();
    void reportBenchmark();
    std::vector<std::string> eventsSnapshot() const;
    void onNewJob(const Job& job);
    void onShareResult(bool ok, const std::string& reason, int delayMs, int workerIdx);
    void onPoolChange(int idx, const std::string& host);
    bool submitShareCallback(const Share& s);
    void dropShareCallback(const Share& s, const std::string& reason);
    std::shared_ptr<const Job> loadJob();
    void logEvent(const std::string& msg);  // ring buffer for the dashboard

    // Dataset (shared, immutable once built)
    randomx_dataset* m_dataset = nullptr;
    randomx_cache* m_cache = nullptr;
    randomx_flags m_flags;

    std::vector<std::unique_ptr<Worker>> m_workers;
    int m_numThreads = 1;

    // Current job. Workers hold their own shared_ptr snapshot: the header
    // buffer they hash is immutable and can never be freed underneath them.
    std::mutex m_jobMutex;
    std::shared_ptr<const Job> m_job;
    std::atomic<uint64_t> m_globalNonce{0};

    // Telemetry
    Stats m_stats;

    // Control
    std::atomic<bool> m_running{false};
    std::unique_ptr<StratumClient> m_client;
    Submitter m_submitter;
    bool m_verboseShares = false;

    // Live dashboard (TTY only)
    UiMode m_uiMode = UiMode::Auto;
    bool m_dashboard = false;  // ANSI dashboard active
    std::string m_poolLabel;
    mutable std::mutex m_eventsMutex;
    std::deque<std::string> m_events;
    std::unique_ptr<Dashboard> m_dash;  // FTXUI dashboard
    std::chrono::steady_clock::time_point m_lastHrReport;

    // Benchmark mode
    bool m_benchmark = false;
    uint64_t m_benchmarkNonces = 0;
    std::atomic<uint64_t> m_benchmarkDone{0};
    std::chrono::steady_clock::time_point m_benchmarkStart;
    bool m_benchmarkReported = false;
};
