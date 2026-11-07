#include "RandomXV2.hpp"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

// Wajib sebelum randomx.h: nama API publik di-prefix rxv2_ (lihat
// external/randomx-v2/rxv2_api.h + prefix_symbols.sh).
#include "../../external/randomx-v2/rxv2_api.h"
#include "../../external/randomx-v2/src/randomx.h"
// configuration.h tidak punya include guard dan berbeda dari RandomY, jadi
// hanya di-include di sini — bukan di Config.cpp yang sudah memuat versi
// RandomY.
#include "../../external/randomx-v2/src/configuration.h"

namespace rxv2 {

MemNeed memNeed() {
    const long long datasetBytes =
        static_cast<long long>(RANDOMX_DATASET_BASE_SIZE) + RANDOMX_DATASET_EXTRA_SIZE;
    // Bulatkan ke atas: 2181038016 byte = 2079,99 MiB, dan memakai 2079 akan
    // meloloskan host yang sebenarnya kurang 1 MB.
    const long long mib = 1024 * 1024;
    return {static_cast<long>((datasetBytes + mib - 1) / mib), RANDOMX_ARGON_MEMORY / 1024};
}

namespace {

constexpr char kGoldenInput[] = "This is a test";
constexpr char kGoldenHash[] =
    "22ec6b861b3eb23686b2efbad69513c967ecfce80983df66c9c5b4fbfb4cdb6f";

std::string toHex(const uint8_t* p, size_t n) {
    static const char hx[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += hx[p[i] >> 4];
        s += hx[p[i] & 0xf];
    }
    return s;
}

rxv2_flags baseFlags() {
    rxv2_flags f = randomx_get_flags();
    // HAPUS flag FULL_MEM/LARGE_PAGES/SECURE/V2 yang mungkin terwarisi, lalu
    // set ulang sesuai kebutuhan.
    f = static_cast<rxv2_flags>(f & ~(RANDOMX_FLAG_FULL_MEM | RANDOMX_FLAG_LARGE_PAGES |
                                         RANDOMX_FLAG_SECURE | RANDOMX_FLAG_V2));
    // Consensus ConnectCoin WAJIB V2 (program 384). Tanpa flag ini library
    // menghitung RandomX 256-program -> semua share/blok invalid.
    return static_cast<rxv2_flags>(f | RANDOMX_FLAG_V2);
}

}  // namespace

Context::Context(const uint8_t key[32], bool fast, bool largePages, int initThreads)
    : Context(key, 32, fast, largePages, initThreads) {}

Context::Context(const uint8_t* key, size_t keyLen, bool fast, bool largePages, int initThreads) {
    const rxv2_flags base = baseFlags();
    rxv2_flags allocFlags = base;
    if (largePages) allocFlags = static_cast<rxv2_flags>(allocFlags | RANDOMX_FLAG_LARGE_PAGES);

    if (fast) {
        m_cache = randomx_alloc_cache(allocFlags);
        if (!m_cache && largePages) m_cache = randomx_alloc_cache(base);
        if (!m_cache) {
            m_error = "alokasi RandomX cache gagal";
            return;
        }
        randomx_init_cache(static_cast<randomx_cache*>(m_cache), key, keyLen);

        m_dataset = randomx_alloc_dataset(allocFlags);
        m_largePages = (m_dataset != nullptr) && largePages;
        if (!m_dataset && largePages) m_dataset = randomx_alloc_dataset(base);
        if (!m_dataset) {
            randomx_release_cache(static_cast<randomx_cache*>(m_cache));
            m_cache = nullptr;
            m_error = "alokasi RandomX dataset (2 GiB) gagal";
            return;
        }

        // Inisialisasi dataset paralel: satu-satunya bagian lambat (10-30 s
        // di 4 core). randomx_init_dataset aman dipanggil pada rentang item
        // yang saling lepas.
        const unsigned long total = randomx_dataset_item_count();
        int nt = std::max(1, initThreads);
        nt = std::min<int>(nt, 32);
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(nt));
        for (int i = 0; i < nt; i++) {
            const unsigned long start = total * static_cast<unsigned long>(i) / static_cast<unsigned long>(nt);
            const unsigned long end = total * static_cast<unsigned long>(i + 1) / static_cast<unsigned long>(nt);
            threads.emplace_back([this, start, end]() {
                randomx_init_dataset(static_cast<randomx_dataset*>(m_dataset),
                                     static_cast<randomx_cache*>(m_cache), start, end - start);
            });
        }
        for (auto& t : threads) t.join();

        // FAST VM membaca dataset saja; cache 256 MiB bisa dilepas.
        randomx_release_cache(static_cast<randomx_cache*>(m_cache));
        m_cache = nullptr;
        m_fast = true;
        m_ok = true;
        return;
    }

    m_cache = randomx_alloc_cache(allocFlags);
    if (!m_cache && largePages) m_cache = randomx_alloc_cache(base);
    if (!m_cache) {
        m_error = "alokasi RandomX cache gagal";
        return;
    }
    randomx_init_cache(static_cast<randomx_cache*>(m_cache), key, keyLen);
    m_fast = false;
    m_ok = true;
}

Context::~Context() {
    if (m_dataset) randomx_release_dataset(static_cast<randomx_dataset*>(m_dataset));
    if (m_cache) randomx_release_cache(static_cast<randomx_cache*>(m_cache));
}

std::unique_ptr<Context::VM> Context::createVM() {
    if (!m_ok) return nullptr;
    rxv2_flags f = baseFlags();
    if (m_fast) f = static_cast<rxv2_flags>(f | RANDOMX_FLAG_FULL_MEM);

    randomx_cache* cache = m_fast ? nullptr : static_cast<randomx_cache*>(m_cache);
    randomx_dataset* dataset = m_fast ? static_cast<randomx_dataset*>(m_dataset) : nullptr;

    void* vm = nullptr;
    if (m_largePages)
        vm = randomx_create_vm(static_cast<rxv2_flags>(f | RANDOMX_FLAG_LARGE_PAGES), cache, dataset);
    if (!vm) vm = randomx_create_vm(f, cache, dataset);

    auto out = std::make_unique<VM>();
    out->m_vm = vm;
    return out;
}

Context::VM::~VM() {
    if (m_vm) randomx_destroy_vm(static_cast<randomx_vm*>(m_vm));
}

void Context::VM::hash(const uint8_t* data, size_t len, uint8_t out[32]) {
    randomx_calculate_hash(static_cast<randomx_vm*>(m_vm), data, len, out);
}

bool selftest(std::string& detail) {
    static const char kGoldenKey[] = "test key 000";  // 12 byte, bukan 32
    Context ctx(reinterpret_cast<const uint8_t*>(kGoldenKey), sizeof(kGoldenKey) - 1,
                /*fast=*/false, /*largePages=*/false, 1);
    if (!ctx.ok()) {
        detail = "RandomX V2 context gagal: " + ctx.error();
        return false;
    }
    auto vm = ctx.createVM();
    if (!vm || !vm->ok()) {
        detail = "RandomX V2 VM gagal dibuat";
        return false;
    }
    uint8_t out[32];
    vm->hash(reinterpret_cast<const uint8_t*>(kGoldenInput), sizeof(kGoldenInput) - 1, out);
    const std::string got = toHex(out, 32);
    const bool ok = (got == kGoldenHash);
    detail = "vec key=\"test key 000\" got=" + got;
    return ok;
}

}  // namespace rxv2
