#include "Miner.hpp"

#include "Log.hpp"
#include "Sha3_512.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <endian.h>
#include <iostream>
#include <pthread.h>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>

// ============================================================
// Hex helpers
// ============================================================
static std::string bytes_to_hex(const uint8_t* data, int len) {
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.resize(len * 2);
    for (int i = 0; i < len; i++) {
        s[i * 2] = hex[(data[i] >> 4) & 0xf];
        s[i * 2 + 1] = hex[data[i] & 0xf];
    }
    return s;
}

static std::string nowTime() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&t, &tm);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

// ============================================================
// Miner
// ============================================================
Miner::Miner() = default;
Miner::~Miner() { stop(); }

void Miner::start(const MinerConfig& cfg) {
    if (m_running.load()) return;

    m_numThreads = cfg.threads;
    m_benchmark = cfg.benchmarkNonces > 0;
    m_benchmarkNonces = cfg.benchmarkNonces;

    // Verbose share logging? LOG_SHARES=1 => print every share at info level.
    const char* envLog = getenv("LOG_SHARES");
    m_verboseShares =
        (envLog && envLog[0] != '\0' && std::string(envLog) != "0" &&
         std::string(envLog) != "false" && std::string(envLog) != "no");

    // Display mode. Full-screen dashboards need a real terminal in text mode;
    // otherwise everything falls back to plain log lines (docker/CI safe).
    const bool tty = isatty(STDOUT_FILENO) != 0 && !lg::jsonMode();
    if (!tty) {
        m_uiMode = UiMode::Log;
    } else if (cfg.ui == "ftxui") {
        m_uiMode = UiMode::Ftxui;
    } else if (cfg.ui == "ansi") {
        m_uiMode = UiMode::Ansi;
    } else if (cfg.ui == "log") {
        m_uiMode = UiMode::Log;
    } else {
        m_uiMode = UiMode::Ftxui;
    }
    m_dashboard = (m_uiMode == UiMode::Ansi);
    m_lastHrReport = std::chrono::steady_clock::now();

    if (!cfg.pools.empty()) {
        m_poolLabel = cfg.pools[0].host + ":" + std::to_string(cfg.pools[0].port);
        std::lock_guard<std::mutex> lock(m_stats.farm().mu);
        m_stats.farm().poolHost = m_poolLabel;
        m_stats.farm().wallet = cfg.pools[0].wallet;
        m_stats.farm().worker = cfg.pools[0].worker;
    }

    // --- RandomY flags (respect config, but never force unsupported flags) ---
    m_flags = randomx_get_flags();
    if (cfg.fullMem)
        m_flags = static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_FULL_MEM);
    else
        m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_FULL_MEM);
    if (cfg.largePages)
        m_flags = static_cast<randomx_flags>(m_flags | RANDOMX_FLAG_LARGE_PAGES);
    else
        m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_LARGE_PAGES);
    if (!cfg.useJIT) m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_JIT);
    if (!cfg.hardAES) m_flags = static_cast<randomx_flags>(m_flags & ~RANDOMX_FLAG_HARD_AES);

    lg::info("miner", "RandomY flags: JIT=" + std::to_string((m_flags & RANDOMX_FLAG_JIT) != 0) +
                           " FULL_MEM=" +
                           std::to_string((m_flags & RANDOMX_FLAG_FULL_MEM) != 0) +
                           " HARD_AES=" +
                           std::to_string((m_flags & RANDOMX_FLAG_HARD_AES) != 0) +
                           " LARGE_PAGES=" +
                           std::to_string((m_flags & RANDOMX_FLAG_LARGE_PAGES) != 0));

    initDataset();

    // --- Build workers (VMs + statsIdx ready, threads NOT started yet) ---
    int statsIdx = 0;
    for (int i = 0; i < m_numThreads; i++) {
        auto w = std::make_unique<Worker>();
        w->index = i;
        w->vm = randomx_create_vm(m_flags, m_cache, m_dataset);
        if (!w->vm) {
            lg::error("miner", "VM creation failed for worker " + std::to_string(i));
            continue;
        }
        w->statsIdx = statsIdx++;
        m_workers.push_back(std::move(w));
    }
    if (m_workers.empty()) {
        lg::error("miner", "No workers created, aborting");
        m_running.store(false);
        return;
    }
    m_stats.setWorkers(statsIdx);
    for (auto& w : m_workers) m_stats.worker(w->statsIdx).running = true;

    // Workers must only start once setWorkers() has populated the stats array
    // AND m_running is true, otherwise a worker can touch Stats::worker(idx)
    // (e.g. running=false on exit) before the vector exists -> out_of_range.
    m_running.store(true);
    if (m_uiMode != UiMode::Log)
        logEvent("miner started, " + std::to_string(m_workers.size()) + " threads");
    lg::info("miner", "Mining with " + std::to_string(m_workers.size()) + " workers");

    if (m_benchmark) {
        // Fixed benchmark job: workers hash this header for N nonces.
        std::vector<uint8_t> hdr(32);
        for (int i = 0; i < 32; i++) hdr[i] = (uint8_t)i;
        Job job;
        job.jobId = "benchmark";
        job.header = std::move(hdr);
        job.targetInt = 0x00ffffffffffffffULL;
        {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            m_job = std::make_shared<const Job>(job);
        }
        m_benchmarkStart = std::chrono::steady_clock::now();
        lg::info("bench", "Benchmarking " + std::to_string(m_benchmarkNonces) +
                               " nonces with " + std::to_string(m_workers.size()) + " threads");
    } else {
        // --- Stratum client + async submitter ---
        m_client = std::make_unique<StratumClient>(cfg.pools);
        m_client->setPollMs(cfg.pollMs);
        m_client->setJobCallback([this](const Job& job) { onNewJob(job); });
        m_client->setResultCallback(
            [this](bool ok, const std::string& reason, int delayMs, int workerIdx) {
                onShareResult(ok, reason, delayMs, workerIdx);
            });
        m_client->setPoolChangeCallback(
            [this](int idx, const std::string& host) { onPoolChange(idx, host); });

        m_submitter.start(
            [this](const Share& s) { return submitShareCallback(s); },
            [this](const Share& s, const std::string& reason) { dropShareCallback(s, reason); });

        m_client->connect();
        lg::info("miner", "Stratum client started");
    }

    // --- Start worker threads (job is available / will arrive shortly) ---
    for (auto& w : m_workers) w->thread = std::thread(&Miner::workerLoop, this, w.get());

    // --- Stats printer thread ---
    std::thread statsThread([this]() { statsLoop(); });
    statsThread.detach();
}

void Miner::stop() {
    // Ask the FTXUI dashboard to leave fullscreen (thread-safe).
    if (m_dash) m_dash->requestStop();
    if (m_dashboard) {
        // The detached stats thread may not have woken up yet.
        printf("\x1b[?25h\n");
        fflush(stdout);
    }
    if (!m_running.exchange(false)) {
        // Still need to release resources if start() partially failed.
        if (!m_workers.empty() || m_dataset || m_cache) {
            // Fall through to cleanup.
        } else {
            return;
        }
    }
    // Stop workers first so no new shares are pushed, then flush the submit
    // queue while the connection is still alive.
    for (auto& w : m_workers) {
        if (w->thread.joinable()) w->thread.join();
        if (w->vm) randomx_destroy_vm(w->vm);
    }
    m_submitter.stop();  // drain remaining queued shares
    m_workers.clear();

    if (m_client) {
        m_client->disconnect();
        m_client.reset();
    }

    if (m_dataset) {
        randomx_release_dataset(m_dataset);
        m_dataset = nullptr;
    }
    if (m_cache) {
        randomx_release_cache(m_cache);
        m_cache = nullptr;
    }
}

std::string Miner::finalSummary() const { return m_stats.finalSummary(); }

std::shared_ptr<const Job> Miner::loadJob() {
    std::lock_guard<std::mutex> lock(m_jobMutex);
    return m_job;  // shared_ptr copy: refcount keeps the snapshot alive
}

void Miner::initDataset() {
    // RandomY Core Coin fixed key: {'5','6','7','8','9'}
    const char key[] = {'5', '6', '7', '8', '9'};
    auto t1 = std::chrono::steady_clock::now();

    if (m_flags & RANDOMX_FLAG_FULL_MEM) {
        m_cache = randomx_alloc_cache(m_flags);
        if (!m_cache) {
            lg::error("miner", "Cache alloc failed (LARGE_PAGES unavailable? set LARGE_PAGES=0)");
            return;
        }
        randomx_init_cache(m_cache, key, sizeof(key));

        m_dataset = randomx_alloc_dataset(m_flags);
        if (!m_dataset) {
            lg::error("miner", "Dataset alloc failed");
            return;
        }
        uint32_t datasetItems = randomx_dataset_item_count();
        lg::info("miner", "Dataset items: " + std::to_string(datasetItems));

        // Parallel dataset init across all cores.
        int nthreads = std::min(m_numThreads, static_cast<int>(std::thread::hardware_concurrency()));
        if (nthreads < 1) nthreads = 1;
        uint32_t chunk = datasetItems / nthreads;
        std::vector<std::thread> initThreads;
        for (int t = 0; t < nthreads; t++) {
            uint32_t start = t * chunk;
            uint32_t count = (t == nthreads - 1) ? (datasetItems - start) : chunk;
            initThreads.emplace_back([this, start, count]() {
                randomx_init_dataset(m_dataset, m_cache, start, count);
            });
        }
        for (auto& th : initThreads) th.join();

        randomx_release_cache(m_cache);
        m_cache = nullptr;
    } else {
        m_cache = randomx_alloc_cache(m_flags);
        if (!m_cache) {
            lg::error("miner", "Cache alloc failed (LARGE_PAGES unavailable? set LARGE_PAGES=0)");
            return;
        }
        randomx_init_cache(m_cache, key, sizeof(key));
        lg::info("miner", "Light mode — using cache (no full dataset)");
    }

    auto t2 = std::chrono::steady_clock::now();
    auto initMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    lg::info("miner", "Dataset ready in " + std::to_string(initMs / 1000.0) + "s");
}

void Miner::onNewJob(const Job& job) {
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_job = std::make_shared<const Job>(job);
    }
    m_globalNonce.store(0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(m_stats.farm().mu);
        m_stats.farm().jobId = job.jobId;
        m_stats.farm().target = job.targetInt;
        m_stats.farm().difficulty =
            (job.targetInt <= 1) ? 0xffffffffffffffffULL
                                 : (uint64_t)(1.8446744073709552e19 / (double)job.targetInt);
    }
    lg::info("miner", "New job " + job.jobId);
    logEvent("new job " + job.jobId + " (diff " +
             std::to_string(m_stats.farm().difficulty) + ")");
}

void Miner::onShareResult(bool ok, const std::string& reason, int delayMs, int workerIdx) {
    auto& ws = m_stats.worker(workerIdx);
    if (ok) {
        ws.accepted.fetch_add(1, std::memory_order_relaxed);
        uint64_t n = ws.accepted.load(std::memory_order_relaxed);
        std::string msg = "Share accepted (W" + std::to_string(workerIdx) + ", " +
                          std::to_string(delayMs) + "ms) [#" + std::to_string(n) + "]";
        if (m_verboseShares)
            lg::good("miner", msg);
        else
            lg::debug("miner", msg);
        logEvent("accepted W" + std::to_string(workerIdx) + " (" + std::to_string(delayMs) +
                 "ms, #" + std::to_string(n) + ")");
    } else {
        ws.rejected.fetch_add(1, std::memory_order_relaxed);
        lg::warn("miner", "Share rejected (W" + std::to_string(workerIdx) + "): " + reason +
                               " (" + std::to_string(delayMs) + "ms)");
        logEvent("rejected W" + std::to_string(workerIdx) + ": " + reason);
    }
}

void Miner::onPoolChange(int idx, const std::string& host) {
    std::lock_guard<std::mutex> lock(m_stats.farm().mu);
    m_stats.farm().poolIndex = idx;
    m_stats.farm().poolHost = host;
    lg::info("stratum", "Active pool: " + host);
}

bool Miner::submitShareCallback(const Share& s) {
    // Drop shares mined on a job that is no longer current (stale).
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        if (m_job && m_job->jobId != s.jobId) {
            m_stats.worker(s.workerIdx).wasted.fetch_add(1, std::memory_order_relaxed);
            lg::debug("miner", "Share dropped: stale job " + s.jobId);
            return true;  // handled (counted as wasted)
        }
    }
    if (!m_client || !m_client->submitShare(s.headerHex, s.nonceHex, s.mixHex, s.workerIdx)) {
        m_stats.worker(s.workerIdx).wasted.fetch_add(1, std::memory_order_relaxed);
        lg::debug("miner", "Share dropped: no connection");
        return true;
    }
    return true;
}

void Miner::dropShareCallback(const Share& s, const std::string& reason) {
    m_stats.worker(s.workerIdx).wasted.fetch_add(1, std::memory_order_relaxed);
    lg::warn("miner", "Share dropped: " + reason);
    logEvent("wasted: " + reason);
}

void Miner::logEvent(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_eventsMutex);
    m_events.push_back(nowTime() + "  " + msg);
    if (m_events.size() > 8) m_events.pop_front();
}

std::vector<std::string> Miner::eventsSnapshot() const {
    std::lock_guard<std::mutex> lock(m_eventsMutex);
    return std::vector<std::string>(m_events.begin(), m_events.end());
}

void Miner::reportBenchmark() {
    if (m_benchmarkReported) return;
    m_benchmarkReported = true;
    auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
                       std::chrono::steady_clock::now() - m_benchmarkStart)
                       .count();
    double rate = elapsed > 0 ? (double)m_benchmarkNonces / elapsed : 0.0;
    std::ostringstream os;
    os << "Done " << m_benchmarkNonces << " nonces in " << elapsed << "s\n";
    for (auto& w : m_workers)
        os << "  W" << w->index << ": "
           << Stats::formatRate((double)m_stats.worker(w->index)
                                    .totalHashes.load(std::memory_order_relaxed) /
                                elapsed)
           << "\n";
    os << "Total: " << Stats::formatRate(rate);
    lg::info("bench", os.str());
}

void Miner::runFtxuiLoop() {
    Dashboard::Config dc;
    {
        std::lock_guard<std::mutex> lock(m_stats.farm().mu);
        dc.pool = m_stats.farm().poolHost;
        dc.wallet = m_stats.farm().wallet;
        dc.worker = m_stats.farm().worker;
    }
    dc.benchmark = m_benchmark;
    dc.benchTotal = m_benchmarkNonces;
    dc.benchDone = [this] { return m_benchmarkDone.load(std::memory_order_relaxed); };
    dc.events = [this] { return eventsSnapshot(); };
    dc.onTick = [this] {
        auto now = std::chrono::steady_clock::now();
        if (m_client && m_client->isConnected() &&
            std::chrono::duration_cast<std::chrono::seconds>(now - m_lastHrReport).count() >=
                60) {
            m_lastHrReport = now;
            m_client->submitHashrate((uint64_t)m_stats.farmCurrentRate());
        }
    };
    dc.onQuit = [this] { m_running.store(false, std::memory_order_relaxed); };

    m_dash = std::make_unique<Dashboard>(m_stats, std::move(dc));
    logEvent("dashboard: 1/2/3 tab, q quit");
    m_dash->run();  // blocks; refresh + hashrate reporting happen inside
    m_dash.reset();

    if (m_benchmark && m_benchmarkDone.load(std::memory_order_relaxed) >= m_benchmarkNonces) {
        reportBenchmark();
        m_running.store(false, std::memory_order_relaxed);
    }
}

// ============================================================
// WORKER LOOP — hot path: no heap, no locks per hash, no network
// ============================================================
void Miner::workerLoop(Worker* w) {
    const int idx = w->statsIdx;
    constexpr int BLOCKSIZE = 32;

    // Pin to a specific CPU core.
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        int ncpus = std::max(1, static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
        CPU_SET(idx % ncpus, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }
    // Higher scheduling priority (best effort).
    setpriority(PRIO_PROCESS, 0, -10);

    // Stack buffers — zero heap allocation in the hot path.
    uint8_t blob[40];    // header(32) + nonce LE(8)
    uint8_t seed[64];    // SHA3-512(blob) -> 64 bytes
    uint8_t hashout[32]; // RandomY -> 32 bytes

    // Warmup: verify VM works before mining.
    {
        uint8_t dummy_in[40] = {0};
        uint8_t dummy_seed[64];
        uint8_t dummy_out[32];
        sha3::sha3_512_40(dummy_in, dummy_seed);
        randomx_calculate_hash(w->vm, dummy_seed, 64, dummy_out);
    }

    // Take a snapshot of the current job (may be null until first job).
    auto current = loadJob();
    int idleSpins = 0;

    while (m_running.load(std::memory_order_relaxed)) {
        if (m_benchmark && m_benchmarkDone.load(std::memory_order_relaxed) >= m_benchmarkNonces)
            break;

        auto job = loadJob();
        if (!job) {
            idleSpins++;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(idleSpins > 10 ? 100 : 10));
            continue;
        }
        idleSpins = 0;

        // New job? Re-snapshot (header buffer stays alive via shared_ptr).
        if (job.get() != current.get()) current = std::move(job);

        const uint8_t* headerPtr = current->header.data();
        const uint64_t targetInt = current->targetInt;
        const std::string jobId = current->jobId;  // used only when a share is found

        // Grab a batch of nonces.
        uint64_t nonceBase = m_globalNonce.fetch_add(BLOCKSIZE, std::memory_order_relaxed);
        uint64_t blockHashes = 0;

        for (int i = 0; i < BLOCKSIZE && m_running.load(std::memory_order_relaxed); i++) {
            uint64_t nonce = nonceBase + i;

            // blob = header(32) || nonce LE(8)
            memcpy(blob, headerPtr, 32);
            memcpy(blob + 32, &nonce, 8);

            sha3::sha3_512_40(blob, seed);
            randomx_calculate_hash(w->vm, seed, 64, hashout);

            blockHashes++;

            // Target check: first 8 bytes of hash as big-endian uint64.
            uint64_t hashVal = 0;
            for (int b = 0; b < 8; b++) hashVal = (hashVal << 8) | hashout[b];

            if (hashVal < targetInt) {
                m_stats.worker(idx).sharesFound.fetch_add(1, std::memory_order_relaxed);
                if (m_benchmark) continue;  // no submission in benchmark

                uint64_t nonceBE = htobe64(nonce);
                Share s;
                s.headerHex = bytes_to_hex(headerPtr, 32);
                s.nonceHex = "0x" + bytes_to_hex(reinterpret_cast<const uint8_t*>(&nonceBE), 8);
                s.mixHex = "0x" + bytes_to_hex(hashout, 32);
                s.jobId = jobId;
                s.workerIdx = idx;
                s.foundAt = std::chrono::steady_clock::now();

                if (!m_submitter.push(std::move(s)))
                    m_stats.worker(idx).wasted.fetch_add(1, std::memory_order_relaxed);
            }
        }

        m_stats.worker(idx).totalHashes.fetch_add(blockHashes, std::memory_order_relaxed);
        m_stats.worker(idx).windowHashes.fetch_add(blockHashes, std::memory_order_relaxed);
        if (m_benchmark)
            m_benchmarkDone.fetch_add(blockHashes, std::memory_order_relaxed);
    }
    m_stats.worker(idx).running = false;
}

// ============================================================
// STATS LOOP — compact periodic output, share summary, hashrate report
// ============================================================
void Miner::statsLoop() {
    if (m_uiMode == UiMode::Ftxui) {
        runFtxuiLoop();
        return;
    }

    auto lastDetail = std::chrono::steady_clock::now();
    const int intervalSec = m_benchmark ? 1 : (m_dashboard ? 1 : 5);

    if (m_dashboard) {
        printf("\x1b[?25l");  // hide cursor while the dashboard is live
        fflush(stdout);
    }

    while (m_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(intervalSec));
        if (!m_running.load(std::memory_order_relaxed)) break;

        m_stats.sample();
        auto now = std::chrono::steady_clock::now();

        if (m_dashboard) {
            // Full-screen redraw.
            Stats::DashInfo d;
            {
                std::lock_guard<std::mutex> lock(m_stats.farm().mu);
                d.pool = m_stats.farm().poolHost;
                d.wallet = m_stats.farm().wallet;
                d.worker = m_stats.farm().worker;
            }
            {
                std::lock_guard<std::mutex> lock(m_eventsMutex);
                d.events.assign(m_events.begin(), m_events.end());
            }
            d.benchmark = m_benchmark;
            d.benchDone = m_benchmarkDone.load(std::memory_order_relaxed);
            d.benchTotal = m_benchmarkNonces;
            fputs(m_stats.dashboard(d).c_str(), stdout);
            fflush(stdout);
        } else {
            // Compact one-liner every interval.
            if (lg::jsonMode()) {
                printf("%s\n", m_stats.summaryJson().c_str());
                fflush(stdout);
            } else {
                lg::info("stats", m_stats.summaryLine());
            }

            // Detailed table every 60 s.
            auto sinceDetail =
                std::chrono::duration_cast<std::chrono::seconds>(now - lastDetail).count();
            if (sinceDetail >= 60) {
                lastDetail = now;
                lg::info("miner", m_stats.detailBlock());
            }
        }

        // eth_submitHashrate every 60 s.
        auto sinceHr = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastHrReport).count();
        if (m_client && m_client->isConnected() && sinceHr >= 60) {
            m_lastHrReport = now;
            m_client->submitHashrate((uint64_t)m_stats.farmCurrentRate());
        }

        // Benchmark completion.
        if (m_benchmark && !m_benchmarkReported &&
            m_benchmarkDone.load(std::memory_order_relaxed) >= m_benchmarkNonces) {
            reportBenchmark();
            m_running.store(false, std::memory_order_relaxed);
            break;
        }
    }

    if (m_dashboard) {
        printf("\x1b[?25h\n");  // restore cursor, fresh line for the exit logs
        fflush(stdout);
    }
}
