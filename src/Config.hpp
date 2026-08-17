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
};
