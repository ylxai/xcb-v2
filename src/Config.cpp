#include "Config.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <pwd.h>
#include <sched.h>
#include <sstream>
#include <unistd.h>

// Ambang memori auto-detect harus ikut parameter RandomY yang sebenarnya
// dikompilasi, bukan angka yang disalin dari RandomX. randomx.h memberi
// RANDOMX_DATASET_ITEM_SIZE + randomx_dataset_item_count(), configuration.h
// memberi RANDOMX_ARGON_MEMORY (ukuran cache, dalam KiB).
#include <configuration.h>
#include <randomx.h>

// CPU yang boleh dipakai proses ini. sched_getaffinity menghormati cpuset
// (docker --cpuset-cpus, k8s cpu manager, taskset), sementara
// _SC_NPROCESSORS_ONLN tidak — memakai yang kedua berarti membuat thread untuk
// CPU yang tidak boleh disentuh dan pin-nya gagal EINVAL.
std::vector<int> Config::usableCpus() {
    std::vector<int> cpus;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int i = 0; i < CPU_SETSIZE; i++)
            if (CPU_ISSET(i, &set)) cpus.push_back(i);
    }
    if (cpus.empty()) {
        // Fallback: kernel tanpa sched_getaffinity yang bisa dibaca.
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        for (long i = 0; i < n; i++) cpus.push_back(static_cast<int>(i));
    }
    return cpus;
}

// Nama worker dari hostname. Pool ETHPROXY memakai worker sebagai label bebas,
// tapi ia ikut dalam string login "wallet.worker" — karakter aneh (titik,
// spasi, kutip) bisa merusak pemisahan itu atau JSON-nya, jadi dibersihkan
// ketat: huruf/angka/'-'/'_' dipertahankan, sisanya jadi '-'.
std::string Config::hostWorkerName() {
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0) return "worker";
    buf[sizeof(buf) - 1] = '\0';

    std::string h(buf);
    // Hanya bagian pertama FQDN: "mesin.domain.tld" -> "mesin". Titik juga
    // pemisah wallet.worker, jadi tidak boleh ikut.
    auto dot = h.find('.');
    if (dot != std::string::npos) h = h.substr(0, dot);

    std::string out;
    for (char c : h) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
            out += c;
        else if (!out.empty() && out.back() != '-')
            out += '-';
        if (out.size() >= 32) break;  // pool umumnya membatasi panjang worker
    }
    while (!out.empty() && out.back() == '-') out.pop_back();

    return out.empty() ? "worker" : out;
}

static std::string expandHome(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) home = getpwuid(getuid())->pw_dir;
    return std::string(home) + path.substr(1);
}

// ------------------------------------------------------------
// Parsing angka yang tidak boleh mematikan proses.
// std::stoi/std::stoul/std::stoull melempar std::invalid_argument untuk input
// bukan angka dan std::out_of_range untuk yang kelewat besar. Nilai-nilai ini
// datang dari env & argv (THREADS=abc, -t x, POLL_MS=…), jadi input salah harus
// jadi peringatan + nilai default, bukan std::terminate.
// ------------------------------------------------------------
static bool parseLong(const std::string& s, long long& out) {
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        long long v = std::stoll(s, &pos);
        if (pos != s.size()) return false;  // ada sisa karakter: "12abc"
        out = v;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Angka wajar (thread, port, poll interval). Di luar [min,max] -> fallback.
static long long numOr(const std::string& s, long long fallback, long long min, long long max,
                       const char* what) {
    long long v = 0;
    if (!parseLong(s, v)) {
        std::cerr << "[Config] " << what << " bukan angka ('" << s << "'), pakai " << fallback
                  << std::endl;
        return fallback;
    }
    if (v < min || v > max) {
        std::cerr << "[Config] " << what << "=" << v << " di luar rentang [" << min << ".." << max
                  << "], pakai " << fallback << std::endl;
        return fallback;
    }
    return v;
}

MinerConfig Config::loadFile(const std::string& path) {
    MinerConfig cfg;
    std::string expanded = expandHome(path);
    std::ifstream file(expanded);
    if (!file.is_open()) {
        std::cerr << "[Config] Cannot open: " << expanded << std::endl;
        return cfg;
    }
    
    std::cout << "[Config] Loading: " << expanded << std::endl;
    PoolConfig pool;
    std::string line;
    
    while (std::getline(file, line)) {
        // Strip comments
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        
        // Trim whitespace
        auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        auto end = line.find_last_not_of(" \t\r");
        line = line.substr(start, end - start + 1);
        
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        
        // Remove surrounding quotes
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        
        if (key == "wallet") {
            // Pool config uses single wallet for all servers
            if (cfg.pools.empty()) {
                pool.wallet = val;
            } else {
                for (auto& p : cfg.pools) p.wallet = val;
            }
        } else if (key == "worker") {
            pool.worker = val;
        } else if (key.compare(0, 6, "server") == 0 && key.back() == ']') {
            // server[N]=host — push previous pool if exists
            if (!pool.host.empty()) {
                cfg.pools.push_back(pool);
                pool = PoolConfig();
                // Apply shared wallet/worker across all pools
                if (!cfg.pools.empty()) {
                    pool.wallet = cfg.pools[0].wallet;
                    pool.worker = cfg.pools[0].worker;
                }
            }
            pool.host = val;
        } else if (key.compare(0, 4, "port") == 0 && key.back() == ']') {
            pool.port = static_cast<uint16_t>(numOr(val, 8008, 1, 65535, "port"));
        } else if (key == "threads") {
            cfg.threads = static_cast<int>(numOr(val, 0, 0, 4096, "threads"));
        } else if (key == "no_jit") {
            cfg.useJIT = (val != "true" && val != "1" && val != "yes");
        } else if (key == "light") {
            cfg.fullMem = (val == "false" || val == "0" || val == "no");
            cfg.fullMemAuto = false;
        } else if (key == "no_aes") {
            cfg.hardAES = (val != "true" && val != "1" && val != "yes");
        }
    }
    
    // Push last pool
    if (!pool.host.empty()) {
        cfg.pools.push_back(pool);
    }
    
    return cfg;
}

MinerConfig Config::parse(int argc, char* argv[]) {
    MinerConfig cfg;
    
    // 1. Try environment variables first (Docker/Akash friendly)
    const char* envWallet = getenv("WALLET");
    const char* envPool = getenv("POOL");
    const char* envWorker = getenv("WORKER");
    const char* envThreads = getenv("THREADS");
    const char* envFullMem = getenv("FULL_MEM");
    const char* envLargePages = getenv("LARGE_PAGES");
    const char* envReportHr = getenv("REPORT_HASHRATE");
    const char* envPollMs = getenv("POLL_MS");
    const char* envBench = getenv("BENCHMARK");

    // Env feature flags, applied in both the env-pool and file-pool paths.
    auto applyEnvFlags = [&]() {
        if (envFullMem && envFullMem[0] != '\0') {
            std::string fm = envFullMem;
            cfg.fullMem = (fm != "0" && fm != "false" && fm != "no");
            cfg.fullMemAuto = false;
        }
        if (envLargePages && envLargePages[0] != '\0') {
            std::string lp = envLargePages;
            cfg.largePages = (lp != "0" && lp != "false" && lp != "no");
            cfg.largePagesAuto = false;
        }
        if (envReportHr && envReportHr[0] != '\0') {
            std::string rh = envReportHr;
            cfg.reportHashrate = (rh != "0" && rh != "false" && rh != "no");
        }
        if (envPollMs && envPollMs[0] != '\0')
            cfg.pollMs = static_cast<int>(numOr(envPollMs, 1000, 200, 600000, "POLL_MS"));
        if (envBench && envBench[0] != '\0')
            cfg.benchmarkNonces =
                static_cast<uint64_t>(numOr(envBench, 0, 0, 1000000000LL, "BENCHMARK"));
    };

    if (envWallet && envPool) {
        PoolConfig p;
        std::string poolStr = envPool;
        auto colon = poolStr.find(':');
        if (colon != std::string::npos) {
            p.host = poolStr.substr(0, colon);
            p.port = static_cast<uint16_t>(
                numOr(poolStr.substr(colon + 1), 8008, 1, 65535, "POOL port"));
        } else {
            p.host = poolStr;
            p.port = 8008;
        }
        p.wallet = envWallet;
        p.worker = envWorker ? envWorker : "worker";
        cfg.pools.push_back(p);
        if (envThreads && envThreads[0] != '\0')
            cfg.threads = static_cast<int>(numOr(envThreads, 0, 0, 4096, "THREADS"));
        // Default threads = CPU yang boleh dipakai (sama seperti jalur file/CLI)
        if (cfg.threads <= 0)
            cfg.threads = std::max(1, static_cast<int>(usableCpus().size()));
        applyEnvFlags();
        std::cout << "[Config] Using env: POOL=" << envPool << " WALLET=" << envWallet
                  << std::endl;
    } else {
        // 2. Try loading pool.cfg (skipped when env WALLET+POOL are set)
        cfg = loadFile("pool.cfg");
        if (cfg.pools.empty()) cfg = loadFile("/miner/pool.cfg");
        if (cfg.pools.empty()) cfg = loadFile("~/xcb/pool.cfg");
        
        // 3. Apply env overrides on top of the file (precedence: file < env < CLI).
        applyEnvFlags();
    }
    
    // 4. Parse CLI args (override file & env). Always runs, even with env
    //    pools, so flags like --selftest/--benchmark/-t/--light still work
    //    (e.g. inside the docker image where WALLET/POOL are always set).
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-o" && i + 1 < argc) {
            // pool:port format
            PoolConfig p;
            std::string s = argv[++i];
            auto colon = s.find(':');
            if (colon != std::string::npos) {
                p.host = s.substr(0, colon);
                p.port = static_cast<uint16_t>(numOr(s.substr(colon + 1), 8008, 1, 65535, "-o port"));
            } else {
                p.host = s;
                p.port = 8008;
            }
            cfg.pools.push_back(p);
            
        } else if (arg == "-u" && i + 1 < argc) {
            std::string s = argv[++i];
            auto dot = s.find('.');
            if (dot != std::string::npos) {
                // wallet.worker format
                for (auto& p : cfg.pools) {
                    p.wallet = s.substr(0, dot);
                    p.worker = s.substr(dot + 1);
                }
            } else {
                for (auto& p : cfg.pools) p.wallet = s;
            }
            
        } else if (arg == "-p" && i + 1 < argc) {
            for (auto& p : cfg.pools) p.password = argv[++i];
            
        } else if (arg == "-t" && i + 1 < argc) {
            cfg.threads = static_cast<int>(numOr(argv[++i], 0, 0, 4096, "-t"));
            
        } else if (arg == "--light") {
            cfg.fullMem = false;
            cfg.fullMemAuto = false;

        } else if (arg == "--full-mem") {
            cfg.fullMem = true;
            cfg.fullMemAuto = false;

        } else if (arg == "--large-pages") {
            cfg.largePages = true;
            cfg.largePagesAuto = false;

        } else if (arg == "--no-large-pages") {
            cfg.largePages = false;
            cfg.largePagesAuto = false;

        } else if (arg == "--no-jit") {
            cfg.useJIT = false;
            
        } else if (arg == "-R" || arg == "--report-hashrate") {
            cfg.reportHashrate = true;
            
        } else if (arg == "--selftest") {
            cfg.selftest = true;
            
        } else if (arg == "--benchmark" && i + 1 < argc) {
            cfg.benchmarkNonces =
                static_cast<uint64_t>(numOr(argv[++i], 0, 0, 1000000000LL, "--benchmark"));
            
        } else if (arg.compare(0, 5, "--ui=") == 0) {
            cfg.ui = arg.substr(5);
            
        } else if (arg == "--ui" && i + 1 < argc) {
            cfg.ui = argv[++i];
            
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: miner-saya [options]\n"
                      << "  -o host:port          Pool address\n"
                      << "  -u wallet[.worker]    Wallet address (worker 'auto' = hostname)\n"
                      << "  -p password           Pool password\n"
                      << "  -t N                  Thread count\n"
                      << "  -R, --report-hashrate Submit hashrate to pool every 60s\n"
                      << "  --benchmark N         Benchmark N nonces (no pool)\n"
                      << "  --selftest            Verify SHA3-512 implementation, then exit\n"
                      << "  --ui=MODE             Display: auto|ftxui|ansi|log (default auto)\n"
                      << "  --light               Force light dataset (~256MB)\n"
                      << "  --full-mem            Force full dataset (~264MB)\n"
                      << "  --large-pages         Force hugepages on\n"
                      << "  --no-large-pages      Force hugepages off\n"
                      << "  --no-jit              Disable JIT\n"
                      << "  Tanpa flag di atas, FULL_MEM & LARGE_PAGES dipilih otomatis\n"
                      << "  dari RAM tersedia / cgroup limit / hugepages bebas.\n"
                      << "  Config file: pool.cfg\n"
                      << "  Env: WALLET POOL WORKER THREADS FULL_MEM LARGE_PAGES LOG_LEVEL\n"
                      << "       LOG_FORMAT REPORT_HASHRATE POLL_MS BENCHMARK\n";
            exit(0);
        }
    }
    
    // Apply wallet from file if CLI didn't override
    // (already done in loadFile)

    // worker=auto -> nama hostname mesin. Berguna saat satu pool.cfg dipakai di
    // beberapa mesin (devbox, VPS, laptop): tiap mesin muncul dengan namanya
    // sendiri di dashboard pool tanpa perlu file berbeda per mesin. Wallet
    // tidak tersentuh. Berlaku untuk nilai dari pool.cfg, env WORKER, maupun
    // -u wallet.auto.
    {
        std::string hostName;
        for (auto& p : cfg.pools) {
            if (p.worker != "auto") continue;
            if (hostName.empty()) hostName = hostWorkerName();
            p.worker = hostName;
        }
        if (!hostName.empty())
            std::cout << "[Config] worker=auto -> \"" << hostName << "\" (hostname mesin)"
                      << std::endl;
    }

    // Validate — but only for modes that actually talk to a pool.
    // --selftest and --benchmark never open a stratum connection, so requiring
    // pool.cfg there breaks `miner-saya --selftest` from any directory (CI
    // gate, docker image, k8s probe) for no reason.
    const bool needsPool = !cfg.selftest && cfg.benchmarkNonces == 0;
    if (needsPool) {
        if (cfg.pools.empty()) {
            std::cerr << "[Config] No pool configured! Use -o or set in pool.cfg" << std::endl;
            exit(1);
        }
        for (auto& p : cfg.pools) {
            if (p.wallet.empty()) {
                std::cerr << "[Config] No wallet configured! Use -u or set in pool.cfg" << std::endl;
                exit(1);
            }
        }
    }
    
    // Default threads = jumlah CPU yang boleh dipakai proses ini.
    // Di cpuset (docker --cpuset-cpus=2,3 / k8s cpu pinning) jumlah CPU online
    // lebih besar dari yang diizinkan, jadi memakai _SC_NPROCESSORS_ONLN
    // membuat thread berlebih yang berebut core yang sama.
    if (cfg.threads <= 0) {
        const size_t usable = usableCpus().size();
        cfg.threads = std::max(1, static_cast<int>(usable));
        const long online = sysconf(_SC_NPROCESSORS_ONLN);
        if (online > 0 && usable > 0 && static_cast<size_t>(online) != usable)
            std::cout << "[Config] threads=" << cfg.threads << " (CPU boleh dipakai " << usable
                      << " dari " << online << " online — cpuset aktif)" << std::endl;
    }

    // 5. Isi fullMem/largePages dari kondisi host untuk yang belum di-set user.
    autoDetect(cfg);

    return cfg;
}

// ============================================================
// Auto-detect: pilih fullMem & largePages dari kondisi host
// ============================================================

// Baca satu field angka dari /proc/meminfo (satuan kB, atau jumlah halaman
// untuk field HugePages_*). -1 kalau field tidak ada.
// Parsing per-baris, bukan `f >> key >> val >> unit`: baris HugePages_* tidak
// punya kolom satuan, jadi stream extraction akan menyerap key baris berikutnya.
static long readMeminfoKb(const std::string& field) {
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return -1;
    std::string line;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (line.compare(0, colon, field) != 0) continue;
        std::istringstream iss(line.substr(colon + 1));
        long val = 0;
        if (iss >> val) return val;
        return -1;
    }
    return -1;
}

// Baca satu integer dari file /proc atau /sys, -1 kalau gagal.
static long readLong(const char* path) {
    std::ifstream f(path);
    if (!f.is_open()) return -1;
    long v = 0;
    if (!(f >> v)) return -1;
    return v;
}

// Cgroup memory limit (v2 lalu v1). -1 = unlimited / tidak diketahui.
static long cgroupLimitMb() {
    std::ifstream v2("/sys/fs/cgroup/memory.max");
    if (v2.is_open()) {
        std::string s;
        v2 >> s;
        if (s == "max") return -1;
        try {
            long long bytes = std::stoll(s);
            if (bytes > 0) return static_cast<long>(bytes / (1024 * 1024));
        } catch (...) {
        }
    }
    long v1 = readLong("/sys/fs/cgroup/memory/memory.limit_in_bytes");
    // Nilai "unlimited" di cgroup v1 adalah angka raksasa (~2^63 dibulatkan).
    if (v1 > 0 && v1 < (1LL << 53)) return static_cast<long>(v1 / (1024 * 1024));
    return -1;
}

void Config::autoDetect(MinerConfig& cfg) {
    // Selftest tidak mengalokasi dataset sama sekali — jangan buang waktu probe.
    if (cfg.selftest) {
        if (cfg.fullMemAuto) cfg.fullMem = false;
        if (cfg.largePagesAuto) cfg.largePages = false;
        return;
    }

    // --- Memori yang benar-benar bisa dipakai ---
    long availMb = readMeminfoKb("MemAvailable");
    availMb = (availMb > 0) ? availMb / 1024 : -1;
    long limitMb = cgroupLimitMb();
    // Di container, cgroup limit lebih jujur daripada MemAvailable host.
    long usableMb = availMb;
    if (limitMb > 0 && (usableMb < 0 || limitMb < usableMb)) usableMb = limitMb;

    // --- Hugepages bebas ---
    long hpFree = readMeminfoKb("HugePages_Free");
    long hpSizeKb = readMeminfoKb("Hugepagesize");
    if (hpSizeKb <= 0) hpSizeKb = 2048;

    if (cfg.fullMemAuto) {
        // Ambang harus diturunkan dari parameter RandomY yang SEDANG dipakai,
        // bukan angka RandomX. Fork core-coin memakai DATASET_BASE_SIZE
        // 256MiB + EXTRA 8MiB (RandomX: 2GiB + 32MiB), jadi dataset di sini
        // 264MB — delapan kali lebih kecil. Ambang lama 3072MB adalah angka
        // RandomX dan memaksa light mode di mesin 576MB-2GB yang sebenarnya
        // sanggup full mode; terukur 400 H/s vs 1.20 kH/s, kehilangan 3x.
        //
        // Cache dan dataset hidup bersamaan selama init (cache baru dilepas
        // setelah randomx_init_dataset selesai), jadi puncaknya jumlah keduanya.
        // Diukur di devbox: RSS puncak 524MB rata dari 1 sampai 8 thread —
        // scratchpad RandomY hanya 128KiB per VM, jadi thread tidak menambah.
        // Lantai OOM dengan swap dimatikan ada di antara 512MB dan 576MB.
        const long datasetMb =
            (long)((randomx_dataset_item_count() * RANDOMX_DATASET_ITEM_SIZE) / (1024 * 1024));
        const long cacheMb = RANDOMX_ARGON_MEMORY / 1024;
        // Sisa untuk biner, stack, kode hasil JIT, buffer stratum, dan
        // optimisme MemAvailable/proses lain di mesin yang sama.
        const long kInitOverheadMb = 256;
        const long kFullMemNeedMb = datasetMb + cacheMb + kInitOverheadMb;
        cfg.fullMem = (usableMb < 0) ? false : (usableMb >= kFullMemNeedMb);
        std::cout << "[Config] auto FULL_MEM=" << (cfg.fullMem ? 1 : 0) << " (usable "
                  << (usableMb < 0 ? std::string("unknown") : std::to_string(usableMb) + "MB")
                  << ", butuh " << kFullMemNeedMb << "MB untuk mode full: dataset " << datasetMb
                  << "MB + cache " << cacheMb << "MB + overhead " << kInitOverheadMb << "MB)"
                  << std::endl;
    }

    if (cfg.largePagesAuto) {
        // Butuh hugepages yang sudah dialokasikan host DAN cukup untuk mode yang
        // dipilih. Kalau nol (default hampir semua distro, container, CI runner),
        // langsung matikan supaya tidak ada alloc gagal + fallback.
        // Angkanya juga diturunkan dari parameter nyata, bukan 2560MB RandomX.
        const long datasetMb =
            (long)((randomx_dataset_item_count() * RANDOMX_DATASET_ITEM_SIZE) / (1024 * 1024));
        const long cacheMb = RANDOMX_ARGON_MEMORY / 1024;
        const long needMb = cfg.fullMem ? datasetMb + cacheMb : cacheMb;
        const long freeMb = (hpFree > 0) ? (hpFree * hpSizeKb) / 1024 : 0;
        cfg.largePages = (freeMb >= needMb);
        std::cout << "[Config] auto LARGE_PAGES=" << (cfg.largePages ? 1 : 0) << " (hugepages free "
                  << freeMb << "MB, butuh " << needMb << "MB)" << std::endl;
    }
}
