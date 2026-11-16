#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>

#include <unistd.h>

#include <chrono>

#include "../Hashing/RandomXV2.hpp"
#include "../Log.hpp"
#include "ZcdCommitment.hpp"
#include "ZcdMiner.hpp"

// Entry point miner Zycord (ZCD). Biner terpisah dari xcb: tidak ada
// #include ke Config/ConnMiner/ConnPool/StratumClient/Miner, jadi backend
// CONN dan XCB tidak bisa terpengaruh oleh perubahan di sini.
//
// Konfigurasi: CLI > environment > file (~/.config/zcd/miner.conf).
//   --pool HOST[:PORT]     pool stratum (default randomx.rplant.xyz:17197)
//   --address ADDR         alamat wallet ZCD (wajib, 0x02…)
//   --worker NAME          label worker (default: hostname)
//   --threads N            jumlah thread (default: CPU yang boleh dipakai)
//   --plain                matikan TLS
//   --light                LIGHT mode (cache 256 MB, bukan dataset 2 GiB)
//   --no-hugepages         jangan pakai hugepages
//   --nonce-offset N       awal ruang nonce
//   --selftest             uji engine + aturan commitment lalu keluar
//   --version              cetak versi
//
// Env setara: ZCD_POOL, ZCD_ADDRESS, ZCD_WORKER, ZCD_THREADS, ZCD_PLAIN,
// ZCD_LIGHT, ZCD_NO_HUGEPAGES, ZCD_NONCE_OFFSET.
// File: pool=, address=, worker=, threads=, plain=, light=, no_hugepages=,
// nonce_offset=

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

std::string hostWorkerName() {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0) return "worker";
    buf[sizeof(buf) - 1] = '\0';
    std::string s(buf);
    const size_t dot = s.find('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    for (auto& c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) c = '-';
    }
    if (s.empty()) s = "worker";
    if (s.size() > 32) s = s.substr(0, 32);
    return s;
}

bool splitHostPort(const std::string& in, std::string& host, uint16_t& port) {
    if (in.empty()) return false;
    std::string h = in;
    uint16_t p = 17197;
    // [::1]:9425 atau host:port
    const size_t colon = in.rfind(':');
    if (colon != std::string::npos && in.find(']') != std::string::npos && colon > in.find(']')) {
        h = in.substr(0, colon);
        if (h.size() >= 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
        const long v = std::strtol(in.c_str() + colon + 1, nullptr, 10);
        if (v <= 0 || v > 65535) return false;
        p = static_cast<uint16_t>(v);
    } else if (colon != std::string::npos && in.find(':') == colon) {
        h = in.substr(0, colon);
        const long v = std::strtol(in.c_str() + colon + 1, nullptr, 10);
        if (v <= 0 || v > 65535) return false;
        p = static_cast<uint16_t>(v);
    }
    if (h.empty()) return false;
    host = h;
    port = p;
    return true;
}

bool truthy(const std::string& v) {
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

struct Options {
    std::string pool = "randomx.rplant.xyz:17197";
    std::string address;
    std::string worker = hostWorkerName();
    int threads = 0;
    bool tls = true;
    bool light = false;
    bool largePages = true;
    uint32_t nonceOffset = 0;
    bool selftest = false;
    bool version = false;
};

bool applyKeyValue(Options& o, const std::string& k, const std::string& v, std::string& err) {
    if (k == "pool") o.pool = v;
    else if (k == "address") o.address = v;
    else if (k == "worker") o.worker = v;
    else if (k == "threads") o.threads = std::atoi(v.c_str());
    else if (k == "plain") o.tls = !truthy(v);
    else if (k == "light") o.light = truthy(v);
    else if (k == "no_hugepages") o.largePages = !truthy(v);
    else if (k == "nonce_offset") o.nonceOffset = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    else {
        err = "key tidak dikenal di file: " + k;
        return false;
    }
    return true;
}

void loadFile(Options& o, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= s.size() || s[i] == '#') continue;
        const size_t eq = s.find('=', i);
        if (eq == std::string::npos) continue;
        std::string k = s.substr(i, eq - i);
        std::string v = s.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        size_t j = 0;
        while (j < v.size() && (v[j] == ' ' || v[j] == '\t')) j++;
        v = v.substr(j);
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
            v = v.substr(1, v.size() - 2);
        if (v.empty()) continue;
        std::string err;
        applyKeyValue(o, k, v, err);  // key tak dikenal diabaikan agar forward-compatible
    }
    std::fclose(f);
}

void usage(const char* prog) {
    std::printf(
        "Zycord (ZCD) miner — RandomX v2, pool dialek xmrig\n"
        "\n"
        "Penggunaan: %s --address ADDR [opsi]\n"
        "\n"
        "  --pool HOST[:PORT]   pool stratum (default randomx.rplant.xyz:17197)\n"
        "  --address ADDR       alamat wallet ZCD (wajib, 0x02…)\n"
        "  --worker NAME        label worker (default: hostname)\n"
        "  --threads N          jumlah thread (default: CPU yang boleh dipakai)\n"
        "  --plain              matikan TLS\n"
        "  --light              LIGHT mode (cache 256 MB, bukan dataset 2 GiB)\n"
        "  --no-hugepages       jangan pakai hugepages\n"
        "  --nonce-offset N     awal ruang nonce\n"
        "  --selftest           uji engine + aturan commitment, lalu keluar\n"
        "  --version            cetak versi\n"
        "  -h, --help           bantuan ini\n"
        "\n"
        "Env: ZCD_POOL ZCD_ADDRESS ZCD_WORKER ZCD_THREADS ZCD_PLAIN ZCD_LIGHT\n"
        "     ZCD_NO_HUGEPAGES ZCD_NONCE_OFFSET\n",
        prog);
}

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

int main(int argc, char** argv) {
    lg::init();

    Options o;
    loadFile(o, std::string(getenv("HOME") ? getenv("HOME") : "") + "/.config/zcd/miner.conf");

    // Env menimpa file.
    const char* envMap[][2] = {{"ZCD_POOL", "pool"},
                               {"ZCD_ADDRESS", "address"},
                               {"ZCD_WORKER", "worker"},
                               {"ZCD_THREADS", "threads"},
                               {"ZCD_PLAIN", "plain"},
                               {"ZCD_LIGHT", "light"},
                               {"ZCD_NO_HUGEPAGES", "no_hugepages"},
                               {"ZCD_NONCE_OFFSET", "nonce_offset"}};
    for (auto& kv : envMap) {
        const char* v = std::getenv(kv[0]);
        if (!v || !*v) continue;
        std::string err;
        applyKeyValue(o, kv[1], v, err);
    }

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s butuh argumen\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (a == "--version") {
            o.version = true;
        } else if (a == "--selftest") {
            o.selftest = true;
        } else if (a == "--pool") {
            o.pool = need("--pool");
        } else if (a.rfind("--pool=", 0) == 0) {
            o.pool = a.substr(7);
        } else if (a == "--address" || a == "-u") {
            o.address = need("--address");
        } else if (a.rfind("--address=", 0) == 0) {
            o.address = a.substr(10);
        } else if (a == "--worker") {
            o.worker = need("--worker");
        } else if (a.rfind("--worker=", 0) == 0) {
            o.worker = a.substr(9);
        } else if (a == "--threads" || a == "-t") {
            o.threads = std::atoi(need("--threads"));
        } else if (a.rfind("--threads=", 0) == 0) {
            o.threads = std::atoi(a.substr(10).c_str());
        } else if (a == "--nonce-offset") {
            o.nonceOffset = static_cast<uint32_t>(std::strtoul(need("--nonce-offset"), nullptr, 10));
        } else if (a == "--plain") {
            o.tls = false;
        } else if (a == "--light") {
            o.light = true;
        } else if (a == "--no-hugepages") {
            o.largePages = false;
        } else {
            std::fprintf(stderr, "opsi tidak dikenal: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (o.version) {
        std::printf("zcd-miner 0.1.0 (RandomX v2 / rx/2)\n");
        return 0;
    }
    if (o.selftest) return runSelftest() ? 0 : 1;

    if (o.address.empty()) {
        std::fprintf(stderr, "[zcd] butuh alamat wallet: --address ADDR (atau ZCD_ADDRESS)\n");
        return 1;
    }

    zcd::MinerConfig mc;
    if (!splitHostPort(o.pool, mc.pool.host, mc.pool.port)) {
        std::fprintf(stderr, "[zcd] --pool tidak valid: %s\n", o.pool.c_str());
        return 2;
    }
    mc.pool.tls = o.tls;
    mc.pool.login = o.address;
    mc.pool.worker = o.worker;
    mc.fastMem = !o.light;
    mc.largePages = o.largePages;
    mc.nonceOffset = o.nonceOffset;
    mc.threads = o.threads;
    if (mc.threads <= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        int n = 1;
        if (sched_getaffinity(0, sizeof(set), &set) == 0) {
            n = 0;
            for (int i = 0; i < CPU_SETSIZE; i++)
                if (CPU_ISSET(i, &set)) n++;
            if (n < 1) n = 1;
        }
        mc.threads = n;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    lg::info("zcd", "zcd-miner 0.1.0 — pool " + mc.pool.host + ":" +
                        std::to_string(mc.pool.port) + (mc.pool.tls ? " (tls)" : " (plain)") +
                        ", worker " + mc.pool.worker + ", threads " + std::to_string(mc.threads) +
                        (mc.fastMem ? ", FAST" : ", LIGHT"));

    zcd::Miner miner;
    std::string err;
    if (!miner.start(mc, err)) {
        lg::error("zcd", "start gagal: " + err);
        return 1;
    }

    uint64_t lastHashes = 0;
    auto lastTick = std::chrono::steady_clock::now();
    while (!g_stop && miner.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto now = std::chrono::steady_clock::now();
        const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count();
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
    lg::info("zcd", miner.finalSummary());
    return 0;
}
