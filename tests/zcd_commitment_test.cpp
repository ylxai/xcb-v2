// Uji aturan share Zycord tanpa jaringan dan tanpa RandomX dataset.
//
// Yang dibuktikan:
//  1. commitment = blake2b-256(blob || digest) — vektor dihitung terpisah
//     dengan Python hashlib.blake2b.
//  2. nilai share = 8 byte di offset 24, little-endian.
//  3. perbandingan ketat: value == target ditolak (matches Zycord
//     meetsJobTarget yang memakai `<`, bukan `<=`).
//  4. commitment dan digest mentah adalah dua nilai berbeda — dasar mengapa
//     memfilter pada digest (perilaku lama) membuat share selalu salah.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "zcd/ZcdCommitment.hpp"

namespace {

int g_fail = 0;

void check(const char* name, bool pass, const std::string& detail = "") {
    if (pass) {
        std::printf("  OK   %s\n", name);
    } else {
        std::printf("  FAIL %s %s\n", name, detail.c_str());
        g_fail++;
    }
}

std::string hex(const uint8_t* p, size_t n) {
    static const char hx[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += hx[p[i] >> 4];
        s += hx[p[i] & 0x0f];
    }
    return s;
}

}  // namespace

int main() {
    std::printf("zcd-commitment\n");

    // --- 1. Vektor blake2b, dihitung dengan Python:
    //     hashlib.blake2b(blob + digest, digest_size=32)
    uint8_t blob[zcd::BLOB_SIZE];
    for (size_t i = 0; i < sizeof(blob); i++) blob[i] = static_cast<uint8_t>(i * 7 + 1);
    uint8_t digest[zcd::DIGEST_SIZE];
    for (size_t i = 0; i < sizeof(digest); i++) digest[i] = static_cast<uint8_t>(i);

    uint8_t c[zcd::COMMITMENT_SIZE];
    zcd::commitment(blob, sizeof(blob), digest, c);
    const std::string want = "19aa8e953c6c99411e12263e48b7a747e215b369f2a19d1a580817355d0a0269";
    const std::string got = hex(c, sizeof(c));
    check("commitment = blake2b256(blob||digest)", got == want, "got " + got);

    // --- 2. Nilai share = limb 24..31 LE.
    {
        uint64_t expect = 0;
        for (size_t i = 0; i < 8; i++) expect |= static_cast<uint64_t>(c[24 + i]) << (8 * i);
        check("shareValue = limb24_31 LE", zcd::shareValue(c) == expect);
    }

    // --- 3. Batas perbandingan: value == target harus ditolak.
    {
        const uint64_t v = zcd::shareValue(c);
        check("value == target ditolak", !zcd::meetsTarget(c, v), "target ==" + std::to_string(v));
        check("value + 1 diterima", zcd::meetsTarget(c, v + 1));
        check("value - 1 ditolak", !zcd::meetsTarget(c, v - 1));
        check("target 0 ditolak", !zcd::meetsTarget(c, 0));
    }

    // --- 4. Commitment != digest. Kalau limb 24..31 keduanya kebetulan sama,
    //     uji ini akan gagal; itu justru bagus karena berarti vektor uji tidak
    //     membedakan dua kuantitas tersebut.
    {
        uint64_t dv = 0;
        for (size_t i = 0; i < 8; i++) dv |= static_cast<uint64_t>(digest[24 + i]) << (8 * i);
        const uint64_t cv = zcd::shareValue(c);
        check("commitment != digest (nilai share berbeda)", dv != cv,
              "digest=" + std::to_string(dv) + " commitment=" + std::to_string(cv));
    }

    // --- 5. Nonce mengubah hasil: blob dengan nonce berbeda menghasilkan
    //     nilai share yang berbeda (membuktikan nonce ikut hashing).
    {
        uint8_t blob2[zcd::BLOB_SIZE];
        std::memcpy(blob2, blob, sizeof(blob2));
        for (size_t k = 0; k < zcd::NONCE_SIZE; k++)
            blob2[zcd::NONCE_OFFSET + k] = static_cast<uint8_t>(0xab >> (8 * k));
        uint8_t c2[zcd::COMMITMENT_SIZE];
        zcd::commitment(blob2, sizeof(blob2), digest, c2);
        check("nonce di offset 39 mengubah commitment",
              std::memcmp(c, c2, sizeof(c)) != 0);
    }

    if (g_fail == 0) std::printf("SEMUA OK\n");
    else std::printf("%d GAGAL\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
