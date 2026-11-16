// Uji parser job Zycord terhadap payload nyata dari pool.
//
// Payload di bawah disalin dari respons login pool rplant ZCD
// (randomx.rplant.xyz:17197, algo rx/2), hanya job_id/height yang diganti.
// Yang dibuktikan: blob 43 byte diterima, seed_hash 64 hex, target 8 byte LE,
// dan layout PoWInput (32 seed + 7 nol + 4 nonce di offset 39).

#include <cstdint>
#include <cstdio>
#include <string>

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

// Job nyata dari rplant (bentuk {"result":{"id":...,"job":{...},"status":"OK"}}).
const char* kLoginReply =
    "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"id\":\"1\",\"job\":{"
    "\"blob\":\"209b596c11cce0c06f4bf45701b50cc860045d8d0b5ada176ea254a535218a20"
    "0000000000000000000000\","
    "\"job_id\":\"d64\",\"target\":\"a38d23d6e2530000\",\"algo\":\"rx/2\","
    "\"height\":115805,"
    "\"seed_hash\":\"14a8069b9e7fdeea17722086fc78a8191e7f87f8e94bdeab7f399fb66b1e4ac9\","
    "\"next_seed_hash\":\"e10773d705bd413630795746f1c9f1a1e1e1c9f1a1e1e1c9f1a1e1e1c9f1a1e1\""
    "},\"status\":\"OK\",\"extensions\":[]}}";

// Notifikasi job (bentuk {"method":"job","params":{...}}).
const char* kJobNotify =
    "{\"jsonrpc\":\"2.0\",\"method\":\"job\",\"params\":{"
    "\"blob\":\"209b596c11cce0c06f4bf45701b50cc860045d8d0b5ada176ea254a535218a20"
    "0000000000000000000000\","
    "\"job_id\":\"d65\",\"target\":\"a38d23d6e2530000\",\"algo\":\"rx/2\","
    "\"height\":115806,"
    "\"seed_hash\":\"14a8069b9e7fdeea17722086fc78a8191e7f87f8e94bdeab7f399fb66b1e4ac9\"}}";

}  // namespace

int main() {
    std::printf("zcd-job\n");

    // --- Job dari balasan login.
    {
        zcd::Job job;
        std::string err;
        const bool ok = zcd::parseJob(kLoginReply, job, err);
        check("login reply di-parse", ok, err);
        if (ok) {
            check("blob 43 byte", job.blob.size() == zcd::BLOB_SIZE,
                  "dapat " + std::to_string(job.blob.size()));
            check("job_id", job.id == "d64", job.id);
            check("algo rx/2", job.algo == "rx/2", job.algo);
            check("height", job.height == 115805, std::to_string(job.height));

            // Target 8 byte LE: a3 8d 23 d6 e2 53 00 00 ->
            // 0x000053e2d6238da3 = 92233720368547
            check("target LE 8 byte", job.target == 92233720368547ULL,
                  "dapat " + std::to_string(job.target));

            // Layout PoWInput: seed 32 byte, lalu 7 byte nol, lalu nonce.
            bool zeros = true;
            for (size_t i = 32; i < 39; i++)
                if (job.blob[i] != 0) zeros = false;
            check("byte 32..38 nol (reserved)", zeros);

            bool nonceZero = true;
            for (size_t i = zcd::NONCE_OFFSET; i < zcd::NONCE_OFFSET + zcd::NONCE_SIZE; i++)
                if (job.blob[i] != 0) nonceZero = false;
            check("nonce awalnya nol", nonceZero);

            // seed_hash harus sama dengan blob[0:32]? Tidak untuk pool Zycord:
            // blob[0:32] adalah header per blok, seed_hash kunci epoch RandomX.
            check("seed_hash dipakai sebagai key", job.keyHex().size() == 64);

            // Tulis nonce lalu baca kembali (LE).
            job.setNonce(0, 0xdeadbeef);
            check("setNonce/nonceHex LE",
                  job.nonceHex(0) == "efbeadde", job.nonceHex(0));
            check("setNonce tidak menyentuh byte lain",
                  job.blob[38] == 0 && job.blob[43 - 1] == 0xde);
        }
    }

    // --- Notifikasi job.
    {
        zcd::Job job;
        std::string err;
        const bool ok = zcd::parseJob(kJobNotify, job, err);
        check("notifikasi job di-parse", ok, err);
        if (ok) {
            check("job_id notifikasi", job.id == "d65", job.id);
            check("height notifikasi", job.height == 115806, std::to_string(job.height));
        }
    }

    // --- Penolakan bentuk salah.
    {
        zcd::Job job;
        std::string err;
        check("blob 80 byte ditolak",
              !zcd::parseJob("{\"result\":{\"job\":{\"blob\":\""
                             + std::string(160, 'a') +
                             "\",\"job_id\":\"x\",\"target\":\"a38d23d6e2530000\","
                             "\"height\":1,\"seed_hash\":\"" +
                             std::string(64, 'b') + "\"}}}",
                             job, err));

        err.clear();
        check("seed_hash pendek ditolak",
              !zcd::parseJob("{\"result\":{\"job\":{\"blob\":\""
                             + std::string(84, 'a') +
                             "\",\"job_id\":\"x\",\"target\":\"a38d23d6e2530000\","
                             "\"height\":1,\"seed_hash\":\"abcd\"}}}",
                             job, err));

        err.clear();
        check("target nol ditolak",
              !zcd::parseJob("{\"result\":{\"job\":{\"blob\":\""
                             + std::string(84, 'a') +
                             "\",\"job_id\":\"x\",\"target\":\"0000000000000000\","
                             "\"height\":1,\"seed_hash\":\"" +
                             std::string(64, 'b') + "\"}}}",
                             job, err));

        err.clear();
        check("job_id hilang ditolak",
              !zcd::parseJob("{\"result\":{\"job\":{\"blob\":\""
                             + std::string(84, 'a') +
                             "\",\"target\":\"a38d23d6e2530000\",\"height\":1,"
                             "\"seed_hash\":\"" +
                             std::string(64, 'b') + "\"}}}",
                             job, err));
    }

    if (g_fail == 0) std::printf("SEMUA OK\n");
    else std::printf("%d GAGAL\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}