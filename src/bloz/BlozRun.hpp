#pragma once

#include <atomic>

#include "../Config.hpp"

// Integrasi backend Block Zero (BLOZ) ke biner tunggal `xcb`.
//
// Sebelumnya bloz-miner adalah biner sendiri dengan main() dan parser argv
// sendiri. Sekarang `-a bloz` memilih backend ini dan Config yang membaca
// wallet/pool/threads, sama seperti `-a randomy` dan `-a conn`.
namespace bloz {

// Uji engine RandomX v1 + aturan share rx/blockzero. Dipanggil dari
// `xcb --selftest`; true bila semua vektor cocok.
bool selftest();

// Jalankan miner sampai `running` menjadi false atau miner berhenti sendiri.
// Return code proses (0 = selesai normal).
int run(const ::MinerConfig& cfg, std::atomic<bool>& running);

}  // namespace bloz
