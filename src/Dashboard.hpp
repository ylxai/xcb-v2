#pragma once
#include "Stats.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// FTXUI full-screen dashboard (the "best miner" style UI, keyboard driven).
//
//   keys:
//     1 / 2 / 3    switch tab: Overview / Threads / Shares
//     q / x / Esc  quit
//
// run() blocks and owns the FTXUI event loop plus a 1 s refresh thread that
// samples Stats and redraws. requestStop() may be called from any thread
// (e.g. the SIGINT path) to tear it down.
class Dashboard {
public:
    using EventsProvider = std::function<std::vector<std::string>()>;
    using UintProvider = std::function<uint64_t()>;
    using VoidCallback = std::function<void()>;

    struct Config {
        std::string pool;     // host:port
        std::string wallet;
        std::string worker;
        bool benchmark = false;
        uint64_t benchTotal = 0;
        EventsProvider events;    // recent share/job events (HH:MM:SS ...)
        UintProvider benchDone;   // nonces done so far (benchmark mode)
        VoidCallback onTick;      // called once per refresh (hashrate report, ...)
        VoidCallback onQuit;      // called when the user presses quit
    };

    Dashboard(Stats& stats, Config cfg);
    ~Dashboard();

    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    // Blocks until the user quits, benchmark finishes, or requestStop() is
    // called. Must be called from exactly one thread.
    void run();

    // Thread-safe: makes run() return as soon as possible.
    void requestStop();

private:
    Stats& m_stats;
    Config m_cfg;
    int m_tab = 0;  // 0=Overview 1=Threads 2=Shares (loop thread only)
    std::atomic<bool> m_stop{false};

    struct Impl;
    std::unique_ptr<Impl> m_impl;  // owns the FTXUI screen
};
