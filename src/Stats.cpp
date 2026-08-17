#include "Stats.hpp"

#include "Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace {
std::string nowTs() {
    auto now = std::chrono::system_clock::now();
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    size_t len = strlen(buf);
    snprintf(buf + len, sizeof(buf) - len, ".%03u", (unsigned)ms.count());
    return buf;
}
}  // namespace

void Stats::setWorkers(size_t n) {
    m_workers.clear();
    m_workers.reserve(n);
    for (size_t i = 0; i < n; i++) m_workers.push_back(std::make_unique<WorkerStats>());
}

WorkerStats& Stats::worker(size_t i) { return *m_workers.at(i); }

void Stats::sample() {
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration_cast<std::chrono::duration<double>>(now - m_lastSample)
                    .count();
    if (dt <= 0) dt = 1e-6;
    m_lastSample = now;

    double inst = 0;
    for (auto& w : m_workers) {
        double rate = static_cast<double>(w->windowHashes.exchange(0)) / dt;
        w->currentRate = rate;
        inst += rate;
    }
    // Keep the last ~120 samples (2 min at 1 s sampling).
    {
        std::lock_guard<std::mutex> lock(m_rateMu);
        m_rateHistory.push_back(inst);
        if (m_rateHistory.size() > 120) m_rateHistory.erase(m_rateHistory.begin());
    }
}

double Stats::farmCurrentRate() const {
    // Smooth over the last 5 samples (a 5 s window at 1 s sampling) so the
    // displayed hashrate does not jump between 0 and 1 on slow RandomX VMs.
    std::lock_guard<std::mutex> lock(m_rateMu);
    const size_t n = std::min<size_t>(5, m_rateHistory.size());
    if (n == 0) return 0.0;
    double total = 0;
    for (size_t i = m_rateHistory.size() - n; i < m_rateHistory.size(); i++)
        total += m_rateHistory[i];
    return total / static_cast<double>(n);
}

double Stats::farmBestRate() const {
    std::lock_guard<std::mutex> lock(m_rateMu);
    if (m_rateHistory.empty()) return 0.0;
    return *std::max_element(m_rateHistory.begin(), m_rateHistory.end());
}

std::vector<double> Stats::rateHistory() const {
    std::lock_guard<std::mutex> lock(m_rateMu);
    return m_rateHistory;
}

double Stats::farmAvgRate() const {
    auto elapsed = std::chrono::steady_clock::now() - m_farm.start;
    double sec = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
    if (sec <= 0) sec = 1e-6;
    return static_cast<double>(farmTotalHashes()) / sec;
}

uint64_t Stats::farmTotalHashes() const {
    uint64_t total = 0;
    for (const auto& w : m_workers) total += w->totalHashes.load(std::memory_order_relaxed);
    return total;
}

uint64_t Stats::farmAccepted() const {
    uint64_t t = 0;
    for (const auto& w : m_workers) t += w->accepted.load(std::memory_order_relaxed);
    return t;
}

uint64_t Stats::farmRejected() const {
    uint64_t t = 0;
    for (const auto& w : m_workers) t += w->rejected.load(std::memory_order_relaxed);
    return t;
}

uint64_t Stats::farmWasted() const {
    uint64_t t = 0;
    for (const auto& w : m_workers) t += w->wasted.load(std::memory_order_relaxed);
    return t;
}

uint64_t Stats::farmSharesFound() const {
    uint64_t t = 0;
    for (const auto& w : m_workers) t += w->sharesFound.load(std::memory_order_relaxed);
    return t;
}

double Stats::acceptRatio() const {
    uint64_t a = farmAccepted();
    uint64_t r = farmRejected();
    uint64_t total = a + r;
    return total ? 100.0 * static_cast<double>(a) / static_cast<double>(total) : 0.0;
}

std::string Stats::formatRate(double h) {
    char buf[32];
    if (h >= 1e9) snprintf(buf, sizeof(buf), "%.2f GH/s", h / 1e9);
    else if (h >= 1e6) snprintf(buf, sizeof(buf), "%.2f MH/s", h / 1e6);
    else if (h >= 1e3) snprintf(buf, sizeof(buf), "%.2f kH/s", h / 1e3);
    else snprintf(buf, sizeof(buf), "%.2f H/s", h);
    return buf;
}

std::string Stats::formatCount(uint64_t n) {
    char buf[32];
    if (n >= 1'000'000'000ULL) snprintf(buf, sizeof(buf), "%.2fB", n / 1e9);
    else if (n >= 1'000'000ULL) snprintf(buf, sizeof(buf), "%.2fM", n / 1e6);
    else if (n >= 1'000ULL) snprintf(buf, sizeof(buf), "%.2fK", n / 1e3);
    else snprintf(buf, sizeof(buf), "%llu", (unsigned long long)n);
    return buf;
}

std::string Stats::formatDuration(std::chrono::steady_clock::duration d) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(d).count();
    char buf[32];
    snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", (long long)(secs / 3600),
             (long long)((secs % 3600) / 60), (long long)(secs % 60));
    return buf;
}

static std::string fmtDifficulty(uint64_t target) {
    // difficulty = 2^64 / target (target = first 8 bytes of pool target)
    if (!target) return "?";
    double diff = (target <= 1) ? 1.8446744073709552e19 : 1.8446744073709552e19 / (double)target;
    char buf[32];
    if (diff >= 1e9) snprintf(buf, sizeof(buf), "%.2fG", diff / 1e9);
    else if (diff >= 1e6) snprintf(buf, sizeof(buf), "%.2fM", diff / 1e6);
    else if (diff >= 1e3) snprintf(buf, sizeof(buf), "%.2fK", diff / 1e3);
    else snprintf(buf, sizeof(buf), "%.0f", diff);
    return buf;
}

std::string Stats::summaryLine() const {
    std::lock_guard<std::mutex> lock(m_farm.mu);
    std::ostringstream os;
    char buf[128];
    snprintf(buf, sizeof(buf), "HR %s (avg %s) | A %llu R %llu W %llu | diff %s | %s | job %.10s",
             formatRate(farmCurrentRate()).c_str(), formatRate(farmAvgRate()).c_str(),
             (unsigned long long)farmAccepted(), (unsigned long long)farmRejected(),
             (unsigned long long)farmWasted(), fmtDifficulty(m_farm.target).c_str(),
             m_farm.poolHost.empty() ? "-" : m_farm.poolHost.c_str(), m_farm.jobId.c_str());
    os << buf;
    return os.str();
}

std::string Stats::summaryJson() const {
    std::lock_guard<std::mutex> lock(m_farm.mu);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ts\":\"%s\",\"level\":\"info\",\"tag\":\"stats\",\"hashrate\":%.1f,"
             "\"hashrate_avg\":%.1f,\"accepted\":%llu,\"rejected\":%llu,\"wasted\":%llu,"
             "\"found\":%llu,\"accept_pct\":%.1f,\"diff\":%llu,\"pool\":\"%s\",\"job\":\"%.10s\"}",
             nowTs().c_str(), farmCurrentRate(), farmAvgRate(), (unsigned long long)farmAccepted(),
             (unsigned long long)farmRejected(), (unsigned long long)farmWasted(),
             (unsigned long long)farmSharesFound(), acceptRatio(),
             (unsigned long long)m_farm.difficulty, m_farm.poolHost.c_str(), m_farm.jobId.c_str());
    return buf;
}

std::string Stats::detailBlock() const {
    std::lock_guard<std::mutex> lock(m_farm.mu);
    std::ostringstream os;
    os << "=== MINER ===\n";
    os << "Uptime " << formatDuration(std::chrono::steady_clock::now() - m_farm.start)
       << " | Pool " << m_farm.poolHost << " | job " << m_farm.jobId << "\n";
    for (size_t i = 0; i < m_workers.size(); i++) {
        const auto& w = m_workers[i];
        char buf[192];
        snprintf(buf, sizeof(buf), "  W%zu: %s | %s total | A %llu R %llu W %llu", i,
                 formatRate(w->currentRate).c_str(),
                 formatRate((double)w->totalHashes.load(std::memory_order_relaxed)).c_str(),
                 (unsigned long long)w->accepted.load(std::memory_order_relaxed),
                 (unsigned long long)w->rejected.load(std::memory_order_relaxed),
                 (unsigned long long)w->wasted.load(std::memory_order_relaxed));
        os << buf << "\n";
    }
    char buf[192];
    snprintf(buf, sizeof(buf), "Farm: %s current, %s avg | A %llu (%.1f%%) R %llu W %llu F %llu",
             formatRate(farmCurrentRate()).c_str(), formatRate(farmAvgRate()).c_str(),
             (unsigned long long)farmAccepted(), acceptRatio(), (unsigned long long)farmRejected(),
             (unsigned long long)farmWasted(), (unsigned long long)farmSharesFound());
    os << buf << "\n";
    os << "=================";
    return os.str();
}

std::string Stats::finalSummary() const {
    std::lock_guard<std::mutex> lock(m_farm.mu);
    std::ostringstream os;
    auto elapsed = std::chrono::steady_clock::now() - m_farm.start;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "Session %s | Total %llu hashes | Avg %s | Shares A %llu R %llu W %llu (found %llu, "
             "accept %.1f%%)",
             formatDuration(elapsed).c_str(),
             (unsigned long long)farmTotalHashes(),
             formatRate(farmAvgRate()).c_str(), (unsigned long long)farmAccepted(),
             (unsigned long long)farmRejected(), (unsigned long long)farmWasted(),
             (unsigned long long)farmSharesFound(), acceptRatio());
    os << buf;
    return os.str();
}

// ============================================================
// Live dashboard (TTY)
// ============================================================

std::string Stats::sparkline(size_t max) const {
    std::lock_guard<std::mutex> lock(m_rateMu);
    if (m_rateHistory.empty()) return "";
    size_t n = std::min(max, m_rateHistory.size());
    size_t off = m_rateHistory.size() - n;
    double peak = *std::max_element(m_rateHistory.begin() + off, m_rateHistory.end());
    if (peak <= 0) return "";
    static const char bars[] = "\xe2\x96\x81\xe2\x96\x82\xe2\x96\x83\xe2\x96\x84"  // ▁▂▃▄
                               "\xe2\x96\x85\xe2\x96\x86\xe2\x96\x87\xe2\x96\x88";  // ▅▆▇█
    std::string s;
    s.reserve(n);
    for (size_t i = off; i < m_rateHistory.size(); i++) {
        int level = static_cast<int>(m_rateHistory[i] / peak * 7.999);
        if (level < 0) level = 0;
        if (level > 7) level = 7;
        s += std::string(bars + level * 3, 3);
    }
    return s;
}

std::string Stats::dashboard(const DashInfo& d) const {
    std::lock_guard<std::mutex> lock(m_farm.mu);
    const bool col = lg::colorEnabled();
    auto C = [col](const char* code) { return col ? code : ""; };
    auto R = [col]() { return col ? "\x1b[0m" : ""; };

    std::ostringstream os;
    // Move cursor home and erase the display, then draw.
    os << "\x1b[H\x1b[J";

    // ---- Header ----
    os << C("\x1b[96m") << "  miner-saya v2  |  RandomY" << R();
    if (!d.pool.empty()) os << "  |  pool " << d.pool;
    if (!d.wallet.empty()) {
        std::string w = d.wallet.size() > 16 ? d.wallet.substr(0, 16) + "..." : d.wallet;
        os << "  |  wallet " << w;
    }
    if (!d.worker.empty() && d.worker != "worker") os << "  |  worker " << d.worker;
    os << "\n";

    os << C("\x1b[90m") << "  diff " << R() << fmtDifficulty(m_farm.target) << C("\x1b[90m")
       << "  |  job " << R() << (m_farm.jobId.empty() ? "-" : m_farm.jobId)
       << C("\x1b[90m") << "  |  uptime " << R()
       << formatDuration(std::chrono::steady_clock::now() - m_farm.start) << "\n";
    os << "\n";

    // ---- Hashrate / benchmark ----
    if (d.benchmark) {
        os << C("\x1b[93m") << "  BENCHMARK" << R() << "  ";
        if (d.benchTotal) {
            double pct = 100.0 * static_cast<double>(d.benchDone) / static_cast<double>(d.benchTotal);
            const int w = 20;
            int filled = static_cast<int>(pct / 100.0 * w + 0.5);
            if (filled > w) filled = w;
            os << std::string(filled, '#') << std::string(w - filled, '.') << " "
               << std::to_string((int)pct) << "%  ";
        }
        os << formatCount(d.benchDone) << " / " << formatCount(d.benchTotal) << " nonces  |  "
           << formatRate(farmCurrentRate()) << "\n\n";
    } else {
        os << C("\x1b[92m") << "  HASHRATE" << R() << "  cur "
           << formatRate(farmCurrentRate()) << "  |  avg " << formatRate(farmAvgRate())
           << "  |  best " << formatRate(farmBestRate()) << "\n";
        std::string sp = sparkline();
        if (!sp.empty()) os << "  " << C("\x1b[36m") << sp << R() << "\n";
        os << "\n";
    }

    // ---- Per-thread table ----
    os << C("\x1b[96m") << "  THREADS" << R() << "\n";
    for (size_t i = 0; i < m_workers.size(); i++) {
        const auto& w = m_workers[i];
        char buf[192];
        snprintf(buf, sizeof(buf), "    T%zu  %-9s  total %-7s  A %llu  R %llu  W %llu", i,
                 formatRate(w->currentRate).c_str(),
                 formatCount(w->totalHashes.load(std::memory_order_relaxed)).c_str(),
                 (unsigned long long)w->accepted.load(std::memory_order_relaxed),
                 (unsigned long long)w->rejected.load(std::memory_order_relaxed),
                 (unsigned long long)w->wasted.load(std::memory_order_relaxed));
        os << buf << "\n";
    }
    os << "\n";

    // ---- Shares + events ----
    os << C("\x1b[96m") << "  SHARES" << R() << "  A " << C("\x1b[92m") << farmAccepted()
       << R() << " (" << std::fixed << std::setprecision(1) << acceptRatio() << "%)  "
       << "R " << C("\x1b[91m") << farmRejected() << R() << "  W " << farmWasted()
       << "  found " << farmSharesFound() << "  |  total " << formatCount(farmTotalHashes())
       << " hashes\n";

    if (d.events.empty()) {
        os << C("\x1b[90m") << "  events: (none yet)" << R() << "\n";
    } else {
        os << C("\x1b[96m") << "  EVENTS" << R() << "\n";
        for (const auto& e : d.events) os << "    " << e << "\n";
    }

    return os.str();
}
