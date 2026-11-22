#pragma once

#include <string>

namespace xcb {

// Alamat wallet adalah identitas reward: siapa pun yang bisa membaca journal
// (mis. grup adm) bisa mengaitkan host dengan wallet dan memantau hasilnya.
// Log cukup menampilkan potongan yang masih bisa dibedakan operator.
// Bentuk masuknya "alamat" atau "alamat.worker"; suffix worker dipertahankan.
inline std::string redactWallet(const std::string& login) {
    const size_t dot = login.find('.');
    const std::string addr = login.substr(0, dot);
    const std::string rest =
        dot == std::string::npos ? std::string() : login.substr(dot);
    if (addr.size() <= 12) return "***" + rest;
    return addr.substr(0, 6) + "…" + addr.substr(addr.size() - 4) + rest;
}

}  // namespace xcb
