// Uji parser job Block Zero terhadap payload nyata dari pool.
//
// Payload di bawah disalin dari respons login rplant (eu.rplant.xyz:7170,
// algo rx/blockzero). Yang dibuktikan: blob 80 byte diterima, seed_hash 64
// hex, target 8 byte LE, nonce di offset 76, dan algo selain rx/blockzero
// ditolak.

#include <cstdint>
#include <cstdio>
#include <string>

#include "bloz/BlozJob.hpp"

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

// Job nyata dari rplant (bentuk {"result":{"id":...,"job":{...},"status":"OK"}}).
const char* kLoginReply =
    "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"id\":\"sess-1\",\"job\":{"
    "\"blob\":\"0000002071dffbd94c0715ffe9f56079eeb6f3658926c2a48f0824a354c31ec1"
    "8b5e6177d8fe1b9feea005d2a7fb485bfda7641ffc6716fd7fcf6d64b4062038"
    "ebb831fa27e9016b5680071d00000000\","
    "\"job_id\":\"683b\",\"target\":\"6bfa4b47d3530000\",\"algo\":\"rx/blockzero\","
    "\"height\":23185,"
    "\"seed_hash\":\"7b5cdc9c17790fb2e9400cf29a3b373167d29657303269362d5e61e332de1edd\""
    "},\"status\":\"OK\",\"extensions\":[\"algo\",\"keepalive\"]}}";

// Notifikasi job (bentuk {"method":"job","params":{...}}).
const char* kJobNotify =
    "{\"jsonrpc\":\"2.0\",\"method\":\"job\",\"params\":{"
    "\"blob\":\"0000002071dffbd94c0715ffe9f56079eeb6f3658926c2a48f0824a354c31ec1"
    "8b5e6177d8fe1b9feea005d2a7fb485bfda7641ffc6716fd7fcf6d64b4062038"
    "ebb831fa27e9016b5680071d00000000\","
    "\"job_id\":\"683c\",\"target\":\"6bfa4b47d3530000\",\"algo\":\"rx/blockzero\","
    "\"height\":23186,"
    "\"seed_hash\":\"7b5cdc9c17790fb2e9400cf29a3b373167d29657303269362d5e61e332de1edd\"}}";

}  // namespace

int main() {
    std::printf("bloz-job\n");

    // --- Job dari balasan login.
    {
        bloz::Job job;
        std::string err;
        const bool ok = bloz::parseJob(kLoginReply, job, err);
        check("login reply di-parse", ok, err);
        if (ok) {
            check("blob 80 byte", job.blob.size() == bloz::BLOB_SIZE,
                  "dapat " + std::to_string(job.blob.size()));
            check("job_id", job.id == "683b", job.id);
            check("height", job.height == 23185, std::to_string(job.height));
            check("algo", job.algo == "rx/blockzero", job.algo);
            // target "6bfa4b47d3530000" little-endian = 0x000053d3474bfa6b
            check("target LE", job.target == 0x000053d3474bfa6bULL,
                  std::to_string(job.target));
            check("seed 32 byte pertama",
                  job.key[0] == 0x7b && job.key[1] == 0x5c && job.key[31] == 0xdd);
            // difficulty = 2^64 / target ≈ 200145 (angka yang dicetak pool).
            const double d = job.difficultyValue();
            check("difficulty ~200145", d > 199000.0 && d < 201000.0, std::to_string(d));
            check("job valid", job.valid());
        }
    }

    // --- Notifikasi job.
    {
        bloz::Job job;
        std::string err;
        const bool ok = bloz::parseJob(kJobNotify, job, err);
        check("notifikasi job di-parse", ok, err);
        if (ok) check("job_id notifikasi", job.id == "683c", job.id);
    }

    // --- Nonce di offset 76, little-endian.
    {
        bloz::Job job;
        std::string err;
        bloz::parseJob(kLoginReply, job, err);
        job.setNonce(0, 0x00000355u);
        check("nonce offset 76", job.blob[76] == 0x55 && job.blob[77] == 0x03 &&
                                     job.blob[78] == 0x00 && job.blob[79] == 0x00);
        check("nonceAt", job.nonceAt(0) == 0x00000355u, std::to_string(job.nonceAt(0)));
        check("nonceHex", job.nonceHex(0) == "55030000", job.nonceHex(0));
        // Byte 75 tidak boleh tersentuh: itu byte terakhir field bits (0x1d di
        // header ini), bukan bagian nonce.
        check("byte 75 utuh", job.blob[75] == 0x1d);
    }

    // --- Penolakan: algo lain, blob salah panjang, target nol.
    {
        bloz::Job job;
        std::string err;
        std::string s = kLoginReply;
        const std::string from = "\"algo\":\"rx/blockzero\"";
        s.replace(s.find(from), from.size(), "\"algo\":\"rx/2\"");
        check("algo rx/2 ditolak", !bloz::parseJob(s, job, err), err);
    }
    {
        bloz::Job job;
        std::string err;
        std::string s = kLoginReply;
        const std::string from = "ebb831fa27e9016b5680071d00000000";
        s.replace(s.find(from), from.size(), "ebb831fa27e9016b5680071d00");
        check("blob 78 byte ditolak", !bloz::parseJob(s, job, err), err);
    }
    {
        bloz::Job job;
        std::string err;
        std::string s = kLoginReply;
        const std::string from = "\"target\":\"6bfa4b47d3530000\"";
        s.replace(s.find(from), from.size(), "\"target\":\"0000000000000000\"");
        check("target nol ditolak", !bloz::parseJob(s, job, err), err);
    }
    // height 2^64: double membulatkan UINT64_MAX ke 2^64, jadi batas `>` lama
    // meloloskannya dan static_cast<uint64_t> menjadi tidak terdefinisi.
    {
        bloz::Job job;
        std::string err;
        std::string s = kLoginReply;
        const std::string from = "\"height\":23185";
        s.replace(s.find(from), from.size(), "\"height\":18446744073709551616");
        check("height 2^64 ditolak", !bloz::parseJob(s, job, err), err);
    }

    std::printf(g_fail == 0 ? "SEMUA OK\n" : "%d GAGAL\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
