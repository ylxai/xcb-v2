#include "ZcdCommitment.hpp"

// Satu-satunya tempat di backend ZCD yang menyentuh library RandomX V2.
// Pemanggilan randomx_calculate_commitment() di-resolve ke simbol ber-prefix
// rxv2_ lewat rxv2_api.h, jadi tidak pernah menyentuh simbol randomx_* milik
// RandomY (Core Coin XCB) atau RandomX v2 milik ConnectCoin yang dipakai
// backend lain. File ini tidak meng-include header backend lain mana pun.
#include "../../external/randomx-v2/rxv2_api.h"
#include "../../external/randomx-v2/src/randomx.h"

namespace zcd {

void commitment(const uint8_t* blob, size_t blobLen, const uint8_t digest[DIGEST_SIZE],
                uint8_t out[COMMITMENT_SIZE]) {
    randomx_calculate_commitment(blob, blobLen, digest, out);
}

uint64_t shareValue(const uint8_t c[COMMITMENT_SIZE]) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++)
        v |= static_cast<uint64_t>(c[24 + i]) << (8 * i);
    return v;
}

bool meetsTarget(const uint8_t c[COMMITMENT_SIZE], uint64_t target) {
    if (target == 0) return false;
    return shareValue(c) < target;
}

double difficulty(uint64_t target) {
    if (target == 0) return 0.0;
    return 18446744073709551616.0 / static_cast<double>(target);
}

}  // namespace zcd
