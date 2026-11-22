#include "BlozMiner.hpp"

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

namespace bloz {

namespace {

// CPU yang boleh dipakai proses ini (cpuset-aware). Tidak memakai
// Config::usableCpus() supaya backend BLOZ tidak bergantung pada Config.
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

// Satu template kerja yang sedang dicari. Work dibuat ulang setiap seed
// berubah, dan `job` sengaja dimiliki lewat shared_ptr<const Job>: job baru
// mengganti pointer, tidak memutasi objek yang sedang dibaca worker. Dulu
// `work->job` diganti di tempat dengan move-assignment, yang membebaskan
// buffer blob milik worker yang sedang memcpy — use-after-free setiap blok.
struct Work {
    // gen ditulis io thread saat job berganti dan dibaca worker di tengah
    // batch. Atomik supaya pembacaan periodik itu tidak berlomba dengan
    // penulisan; job sendiri dilindungi mu (lihat snapshot()).
    std::atomic<uint64_t> gen{0};
    std::shared_ptr<const Job> job;
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
    std::mutex errMu;
    std::string engineError;

    void clearEngineError() {
        std::lock_guard<std::mutex> lock(errMu);
        engineError.clear();
    }

    void setEngineError(const std::string& msg) {
        std::lock_guard<std::mutex> lock(errMu);
        engineFailed.store(true, std::memory_order_relaxed);
        engineError = msg;
    }

    std::string getEngineError() {
        std::lock_guard<std::mutex> lock(errMu);
        return engineError;
    }

    // Bangun engine untuk seed baru. Dataset lama dilepas lebih dulu di sini
    // dan ditunggu benar-benar hilang: satu dataset FAST 2080 MB, sedangkan
    // hugepages biasanya hanya cukup untuk satu. Tanpa penantian, alokasi baru
    // gagal mendapat hugepages lalu turun diam-diam ke page biasa dan hash rate
    // tetap rendah sampai proses di-restart.
    void buildEngine(const Job& job) {
        engineFailed.store(false, std::memory_order_relaxed);
        clearEngineError();

        releaseOldWork();

        auto w = std::make_shared<Work>();
        w->job = std::make_shared<const Job>(job);
        w->nonce.store(cfg.nonceOffset, std::memory_order_relaxed);

        const std::string kh = job.keyHex();
        uint8_t key[32];
        std::memcpy(key, job.key, sizeof(key));

        lg::info("bloz", "membangun engine RandomX v1 (seed " + kh.substr(0, 16) + "…, " +
                            std::to_string(cfg.fastMem ? 2080 : 256) + " MB)");
        // v2=false: Block Zero memakai program 256 instruksi (RandomX v1
        // kanonik). Dengan flag V2, digest berbeda dan semua share ditolak.
        auto ctx = std::make_shared<rxv2::Context>(key, sizeof(key), cfg.fastMem, cfg.largePages,
                                                  static_cast<int>(cfg.threads), /*v2=*/false);
        if (!ctx->ok()) {
            setEngineError(ctx->error());
            lg::error("bloz", "engine gagal: " + ctx->error());
            // Engine lama tidak boleh tetap aktif: share untuk seed usang akan
            // ditolak pool, dan lebih buruk lagi menutupi kegagalan ini.
            invalidateWork();
            return;
        }
        const char* mode = cfg.fastMem ? (ctx->usesLargePages() ? "FAST, hugepages"
                                                                : "FAST, regular pages")
                                       : "LIGHT";
        lg::info("bloz", std::string("RandomX v1 siap (") + mode + ")");
        // Jatuh ke page biasa menurunkan hash rate sekitar setengah dan tidak
        // terlihat dari log biasa, jadi diperingatkan sekali di sini.
        if (cfg.fastMem && cfg.largePages && !ctx->usesLargePages()) {
            lg::warn("bloz", "hugepages tidak terpakai (minta " +
                                std::to_string(cfg.fastMem ? 2080 : 256) +
                                " MB); hash rate turun ~50%. Periksa vm.nr_hugepages "
                                "atau HugePages_Free.");
        }

        const int n = std::max(1, static_cast<int>(cfg.threads));
        w->vms.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            auto vm = ctx->createVM();
            if (!vm || !vm->ok()) {
                setEngineError("VM gagal dibuat");
                lg::error("bloz", "VM RandomX gagal dibuat");
                invalidateWork();
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

    // Lepas work lama dan tunggu worker berhenti memakainya. Worker memegang
    // snapshot `shared_ptr<Work>` selama satu batch, jadi dataset lama baru
    // benar-benar bebas setelah referensinya habis. Dipanggil sebelum engine
    // baru dialokasikan supaya puncak pemakaian memori tidak dua dataset.
    void releaseOldWork() {
        std::shared_ptr<Work> old;
        {
            std::lock_guard<std::mutex> lock(mu);
            old = std::move(work);
            work.reset();
            // Naikkan generasi supaya worker tidak memakai snapshot lama untuk
            // batch berikutnya.
            engineGen.fetch_add(1, std::memory_order_acq_rel);
        }
        if (!old) return;
        // Tunggu sampai tidak ada worker yang masih memegang Work ini. Batas
        // waktu menjaga agar worker yang macet tidak menggantungkan ioLoop.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (old.use_count() > 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (old.use_count() > 1) {
            lg::warn("bloz", "worker masih memegang dataset lama setelah 10 detik; "
                            "engine baru mungkin jatuh ke page biasa");
        }
    }

    // Hentikan semua worker dari work yang sekarang tanpa membiarkan engine
    // gagal tampak "jalan": workers menunggu work kosong sampai engine baru
    // berhasil dibangun.
    void invalidateWork() {
        std::lock_guard<std::mutex> lock(mu);
        work.reset();
        engineGen.fetch_add(1, std::memory_order_acq_rel);
    }

    // Salin job dan gen di bawah mu. Worker memegang shared_ptr<Work> sendiri,
    // tetapi isi Work (job/gen) ditulis io thread saat job baru datang; menyalin
    // keduanya di sini membuat worker tidak pernah membaca shared_ptr yang
    // sedang di-assign (itu data race dan bisa merusak refcount).
    std::shared_ptr<Work> snapshot(std::shared_ptr<const Job>* jobOut = nullptr,
                                   uint64_t* genOut = nullptr) {
        std::lock_guard<std::mutex> lock(mu);
        if (work) {
            if (jobOut) *jobOut = work->job;
            if (genOut) *genOut = work->gen.load(std::memory_order_relaxed);
        }
        return work;
    }

    void publishJob(const Job& job) {
        auto cur = snapshot();
        // Bandingkan isi job, bukan hanya id: pool boleh memakai ulang job_id
        // untuk pekerjaan yang berbeda (termasuk setelah reconnect), dan
        // menganggapnya sama membuat worker terus menambang blob lama.
        if (cur && cur->job && sameJob(*cur->job, job)) return;

        // Blob berubah tiap blok tetapi seed biasanya tetap: engine hanya
        // dibangun ulang saat seed_hash berubah. Selain itu cukup tukar
        // pointer job, sehingga VM/dataset dipakai ulang.
        if (cur && cur->job && std::memcmp(cur->job->key, job.key, 32) == 0) {
            auto next = std::make_shared<const Job>(job);
            std::lock_guard<std::mutex> lock(mu);
            if (work && work->job && std::memcmp(work->job->key, job.key, 32) == 0) {
                // Tukar pointer, bukan memutasi Job: worker yang sedang memcpy
                // memegang shared_ptr sendiri sehingga buffernya tetap hidup.
                work->job = std::move(next);
                // Naikkan generasi supaya worker berhenti memakai snapshot job
                // lama. Tanpa ini worker menyelesaikan sisa batch 2048 hash
                // untuk blob yang sudah usang (hash rate tampak turun dan
                // share dikirim untuk job yang sudah lewat).
                work->gen = engineGen.fetch_add(1, std::memory_order_acq_rel) + 1;
                work->done.store(false, std::memory_order_relaxed);
                return;
            }
        }
        // Seed berganti: lepas snapshot di sini juga. Kalau `cur` masih hidup
        // saat releaseOldWork() menghitung use_count, penantiannya selalu
        // timeout dan dataset baru dibangun berdampingan dengan yang lama.
        cur.reset();
        buildEngine(job);
    }

    static bool sameJob(const Job& a, const Job& b) {
        return a.id == b.id && a.target == b.target && a.height == b.height &&
               a.blob == b.blob && std::memcmp(a.key, b.key, 32) == 0;
    }

    void ioLoop(Miner& mself) {
        while (run.load(std::memory_order_acquire)) {
            std::string err;
            if (!pool->ready()) {
                if (!pool->connect(err)) {
                    lg::warn("bloz", "pool: " + err + " — coba lagi dalam 5 s");
                    for (int i = 0; i < 50 && run.load(); i++)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                // Job yang dibawa balasan login belum pernah dipublikasikan.
                // Pool boleh tidak mengirim notifikasi sampai blok berikutnya,
                // jadi tanpa ini worker tetap menambang blob sebelum reconnect
                // (dan share-nya basi). publishJob mengabaikan job yang sama.
                Job loginJob;
                if (pool->currentJob(loginJob)) publishJob(loginJob);
                continue;
            }

            Job job;
            if (pool->nextJob(job, 5000, err)) {
                // Stop bisa datang selama nextJob menunggu; jangan pasang job
                // baru ke miner yang sudah diminta berhenti.
                if (!run.load(std::memory_order_acquire)) break;
                publishJob(job);
                lg::info("bloz", "job baru tinggi " + std::to_string(job.height) + " diff " +
                                    std::to_string(static_cast<long long>(job.difficultyValue())));
                continue;
            }
            if (!err.empty() && err != "timeout menunggu data pool") {
                lg::warn("bloz", "pool: " + err + " — sambung ulang");
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

        std::shared_ptr<Work> engineAtStart;

        while (run.load(std::memory_order_acquire)) {
            std::shared_ptr<const Job> job;
            uint64_t gen = 0;
            auto w = snapshot(&job, &gen);
            if (!w || !job || id >= static_cast<int>(w->vms.size())) {
                // Work dikosongkan saat rekey: lepas juga referensi terakhir
                // worker ke dataset lama, kalau tidak releaseOldWork() menunggu
                // sampai timeout dan engine baru terpaksa memakai page biasa.
                engineAtStart.reset();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            // Context berganti: lepas referensi lama supaya dataset lama tidak
            // ikut hidup saat engine baru dibangun.
            if (engineAtStart && engineAtStart->ctx != w->ctx) engineAtStart.reset();
            engineAtStart = w;

            auto& vm = w->vms[static_cast<size_t>(id)];
            // Hash satu per satu membuang satu lintasan penuh atas scratchpad:
            // blake2b akhir (baca 2 MB) lalu pengisian untuk nonce berikutnya
            // (tulis 2 MB) dikerjakan terpisah. API batch memfusikan keduanya.
            // BATCH 16 terukur lebih baik daripada 8 (1.648-1.660 vs 1.597-1.598
            // H/s di fl, dua ronde); 32 justru turun lagi.
            constexpr int BATCH = 16;
            for (int i = 0; i < 2048 / BATCH && run.load(std::memory_order_relaxed); i++) {
                if (w->done.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    break;
                }
                // Cek pergantian job tiap 4 batch (64 hash, ~40 ms): dua atomic
                // load per hash memotong hash rate, dan 64 hash cukup cepat
                // untuk job baru 30 detik. Nonce tetap unik karena diambil dari
                // counter bersama per blok BATCH.
                if ((i & 3) == 0 &&
                    w->gen.load(std::memory_order_relaxed) != gen) break;

                const Job& j = *job;
                uint8_t blobs[BATCH][BLOB_SIZE];
                uint8_t digests[BATCH][32];
                const uint32_t base =
                    static_cast<uint32_t>(w->nonce.fetch_add(BATCH, std::memory_order_relaxed));
                for (int k = 0; k < BATCH; k++) {
                    std::memcpy(blobs[k], j.blob.data(), BLOB_SIZE);
                    const uint32_t nonce = base + static_cast<uint32_t>(k);
                    for (size_t b = 0; b < NONCE_SIZE; b++)
                        blobs[k][NONCE_OFFSET + b] = static_cast<uint8_t>(nonce >> (8 * b));
                }

                // first(in0); next(in1,out0) … next(in7,out6); last(out7).
                vm->hashFirst(blobs[0], BLOB_SIZE);
                for (int k = 1; k < BATCH; k++)
                    vm->hashNext(blobs[k], BLOB_SIZE, digests[k - 1]);
                vm->hashLast(digests[BATCH - 1]);

                hashes.fetch_add(BATCH, std::memory_order_relaxed);
                mself.m_hashes.fetch_add(BATCH, std::memory_order_relaxed);

                for (int k = 0; k < BATCH; k++) {
                    if (!meetsTarget(digests[k], j.target)) continue;
                    found.fetch_add(1, std::memory_order_relaxed);
                    mself.m_found.fetch_add(1, std::memory_order_relaxed);

                    Job solved = j;
                    std::memcpy(solved.blob.data(), blobs[k], BLOB_SIZE);
                    // Job bisa berganti antara hash ini dan submit. Share untuk
                    // job lama pasti dijawab "stale share" oleh pool, jadi
                    // jangan dikirim (menghemat kuota submit dan tidak
                    // mengotori statistik). Nonce sudah terpakai, jadi
                    // melewatkannya tidak kehilangan peluang.
                    if (w->gen != gen) continue;
                    lg::good("bloz", "[share] tinggi " + std::to_string(solved.height) + " nonce 0x" +
                                        solved.nonceHex(0) + " value " +
                                        std::to_string(digestValue(digests[k])) + " < target " +
                                        std::to_string(solved.target));
                    std::string serr;
                    if (pool->submit(solved, digests[k], serr))
                        submitted.fetch_add(1, std::memory_order_relaxed);
                    else
                        lg::warn("bloz", "submit gagal: " + serr);
                }
            }
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
    // Kontrak MinerConfig.threads: 0 = auto. Diselesaikan di sini supaya
    // pemanggil selain BlozRun juga mendapat perilaku yang sama.
    if (m_impl->cfg.threads <= 0) {
        const int n = static_cast<int>(usableCpus().size());
        m_impl->cfg.threads = n > 0 ? n : 1;
    }
    m_impl->pool = std::make_unique<Pool>(cfg.pool);

    if (!m_impl->pool->connect(err)) return false;

    Job first;
    if (!m_impl->pool->currentJob(first)) {
        err = "pool tidak memberi job";
        return false;
    }

    m_impl->run.store(true, std::memory_order_release);
    m_running.store(true, std::memory_order_release);

    // engineFailed di-reset di buildEngine, jadi start yang diulang setelah
    // kegagalan tidak lagi melihat status lama.
    m_impl->buildEngine(first);
    if (m_impl->engineFailed.load(std::memory_order_relaxed)) {
        err = "engine: " + m_impl->getEngineError();
        m_impl->run.store(false);
        m_running.store(false);
        return false;
    }

    const int n = std::max(1, static_cast<int>(m_impl->cfg.threads));
    const std::vector<int> cpus = usableCpus();
    m_impl->workers.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
        const int cpu = cpus.empty() ? -1 : cpus[static_cast<size_t>(i) % cpus.size()];
        m_impl->workers.emplace_back([this, i, cpu]() { m_impl->workerLoop(*this, i, cpu); });
    }
    m_impl->ioThread = std::thread([this]() { m_impl->ioLoop(*this); });

    lg::info("bloz", "miner BLOZ jalan: " + std::to_string(n) + " thread, algo " + first.algo +
                        ", tinggi " + std::to_string(first.height) + ", diff " +
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
    std::string s = "BLOZ: hash " + std::to_string(h) + " | found " + std::to_string(f) +
                    " | submit " + std::to_string(m_impl->submitted.load()) + " | pool OK " +
                    std::to_string(acc) + " ditolak " + std::to_string(rej);
    if (rej > 0 && m_impl->pool) {
        const std::string last = m_impl->pool->lastReject();
        if (!last.empty()) s += " | terakhir: " + last;
    }
    return s;
}

}  // namespace bloz
