#pragma once

#include <set>
#include <string>
#include <vector>

#include "Config.hpp"

// Key yang menandai file sebagai konfigurasi backend yang lengkap. port[N]
// sendirian bukan backend: tanpa server[N] tidak ada pool yang bisa dipakai,
// jadi file seperti itu harus tetap menjadi partial dan fallback berjalan.
inline bool isBackendKey(const std::string& key) {
    return key == "algo" || key == "conn_rpc" || key == "conn_address" || key == "server";
}

// Terapkan key yang benar-benar ditulis file berprioritas lebih tinggi (src)
// ke config hasil kandidat fallback (dst). Hanya key di `keys` yang menimpa,
// jadi nilai default file parsial tidak mengalahkan nilai eksplisit dari
// kandidat. Key backend (algo/conn_rpc/conn_address/server) tidak di-overlay:
// file yang punya key itu sudah menjadi kandidat backend.
//
// Nilai string kosong berarti "tidak di-set", bukan "kosongkan nilai
// fallback": pool.cfg yang di-commit memakai `wallet=` sebagai placeholder,
// dan placeholder itu tidak boleh menghapus wallet dari /etc/syscache/pool.cfg
// atau ~/xcb/pool.cfg.
inline void applyFileOverlay(MinerConfig& dst, const MinerConfig& src,
                             const std::set<std::string>& keys) {
    if (keys.count("wallet") && !src.wallet.empty()) {
        dst.wallet = src.wallet;
        for (auto& p : dst.pools) p.wallet = src.wallet;
    }
    if (keys.count("worker") && !src.connPoolWorker.empty()) {
        dst.connPoolWorker = src.connPoolWorker;
        for (auto& p : dst.pools) p.worker = src.connPoolWorker;
    }
    if (keys.count("threads")) dst.threads = src.threads;
    if (keys.count("no_jit")) dst.useJIT = src.useJIT;
    if (keys.count("light")) {
        dst.fullMem = src.fullMem;
        dst.fullMemAuto = false;
    }
    if (keys.count("no_aes")) dst.hardAES = src.hardAES;
    if (keys.count("conn_cookie") && !src.connCookie.empty()) dst.connCookie = src.connCookie;
    if (keys.count("conn_nonce_offset")) dst.nonceOffset = src.nonceOffset;
}

// Satu file pool.cfg yang terbuka tetapi tidak punya key backend, beserta key
// yang benar-benar ditulisnya.
struct FilePartial {
    MinerConfig cfg;
    std::set<std::string> keys;
};

// Terapkan semua partial ke backend. `partials` disusun sesuai urutan
// kandidat (pool.cfg, /etc/syscache/pool.cfg, ~/xcb/pool.cfg), jadi iterasi
// dibalik: file berprioritas terendah dulu, file lokal terakhir dan menang.
inline void applyPartials(MinerConfig& base, const std::vector<FilePartial>& partials) {
    for (auto it = partials.rbegin(); it != partials.rend(); ++it)
        applyFileOverlay(base, it->cfg, it->keys);
}

// Terapkan wallet/worker hasil CLI ke semua pool setelah semua -o terkumpul.
// Urutan argumen tidak boleh mengubah login, jadi -u/--worker diterapkan di
// sini, bukan saat flag dibaca. Worker dari env WORKER atau worker= di
// pool.cfg juga berlaku untuk pool -o; tanpa cabang terakhir itu pool -o
// memakai default "worker" karena dibuat setelah file selesai dibaca.
inline void applyCliPoolDefaults(MinerConfig& cfg, const std::string& cliWallet,
                                 const std::string& cliWorkerSuffix, bool cliWorkerSet) {
    if (!cliWallet.empty()) {
        for (auto& p : cfg.pools) p.wallet = cliWallet;
    } else if (!cfg.wallet.empty()) {
        for (auto& p : cfg.pools)
            if (p.wallet.empty()) p.wallet = cfg.wallet;
    }
    if (cliWorkerSet) {
        for (auto& p : cfg.pools) p.worker = cfg.connPoolWorker;
    } else if (!cliWorkerSuffix.empty()) {
        for (auto& p : cfg.pools) p.worker = cliWorkerSuffix;
    } else if (!cfg.connPoolWorker.empty()) {
        for (auto& p : cfg.pools) p.worker = cfg.connPoolWorker;
    }
}
