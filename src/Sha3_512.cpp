#include "Sha3_512.hpp"
#include <cstring>

namespace sha3 {
namespace {

constexpr uint64_t kRoundConstants[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};

inline uint64_t rol64(uint64_t a, unsigned o) noexcept {
    return o ? ((a << o) | (a >> (64 - o))) : a;
}

inline uint64_t load64le(const uint8_t* p) noexcept {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

inline void store64le(uint8_t* p, uint64_t v) noexcept {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}

// Keccak-f[1600] — fully unrolled theta/rho+pi/chi, 24 rounds.
// The rho+pi step is written out explicitly from the standard piln/rotc
// tables; correctness is enforced by the SHA3 self-test (compare vs
// reference picosha3) run at startup with --selftest.
inline void keccakf1600(uint64_t st[25]) noexcept {
    uint64_t bc[5], t, old;

    for (int round = 0; round < 24; ++round) {
        // --- theta ---
        bc[0] = st[0] ^ st[5] ^ st[10] ^ st[15] ^ st[20];
        bc[1] = st[1] ^ st[6] ^ st[11] ^ st[16] ^ st[21];
        bc[2] = st[2] ^ st[7] ^ st[12] ^ st[17] ^ st[22];
        bc[3] = st[3] ^ st[8] ^ st[13] ^ st[18] ^ st[23];
        bc[4] = st[4] ^ st[9] ^ st[14] ^ st[19] ^ st[24];
        t = bc[4] ^ rol64(bc[1], 1);
        st[0] ^= t; st[5] ^= t; st[10] ^= t; st[15] ^= t; st[20] ^= t;
        t = bc[0] ^ rol64(bc[2], 1);
        st[1] ^= t; st[6] ^= t; st[11] ^= t; st[16] ^= t; st[21] ^= t;
        t = bc[1] ^ rol64(bc[3], 1);
        st[2] ^= t; st[7] ^= t; st[12] ^= t; st[17] ^= t; st[22] ^= t;
        t = bc[2] ^ rol64(bc[4], 1);
        st[3] ^= t; st[8] ^= t; st[13] ^= t; st[18] ^= t; st[23] ^= t;
        t = bc[3] ^ rol64(bc[0], 1);
        st[4] ^= t; st[9] ^= t; st[14] ^= t; st[19] ^= t; st[24] ^= t;

        // --- rho + pi (unrolled; t carries the previous lane) ---
        t = st[1];
        old = st[10]; st[10] = rol64(t, 1); t = old;
        old = st[7];  st[7]  = rol64(t, 3); t = old;
        old = st[11]; st[11] = rol64(t, 6); t = old;
        old = st[17]; st[17] = rol64(t, 10); t = old;
        old = st[18]; st[18] = rol64(t, 15); t = old;
        old = st[3];  st[3]  = rol64(t, 21); t = old;
        old = st[5];  st[5]  = rol64(t, 28); t = old;
        old = st[16]; st[16] = rol64(t, 36); t = old;
        old = st[8];  st[8]  = rol64(t, 45); t = old;
        old = st[21]; st[21] = rol64(t, 55); t = old;
        old = st[24]; st[24] = rol64(t, 2); t = old;
        old = st[4];  st[4]  = rol64(t, 14); t = old;
        old = st[15]; st[15] = rol64(t, 27); t = old;
        old = st[23]; st[23] = rol64(t, 41); t = old;
        old = st[19]; st[19] = rol64(t, 56); t = old;
        old = st[13]; st[13] = rol64(t, 8); t = old;
        old = st[12]; st[12] = rol64(t, 25); t = old;
        old = st[2];  st[2]  = rol64(t, 43); t = old;
        old = st[20]; st[20] = rol64(t, 62); t = old;
        old = st[14]; st[14] = rol64(t, 18); t = old;
        old = st[22]; st[22] = rol64(t, 39); t = old;
        old = st[9];  st[9]  = rol64(t, 61); t = old;
        old = st[6];  st[6]  = rol64(t, 20); t = old;
        old = st[1];  st[1]  = rol64(t, 44);

        // --- chi (unrolled per row of 5 lanes) ---
        bc[0] = st[0]; bc[1] = st[1]; bc[2] = st[2]; bc[3] = st[3]; bc[4] = st[4];
        st[0] ^= (~bc[1]) & bc[2];
        st[1] ^= (~bc[2]) & bc[3];
        st[2] ^= (~bc[3]) & bc[4];
        st[3] ^= (~bc[4]) & bc[0];
        st[4] ^= (~bc[0]) & bc[1];
        bc[0] = st[5]; bc[1] = st[6]; bc[2] = st[7]; bc[3] = st[8]; bc[4] = st[9];
        st[5] ^= (~bc[1]) & bc[2];
        st[6] ^= (~bc[2]) & bc[3];
        st[7] ^= (~bc[3]) & bc[4];
        st[8] ^= (~bc[4]) & bc[0];
        st[9] ^= (~bc[0]) & bc[1];
        bc[0] = st[10]; bc[1] = st[11]; bc[2] = st[12]; bc[3] = st[13]; bc[4] = st[14];
        st[10] ^= (~bc[1]) & bc[2];
        st[11] ^= (~bc[2]) & bc[3];
        st[12] ^= (~bc[3]) & bc[4];
        st[13] ^= (~bc[4]) & bc[0];
        st[14] ^= (~bc[0]) & bc[1];
        bc[0] = st[15]; bc[1] = st[16]; bc[2] = st[17]; bc[3] = st[18]; bc[4] = st[19];
        st[15] ^= (~bc[1]) & bc[2];
        st[16] ^= (~bc[2]) & bc[3];
        st[17] ^= (~bc[3]) & bc[4];
        st[18] ^= (~bc[4]) & bc[0];
        st[19] ^= (~bc[0]) & bc[1];
        bc[0] = st[20]; bc[1] = st[21]; bc[2] = st[22]; bc[3] = st[23]; bc[4] = st[24];
        st[20] ^= (~bc[1]) & bc[2];
        st[21] ^= (~bc[2]) & bc[3];
        st[22] ^= (~bc[3]) & bc[4];
        st[23] ^= (~bc[4]) & bc[0];
        st[24] ^= (~bc[0]) & bc[1];

        // --- iota ---
        st[0] ^= kRoundConstants[round];
    }
}

// XOR a 72-byte block into lanes 0..8 of the state.
inline void absorbBlock(uint64_t st[25], const uint8_t block[72]) noexcept {
    st[0] ^= load64le(block + 0);
    st[1] ^= load64le(block + 8);
    st[2] ^= load64le(block + 16);
    st[3] ^= load64le(block + 24);
    st[4] ^= load64le(block + 32);
    st[5] ^= load64le(block + 40);
    st[6] ^= load64le(block + 48);
    st[7] ^= load64le(block + 56);
    st[8] ^= load64le(block + 64);
}

inline void squeeze64(const uint64_t st[25], uint8_t out[64]) noexcept {
    store64le(out + 0, st[0]);
    store64le(out + 8, st[1]);
    store64le(out + 16, st[2]);
    store64le(out + 24, st[3]);
    store64le(out + 32, st[4]);
    store64le(out + 40, st[5]);
    store64le(out + 48, st[6]);
    store64le(out + 56, st[7]);
}

}  // namespace

void sha3_512_40(const uint8_t in[40], uint8_t out[64]) noexcept {
    uint64_t st[25] = {0};
    // Absorb the 40-byte input into lanes 0..4.
    st[0] = load64le(in + 0);
    st[1] = load64le(in + 8);
    st[2] = load64le(in + 16);
    st[3] = load64le(in + 24);
    st[4] = load64le(in + 32);
    // SHA3 padding for rate 72: 0x06 at byte 40, 0x80 at byte 71.
    st[5] ^= 0x06ULL;
    st[8] ^= 0x8000000000000000ULL;
    keccakf1600(st);
    squeeze64(st, out);
}

void sha3_512(const uint8_t* in, size_t len, uint8_t out[64]) noexcept {
    uint64_t st[25] = {0};
    constexpr size_t kRate = 72;

    // Absorb full blocks.
    while (len >= kRate) {
        absorbBlock(st, in);
        keccakf1600(st);
        in += kRate;
        len -= kRate;
    }

    // Final padded block: 0x06 domain byte at the message end, pad10*1
    // terminator 0x80 at the last byte. If len == rate-1 both land on the
    // same byte (0x06 ^ 0x80 = 0x86) — still a single block.
    uint8_t block[kRate] = {0};
    std::memcpy(block, in, len);
    block[len] ^= 0x06;
    block[kRate - 1] ^= 0x80;
    absorbBlock(st, block);
    keccakf1600(st);

    squeeze64(st, out);
}

}  // namespace sha3
