// Unit test untuk applyFileOverlay(): file lokal (prioritas lebih tinggi)
// hanya menimpa key yang benar-benar ditulisnya. Dijalankan lewat ctest
// (xcb-config-merge).
#include <cstdio>
#include <set>
#include <string>
#include <vector>

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

    // Placeholder kosong (`wallet=`, `worker=`, `conn_cookie=`) tidak boleh
    // menghapus nilai fallback.
    {
        MinerConfig dst = fallbackCfg();
        MinerConfig src;  // semua string kosong, seperti pool.cfg tanpa isi
        src.connCookie = "";
        applyFileOverlay(dst, src, {"wallet", "worker", "conn_cookie"});
        expectStr("wallet kosong", dst.wallet, "fbwallet");
        expectStr("pool wallet kosong", dst.pools[0].wallet, "fbwallet");
        expectStr("worker kosong", dst.connPoolWorker, "fbworker");
        expectStr("pool worker kosong", dst.pools[0].worker, "fbworker");
        expectStr("cookie kosong", dst.connCookie, "/fb/cookie");
    }

    // Worker dari env WORKER / worker= file berlaku untuk pool -o yang masih
    // memakai default "worker" (pool dibuat setelah file dibaca).
    {
        MinerConfig cfg;
        PoolConfig p;
        p.host = "127.0.0.1";
        cfg.pools.push_back(p);
        cfg.connPoolWorker = "envworker";
        applyCliPoolDefaults(cfg, "", "", false);
        expectStr("worker -o", cfg.pools[0].worker, "envworker");
    }

    // -u wallet.worker menang atas worker env; --worker menang atas suffix.
    {
        MinerConfig cfg;
        PoolConfig p;
        p.host = "127.0.0.1";
        cfg.pools.push_back(p);
        cfg.connPoolWorker = "envworker";
        applyCliPoolDefaults(cfg, "wallet", "cliworker", false);
        expectStr("suffix -u", cfg.pools[0].worker, "cliworker");
        expectStr("wallet -u", cfg.pools[0].wallet, "wallet");

        MinerConfig cfg2;
        PoolConfig p2;
        p2.host = "127.0.0.1";
        cfg2.pools.push_back(p2);
        cfg2.connPoolWorker = "flagworker";
        applyCliPoolDefaults(cfg2, "wallet", "cliworker", true);
        expectStr("--worker", cfg2.pools[0].worker, "flagworker");
    }

    // port[N] sendirian bukan backend; server[N]/algo tetap backend.
    {
        expectBool("algo backend", isBackendKey("algo"), true);
        expectBool("server backend", isBackendKey("server"), true);
        expectBool("conn_rpc backend", isBackendKey("conn_rpc"), true);
        expectBool("port bukan backend", isBackendKey("port"), false);
        expectBool("threads bukan backend", isBackendKey("threads"), false);
    }

    // Tiga file: pool.cfg lokal kosong, /etc berisi tuning+wallet, ~/xcb
    // backend. Tuning /etc tidak boleh hilang karena file lokal kosong.
    {
        MinerConfig backend;
        PoolConfig p;
        p.host = "home";
        backend.pools.push_back(p);  // threads=0 (auto), wallet kosong
        MinerConfig etc;
        etc.threads = 1;
        etc.wallet = "etcwallet";
        etc.connPoolWorker = "etcworker";
        std::vector<FilePartial> partials = {
            {MinerConfig{}, {}},  // pool.cfg kosong
            {etc, {"threads", "wallet", "worker"}},
        };
        applyPartials(backend, partials);
        expectInt("3 file threads", backend.threads, 1);
        expectStr("3 file wallet", backend.wallet, "etcwallet");
        expectStr("3 file pool wallet", backend.pools[0].wallet, "etcwallet");
        expectStr("3 file worker", backend.pools[0].worker, "etcworker");
    }

    // File lokal yang menulis key tetap menang atas /etc.
    {
        MinerConfig backend;
        PoolConfig p;
        p.host = "home";
        backend.pools.push_back(p);
        MinerConfig local;
        local.threads = 2;
        MinerConfig etc;
        etc.threads = 8;
        std::vector<FilePartial> partials = {
            {local, {"threads"}},
            {etc, {"threads"}},
        };
        applyPartials(backend, partials);
        expectInt("lokal > etc", backend.threads, 2);
    }

    if (g_fail) {
        std::fprintf(stderr, "%d kasus gagal\n", g_fail);
        return 1;
    }
    std::printf("applyFileOverlay: semua kasus lulus\n");
    return 0;
}
