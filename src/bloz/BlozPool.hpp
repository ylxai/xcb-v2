#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "BlozJob.hpp"

// Klien stratum dialek Monero/XMRig untuk pool Block Zero (rplant rx/blockzero
// atau pool.bloz.org).
//
// Mandiri: punya socket, TLS, dan buffering sendiri. Tidak memakai ConnPool,
// StratumClient, maupun ConnRpc — satu-satunya bagian yang dipinjam dari luar
// adalah parser JSON generik (lihat BlozJob.cpp).
//
// Alur: login -> result.job (job pertama) -> notifikasi "job" tiap blok ->
// submit nonce. Semua TLS di bawah satu mutex, karena satu SSL object tidak
// boleh dipakai dua thread bersamaan.
namespace bloz {

struct PoolConfig {
    std::string host;
    uint16_t port = 7170;
    bool tls = false;
    std::string login;   // alamat wallet BLOZ (bz1…)
    std::string worker;  // label worker
    std::string agent = "xcb-bloz/0.1";
};

class Pool {
public:
    explicit Pool(PoolConfig cfg);
    ~Pool();

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    // Sambung, TLS handshake, kirim login, tunggu job pertama.
    bool connect(std::string& err);
    void disconnect();

    bool ready() const { return m_ready.load(std::memory_order_acquire); }

    // Tunggu job baru (notifikasi "job" atau job dari login) maksimal timeoutMs.
    // Mengisi out bila ada job baru; mengembalikan false bila timeout/putus.
    bool nextJob(Job& out, int timeoutMs, std::string& err);

    // Job terakhir yang diterima pool. Dipakai worker untuk submit.
    bool currentJob(Job& out);

    // Kirim share. `digest` = digest RandomX v1 mentah atas header 80 byte;
    // itulah yang dibandingkan pool dengan target, jadi field "result" berisi
    // digest apa adanya. Block Zero tidak memakai commitment seperti Zycord.
    bool submit(const Job& job, const uint8_t digest[32], std::string& err);

    // Hitung submit yang dijawab status OK / error.
    uint64_t acceptedCount() const { return m_accepted.load(std::memory_order_relaxed); }
    uint64_t rejectedCount() const { return m_rejected.load(std::memory_order_relaxed); }
    std::string lastReject() const;

private:
    bool sendLine(const std::string& line, std::string& err);
    bool recvLine(std::string& line, int timeoutMs, std::string& err);
    // Kirim isi antrean. Pemanggil memegang m_ioMu (kunci socket).
    bool flushOutLocked(std::string& err);
    bool ensureConnected(std::string& err);

    PoolConfig m_cfg;
    int m_fd = -1;
    void* m_ssl = nullptr;        // SSL*
    void* m_sslCtx = nullptr;     // SSL_CTX*
    std::string m_buf;
    std::atomic<bool> m_ready{false};
    // m_ioMu hanya untuk pemilik socket: io thread dan disconnect(). Jangan
    // pernah menahannya selama menunggu worker — submit() tidak menyentuh
    // socket, ia hanya menaruh pesan di m_outBuf.
    std::mutex m_ioMu;
    std::mutex m_outMu;
    std::string m_outBuf;
    std::mutex m_jobMu;
    Job m_job;
    bool m_haveJob = false;
    std::atomic<long long> m_msgId{1};
    std::string m_loginId = "1";
    std::atomic<uint64_t> m_accepted{0};
    std::atomic<uint64_t> m_rejected{0};
    mutable std::mutex m_rejectMu;
    std::string m_lastReject;
};

}  // namespace bloz
