#pragma once

#include <string>

// Normalisasi target share dari pool ke 64 hex char (256-bit big-endian).
//
// CatchThatRabbit mengirim target 30 byte sebagai 60 hex char karena nol di
// depan dipotong. Tanpa padding kiri, 16 char pertama bukan 8 byte teratas
// target 256-bit: nilainya 2^16 lebih besar, jadi share 65.536x lebih mudah
// dan semuanya dibuang pool. Miner resmi mem-pad ke 64 char dengan
// "coinmine.pl fix".
//
// Aturan:
//   - prefix "0x"/"0X" opsional dibuang;
//   - nol di depan dibuang sebelum mengukur lebar, jadi target 64+ char yang
//     hanya menambah nol tetap diterima;
//   - target nol, bukan hex, atau >256-bit (lebih dari 64 digit signifikan)
//     ditolak;
//   - target dengan 8 byte teratas nol (< 2^192) juga ditolak: targetInt-nya
//     akan 0 dan miner tidak akan pernah mengirim share.
// Hasil selalu 64 hex char dengan 8 byte teratas bukan nol.
inline bool normalizeTargetHex(const std::string& in, std::string& out) {
    out.clear();

    std::string s = in;
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.erase(0, 2);
    if (s.empty()) return false;

    for (char c : s) {
        const bool hex =
            (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }

    const auto first = s.find_first_not_of('0');
    if (first == std::string::npos) return false;  // nol semua
    s.erase(0, first);
    if (s.size() > 64) return false;  // >256-bit: bukan target yang masuk akal
    if (s.size() <= 48) return false;  // 8 byte teratas nol

    out.assign(64 - s.size(), '0');
    out += s;
    return true;
}
