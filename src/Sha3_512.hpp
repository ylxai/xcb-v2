#pragma once
#include <cstddef>
#include <cstdint>

// Optimized SHA3-512 (Keccak-f[1600]) used in the RandomY hot path.
//
// The mining pipeline needs SHA3-512 of the 40-byte blob (header||nonce-LE)
// as the 64-byte seed fed to randomx_calculate_hash().  This is exactly one
// Keccak block (rate 72 bytes), so we specialize that case and skip all
// generic absorb/squeeze machinery.

namespace sha3 {

// SHA3-512 of a 40-byte input (header(32) || nonce(8) little-endian).
// One block, one permutation, no heap, no loops over the state structure.
// out must be 64 bytes.
void sha3_512_40(const uint8_t in[40], uint8_t out[64]) noexcept;

// Generic SHA3-512 fallback (correct for any length; also used by self-test).
void sha3_512(const uint8_t* in, size_t len, uint8_t out[64]) noexcept;

}  // namespace sha3
