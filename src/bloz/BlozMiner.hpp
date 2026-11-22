#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BlozJob.hpp"
#include "BlozPool.hpp"

// Miner Block Zero (BLOZ) — mandiri, tidak memakai ConnMiner/ConnPool/
// StratumClient.
//
// Satu Context RandomX v1 per seed pool; VM di-lease satu per worker thread.
// Tiap worker menyalin blob job (header 80 byte), menulis nonce di offset 76,
// menghitung digest, lalu membandingkan limb 24..31 digest dengan target.
// Tidak ada commitment: daemon Block Zero memakai randomx_calculate_hash apa
// adanya dan membandingkan uint256 hasilnya dengan target blok.
namespace rxv2 {
class Context;
}

namespace bloz {

struct MinerConfig {
    PoolConfig pool;
    int threads = 0;          // 0 = auto (jumlah CPU yang boleh dipakai proses)
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
    // Minta berhenti tanpa join. Aman dipanggil dari thread lain saat start()
    // masih menunggu balasan login: connect() dibatalkan dalam hitungan
    // milidetik, bukan menunggu timeout socket.
    void requestStop();
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

}  // namespace bloz
