// Unit test untuk normalizeTargetHex(). Dijalankan lewat ctest (xcb-target-norm).
#include <cstdio>
#include <string>

#include "TargetNorm.hpp"

static int g_fail = 0;

static void expect(const char* what, const std::string& in, bool wantOk,
                   const std::string& wantOut) {
    std::string out;
    const bool ok = normalizeTargetHex(in, out);
    if (ok == wantOk && out == wantOut) return;
    std::fprintf(stderr, "FAIL %s: '%s' -> ok=%d out=[%s] (want ok=%d [%s])\n", what, in.c_str(),
                 ok, out.c_str(), wantOk, wantOut.c_str());
    ++g_fail;
}

int main() {
    // Target nyata dari CatchThatRabbit, 2026-11-10: 30 byte / 60 hex char
    // karena nol di depan dipotong.
    const std::string sample60 =
        "0602a1b6ff38d0c8ab5dc918c5b0cfe3870868566f98ed5e2f7597e57aa3";
    const std::string sample64 = "0000" + sample60;

    expect("60 char dipad", sample60, true, sample64);
    expect("64 char sudah benar", sample64, true, sample64);

    // Prefix 0x (pool bisa mengirimnya; doEthGetWork juga sudah strip).
    expect("prefix 0x", "0x" + sample60, true, sample64);
    expect("prefix 0X", "0X" + sample60, true, sample64);

    // Lebih dari 64 char: nol tambahan di depan tetap diterima.
    expect("68 char nol depan", "00000000" + sample60, true, sample64);
    // Digit signifikan lebih dari 64 = >256-bit: ditolak.
    expect("65 char signifikan", "1" + std::string(64, 'f'), false, "");

    // Target nol tidak bisa dipakai (tidak ada hash yang lolos).
    expect("nol semua 60", std::string(60, '0'), false, "");
    expect("nol semua 64", std::string(64, '0'), false, "");

    // Non-hex: dulu "zz" dipad jadi target nol dan lolos tanpa peringatan.
    expect("non-hex pendek", "zz", false, "");
    expect("non-hex di tengah", "0000" + std::string(56, 'a') + "zz", false, "");
    expect("string kosong", "", false, "");

    // 8 byte teratas nol (< 2^192) -> targetInt 0, tidak akan pernah submit.
    expect("terlalu kecil 48 digit", std::string(48, 'f'), false, "");
    expect("batas 49 digit", std::string(49, 'f'), true, std::string(15, '0') + std::string(49, 'f'));

    // Uppercase diterima dan dipertahankan apa adanya.
    const std::string upper60 = "0602A1B6FF38D0C8AB5DC918C5B0CFE3870868566F98ED5E2F7597E57AA3";
    expect("uppercase", upper60, true, "0000" + upper60);

    // 2^255 (satu bit teratas): 64 char signifikan, harus diterima.
    expect("2^255", "8" + std::string(63, '0'), true, "8" + std::string(63, '0'));

    if (g_fail) {
        std::fprintf(stderr, "%d test gagal\n", g_fail);
        return 1;
    }
    std::printf("target-norm: semua test lulus\n");
    return 0;
}
