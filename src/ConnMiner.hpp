#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// Miner solo ConnectCoin (CONN).
//
// ConnectCoin tidak punya pool/stratum: miner memakai JSON-RPC ke node
// (getblocktemplate / submitblock) dan menyusun blok sendiri:
//   - coinbase dengan output typed type-1 (P2PK 32-byte x-only key),
//   - witness commitment dari template (commitment ConnectCoin ada di
//     scriptSig coinbase, bukan output OP_RETURN),
//   - merkle root dari coinbase + transaksi template,
//   - header 80-byte, nonce 32-bit, PoW = RandomX V2 (external/randomx-v2)
//     dengan key epoch dari template.
struct ConnMinerConfig {
    std::string rpcUrl = "http://127.0.0.1:48172";
    std::string rpcUser;
    std::string rpcPass;
    std::string cookie = "~/.connectcoin/.cookie";
    std::string address;  // alamat reward (P2TR, 5120<key>)
    int threads = 1;
    bool fastMem = true;    // FAST dataset ~2 GiB (hash sama dengan LIGHT)
    bool largePages = true; // coba hugepages, fallback otomatis
};

class ConnMiner {
public:
    ConnMiner();
    ~ConnMiner();

    ConnMiner(const ConnMiner&) = delete;
    ConnMiner& operator=(const ConnMiner&) = delete;

    // Blocking singkat: autentikasi RPC, ambil scriptPubKey alamat, tarik
    // template pertama, siapkan context RandomX (FAST bisa 10-30 s), lalu
    // menjalankan thread worker + template.
    bool start(const ConnMinerConfig& cfg);
    void stop();
    bool isRunning() const { return m_running.load(); }

    std::string finalSummary();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_running{false};
};
