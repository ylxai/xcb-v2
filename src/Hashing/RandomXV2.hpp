#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Backend PoW RandomX V2 untuk ConnectCoin (CONN).
//
// "V2" bukan RandomX tevador standar: program VM 384 instruksi (bukan 256),
// dataset 2 GiB — lihat external/randomx-v2 (vendor dari
// connectcoincrypto/connectcoin, src/randomx). Hash = RandomX V2 atas header
// blok 80-byte dengan key 32 byte yang berganti tiap epoch (12.288 blok,
// lag 64 blok, bootstrap key saat seed height 0).
//
// Satu Context mewakili satu key epoch: cache (LIGHT) atau dataset 2 GiB
// (FAST). VM di-lease per thread dari context yang sama, jadi dataset tidak
// pernah diduplikasi per worker.
namespace rxv2 {

constexpr size_t HEADER_SIZE = 80;

class Context {
public:
    // key = 32 byte dari getblocktemplate "randomxkey" (bootstrap key selama
    // seed height 0).
    Context(const uint8_t key[32], bool fast, bool largePages, int initThreads);
    // Varian umum: RandomX memakai key persis sepanjang keyLen byte (vektor
    // uji upstream memakai key 12 byte "test key 000", bukan 32 byte padded).
    Context(const uint8_t* key, size_t keyLen, bool fast, bool largePages,
            int initThreads);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    bool ok() const { return m_ok; }
    const std::string& error() const { return m_error; }
    bool fast() const { return m_fast; }
    bool usesLargePages() const { return m_largePages; }

    class VM {
    public:
        ~VM();
        bool ok() const { return m_vm != nullptr; }
        // Hitung hash RandomX V2. Thread-safe selama satu VM hanya dipakai
        // satu thread pada satu waktu.
        void hash(const uint8_t* data, size_t len, uint8_t out[32]);

    private:
        friend class Context;
        void* m_vm = nullptr;
    };

    // nullptr bila context tidak siap atau alokasi VM gagal.
    std::unique_ptr<VM> createVM();

private:
    void* m_cache = nullptr;    // randomx_cache*
    void* m_dataset = nullptr;  // randomx_dataset*
    bool m_fast = false;
    bool m_largePages = false;
    bool m_ok = false;
    std::string m_error;
};

// Vektor emas dari connectcoin src/test/randomx_tests.cpp:
//   key "test key 000", input "This is a test", LIGHT
//   -> 22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f
// LIGHT (cache 256 MiB) cukup: hash FAST identik.
bool selftest(std::string& detail);

}  // namespace rxv2
