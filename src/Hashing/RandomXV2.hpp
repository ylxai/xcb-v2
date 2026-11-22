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

// Kebutuhan memori mode FAST dalam MB, dihitung dari parameter RandomX V2
// yang benar-benar dikompilasi. Dipakai auto-detect di Config supaya ambang
// memori tidak memakai angka RandomY (dataset 264 MB) untuk CONN.
struct MemNeed {
    long datasetMb;
    long cacheMb;
};
MemNeed memNeed();

class Context {
public:
    // key = 32 byte dari getblocktemplate "randomxkey" (bootstrap key selama
    // seed height 0).
    //
    // v2=true (default) memilih program VM 384 instruksi — wajib untuk CONN
    // dan ZCD. v2=false memilih program 256 instruksi, yaitu RandomX v1
    // kanonik; itu yang dipakai Block Zero (algo "rx/blockzero"). Ukuran
    // dataset dan salt Argon2 sama di kedua mode, jadi satu library melayani
    // keduanya.
    Context(const uint8_t key[32], bool fast, bool largePages, int initThreads,
            bool v2 = true);
    // Varian umum: RandomX memakai key persis sepanjang keyLen byte (vektor
    // uji upstream memakai key 12 byte "test key 000", bukan 32 byte padded).
    Context(const uint8_t* key, size_t keyLen, bool fast, bool largePages,
            int initThreads, bool v2 = true);
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

        // Batch berantai. hashFirst mulai hash pertama, hashNext menghasilkan
        // hash input sebelumnya sambil mengisi scratchpad untuk input
        // berikutnya (blake2b akhir dan pembacaan dataset berikutnya
        // bertumpang tindih), hashLast menutup batch. Dipakai miner karena
        // memberi ~4% di atas pemanggilan hash() satu per satu.
        // Pola: first(in0); next(in1,out0); … next(inN-1,outN-2); last(outN-1).
        void hashFirst(const uint8_t* data, size_t len);
        void hashNext(const uint8_t* data, size_t len, uint8_t out[32]);
        void hashLast(uint8_t out[32]);

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
    bool m_v2 = true;
    bool m_ok = false;
    std::string m_error;
};

// Vektor emas dari connectcoin src/test/randomx_tests.cpp:
//   key "test key 000", input "This is a test", LIGHT
//   -> 22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f
// LIGHT (cache 256 MiB) cukup: hash FAST identik.
bool selftest(std::string& detail);

// Vektor emas RandomX v1 (mode tanpa RANDOMX_FLAG_V2), diambil dari
// cpuminer-rplant 6.0.26 saat menambang rx/blockzero ke pool palsu:
//   blob 80 byte 0000002071dffbd9…071d00000000 (nonce 0x00000355 di offset 76)
//   seed 7b5cdc9c17790fb2e9400cf29a3b373167d29657303269362d5e61e332de1edd
//   -> d51a8c17b9e6720eb696bf766cd904b4317bb03e0c5a848882349dc061390900
// Kalau vektor ini cocok, backend BLOZ menghitung digest yang sama dengan
// miner lain untuk header yang sama.
bool selftestV1(std::string& detail);

}  // namespace rxv2
