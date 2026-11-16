#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ZcdCommitment.hpp"

// Satu job dari pool stratum dialek Monero/XMRig, sebagaimana dikirim rplant
// untuk Zycord (algo "rx/2"). Contoh payload nyata:
//
//   {"algo":"rx/2","blob":"<86 hex>","height":115805,
//    "job_id":"d64","seed_hash":"<64 hex>","next_seed_hash":"<64 hex>",
//    "target":"a38d23d6e2530000"}
//
// blob 43 byte = seed(0..31) + 7 byte nol(32..38) + nonce 4 byte LE(39..42).
// target 8 byte LE.
namespace zcd {

struct Job {
    std::string id;
    std::string algo = "rx/2";
    uint64_t height = 0;
    std::vector<uint8_t> blob;
    uint8_t key[32]{};
    uint64_t target = 0;

    bool valid() const {
        return !id.empty() && blob.size() == BLOB_SIZE && target != 0;
    }

    // Tempat nonce di dalam blob (39..42).
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

}  // namespace zcd
