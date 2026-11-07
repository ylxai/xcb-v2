// Unit test untuk applyFileOverlay(): file lokal (prioritas lebih tinggi)
// hanya menimpa key yang benar-benar ditulisnya. Dijalankan lewat ctest
// (xcb-config-merge).
#include <cstdio>
#include <set>
#include <string>

#include "ConfigMerge.hpp"

static int g_fail = 0;

static void expectStr(const char* what, const std::string& got, const std::string& want) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL %s: got [%s] want [%s]\n", what, got.c_str(), want.c_str());
    ++g_fail;
}

static void expectInt(const char* what, long got, long want) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL %s: got %ld want %ld\n", what, got, want);
    ++g_fail;
}

static void expectBool(const char* what, bool got, bool want) {
    if (got == want) return;
    std::fprintf(stderr, "FAIL %s: got %d want %d\n", what, static_cast<int>(got),
                 static_cast<int>(want));
    ++g_fail;
}

static MinerConfig fallbackCfg() {
    MinerConfig c;
    PoolConfig p;
    p.host = "fb";
    p.wallet = "fbwallet";
    p.worker = "fbworker";
    c.pools.push_back(p);
    c.wallet = "fbwallet";
    c.connPoolWorker = "fbworker";
    c.threads = 4;
    c.useJIT = false;
    c.fullMem = false;
    c.fullMemAuto = false;
    c.hardAES = false;
    c.connCookie = "/fb/cookie";
    c.nonceOffset = 7;
    return c;
}

static MinerConfig localCfg() {
    MinerConfig c;
    c.wallet = "localwallet";
    c.connPoolWorker = "localworker";
    c.threads = 2;
    c.useJIT = true;
    c.fullMem = true;
    c.fullMemAuto = false;
    c.hardAES = true;
    c.connCookie = "/local/cookie";
    c.nonceOffset = 1000;
    return c;
}

int main() {
    // Hanya key yang ditulis file lokal yang menimpa.
    {
        MinerConfig dst = fallbackCfg();
        applyFileOverlay(dst, localCfg(), {"wallet", "worker", "threads"});
        expectStr("wallet", dst.wallet, "localwallet");
        expectStr("pool wallet", dst.pools[0].wallet, "localwallet");
        expectStr("worker", dst.connPoolWorker, "localworker");
        expectStr("pool worker", dst.pools[0].worker, "localworker");
        expectInt("threads", dst.threads, 2);
        // Tidak ada di keys: nilai fallback harus bertahan.
        expectBool("useJIT tetap", dst.useJIT, false);
        expectBool("fullMem tetap", dst.fullMem, false);
        expectBool("hardAES tetap", dst.hardAES, false);
        expectStr("cookie tetap", dst.connCookie, "/fb/cookie");
        expectInt("nonce tetap", dst.nonceOffset, 7);
    }

    // File lokal kosong: tidak ada yang berubah.
    {
        MinerConfig dst = fallbackCfg();
        applyFileOverlay(dst, localCfg(), {});
        expectStr("kosong wallet", dst.wallet, "fbwallet");
        expectInt("kosong threads", dst.threads, 4);
        expectInt("kosong nonce", dst.nonceOffset, 7);
    }

    // light=true menimpa fullMem dan mematikan auto.
    {
        MinerConfig dst = fallbackCfg();
        dst.fullMem = true;
        dst.fullMemAuto = true;
        MinerConfig src;
        src.fullMem = false;
        applyFileOverlay(dst, src, {"light"});
        expectBool("light fullMem", dst.fullMem, false);
        expectBool("light auto", dst.fullMemAuto, false);
    }

    if (g_fail) {
        std::fprintf(stderr, "%d kasus gagal\n", g_fail);
        return 1;
    }
    std::printf("applyFileOverlay: semua kasus lulus\n");
    return 0;
}
