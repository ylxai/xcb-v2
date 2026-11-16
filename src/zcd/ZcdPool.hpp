#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "ZcdJob.hpp"

// Klien stratum dialek Monero/XMRig untuk pool Zycord (rplant rx/2).
//
// Mandiri: punya socket, TLS, dan buffering sendiri. Tidak memakai ConnPool,
// StratumClient, maupun ConnRpc — satu-satunya bagian yang dipinjam dari luar
// adalah parser JSON generik (lihat ZcdJob.cpp).
//
// Alur: login -> result.job (job pertama) -> notifikasi "job" tiap blok ->
// submit nonce. Semua TLS di bawah satu mutex, karena satu SSL object tidak
// boleh dipakai dua thread bersamaan.
namespace zcd {

struct PoolConfig {
    std::string host;
    uint16_t port = 17197;
    bool tls = true;
    std::string login;   // alamat wallet ZCD (0x02...)
    std::string worker;  // label worker
    std::string agent = "xcb-zcd/0.1";
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

    // Kirim share. `digest` = digest RandomX mentah, `commitment` = 32 byte
    // yang dibandingkan dengan target. Ikut dikirim karena XMRig mengirim
    // keduanya: field "result" membawa commitment, field "commitment" membawa
    // digest mentah (namanya terbalik — lihat pow.go Zycord).
    bool submit(const Job& job, const uint8_t digest[32], const uint8_t commitment[32],
                std::string& err);

    // Hitung submit yang dijawab status OK / error.
    uint64_t acceptedCount() const { return m_accepted.load(std::memory_order_relaxed); }
    uint64_t rejectedCount() const { return m_rejected.load(std::memory_order_relaxed); }
    std::string lastReject() const;

private:
    bool sendLine(const std::string& line, std::string& err);
    bool recvLine(std::string& line, int timeoutMs, std::string& err);
    bool ensureConnected(std::string& err);

    PoolConfig m_cfg;
    int m_fd = -1;
    void* m_ssl = nullptr;        // SSL*
    void* m_sslCtx = nullptr;     // SSL_CTX*
    std::string m_buf;
    std::atomic<bool> m_ready{false};
    std::mutex m_ioMu;
    std::mutex m_jobMu;
    Job m_job;
    bool m_haveJob = false;
    long long m_msgId = 1;
    std::string m_loginId = "1";
    std::atomic<uint64_t> m_accepted{0};
    std::atomic<uint64_t> m_rejected{0};
    mutable std::mutex m_rejectMu;
    std::string m_lastReject;
};

}  // namespace zcd
