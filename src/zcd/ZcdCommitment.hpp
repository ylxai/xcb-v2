#pragma once

#include <cstddef>
#include <cstdint>

// Aturan share Zycord (RandomX v2, algo "rx/2").
//
// Kuantitas yang dibandingkan dengan target BUKAN digest RandomX, melainkan
// commitment: blake2b-256( PoWInput || PoWHash ). PoWInput adalah blob 43 byte
// yang dikirim pool (seed 32 byte, 7 byte nol, nonce 4 byte LE di offset 39),
// PoWHash adalah digest RandomX atas blob itu.
//
// Dokumen acuan: zycord-node core/pow/pow.go (Commitment/CheckCommitment) dan
// node/stratum/seam.go (jobTargetValue/meetsJobTarget). Komentar di pow.go
// menyatakan aturan ini bukan pilihan: XMRig 6.26+ menghitung commitment per
// nonce dan memfilter share pada limb 24..31 dari commitment, little-endian.
//
// Perbandingannya "strictly below" (meetsJobTarget memakai <), bukan <=.
namespace zcd {

constexpr size_t BLOB_SIZE = 43;
constexpr size_t DIGEST_SIZE = 32;
constexpr size_t COMMITMENT_SIZE = 32;
constexpr size_t NONCE_OFFSET = 39;  // PoWInputNonceOffset
constexpr size_t NONCE_SIZE = 4;

// Commitment = blake2b-256(blob || digest). `blobLen` boleh lebih pendek dari
// BLOB_SIZE bila input diuji bukan blob job (mis. test vector).
void commitment(const uint8_t* blob, size_t blobLen, const uint8_t digest[DIGEST_SIZE],
                uint8_t out[COMMITMENT_SIZE]);

// Nilai yang dibandingkan dengan target: 8 byte di offset 24 dari commitment,
// dibaca sebagai uint64 little-endian (sama persis dengan
// `*reinterpret_cast<uint64_t*>(m_hash + 24)` di XMRig).
uint64_t shareValue(const uint8_t commitment[COMMITMENT_SIZE]);

// Terima share bila shareValue < target. Target 0 berarti "mustahil menang",
// jadi ditolak tanpa menghitung.
bool meetsTarget(const uint8_t commitment[COMMITMENT_SIZE], uint64_t target);

// Target yang ditampilkan XMRig: 2^64 / target (0 bila target 0).
double difficulty(uint64_t target);

}  // namespace zcd
