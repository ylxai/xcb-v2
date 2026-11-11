#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Klien stratum rplant untuk algo RandomX v2 ("rx/2b" / randomx2b).
//
// Protokolnya bukan stratum Bitcoin-style dan bukan JSON-RPC node: rplant
// memakai varian xmrig/CryptoNote:
//   -> {"method":"login","params":{"login":WALLET,"pass":"x","algo":["rx/2b",...]}}
//   <- {"result":{"id":"1","job":{job_id, algo, blob, height, seed_hash, target}}}
//   <- {"method":"job","params":{...}}                 (job baru)
//   -> {"method":"submit","params":{"id":"1","job_id":..,"nonce":..,"result":..}}
//
// blob = 76 byte header (tanpa nonce); miner menambahkan nonce 4 byte LE
// menjadi header 80 byte, lalu RandomX v2 dengan key = seed_hash (32 byte).
// target = 8 byte little-endian: share valid bila uint64_le(hash) < target.
struct ConnPoolJob {
    std::string id;
    std::string algo;
    std::vector<uint8_t> blob;  // 76 byte
    std::string seedHex;        // 32 byte (hex) = RandomX key
    uint32_t height = 0;
    uint64_t target = 0;        // LE 64-bit
};

class ConnPool {
public:
    enum class Event { None, Job, Accepted, Rejected, Error };

    ConnPool(std::string host, int port, std::string wallet, std::string worker, bool tls,
             std::string algo = "rx/2b");
    ~ConnPool();

    // DNS + TCP + TLS + login. Blocking (timeout socket 30 s).
    bool connectAndLogin(ConnPoolJob& first, std::string& err);

    // Baca satu event. timeoutMs <= 0 = non-blocking.
    Event poll(ConnPoolJob& job, int timeoutMs, std::string& detail);

    bool submit(const ConnPoolJob& job, uint32_t nonce, const uint8_t hash[32],
                std::string& err);

    // Dipanggil ConnMiner setelah work pertama sesi ini dibangun — dari
    // start() untuk sesi awal dan dari poolLoop setelah reconnect. Sebelum ini
    // submit() menolak menulis: job dari sesi lama tidak berlaku di sesi baru,
    // dan buildEngine untuk seed baru bisa memakan beberapa detik.
    void markReady();

    // Job terakhir yang dikirim pool. Worker bisa masih memakai Work lama
    // (pemeriksaan generasi hanya tiap 256 hash), dan buildEngine untuk seed
    // baru berjalan sebelum snap.work berpindah — jadi submit() membandingkan
    // ke sini, bukan ke snap.work.
    bool isCurrentJob(const std::string& id) const;

    // Hidup berarti sesi siap: login selesai dan work pertama sudah dibangun.
    // Koneksi plaintext tidak punya SSL*, jadi fd yang terbuka saja tidak
    // cukup; dan saat reconnect fd bisa sudah terbuka sebelum TLS siap. Flag
    // yang sama dipakai submit(), sehingga keduanya tidak bisa berbeda
    // pendapat soal kesiapan koneksi.
    bool alive() const { return m_ready.load(); }
    void close();
    const std::string& wallet() const { return m_wallet; }

private:
    bool sendLine(const std::string& line, std::string& err);
    bool sendLineLocked(const std::string& line, std::string& err);
    bool recvLine(std::string& line, int timeoutMs, std::string& err);
    void closeLocked();

    std::string m_host;
    int m_port;
    std::string m_wallet;
    std::string m_worker;
    bool m_tls;
    std::string m_algo = "rx/2b";  // stratum algo (rx/2b=CONN, rx/2=ZCD)

    // SSL* tidak boleh dipakai dua thread sekaligus. poolLoop memanggil
    // SSL_read, worker memanggil SSL_write, dan poolLoop bisa SSL_free saat
    // reconnect — semuanya lewat mutex ini. Poll menunggu data tetap di luar
    // lock supaya submit tidak ikut tertahan sampai timeout.
    mutable std::mutex m_ioMu;

    void* m_ctx = nullptr;  // SSL_CTX*
    void* m_ssl = nullptr;  // SSL*
    int m_fd = -1;
    std::string m_buf;
    std::string m_loginId;
    std::string m_jobId;
    uint64_t m_msgId = 1;
    // Sesi siap menerima share: login selesai dan work pertama sudah dibangun.
    // Menutup celah reconnect, saat fd sudah terbuka tetapi TLS/job belum siap
    // — submit lama akan mengirim plaintext atau job sesi sebelumnya.
    std::atomic<bool> m_ready{false};
};
