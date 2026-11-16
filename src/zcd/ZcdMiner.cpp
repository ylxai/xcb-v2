#include "ZcdMiner.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "../Hashing/RandomXV2.hpp"
#include "../Log.hpp"
#include "ZcdCommitment.hpp"

namespace zcd {

namespace {

// CPU yang boleh dipakai proses ini (cpuset-aware). Tidak memakai
// Config::usableCpus() supaya backend ZCD tidak bergantung pada Config.
std::vector<int> usableCpus() {
    std::vector<int> cpus;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int i = 0; i < CPU_SETSIZE; i++)
            if (CPU_ISSET(i, &set)) cpus.push_back(i);
    }
    if (cpus.empty()) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        for (long i = 0; i < n && i < CPU_SETSIZE; i++) cpus.push_back(static_cast<int>(i));
    }
    return cpus;
}

// Satu template kerja yang sedang dicari. Dibuat ulang setiap seed berubah.
struct Work {
    uint64_t gen = 0;
    Job job;
    std::shared_ptr<rxv2::Context> ctx;
    std::vector<std::unique_ptr<rxv2::Context::VM>> vms;
    // Penghitung nonce tunggal per Work: semua worker menarik nomor dari sini,
    // jadi tidak ada dua thread yang mengerjakan nonce sama.
    std::atomic<uint32_t> nonce{0};
    std::atomic<bool> done{false};
};

}  // namespace

struct Miner::Impl {
    MinerConfig cfg;
    std::unique_ptr<Pool> pool;
    std::thread ioThread;
    std::vector<std::thread> workers;
    std::atomic<bool> run{false};

    std::mutex mu;
    std::shared_ptr<Work> work;
    std::atomic<uint64_t> engineGen{0};

    // Statistik.
    std::atomic<uint64_t> hashes{0};
    std::atomic<uint64_t> found{0};
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> engineRebuilds{0};
    std::atomic<bool> engineFailed{false};
    std::string engineError;

    void buildEngine(const Job& job) {
        auto w = std::make_shared<Work>();
        w->job = job;
        w->nonce.store(cfg.nonceOffset, std::memory_order_relaxed);

        const std::string kh = job.keyHex();
        uint8_t key[32];
        std::memcpy(key, job.key, sizeof(key));

        lg::info("zcd", "membangun engine RandomX V2 (seed " + kh.substr(0, 16) + "…, " +
                            std::to_string(cfg.fastMem ? 2080 : 256) + " MB)");
        auto ctx = std::make_shared<rxv2::Context>(key, sizeof(key), cfg.fastMem, cfg.largePages,
                                                  static_cast<int>(cfg.threads));
        if (!ctx->ok()) {
            std::lock_guard<std::mutex> lock(mu);
            engineFailed.store(true, std::memory_order_relaxed);
            engineError = ctx->error();
            lg::error("zcd", "engine gagal: " + ctx->error());
            return;
        }
        const char* mode = cfg.fastMem ? (ctx->usesLargePages() ? "FAST, hugepages"
                                                                : "FAST, regular pages")
                                       : "LIGHT";
        lg::info("zcd", std::string("RandomX V2 siap (") + mode + ")");

        const int n = std::max(1, static_cast<int>(cfg.threads));
        w->vms.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            auto vm = ctx->createVM();
            if (!vm || !vm->ok()) {
                std::lock_guard<std::mutex> lock(mu);
                engineFailed.store(true, std::memory_order_relaxed);
                engineError = "VM gagal dibuat";
                lg::error("zcd", "VM RandomX gagal dibuat");
                return;
            }
            w->vms.push_back(std::move(vm));
        }
        w->ctx = ctx;

        {
            std::lock_guard<std::mutex> lock(mu);
            w->gen = engineGen.fetch_add(1, std::memory_order_acq_rel) + 1;
            work = std::move(w);
        }
        engineRebuilds.fetch_add(1, std::memory_order_relaxed);
    }

    std::shared_ptr<Work> snapshot() {
        std::lock_guard<std::mutex> lock(mu);
        return work;
    }

    void publishJob(const Job& job) {
        auto cur = snapshot();
        if (cur && cur->job.id == job.id) return;
        // Blob berubah tiap blok tetapi seed biasanya tetap: engine hanya
        // dibangun ulang saat seed_hash berubah. Selain itu cukup tukar job
        // pada Work yang sama, sehingga VM/dataset dipakai ulang.
        if (cur && std::memcmp(cur->job.key, job.key, 32) == 0) {
            std::lock_guard<std::mutex> lock(mu);
            if (work) {
                Job next = job;
                work->job = std::move(next);
                work->done.store(false, std::memory_order_relaxed);
            }
            return;
        }
        buildEngine(job);
    }

    void ioLoop(Miner& mself) {
        while (run.load(std::memory_order_acquire)) {
            std::string err;
            if (!pool->ready()) {
                if (!pool->connect(err)) {
                    lg::warn("zcd", "pool: " + err + " — coba lagi dalam 5 s");
                    for (int i = 0; i < 50 && run.load(); i++)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                Job first;
                if (pool->currentJob(first)) publishJob(first);
            }

            Job job;
            if (pool->nextJob(job, 5000, err)) {
                publishJob(job);
                lg::info("zcd", "job baru tinggi " + std::to_string(job.height) + " diff " +
                                    std::to_string(static_cast<long long>(job.difficultyValue())));
                continue;
            }
            if (!err.empty() && err != "timeout menunggu data pool") {
                lg::warn("zcd", "pool: " + err + " — sambung ulang");
                pool->disconnect();
            }
            (void)mself;
        }
    }

    void workerLoop(Miner& mself, int id, int cpu) {
        if (cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
        }
        ::setpriority(PRIO_PROCESS, 0, -10);

        uint64_t seenGen = 0;
        std::shared_ptr<Work> cur;
        std::shared_ptr<Work> engineAtStart;

        while (run.load(std::memory_order_acquire)) {
            auto w = snapshot();
            if (!w || id >= static_cast<int>(w->vms.size())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            // Context yang context-nya berganti: lepas referensi lama supaya
            // dataset lama tidak ikut hidup saat engine baru dibangun
            // (hugepages hanya cukup untuk satu dataset).
            if (engineAtStart && engineAtStart->ctx != w->ctx) engineAtStart.reset();
            cur = w;
            engineAtStart = w;
            const uint64_t gen = w->gen;

            auto& vm = w->vms[static_cast<size_t>(id)];
            for (int i = 0; i < 2048 && run.load(std::memory_order_relaxed); i++) {
                if (w->done.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    break;
                }

                uint8_t blob[BLOB_SIZE];
                std::memcpy(blob, w->job.blob.data(), BLOB_SIZE);
                const uint32_t nonce =
                    static_cast<uint32_t>(cur->nonce.fetch_add(1, std::memory_order_relaxed));
                for (size_t k = 0; k < NONCE_SIZE; k++)
                    blob[NONCE_OFFSET + k] = static_cast<uint8_t>(nonce >> (8 * k));

                uint8_t digest[DIGEST_SIZE];
                vm->hash(blob, BLOB_SIZE, digest);
                hashes.fetch_add(1, std::memory_order_relaxed);
                mself.m_hashes.fetch_add(1, std::memory_order_relaxed);

                uint8_t commit[COMMITMENT_SIZE];
                commitment(blob, BLOB_SIZE, digest, commit);

                if (meetsTarget(commit, cur->job.target)) {
                    found.fetch_add(1, std::memory_order_relaxed);
                    mself.m_found.fetch_add(1, std::memory_order_relaxed);

                    // Nonce sudah tertulis di blob; submit memakai job yang
                    // membawa blob/nonce ini.
                    Job solved = cur->job;
                    std::memcpy(solved.blob.data(), blob, BLOB_SIZE);
                    const uint64_t val = shareValue(commit);
                    lg::good("zcd", "[share] tinggi " + std::to_string(solved.height) + " nonce 0x" +
                                        solved.nonceHex(0) + " value 0x" +
                                        std::to_string(val) + " < target 0x" +
                                        std::to_string(solved.target));
                    std::string serr;
                    if (pool->submit(solved, digest, commit, serr))
                        submitted.fetch_add(1, std::memory_order_relaxed);
                    else
                        lg::warn("zcd", "submit gagal: " + serr);
                    // Job tidak ditandai selesai: share lain untuk job yang
                    // sama tetap dinamis, dan pool yang memutuskan kapan job
                    // berganti.
                }

                if ((i & 63) == 0 && gen != engineGen.load(std::memory_order_acquire)) break;
            }
            (void)seenGen;
        }
    }
};

Miner::Miner() : m_impl(std::make_unique<Impl>()) {
    m_impl->pool = nullptr;
}

Miner::~Miner() {
    stop();
    m_impl.reset();
}

bool Miner::start(const MinerConfig& cfg, std::string& err) {
    if (running()) {
        err = "miner sudah berjalan";
        return false;
    }
    m_impl->cfg = cfg;
    m_impl->pool = std::make_unique<Pool>(cfg.pool);

    if (!m_impl->pool->connect(err)) return false;

    Job first;
    if (!m_impl->pool->currentJob(first)) {
        err = "pool tidak memberi job";
        return false;
    }

    m_impl->run.store(true, std::memory_order_release);
    m_running.store(true, std::memory_order_release);

    m_impl->buildEngine(first);
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        if (m_impl->engineFailed.load(std::memory_order_relaxed)) {
            err = "engine: " + m_impl->engineError;
            m_impl->run.store(false);
            m_running.store(false);
            return false;
        }
    }

    const int n = std::max(1, static_cast<int>(cfg.threads));
    const std::vector<int> cpus = usableCpus();
    m_impl->workers.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
        const int cpu = cpus.empty() ? -1 : cpus[static_cast<size_t>(i) % cpus.size()];
        m_impl->workers.emplace_back([this, i, cpu]() { m_impl->workerLoop(*this, i, cpu); });
    }
    m_impl->ioThread = std::thread([this]() { m_impl->ioLoop(*this); });

    lg::info("zcd", "miner ZCD jalan: " + std::to_string(n) + " thread, algo " +
                        first.algo + ", tinggi " + std::to_string(first.height) + ", diff " +
                        std::to_string(static_cast<long long>(first.difficultyValue())));
    return true;
}

void Miner::stop() {
    if (!m_impl) return;
    m_impl->run.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    if (m_impl->pool) m_impl->pool->disconnect();
    for (auto& t : m_impl->workers)
        if (t.joinable()) t.join();
    m_impl->workers.clear();
    if (m_impl->ioThread.joinable()) m_impl->ioThread.join();
    {
        std::lock_guard<std::mutex> lock(m_impl->mu);
        m_impl->work.reset();
    }
}

std::string Miner::finalSummary() const {
    if (!m_impl) return "";
    const uint64_t h = m_impl->hashes.load(std::memory_order_relaxed);
    const uint64_t f = m_impl->found.load(std::memory_order_relaxed);
    const uint64_t acc = m_impl->pool ? m_impl->pool->acceptedCount() : 0;
    const uint64_t rej = m_impl->pool ? m_impl->pool->rejectedCount() : 0;
    std::string s = "ZCD: hash " + std::to_string(h) + " | found " + std::to_string(f) +
                    " | submit " + std::to_string(m_impl->submitted.load()) + " | pool OK " +
                    std::to_string(acc) + " ditolak " + std::to_string(rej);
    if (rej > 0 && m_impl->pool) {
        const std::string last = m_impl->pool->lastReject();
        if (!last.empty()) s += " | terakhir: " + last;
    }
    return s;
}

}  // namespace zcd
