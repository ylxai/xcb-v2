// Uji regresi untuk temuan review PR #29 yang bisa diuji tanpa jaringan.
//
//   1. splitHostPort: bracket IPv6, port berisi non-digit, host:port normal
//   2. parseJob: algo selain rx/2 ditolak
//   3. parseJob: height negatif / inf ditolak
//   4. parseJson: eksponen kosong ('1e') ditolak
//
// splitHostPort disalin dari parser Config (--conn-pool / POOL) supaya
// perilakunya terkunci di sini; backend zcd sekarang membaca hasil Config.

#include <cmath>
#include <cstdio>
#include <string>

#include "zcd/ZcdJson.hpp"
#include "zcd/ZcdJob.hpp"

namespace {

int g_fail = 0;

void check(const char* name, bool pass, const std::string& detail = "") {
    if (pass) {
        std::printf("  OK   %s\n", name);
    } else {
        std::printf("  FAIL %s %s\n", name, detail.c_str());
        g_fail++;
    }
}

// --- salinan splitHostPort dari parser Config (dijaga tetap sama) ----------
bool parsePort(const std::string& s, uint16_t& out) {
    if (s.empty() || s.size() > 5) return false;
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    if (v == 0 || v > 65535) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

bool splitHostPort(const std::string& in, std::string& host, uint16_t& port) {
    if (in.empty()) return false;
    port = 17197;
    if (!in.empty() && in.front() == '[') {
        const size_t close = in.find(']');
        if (close == std::string::npos || close == 1) return false;
        const std::string inner = in.substr(1, close - 1);
        const std::string rest = in.substr(close + 1);
        if (rest.empty()) {
            host = inner;
            return true;
        }
        if (rest[0] != ':') return false;
        host = inner;
        return parsePort(rest.substr(1), port);
    }
    const size_t first = in.find(':');
    const size_t last = in.rfind(':');
    if (first == std::string::npos) {
        host = in;
        return true;
    }
    if (first != last) {
        host = in;
        return true;
    }
    host = in.substr(0, first);
    if (host.empty()) return false;
    return parsePort(in.substr(first + 1), port);
}
// ---------------------------------------------------------------------------

bool jobWith(const std::string& extra, zcd::Job& job, std::string& err) {
    const std::string blob(86, 'a');
    const std::string seed(64, 'b');
    const std::string json = "{\"result\":{\"job\":{\"blob\":\"" + blob +
                             "\",\"job_id\":\"x\",\"target\":\"a38d23d6e2530000\"," +
                             "\"height\":1,\"seed_hash\":\"" + seed + "\"" + extra + "}}}";
    return zcd::parseJob(json, job, err);
}

}  // namespace

int main() {
    std::printf("zcd-review\n");

    // --- 1. splitHostPort
    {
        std::string h;
        uint16_t p = 0;
        check("host tanpa port", splitHostPort("pool.example", h, p) && h == "pool.example" &&
                                     p == 17197, h + ":" + std::to_string(p));

        p = 0;
        check("host:port", splitHostPort("pool.example:9425", h, p) && h == "pool.example" &&
                               p == 9425, h + ":" + std::to_string(p));

        // Bug lama: bracket tertinggal saat port tidak diberikan.
        p = 0;
        check("bracket IPv6 tanpa port", splitHostPort("[::1]", h, p) && h == "::1" &&
                                             p == 17197, h + ":" + std::to_string(p));

        p = 0;
        check("bracket IPv6 dengan port", splitHostPort("[2001:db8::1]:3425", h, p) &&
                                              h == "2001:db8::1" && p == 3425,
              h + ":" + std::to_string(p));

        // Bug lama: strtol menerima prefix numerik, "123junk" jadi 123.
        p = 0;
        check("port berisi non-digit ditolak", !splitHostPort("host:123junk", h, p));

        p = 0;
        check("port 0 ditolak", !splitHostPort("host:0", h, p));
        p = 0;
        check("port > 65535 ditolak", !splitHostPort("host:65536", h, p));
        p = 0;
        check("host kosong ditolak", !splitHostPort(":9425", h, p));
        p = 0;
        check("IPv6 tanpa bracket tidak dipotong", splitHostPort("2001:db8::1", h, p) &&
                                                       h == "2001:db8::1" && p == 17197,
              h + ":" + std::to_string(p));
    }

    // --- 2. algo selain rx/2
    {
        zcd::Job job;
        std::string err;
        check("algo rx/2 diterima", jobWith(",\"algo\":\"rx/2\"", job, err), err);
        err.clear();
        check("algo rx/0 ditolak", !jobWith(",\"algo\":\"rx/0\"", job, err), err);
        err.clear();
        check("algo selain rx/2 memberi pesan",
              !jobWith(",\"algo\":\"cn/2\"", job, err) && err.find("algo") != std::string::npos,
              err);
    }

    // --- 3. height tidak valid (mengganti field height yang sudah ada)
    {
        const std::string blob(86, 'a');
        const std::string seed(64, 'b');
        auto make = [&](const std::string& height) {
            return "{\"result\":{\"job\":{\"blob\":\"" + blob +
                   "\",\"job_id\":\"x\",\"target\":\"a38d23d6e2530000\",\"height\":" + height +
                   ",\"seed_hash\":\"" + seed + "\"}}}";
        };
        zcd::Job job;
        std::string err;
        check("height negatif ditolak", !zcd::parseJob(make("-5"), job, err), err);
        err.clear();
        check("height 0 diterima", zcd::parseJob(make("0"), job, err), err);
        err.clear();
        check("height pecahan kecil diterima", zcd::parseJob(make("1.9"), job, err) &&
                                                   job.height == 1,
              std::to_string(job.height));
    }

    // --- 4. eksponen kosong
    {
        std::string err;
        const zcd::Json j = zcd::parseJson("{\"a\":1e}", &err);
        check("eksponen kosong '1e' ditolak", !j.isObj(), err);
    }

    if (g_fail == 0) std::printf("SEMUA OK\n");
    else std::printf("%d GAGAL\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
