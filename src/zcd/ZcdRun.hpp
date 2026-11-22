#pragma once

#include <atomic>

#include "../Config.hpp"

// Integrasi backend Zycord (ZCD) ke biner tunggal `xcb`.
//
// Sebelumnya zcd-miner adalah biner sendiri dengan main() dan parser argv
// sendiri. Sekarang `-a zcd` memilih backend ini dan Config yang membaca
// wallet/pool/threads.
namespace zcd {

// Uji engine RandomX V2 + aturan commitment rx/2. Dipanggil dari
// `xcb --selftest`; true bila semua vektor cocok.
bool selftest();

// Jalankan miner sampai `running` menjadi false atau miner berhenti sendiri.
// Return code proses (0 = selesai normal).
int run(const ::MinerConfig& cfg, std::atomic<bool>& running);

}  // namespace zcd
