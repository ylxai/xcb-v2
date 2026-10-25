#pragma once

// Wrapper C++ di atas yespower upstream (external/yespower, openwall
// YESPOWER_1_0_0). Parameternya untuk Ratatoskr (RATR): yespower 1.0,
// N=2048, R=32, tanpa personalization string — sesuai doc/mining.md resmi.
//
// API upstream (yespower.h):
//   yespower_init_local / yespower / yespower_free_local
// yespower_local_t menyimpan alokasi memori (~8MB per thread pada N=2048,
// r=32) antar pemanggilan, jadi tiap worker thread memegang satu Hasher.

#include <cstddef>
#include <cstdint>

#include <yespower.h>

namespace ypw {

struct Params {
    uint32_t N = 2048;
    uint32_t R = 32;
};

// Per-thread hasher. Tidak thread-safe antar instance yang sama; satu
// instance per worker thread (MT-safe selama local-nya lokal ke thread).
class Hasher {
public:
    Hasher() = default;
    ~Hasher();
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    // Alokasi thread-local storage. false bila memori gagal dialokasikan.
    bool init(const Params& p = Params());

    // dst harus 32 byte. src: header 80 byte (atau input apa pun).
    bool hash(const uint8_t* src, size_t srclen, uint8_t dst32[32]);

private:
    yespower_local_t m_local = {};
    yespower_params_t m_params = {};
    bool m_inited = false;
};

// Verifikasi terhadap vektor emas upstream (TESTS-OK): yespower 1.0,
// N=2048, R=32, tanpa pers, input 80 byte src[i] = i*3.
bool selftest();

}  // namespace ypw
