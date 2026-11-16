#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ZcdJob.hpp"
#include "ZcdPool.hpp"

// Miner Zycord (ZCD) — mandiri, tidak memakai ConnMiner/ConnPool/StratumClient.
//
// Satu Context RandomX V2 per seed pool; VM di-lease satu per worker thread.
// Tiap worker menyalin blob job, menulis nonce, menghitung digest, lalu
// menghitung commitment = blake2b(blob || digest) dan membandingkan limb
// 24..31 commitment dengan target. Digest mentah TIDAK pernah dibandingkan
// dengan target — itu yang membuat share rx/2 selalu ditolak.
namespace rxv2 {
class Context;
}

namespace zcd {

struct MinerConfig {
    PoolConfig pool;
    int threads = 0;          // 0 = auto (CPU yang boleh dipakai proses)
    bool fastMem = true;      // dataset 2 GiB (FAST) vs cache saja (LIGHT)
    bool largePages = true;
    uint32_t nonceOffset = 0;
};

class Miner {
public:
    Miner();
    ~Miner();

    Miner(const Miner&) = delete;
    Miner& operator=(const Miner&) = delete;

    bool start(const MinerConfig& cfg, std::string& err);
    void stop();
    bool running() const { return m_running.load(std::memory_order_acquire); }

    std::string finalSummary() const;

    // Statistik kumulatif sejak start.
    uint64_t hashes() const { return m_hashes.load(std::memory_order_relaxed); }
    uint64_t found() const { return m_found.load(std::memory_order_relaxed); }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_hashes{0};
    std::atomic<uint64_t> m_found{0};
};

}  // namespace zcd
