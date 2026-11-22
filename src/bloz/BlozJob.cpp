#include "BlozJob.hpp"

#include <cmath>
#include <cstring>
#include <limits>

// Parser JSON sendiri (BlozJson) — memakai ConnRpc.hpp akan menyeret backend
// CONN ke backend bloz dan menghapus kemandiriannya.
#include "BlozJson.hpp"

namespace bloz {

namespace {

std::string toHex(const uint8_t* p, size_t n) {
    static const char hx[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += hx[p[i] >> 4];
        s += hx[p[i] & 0x0f];
    }
    return s;
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out) {
    if (hex.size() % 2 != 0) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexNibble(hex[i]);
        const int lo = hexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

}  // namespace

double difficulty(uint64_t target) {
    if (target == 0) return 0.0;
    // 2^64 tepat: dihitung dari long double supaya tidak overflow uint64_t.
    return static_cast<double>(18446744073709551616.0L / static_cast<long double>(target));
}

uint64_t digestValue(const uint8_t d[32]) {
    uint64_t v = 0;
    for (size_t k = 0; k < 8; k++) v |= static_cast<uint64_t>(d[24 + k]) << (8 * k);
    return v;
}

bool meetsTarget(const uint8_t d[32], uint64_t target) { return digestValue(d) < target; }

uint32_t Job::nonceAt(size_t off) const {
    // Nonce hidup di dalam blob (offset 76..79), little-endian.
    uint32_t v = 0;
    for (size_t k = 0; k < NONCE_SIZE; k++)
        v |= static_cast<uint32_t>(blob[NONCE_OFFSET + k]) << (8 * k);
    (void)off;
    return v;
}

void Job::setNonce(size_t off, uint32_t nonce) {
    (void)off;
    for (size_t k = 0; k < NONCE_SIZE; k++)
        blob[NONCE_OFFSET + k] = static_cast<uint8_t>(nonce >> (8 * k));
}

std::string Job::keyHex() const { return toHex(key, sizeof(key)); }

std::string Job::nonceHex(size_t off) const {
    (void)off;
    return toHex(blob.data() + NONCE_OFFSET, NONCE_SIZE);
}

bool parseJob(const std::string& line, Job& out, std::string& err) {
    const Json doc = bloz::parseJson(line, &err);
    if (!doc.isObj()) {
        if (err.empty()) err = "respons bukan objek JSON";
        return false;
    }

    // Terima dua bentuk: {"result":{"job":{...}}} (login) dan
    // {"method":"job","params":{...}} (notifikasi). Anggota job juga boleh
    // datang langsung sebagai objek.
    const Json* j = nullptr;
    if (const Json* r = doc.get("result")) {
        if (r->isObj()) {
            if (const Json* inner = r->get("job")) {
                if (inner->isObj()) j = inner;
            } else {
                j = r;
            }
        }
    }
    if (!j) {
        if (const Json* p = doc.get("params")) {
            if (p->isObj()) j = p;
        }
    }
    if (!j) {
        err = "tidak ada objek job";
        return false;
    }

    std::string id, blobHex, keyHex;
    double height = 0;
    if (!j->getStr("job_id", id)) {
        err = "job: job_id hilang";
        return false;
    }
    if (!j->getStr("blob", blobHex)) {
        err = "job: blob hilang";
        return false;
    }
    if (!j->getStr("seed_hash", keyHex)) {
        err = "job: seed_hash hilang";
        return false;
    }
    if (!j->getNum("height", height)) {
        err = "job: height hilang";
        return false;
    }

    std::vector<uint8_t> blob;
    if (!hexToBytes(blobHex, blob)) {
        err = "job: blob bukan hex";
        return false;
    }
    if (blob.size() != BLOB_SIZE) {
        err = "job: blob harus " + std::to_string(BLOB_SIZE) + " byte, dapat " +
              std::to_string(blob.size());
        return false;
    }
    if (keyHex.size() != 64) {
        err = "job: seed_hash harus 64 hex";
        return false;
    }
    std::vector<uint8_t> k;
    if (!hexToBytes(keyHex, k) || k.size() != 32) {
        err = "job: seed_hash bukan hex 32 byte";
        return false;
    }

    // Target: 8 byte LE. rplant mengirim 16 hex (= 8 byte); bentuk yang lebih
    // pendek dari 8 byte diterima sebagai big-endian yang perlu left-pad
    // (kebiasaan pool lain), bukan dipotong.
    std::string targetHex;
    if (!j->getStr("target", targetHex)) {
        err = "job: target hilang";
        return false;
    }
    std::vector<uint8_t> tgt;
    if (!hexToBytes(targetHex, tgt) || tgt.empty() || tgt.size() > 8) {
        err = "job: target harus 1..8 byte hex";
        return false;
    }
    uint64_t target = 0;
    if (tgt.size() == 8) {
        for (size_t i = 0; i < 8; i++)
            target |= static_cast<uint64_t>(tgt[i]) << (8 * i);
    } else {
        // Bentuk pendek: perlakukan sebagai big-endian tanpa leading zero.
        for (uint8_t b : tgt) target = (target << 8) | b;
    }
    if (target == 0) {
        err = "job: target nol";
        return false;
    }

    std::string algo;
    if (j->getStr("algo", algo) && !algo.empty()) {
        // Backend ini hanya mengimplementasikan rx/blockzero. Menerima algo
        // lain berarti menghitung RandomX untuk pekerjaan yang berbeda, dan
        // hasilnya ditolak pool tanpa pesan yang berguna.
        if (algo != "rx/blockzero") {
            err = "job: algo tidak didukung: " + algo;
            return false;
        }
        out.algo = algo;
    }

    // height datang dari JSON pool sebagai double. Nilai negatif, NaN, inf,
    // atau di luar rentang uint64_t membuat static_cast tidak terdefinisi.
    // Batas atas harus 2^64, bukan static_cast<double>(UINT64_MAX): double
    // membulatkan UINT64_MAX ke 2^64, sehingga perbandingan `>` meloloskan
    // height == 2^64 dan cast-nya tetap tidak terdefinisi.
    constexpr double kTwoPow64 = 18446744073709551616.0;
    if (!std::isfinite(height) || height < 0 || height >= kTwoPow64) {
        err = "job: height di luar rentang";
        return false;
    }

    out.id = id;
    out.height = static_cast<uint64_t>(height);
    out.blob = std::move(blob);
    std::memcpy(out.key, k.data(), sizeof(out.key));
    out.target = target;
    return true;
}

}  // namespace bloz
