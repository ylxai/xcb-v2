#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Thread-safe mining telemetry.
//
// - Per-worker counters are atomics written by worker threads and read by the
//   stats sampler (rolling window hashrate, like ethminer/coreminer's
//   collectData + updateHashRate).
// - Farm-level job/pool info is guarded by a mutex (written on job change /
//   connection events, read by the printer).
// - Share accounting follows coreminer's categories:
//     accepted  - pool said OK
//     rejected  - pool said no (reason logged)
//     wasted    - found but never reached the pool (stale job / no connection /
//                 submit queue full)

struct WorkerStats {
    std::atomic<uint64_t> totalHashes{0};   // cumulative
    std::atomic<uint64_t> windowHashes{0};  // hashes since last sample
    std::atomic<uint64_t> sharesFound{0};
    std::atomic<uint64_t> accepted{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> wasted{0};
    std::atomic<double> currentRate{0.0};  // H/s, set by sample()
    std::atomic<bool> running{false};
};

struct FarmStats {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    mutable std::mutex mu;
    std::string jobId;
    uint64_t target = 0;
    uint64_t difficulty = 0;
    int poolIndex = 0;
    std::string poolHost;
    std::string wallet;
    std::string worker;
    bool connected = false;
};

class Stats {
public:
    Stats() = default;

    void setWorkers(size_t n);
    WorkerStats& worker(size_t i);
    size_t numWorkers() const { return m_workers.size(); }

    FarmStats& farm() { return m_farm; }

    // Compute per-worker current hashrate from the rolling window and reset
    // the window. Call periodically (e.g. every 5 s) from one thread.
    void sample();

    double farmCurrentRate() const;  // smoothed over the last few samples
    double farmBestRate() const;     // peak instantaneous rate in history
    double farmAvgRate() const;      // totalHashes / uptime
    std::vector<double> rateHistory() const;  // copy of the recent rates
    uint64_t farmTotalHashes() const;
    uint64_t farmAccepted() const;
    uint64_t farmRejected() const;
    uint64_t farmWasted() const;
    uint64_t farmSharesFound() const;
    double acceptRatio() const;

    // Formatting.
    static std::string formatRate(double h);
    static std::string formatCount(uint64_t n);
    static std::string formatDuration(std::chrono::steady_clock::duration d);

    std::string summaryLine() const;  // compact one-liner (text)
    std::string summaryJson() const;  // compact one-liner (JSON)
    std::string detailBlock() const;  // multi-line table
    std::string finalSummary() const; // exit report (text)

    // Info needed by the full-screen dashboard renderer.
    struct DashInfo {
        std::string pool;      // host:port
        std::string wallet;    // raw (masked on render)
        std::string worker;
        std::vector<std::string> events;  // recent share/job events (HH:MM:SS ...)
        bool benchmark = false;
        uint64_t benchDone = 0;
        uint64_t benchTotal = 0;
    };

    // Full-screen live dashboard (ANSI, for TTY). Returns a self-contained
    // string: moves cursor home and erases the display before drawing.
    std::string dashboard(const DashInfo& d) const;

    // ASCII bar sparkline of the recent rate history (last `max` samples).
    std::string sparkline(size_t max = 24) const;

private:
    // WorkerStats holds atomics (immovable), so store it behind a pointer.
    std::vector<std::unique_ptr<WorkerStats>> m_workers;
    FarmStats m_farm;
    std::chrono::steady_clock::time_point m_lastSample = std::chrono::steady_clock::now();
    // Instantaneous rates per sample() call (recent last). Written by the
    // sampler and read by the renderer; in FTXUI mode these are different
    // threads, so the history is mutex-guarded (1 Hz traffic, cheap).
    std::vector<double> m_rateHistory;
    mutable std::mutex m_rateMu;
};
