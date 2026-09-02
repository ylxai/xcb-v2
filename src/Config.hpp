#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct PoolConfig {
    std::string host;
    uint16_t port = 8008;
    std::string wallet;
    std::string worker = "worker";
    std::string password = "x";
};

struct MinerConfig {
    std::vector<PoolConfig> pools;
    int threads = 0;          // 0 = auto (CPU cores)
    bool useJIT = true;
    bool fullMem = true;
    bool hardAES = true;
    bool largePages = true;
    // Auto = nilai di atas belum ditentukan user, jadi diisi dari hasil probe
    // host (RAM tersedia / hugepages bebas). Jadi false begitu ada nilai
    // eksplisit dari pool.cfg, env, atau CLI.
    bool fullMemAuto = true;
    bool largePagesAuto = true;
    bool reportHashrate = false;  // eth_submitHashrate every 60 s
    int pollMs = 1000;            // eth_getWork poll interval
    uint64_t benchmarkNonces = 0; // >0 => benchmark mode (no pool)
    bool selftest = false;        // run SHA3/keccak self test and exit
    std::string ui = "auto";      // auto | ftxui | ansi | log (display mode)
};

class Config {
public:
    static MinerConfig parse(int argc, char* argv[]);
    static MinerConfig loadFile(const std::string& path);
    // Isi fullMem/largePages dari kondisi host untuk field yang masih "auto".
    static void autoDetect(MinerConfig& cfg);
    // CPU yang benar-benar boleh dipakai proses ini (sched_getaffinity), bukan
    // semua CPU online. Di cpuset (docker --cpuset-cpus, k8s cpu pinning)
    // _SC_NPROCESSORS_ONLN ikut menghitung CPU yang terlarang, sehingga
    // thread dibuat berlebih dan pinning-nya gagal dengan EINVAL.
    static std::vector<int> usableCpus();
    // Nama worker dari hostname mesin, dibersihkan agar aman dikirim ke pool
    // (huruf/angka/'-'/'_' saja, maks 32 char). Dipakai saat worker=auto,
    // supaya tiap devbox/VPS muncul dengan namanya sendiri di dashboard pool
    // tanpa perlu mengubah pool.cfg per mesin. Kembalikan "worker" bila
    // hostname tidak bisa dibaca atau tidak menyisakan karakter yang sah.
    static std::string hostWorkerName();
};
