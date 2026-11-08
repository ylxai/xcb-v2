#pragma once

#include <cstdlib>
#include <string>

// "host[:port]", "[v6]:port", atau literal IPv6 tanpa port -> host + port.
// Mengembalikan false kalau host kosong atau port ada tetapi tidak valid
// (bukan angka atau di luar 1..65535); host tetap diisi supaya pemanggil bisa
// melaporkan nilai aslinya. Host tanpa port memakai defaultPort.
inline bool splitHostPort(const std::string& v, std::string& host, int& port, int defaultPort) {
    host = v;
    port = defaultPort;
    if (v.empty()) return false;  // host kosong: jangan diam-diam solo RPC

    auto parsePort = [&](const std::string& s) {
        if (s.empty()) return true;  // "host:" -> port default
        if (s.find_first_not_of("0123456789") != std::string::npos) return false;
        char* end = nullptr;
        const long p = std::strtol(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0' || p < 1 || p > 65535) return false;
        port = static_cast<int>(p);
        return true;
    };

    if (v.front() == '[') {
        const auto close = v.find(']');
        if (close == std::string::npos) return false;
        host = v.substr(1, close - 1);
        if (host.empty()) return false;  // "[]:17194"
        if (close + 1 >= v.size()) return true;
        if (v[close + 1] != ':') return false;
        return parsePort(v.substr(close + 2));
    }

    const auto colon = v.find(':');
    if (colon == std::string::npos) return true;
    // Lebih dari satu ':' tanpa bracket = literal IPv6, tidak ada port.
    if (colon != v.rfind(':')) return true;

    host = v.substr(0, colon);
    if (host.empty()) return false;  // ":17194"
    return parsePort(v.substr(colon + 1));
}

// Parse v lalu commit ke host/port hanya bila valid. Nilai yang gagal tidak
// boleh menghapus host/port yang sudah di-set (mis. CONN_POOL dari env yang
// ditimpa --conn-pool invalid).
inline bool applyHostPort(const std::string& v, std::string& host, int& port, int defaultPort) {
    std::string h;
    int p = defaultPort;
    if (!splitHostPort(v, h, p, defaultPort)) return false;
    host = h;
    port = p;
    return true;
}
