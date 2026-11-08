// Test level Config: parse/loadFile untuk env vs CLI invalid, nilai backend
// kosong, dan fallback tiga file. Dijalankan lewat ctest (xcb-config-parse).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include "Config.hpp"
#include "Hashing/RandomXV2.hpp"

// Config.cpp memanggil rxv2::memNeed() di autoDetect(); test ini tidak
// menautkan RandomX, jadi cukup beri nilai yang membuat ambang masuk akal.
namespace rxv2 {
MemNeed memNeed() { return {2080, 256}; }
}

// Config.cpp memakai randomx_dataset_item_count() untuk ambang RandomY; test
// ini tidak menautkan librandomx, jadi beri nilai dataset minimum.
extern "C" unsigned long randomx_dataset_item_count(void) { return 0; }

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

static std::string makeRoot() {
    char tmpl[] = "/tmp/xcb-config-parse-XXXXXX";
    char* d = mkdtemp(tmpl);
    if (!d) {
        std::perror("mkdtemp");
        std::exit(1);
    }
    return d;
}

static void writeFile(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    if (!f) {
        std::fprintf(stderr, "tidak bisa menulis %s\n", path.c_str());
        std::exit(1);
    }
    f << body;
}

static void clearEnv() {
    const char* vars[] = {"WALLET", "POOL", "WORKER", "THREADS", "FULL_MEM", "LARGE_PAGES",
                          "LOG_LEVEL", "LOG_FORMAT", "REPORT_HASHRATE", "POLL_MS", "BENCHMARK",
                          "ALGO", "CONN_RPC", "CONN_USER", "CONN_PASS", "CONN_COOKIE",
                          "CONN_ADDRESS", "CONN_POOL", "CONN_NONCE_OFFSET"};
    for (const char* v : vars) unsetenv(v);
}

static void enter(const std::string& dir) {
    if (chdir(dir.c_str()) != 0) {
        std::perror("chdir");
        std::exit(1);
    }
}

int main() {
    // 1. CONN_POOL valid dari env tidak boleh dihapus --conn-pool yang invalid.
    {
        const std::string root = makeRoot();
        const std::string cwd = root + "/cwd";
        const std::string home = root + "/home";
        mkdir(cwd.c_str(), 0700);
        mkdir(home.c_str(), 0700);
        enter(cwd);
        clearEnv();
        setenv("HOME", home.c_str(), 1);
        setenv("CONN_POOL", "127.0.0.1:17194", 1);
        char a0[] = "xcb", a1[] = "-a", a2[] = "conn", a3[] = "-u", a4[] = "testaddr";
        char a5[] = "--conn-pool", a6[] = ":17194", a7[] = "--ui=log";
        char* argv[] = {a0, a1, a2, a3, a4, a5, a6, a7, nullptr};
        MinerConfig cfg = Config::parse(8, argv);
        expectStr("env pool host", cfg.connPoolHost, "127.0.0.1");
        expectInt("env pool port", cfg.connPoolPort, 17194);
        expectStr("conn address", cfg.connAddress, "testaddr");
        const int rc = std::system(("rm -rf " + root).c_str());
        (void)rc;
    }

    // 2. server[1]= kosong di file lokal tidak menutup fallback. Kandidat
    // diberikan eksplisit supaya test tidak bergantung pada /etc atau $HOME.
    {
        const std::string root = makeRoot();
        const std::string local = root + "/pool.cfg";
        const std::string missing = root + "/tidak-ada.cfg";
        const std::string fallback = root + "/fallback.cfg";
        writeFile(local, "server[1]=\n");
        writeFile(fallback, "server[1]=127.0.0.1\nport[1]=17194\nwallet=fbwallet\n");
        MinerConfig cfg = Config::loadChain({local, missing, fallback});
        expectInt("fallback pools", static_cast<long>(cfg.pools.size()), 1);
        if (!cfg.pools.empty()) {
            expectStr("fallback host", cfg.pools[0].host, "127.0.0.1");
            expectStr("fallback wallet", cfg.pools[0].wallet, "fbwallet");
        }
        const int rc = std::system(("rm -rf " + root).c_str());
        (void)rc;
    }

    // 2b. Placeholder kosong (threads=, light=, conn_nonce_offset=,
    // conn_cookie=) tidak boleh menimpa nilai fallback.
    {
        const std::string root = makeRoot();
        const std::string local = root + "/pool.cfg";
        const std::string fallback = root + "/fallback.cfg";
        writeFile(local, "threads=\nlight=\nconn_nonce_offset=\nconn_cookie=\n");
        writeFile(fallback,
                  "server[1]=127.0.0.1\nport[1]=17194\nwallet=fbwallet\n"
                  "threads=4\nlight=false\nconn_nonce_offset=7\n"
                  "conn_cookie=/tmp/fb-cookie\n");
        MinerConfig cfg = Config::loadChain({local, root + "/tidak-ada.cfg", fallback});
        expectInt("placeholder threads", cfg.threads, 4);
        expectBool("placeholder fullMem", cfg.fullMem, true);
        expectBool("placeholder fullMemAuto", cfg.fullMemAuto, false);
        expectInt("placeholder nonce", static_cast<long>(cfg.nonceOffset), 7);
        expectStr("placeholder cookie", cfg.connCookie, "/tmp/fb-cookie");
        const int rc = std::system(("rm -rf " + root).c_str());
        (void)rc;
    }

    // 3. Nilai backend kosong bukan backend.
    {
        const std::string root = makeRoot();
        const char* bodies[] = {"server[1]=\n", "algo=\n", "conn_rpc=\n", "conn_address=\n"};
        const char* names[] = {"server kosong", "algo kosong", "conn_rpc kosong",
                               "conn_address kosong"};
        for (int i = 0; i < 4; ++i) {
            const std::string f = root + "/case" + std::to_string(i) + ".cfg";
            writeFile(f, bodies[i]);
            bool opened = false, parsed = false;
            std::set<std::string> keys;
            MinerConfig cfg = Config::loadFile(f, &opened, &parsed, &keys);
            expectBool(names[i], parsed, false);
            expectBool((std::string(names[i]) + " opened").c_str(), opened, true);
            (void)cfg;
        }
        const std::string valid = root + "/valid.cfg";
        writeFile(valid, "server[1]=127.0.0.1\n");
        bool opened = false, parsed = false;
        std::set<std::string> keys;
        MinerConfig cfg = Config::loadFile(valid, &opened, &parsed, &keys);
        expectBool("server valid", parsed, true);
        expectInt("server valid pools", static_cast<long>(cfg.pools.size()), 1);
        const int rc = std::system(("rm -rf " + root).c_str());
        (void)rc;
    }

    if (g_fail) {
        std::fprintf(stderr, "%d kasus gagal\n", g_fail);
        return 1;
    }
    std::printf("Config::parse/loadFile: semua kasus lulus\n");
    return 0;
}
