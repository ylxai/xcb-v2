#pragma once

#include <set>
#include <string>

#include "Config.hpp"

// Terapkan key yang benar-benar ditulis file berprioritas lebih tinggi (src)
// ke config hasil kandidat fallback (dst). Hanya key di `keys` yang menimpa,
// jadi nilai default file parsial tidak mengalahkan nilai eksplisit dari
// kandidat. Key backend (algo/conn_rpc/conn_address/server/port) tidak
// di-overlay: file yang punya key itu sudah menjadi kandidat backend.
inline void applyFileOverlay(MinerConfig& dst, const MinerConfig& src,
                             const std::set<std::string>& keys) {
    if (keys.count("wallet")) {
        dst.wallet = src.wallet;
        for (auto& p : dst.pools) p.wallet = src.wallet;
    }
    if (keys.count("worker")) {
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
    if (keys.count("conn_cookie")) dst.connCookie = src.connCookie;
    if (keys.count("conn_nonce_offset")) dst.nonceOffset = src.nonceOffset;
}
