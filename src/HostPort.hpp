#pragma once

#include <cstdlib>
#include <string>

// "host[:port]", "[v6]:port", atau literal IPv6 tanpa port -> host + port.
// Mengembalikan false hanya kalau port ada tetapi tidak valid (bukan angka
// atau di luar 1..65535); host tetap diisi supaya pemanggil bisa melaporkan
// nilai aslinya. Host tanpa port memakai defaultPort.
inline bool splitHostPort(const std::string& v, std::string& host, int& port, int defaultPort) {
    host = v;
    port = defaultPort;
    if (v.empty()) return true;

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
        if (close + 1 >= v.size()) return true;
        if (v[close + 1] != ':') return false;
        return parsePort(v.substr(close + 2));
    }

    const auto colon = v.find(':');
    if (colon == std::string::npos) return true;
    // Lebih dari satu ':' tanpa bracket = literal IPv6, tidak ada port.
    if (colon != v.rfind(':')) return true;

    host = v.substr(0, colon);
    return parsePort(v.substr(colon + 1));
}
