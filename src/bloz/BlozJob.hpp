#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Satu job dari pool stratum dialek Monero/XMRig, sebagaimana dikirim rplant
// untuk Block Zero (algo "rx/blockzero"). Contoh payload nyata:
//
//   {"algo":"rx/blockzero","blob":"<160 hex>","height":23185,
//    "job_id":"683b","seed_hash":"<64 hex>","target":"6bfa4b47d3530000"}
//
// blob 80 byte = header blok Bitcoin (versi 4, prevhash 32, merkle 32,
// time 4, bits 4, nonce 4). Nonce ada di offset 76..79, little-endian.
// target 8 byte little-endian.
namespace bloz {

constexpr size_t BLOB_SIZE = 80;
constexpr size_t NONCE_SIZE = 4;
constexpr size_t NONCE_OFFSET = 76;

// Pool mengirim target 8 byte; difficulty = 2^64 / target, angka yang sama
// yang dicetak cpuminer ("diff 200145" untuk target 6bfa4b47d3530000).
double difficulty(uint64_t target);

// Nilai share = 8 byte teratas digest (offset 24..31) dibaca little-endian.
// Pool mengirim target dalam bentuk yang sama, dan perbandingannya ketat:
// nilai harus BENAR-BENAR lebih kecil dari target.
uint64_t digestValue(const uint8_t d[32]);
bool meetsTarget(const uint8_t d[32], uint64_t target);

struct Job {
    std::string id;
    std::string algo = "rx/blockzero";
    uint64_t height = 0;
    std::vector<uint8_t> blob;
    uint8_t key[32]{};
    uint64_t target = 0;

    bool valid() const {
        return !id.empty() && blob.size() == BLOB_SIZE && target != 0;
    }

    // Tempat nonce di dalam blob (76..79).
    uint32_t nonceAt(size_t i) const;
    void setNonce(size_t off, uint32_t nonce);
    std::string keyHex() const;
    std::string nonceHex(size_t off) const;
    double difficultyValue() const { return difficulty(target); }
};

// Parse objek JSON job (anggota "job" pada result login, atau params pada
// notifikasi "job"). Mengembalikan false + pesan bila ada field wajib yang
// hilang atau formatnya salah.
bool parseJob(const std::string& line, Job& out, std::string& err);

}  // namespace bloz
