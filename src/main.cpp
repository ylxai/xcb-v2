#include "Config.hpp"
#include "Log.hpp"
#include "Miner.hpp"
#include "Sha3_512.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

static std::atomic<bool> g_running{true};
static std::atomic<Miner*> g_miner{nullptr};

extern "C" void signal_handler(int sig) {
    (void)sig;
    g_running.store(false);  // async-signal-safe: only set a flag
}

static std::string bytes_to_hex(const uint8_t* data, int len) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.resize(len * 2);
    for (int i = 0; i < len; i++) {
        s[i * 2] = hex[(data[i] >> 4) & 0xf];
        s[i * 2 + 1] = hex[data[i] & 0xf];
    }
    return s;
}

// Compare our keccak against the NIST test vectors and deterministic vectors
// generated with OpenSSL (Python hashlib). The input pattern is (i*7+3) & 0xFF.
// Coverage includes block boundaries (72/144/512), the SHA3 padding edge case
// len%72==71, and multi-block inputs up to 513 bytes.
static int runSelftest() {
    bool ok = true;
    auto check = [&ok](const std::string& name, const std::vector<uint8_t>& in,
                       const std::string& expectHex) {
        uint8_t out[64];
        sha3::sha3_512(in.data(), in.size(), out);
        std::string got = bytes_to_hex(out, 64);
        bool pass = (got == expectHex);
        ok &= pass;
        if (pass)
            lg::info("selftest", name + ": OK");
        else {
            lg::error("selftest", name + ": FAIL");
            lg::error("selftest", "  got  " + got);
            lg::error("selftest", "  want " + expectHex);
        }
    };

    // NIST SHA3-512 vectors.
    check("sha3-512('')", {},
          "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a615b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26");
    check("sha3-512('abc')", {0x61, 0x62, 0x63},
          "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0");

    // Deterministic vectors generated with OpenSSL (Python hashlib) covering
    // block boundaries (72/144/512), the padding edge case len%72==71, and
    // multi-block inputs up to 513 bytes.
    auto checkBytes = [&ok, &check](const std::string& name, size_t len,
                                    const std::string& expectHex) {
        std::vector<uint8_t> in(len);
        for (size_t i = 0; i < len; i++) in[i] = (uint8_t)((i * 7 + 3) & 0xFF);
        check(name, in, expectHex);
    };
    checkBytes("sha3-512(1B)", 1,
               "a63f9712aadceb0fe16f1881719a802294830fa1f7a0e58ff26a45a814309961c5007f43f4e224a82d3f538a6c8af7d87ec71fb6661bfac0dc0cd9367ddb04ce");
    checkBytes("sha3-512(7B)", 7,
               "ee414cb84d2582db2ec47d3b484551bf43d3871d1c3abfa270a6afeccbeaa572747c92541f11564ef77057b34e15bb12910b57eb355849df93ea8baff04c7662");
    checkBytes("sha3-512(8B)", 8,
               "6363a2c00a00e26f30d476784ac6f7839994060e0a7543c3175965c7c14cb5969ee7802a5f28d3fbfa738d1cbc25164383f50d33553ef1537fabd5e5c9d1898d");
    checkBytes("sha3-512(39B)", 39,
               "8dade13e9ee680e2d5c60f5bb60beae839901fe8763be20a23a7872160b8f90afa1a4f93c84b4df42cf0c74dc74ad02dfbdf799637098b2e8fcd55215a24787f");
    checkBytes("sha3-512(40B)", 40,
               "ff9c1a3b365a05dfe80c84ec3c268e9a091c438993613121daefa7bdd0594ea9b8a14186a533a6115c97655974c7d974dfc6a5d7f57499794ad9f04c2e3f6a17");
    checkBytes("sha3-512(63B)", 63,
               "ef3d22309d3cf4ce62e7f5913e3c8266e92bca25230a0f99db0c2e169c2609f635bd10a26ecc8a640bab84990708cad41520266815a214d4b62c57f9dfd08bd3");
    checkBytes("sha3-512(64B)", 64,
               "f37322220063c7eb72c37e9ede7eaa19d20836e3144d7fa26847d2b2234232b942999eadb626ca668d5d9063f353ddc9460186de2e5b0041d9367e6880068ee0");
    checkBytes("sha3-512(65B)", 65,
               "3cf0f4358e102da7286bf2105d7d980266b3d45d1c229638ecb0a36314f086bde440a282e103b0eb1cd9d9a529495515ee60a5a81bc1b6ad8798b33898547ed8");
    checkBytes("sha3-512(70B)", 70,
               "4516520e533aec15187849f528f6b13d824020d74f8d15327b07af6fb840d747893b6d3e9c597a93f846c9bcb185728d0aa6f7212bcab9d8caf7147563560c7c");
    checkBytes("sha3-512(71B)", 71,
               "a02d5795bffd44cb0ac3cc3401ae89056b8017242eaf7e802033e974672ce7945811760c3b0d9578bc51bf90c364636ac87cda9b4f3e45620ea9c030421e9d86");  // len%72==71
    checkBytes("sha3-512(72B)", 72,
               "2ec0da5ff440c192d33033c4257eb39dcbd27edd7e41b5ac8db9daf13db501a2ef938151aaddd82f600335654f77512cbcc926ec5abb05b79282ec716d685618");
    checkBytes("sha3-512(73B)", 73,
               "d26d1f9fc91b1d6c31405cc4a546f246fa7777f698d956c26a15170bccdd696aab3ee2e0597ee6f33bc7ac0336b1e4bfa0cc4355ab9faf8111cf27312640fd65");
    checkBytes("sha3-512(100B)", 100,
               "4dce4c128f61d48f0c46f0aedcc2e1013a03ec567d2abfd9acd66902c4b786765d296345ba3cf42aad01aeaddaf4cc4ea4d22dd6bf55b1df0fbb9f3909537cc2");
    checkBytes("sha3-512(127B)", 127,
               "591a01c19e9a3b4cc2f910dc95cc057b3b82af53bb7d91d775b2bf00024435733a320a3ab82e7251c10cf1461825a32f86c2c8ac00f5c174e78401dc5370f896");
    checkBytes("sha3-512(128B)", 128,
               "469992409e1433702f7787fc5bf787db2e3db04af63f06c6c34b647adc4deb92dc96cb5e5a50d97a05b685f76fb0ff4c057fe07ed8542d38a988a922fe600ade");
    checkBytes("sha3-512(129B)", 129,
               "a455cead1dac677a33532a8962fb07f985a8d26d176df407cb4b43ce6b5ddd708d6a0ec25237f24261972233aa62afab09d26e70c552fa3ad74440ccf6e02d47");
    checkBytes("sha3-512(143B)", 143,
               "d610c9973ce4c1f18ce6f7358279f05c02f0aec6dc704cc4fd4957dec3276950c55d59a9ce25025692ecdeb0ff6d7d527fe3a2614c52279fcf61fb39ade15ec7");  // len%72==71
    checkBytes("sha3-512(144B)", 144,
               "c6b8908e7b60f09ed38ffaf0ff9d6f3edd53ce3140076fc947bd3962c79c64d845d80b48f63b076dd7f4f0c360eb952dc940c99ed09a6fbdeabb51035b18e41d");
    checkBytes("sha3-512(200B)", 200,
               "14fb36d333d34fccb38c8801d7692c9350a324cbc44448b63aca9d3cfdb12fb52a08934fefda157796735871f1541d1e5bcb70cb03ca13186c94c5c45e1f1cad");
    checkBytes("sha3-512(255B)", 255,
               "e7fd8451b4e342851049fd7fb2be254ae2b67f69bc10e98e9b6cfa4bbf1d9e8a850b872f22f9a56c3694d95728a203fb4173b09c4ecd193472f1f01b0fb378be");
    checkBytes("sha3-512(256B)", 256,
               "01d4ec18186d3ba86fcb935f4a4176c04e7f644cc70f1865e12869677a9d59a06eaed03c464f94ea7d1ae97a69136c6ae9325ebeeb82ce0a63d8eec3bbe9ca4e");
    checkBytes("sha3-512(257B)", 257,
               "79b47ccb3969f6549a783acbfafffb045ad77cc6042d7f894f3eb7a8faca0fc1f80d1bf42ecc5fd29b53e5c4692b9c968d9d493f0347abf81deb2b9ea11c844b");
    checkBytes("sha3-512(300B)", 300,
               "f85b76d4d2c98f6ec82f06651127d19643e2b1117420b700c42a8cdd55379152ca391df72e30886e70beb58cf28c05ae6bdb1197a91542e57fac331233246b93");
    checkBytes("sha3-512(511B)", 511,
               "f01202cc9a4d57f9c326682801120e7653a2cc1a25f045e22fce1fd6a1849c772ef6f55df03e32444d9d92a3aeab9505370a8998f81d5e0d00a4510834c135f9");
    checkBytes("sha3-512(512B)", 512,
               "5cdad7d02f670820da6a273fa427809d4adb35e756b2ff78cb87b59eb027a7ab2328e82842c6e786f8616a5c90489314f405257535deea8243e83765e2958aed");
    checkBytes("sha3-512(513B)", 513,
               "c520052db835c6d595351fda1b3a9ff26587375674f6d8b80ff0ae30ffaa9253a353ab4e81f9aeb4809704fd7374238e323241ff7f3fd2269d3fafea8b4ae8cc");

    // 40-byte specialized path must agree with the generic path AND the
    // OpenSSL vector above.
    {
        uint8_t in[40];
        for (int i = 0; i < 40; i++) in[i] = (uint8_t)(i * 7 + 3);
        uint8_t a[64], b[64];
        sha3::sha3_512_40(in, a);
        sha3::sha3_512(in, 40, b);
        bool pass = memcmp(a, b, 64) == 0 &&
                    bytes_to_hex(a, 64) ==
                        "ff9c1a3b365a05dfe80c84ec3c268e9a091c438993613121daefa7bdd0594ea9b8a14186a533a6115c97655974c7d974dfc6a5d7f57499794ad9f04c2e3f6a17";
        ok &= pass;
        lg::info("selftest", std::string("sha3_512_40 (vs generic + OpenSSL): ") +
                                 (pass ? "OK" : "FAIL"));
    }

    lg::info("selftest", std::string("RESULT: ") + (ok ? "PASS" : "FAIL"));
    return ok ? 0 : 1;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    lg::init();

    auto cfg = Config::parse(argc, argv);

    if (cfg.selftest) return runSelftest();

    lg::info("main", "miner-saya v2 (xcb-v2)");
    lg::info("config", "Pools: " + std::to_string(cfg.pools.size()) + ", Threads: " +
                            std::to_string(cfg.threads) +
                            (cfg.benchmarkNonces > 0 ? ", Benchmark: " +
                                                           std::to_string(cfg.benchmarkNonces)
                                                     : ""));
    for (size_t i = 0; i < cfg.pools.size(); i++) {
        auto& p = cfg.pools[i];
        std::string masked = p.wallet.size() > 16 ? p.wallet.substr(0, 16) + "..." : p.wallet;
        lg::info("config", "  pool " + std::to_string(i + 1) + ": " + p.host + ":" +
                                std::to_string(p.port) + " wallet=" + masked +
                                " worker=" + p.worker);
    }

    Miner miner;
    g_miner.store(&miner);
    miner.start(cfg);

    while (g_running.load() && miner.isRunning())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    g_miner.store(nullptr);

    if (miner.isRunning()) {
        // We are stopping (SIGINT/SIGTERM): wait for workers to wind down, then report.
        lg::info("main", "Stopping...");
        miner.stop();
    }
    if (cfg.benchmarkNonces > 0) lg::info("bench", "Benchmark finished");
    lg::info("miner", miner.finalSummary());
    return 0;
}
