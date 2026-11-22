#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "../Hashing/RandomXV2.hpp"
#include "../Log.hpp"
#include "ZcdCommitment.hpp"
#include "ZcdMiner.hpp"
#include "ZcdRun.hpp"

// Backend Zycord untuk biner tunggal `xcb` (`-a zcd`).
//
// Tidak ada parser argv di sini: wallet, pool, worker, thread, light, hugepages,
// dan nonce-offset semuanya datang dari Config (file / env / CLI). File ini
// hanya berisi selftest backend dan loop yang menjalankan miner.

namespace {

bool runSelftest() {
    bool ok = true;

    // 1. Engine RandomX V2 terhadap vektor emas.
    {
        std::string detail;
        const bool pass = rxv2::selftest(detail);
        ok &= pass;
        lg::info("selftest", std::string("randomx-v2 ") +
                                 (pass ? ": OK" : (": FAIL " + detail)));
    }

    // 2. Aturan commitment: commitment = blake2b256(blob || digest) dan nilai
    //    share dibaca dari limb 24..31 little-endian.
    {
        uint8_t blob[zcd::BLOB_SIZE];
        for (size_t i = 0; i < sizeof(blob); i++) blob[i] = static_cast<uint8_t>(i * 7 + 1);
        uint8_t digest[32];
        for (size_t i = 0; i < sizeof(digest); i++) digest[i] = static_cast<uint8_t>(i);

        uint8_t c[32];
        zcd::commitment(blob, sizeof(blob), digest, c);

        // Vektor acuan dihitung terpisah (Python hashlib.blake2b):
        //   blake2b(blob + digest, digest_size=32)
        // membuktikan commitment memakai blake2b-256 atas blob||digest, bukan
        // hash lain atau urutan operand yang terbalik.
        static const char kExpect[] =
            "19aa8e953c6c99411e12263e48b7a747e215b369f2a19d1a580817355d0a0269";
        const std::string got = [&] {
            static const char hx[] = "0123456789abcdef";
            std::string s;
            for (uint8_t b : c) {
                s += hx[b >> 4];
                s += hx[b & 0x0f];
            }
            return s;
        }();
        const bool pass = (got == kExpect);
        ok &= pass;
        lg::info("selftest", std::string("commitment blake2b(blob||digest): ") +
                                 (pass ? "OK" : ("FAIL got " + got)));

        // Nilai share harus = limb 24..31 LE.
        const uint64_t v = zcd::shareValue(c);
        uint64_t want = 0;
        for (size_t i = 0; i < 8; i++) want |= static_cast<uint64_t>(c[24 + i]) << (8 * i);
        const bool pass2 = (v == want);
        ok &= pass2;
        lg::info("selftest", std::string("shareValue limb24_31: ") + (pass2 ? "OK" : "FAIL"));

        // Perbandingan ketat: value == target harus DITOLAK.
        const bool pass3 = !zcd::meetsTarget(c, 1) && zcd::meetsTarget(c, UINT64_MAX);
        ok &= pass3;
        lg::info("selftest", std::string("meetsTarget batas: ") + (pass3 ? "OK" : "FAIL"));
    }

    lg::info("selftest", ok ? "SEMUA OK" : "ADA YANG GAGAL");
    return ok;
}

}  // namespace

namespace zcd {

bool selftest() { return runSelftest(); }

int run(const ::MinerConfig& cfg, std::atomic<bool>& running) {
    if (cfg.pools.empty()) {
        lg::error("zcd", "tidak ada pool: isi POOL=host:port atau -o host:port");
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

    lg::info("zcd", "xcb -a zcd — pool " + mc.pool.host + ":" +
                        std::to_string(mc.pool.port) + (mc.pool.tls ? " (tls)" : " (plain)") +
                        ", worker " + mc.pool.worker + ", threads " +
                        std::to_string(mc.threads) + (mc.fastMem ? ", FAST" : ", LIGHT"));

    Miner miner;
    std::string err;
    if (!miner.start(mc, err)) {
        lg::error("zcd", "start gagal: " + err);
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
        lg::info("zcd", "HR " + std::to_string(static_cast<long long>(rate)) + " H/s | found " +
                            std::to_string(miner.found()));
    }

    lg::info("zcd", "berhenti…");
    miner.stop();
    lg::info("sysd", miner.finalSummary());
    return 0;
}

}  // namespace zcd
