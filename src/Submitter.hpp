#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Async share submitter.
//
// Worker threads must never block on network I/O: they push a Share onto a
// bounded queue and a dedicated thread drains it and calls the submit
// callback. If the queue is full the share is dropped and the caller counts
// it as wasted (return value of push()).
//
// The submit callback returns true when the share was actually sent to the
// pool, false otherwise (no connection etc.) so the caller can account it.

struct Share {
    std::string headerHex;  // header that was hashed (hex, no 0x)
    std::string nonceHex;   // 0x...
    std::string mixHex;     // 0x...
    std::string jobId;
    int workerIdx = 0;
    std::chrono::steady_clock::time_point foundAt;
};

class Submitter {
public:
    using SubmitFn = std::function<bool(const Share&)>;
    using DropFn = std::function<void(const Share&, const std::string& reason)>;

    Submitter() = default;
    ~Submitter();

    Submitter(const Submitter&) = delete;
    Submitter& operator=(const Submitter&) = delete;

    void start(SubmitFn submit, DropFn drop);
    void stop();

    // Non-blocking. Returns false (and does NOT enqueue) when the queue is
    // full; caller must count the share as wasted.
    bool push(Share&& s);

    size_t queueLen() const;

private:
    void run();

    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<Share> m_q;
    static constexpr size_t kMaxQueue = 4096;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    SubmitFn m_submit;
    DropFn m_drop;
};
