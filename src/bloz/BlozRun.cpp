#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "../Hashing/RandomXV2.hpp"
#include "../Log.hpp"
#include "BlozMiner.hpp"
#include "BlozRun.hpp"

// Backend Block Zero untuk biner tunggal `xcb` (`-a bloz`).
//
// Tidak ada parser argv di sini: wallet, pool, worker, thread, light, hugepages,
// dan nonce-offset semuanya datang dari Config (file / env / CLI). File ini
// hanya berisi selftest backend dan loop yang menjalankan miner.

namespace {

bool runSelftest() {
    bool ok = true;

    // 1. Engine RandomX v1 terhadap vektor yang direkam dari cpuminer-rplant
    //    untuk rx/blockzero (header 80 byte, nonce di offset 76).
    {
        std::string detail;
        const bool pass = rxv2::selftestV1(detail);
        ok &= pass;
        lg::info("selftest", std::string("randomx-v1 rx/blockzero ") +
                                 (pass ? ": OK" : (": FAIL " + detail)));
    }

    // 2. Aturan share: nilai = limb 24..31 little-endian, perbandingan ketat.
    {
        uint8_t digest[32];
        for (size_t i = 0; i < sizeof(digest); i++) digest[i] = static_cast<uint8_t>(i);

        uint64_t want = 0;
        for (size_t i = 0; i < 8; i++) want |= static_cast<uint64_t>(digest[24 + i]) << (8 * i);
        const bool pass2 = (bloz::digestValue(digest) == want);
        ok &= pass2;
        lg::info("selftest", std::string("digestValue limb24_31: ") + (pass2 ? "OK" : "FAIL"));

        // value == target harus DITOLAK (perbandingan ketat).
        const bool pass3 =
            !bloz::meetsTarget(digest, want) && bloz::meetsTarget(digest, want + 1);
        ok &= pass3;
        lg::info("selftest", std::string("meetsTarget batas: ") + (pass3 ? "OK" : "FAIL"));
    }

    lg::info("selftest", ok ? "SEMUA OK" : "ADA YANG GAGAL");
    return ok;
}

}  // namespace

namespace bloz {

bool selftest() { return runSelftest(); }

int run(const ::MinerConfig& cfg, std::atomic<bool>& running) {
    if (cfg.pools.empty()) {
        lg::error("bloz", "tidak ada pool: isi POOL=host:port atau -o host:port");
        return 1;
    }

    MinerConfig mc;
    mc.pool.host = cfg.pools[0].host;
    mc.pool.port = cfg.pools[0].port;
    mc.pool.tls = cfg.poolTls;
    mc.pool.login = cfg.pools[0].wallet;
    mc.pool.worker = cfg.pools[0].worker;
    mc.threads = cfg.threads;
    mc.fastMem = cfg.fullMem;
    mc.largePages = cfg.largePages;
    mc.nonceOffset = cfg.nonceOffset;

    lg::info("bloz", "xcb -a bloz — pool " + mc.pool.host + ":" +
                        std::to_string(mc.pool.port) + (mc.pool.tls ? " (tls)" : " (plain)") +
                        ", worker " + mc.pool.worker + ", threads " +
                        std::to_string(mc.threads) + (mc.fastMem ? ", FAST" : ", LIGHT"));

    Miner miner;
    std::string err;
    if (!miner.start(mc, err)) {
        lg::error("bloz", "start gagal: " + err);
        return 1;
    }

    uint64_t lastHashes = 0;
    auto lastTick = std::chrono::steady_clock::now();
    while (running.load(std::memory_order_relaxed) && miner.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto now = std::chrono::steady_clock::now();
        const auto dt =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count();
        if (dt < 10000) continue;
        const uint64_t h = miner.hashes();
        const double rate = (static_cast<double>(h - lastHashes) * 1000.0) / static_cast<double>(dt);
        lastHashes = h;
        lastTick = now;
        lg::info("bloz", "HR " + std::to_string(static_cast<long long>(rate)) + " H/s | found " +
                            std::to_string(miner.found()));
    }

    lg::info("bloz", "berhenti…");
    miner.stop();
    lg::info("sysd", miner.finalSummary());
    return 0;
}

}  // namespace bloz
