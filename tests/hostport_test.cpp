// Unit test untuk splitHostPort(). Dijalankan lewat ctest (xcb-hostport).
#include <cstdio>
#include <string>

#include "HostPort.hpp"

static int g_fail = 0;

static void expect(const char* what, const std::string& in, const std::string& wantHost,
                   int wantPort, bool wantOk) {
    std::string host;
    int port = 17194;
    const bool ok = splitHostPort(in, host, port, 17194);
    if (ok == wantOk && host == wantHost && port == wantPort) return;
    std::fprintf(stderr, "FAIL %s: '%s' -> host=[%s] port=%d ok=%d (want [%s] %d %d)\n", what,
                 in.c_str(), host.c_str(), port, ok, wantHost.c_str(), wantPort, wantOk);
    ++g_fail;
}

int main() {
    expect("host:port", "eu.rplant.xyz:17194", "eu.rplant.xyz", 17194, true);
    expect("host saja", "eu.rplant.xyz", "eu.rplant.xyz", 17194, true);
    expect("port lain", "pool.local:3333", "pool.local", 3333, true);
    expect("host kosong", ":17194", "", 17194, false);
    expect("string kosong", "", "", 17194, false);
    expect("bracket kosong", "[]:17194", "", 17194, false);
    expect("bracket kosong saja", "[]", "", 17194, false);

    // IPv6
    expect("v6 tanpa bracket", "::1", "::1", 17194, true);
    expect("v6 penuh", "2001:db8::1234", "2001:db8::1234", 17194, true);
    expect("v6 bracket+port", "[::1]:17194", "::1", 17194, true);
    expect("v6 bracket", "[2001:db8::1]", "2001:db8::1", 17194, true);
    expect("v6 bracket tanpa tutup", "[::1", "[::1", 17194, false);
    expect("v6 bracket+junk", "[::1]x", "::1", 17194, false);

    // Port tidak valid: host tetap diisi, port jatuh ke default.
    expect("port bukan angka", "host:abc", "host", 17194, false);
    expect("port nol", "host:0", "host", 17194, false);
    expect("port terlalu besar", "host:70000", "host", 17194, false);
    expect("port negatif", "host:-1", "host", 17194, false);
    expect("port kosong", "host:", "host", 17194, true);

    // applyHostPort: nilai invalid tidak menghapus host/port yang sudah ada.
    {
        std::string host = "envhost";
        int port = 17194;
        if (applyHostPort(":17194", host, port, 17194) || host != "envhost" || port != 17194) {
            std::fprintf(stderr, "FAIL apply invalid: host=[%s] port=%d\n", host.c_str(), port);
            ++g_fail;
        }
        if (!applyHostPort("newhost:3333", host, port, 17194) || host != "newhost" ||
            port != 3333) {
            std::fprintf(stderr, "FAIL apply valid: host=[%s] port=%d\n", host.c_str(), port);
            ++g_fail;
        }
    }

    if (g_fail) {
        std::fprintf(stderr, "%d kasus gagal\n", g_fail);
        return 1;
    }
    std::printf("splitHostPort: semua kasus lulus\n");
    return 0;
}
