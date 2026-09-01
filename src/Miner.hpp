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
    // Dense index into Stats::workers. Worker N selalu ada di slot N, tanpa
    // lubang, walaupun sebagian VM gagal dibuat — jadi tidak ada dua indeks
    // berbeda yang bisa tertukar (dulu bikin Stats::worker() out_of_range).
    int statsIdx = 0;
    std::thread thread;
    randomx_vm* vm = nullptr;
};

class Miner {
public:
    Miner();
    ~Miner();
    // false => the miner never got running (dataset/VM alloc failed). The
    // caller must exit non-zero instead of reporting a successful no-op run.
    bool start(const MinerConfig& cfg);
    void stop();
    bool isRunning() const { return m_running.load(); }
    std::string finalSummary() const;

private:
    // Display backends.
    enum class UiMode { Auto, Ftxui, Ansi, Log };

    // false => cache/dataset allocation failed and no VM can be created.
    bool initDataset();
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
    // CPU yang boleh dipakai proses ini, diambil sekali di start(). Worker
    // di-pin ke cpus[statsIdx % size] supaya tidak pernah menunjuk CPU di luar
    // cpuset.
    std::vector<int> m_usableCpus;

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
    // FTXUI dashboard. Owned by the stats thread (runFtxuiLoop), read by
    // stop(). Guarded by m_dashMutex; never reset before ~Miner() so a
    // pointer read under the lock stays valid (requestStop() on an exited
    // loop is a safe no-op).
    std::unique_ptr<Dashboard> m_dash;
    mutable std::mutex m_dashMutex;
    std::chrono::steady_clock::time_point m_lastHrReport;

    // Stats/UI thread (detached before; joinable so stop()/~Miner() can wait
    // for it instead of racing with its destruction of m_dash).
    std::thread m_statsThread;

    // Benchmark mode
    bool m_benchmark = false;
    uint64_t m_benchmarkNonces = 0;
    std::atomic<uint64_t> m_benchmarkDone{0};
    std::chrono::steady_clock::time_point m_benchmarkStart;
    bool m_benchmarkReported = false;
};
