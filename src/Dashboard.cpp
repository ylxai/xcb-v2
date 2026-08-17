#include "Dashboard.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace ftxui;

// ---------------------------------------------------------------------------
// Dashboard::Impl — owns the FTXUI screen. All FTXUI types live here so the
// header stays light and Miner.cpp never sees them.
// ---------------------------------------------------------------------------
struct Dashboard::Impl {
    std::atomic<ScreenInteractive*> screen{nullptr};

    void requestExit() {
        if (ScreenInteractive* s = screen.load()) s->Exit();
    }

    // ---- Rendering ------------------------------------------------------
    Element render(Dashboard& d) {
        Stats& st = d.m_stats;
        const bool col = true;  // FTXUI handles color itself

        // ---- Header ----
        auto header = vbox({
            hbox({
                color(Color::Cyan, bold(text(" miner-saya v2 "))),
                text("| "),
                color(Color::Green, text("RandomY")),
                filler(),
            }),
            hbox({
                text(" pool "),
                bold(text(d.m_cfg.pool.empty() ? "-" : d.m_cfg.pool)),
                text("  wallet "),
                bold(text(maskWallet(d.m_cfg.wallet))),
                text("  worker "),
                bold(text(d.m_cfg.worker.empty() ? "-" : d.m_cfg.worker)),
                filler(),
            }),
            hbox({
                text(" diff "),
                bold(text(fmtDiff(st.farm().target))),
                text("  job "),
                bold(text(d.m_cfg.benchmark ? "benchmark"
                                            : (st.farm().jobId.empty() ? "-" : st.farm().jobId))),
                text("  uptime "),
                bold(text(Stats::formatDuration(std::chrono::steady_clock::now() - st.farm().start))),
                filler(),
            }),
        });

        // ---- Tab bar ----
        auto tabBtn = [&](int idx, const char* label) {
            Element el = text(label);
            if (d.m_tab == idx) el = color(Color::Black, bgcolor(Color::Cyan, el));
            return el;
        };
        auto tabs = hbox({
            tabBtn(0, " 1 Overview "),
            text(" "),
            tabBtn(1, " 2 Threads "),
            text(" "),
            tabBtn(2, " 3 Shares "),
            filler(),
            dim(text(" [1/2/3] switch tab  [q] quit ")),
        });

        // ---- Content ----
        Element content;
        switch (d.m_tab) {
            case 0: content = tabOverview(d, st); break;
            case 1: content = tabThreads(d, st); break;
            default: content = tabShares(d, st); break;
        }

        // ---- Footer ----
        auto footer = hbox({
            dim(text(" A accepted  R rejected  W wasted  F found ")),
            filler(),
            dim(text(Stats::formatRate(st.farmCurrentRate()) + " now")),
        });

        return border(vbox({
            header,
            separator(),
            tabs,
            separator(),
            flex(content),
            separator(),
            footer,
        }));
    }

    Element tabOverview(Dashboard& d, Stats& st) {
        Elements els;

        // Benchmark progress or hashrate.
        if (d.m_cfg.benchmark) {
            uint64_t done = d.m_cfg.benchDone ? d.m_cfg.benchDone() : 0;
            double pct = d.m_cfg.benchTotal ? 100.0 * (double)done / (double)d.m_cfg.benchTotal : 0.0;
            els.push_back(hbox({
                color(Color::Yellow, bold(text(" BENCHMARK "))),
                filler(),
                text(Stats::formatCount(done) + " / " + Stats::formatCount(d.m_cfg.benchTotal) +
                     " nonces  "),
            }));
            els.push_back(gauge((float)(pct / 100.0)));
            els.push_back(hbox({
                filler(),
                bold(text(std::to_string((int)pct) + "%  ")),
                text(Stats::formatRate(st.farmCurrentRate())),
                filler(),
            }));
            els.push_back(separator());
        } else {
            els.push_back(hbox({
                color(Color::Green, bold(text(" HASHRATE "))),
                filler(),
            }));
            els.push_back(hbox({
                text("  cur "), bold(text(Stats::formatRate(st.farmCurrentRate()))),
                text("   avg "), text(Stats::formatRate(st.farmAvgRate())),
                text("   best "), text(Stats::formatRate(st.farmBestRate())),
                filler(),
            }));
            std::string sp = st.sparkline(48);
            if (!sp.empty())
                els.push_back(hbox({ text("  "), color(Color::RGB(0x00, 0xaf, 0xaf), text(sp)),
                                     filler() }));
            els.push_back(separator());
        }

        // Summary line.
        els.push_back(hbox({
            text("  total "), bold(text(Stats::formatCount(st.farmTotalHashes()))),
            text(" hashes"),
            filler(),
            text(" A "), color(Color::Green, bold(text(std::to_string(st.farmAccepted())))),
            text(" R "), color(Color::Red, bold(text(std::to_string(st.farmRejected())))),
            text(" W "), bold(text(std::to_string(st.farmWasted()))),
            text(" F "), bold(text(std::to_string(st.farmSharesFound()))),
            text("  "), text(Stats::formatRate(st.farmAvgRate()) + " avg"),
            filler(),
        }));

        // Recent events (also shown here so Overview stays informative).
        els.push_back(separator());
        auto ev = d.m_cfg.events ? d.m_cfg.events() : std::vector<std::string>{};
        if (ev.empty()) {
            els.push_back(hbox({ dim(text("  events: (none yet)")), filler() }));
        } else {
            els.push_back(color(Color::Cyan, bold(text(" EVENTS "))));
            size_t n = std::min<size_t>(ev.size(), 5);
            for (size_t i = ev.size() - n; i < ev.size(); i++)
                els.push_back(hbox({ text("   " + ev[i]), filler() }));
        }
        return vbox(els);
    }

    Element tabThreads(Dashboard& d, Stats& st) {
        Elements els;
        els.push_back(hbox({ color(Color::Cyan, bold(text(" THREADS "))), filler(),
                             dim(text(" rate is smoothed over ~5 s ")) }));
        els.push_back(separator());
        for (size_t i = 0; i < st.numWorkers(); i++) {
            const auto& w = st.worker(i);
            char buf[192];
            snprintf(buf, sizeof(buf), "   T%zu", i);
            els.push_back(hbox({
                text(buf),
                filler(),
                text(Stats::formatRate(w.currentRate.load())),
                text("  total "),
                text(Stats::formatCount(w.totalHashes.load(std::memory_order_relaxed))),
                text("  A "),
                color(Color::Green,
                      text(std::to_string(w.accepted.load(std::memory_order_relaxed)))),
                text("  R "),
                color(Color::Red,
                      text(std::to_string(w.rejected.load(std::memory_order_relaxed)))),
                text("  W "),
                text(std::to_string(w.wasted.load(std::memory_order_relaxed))),
                filler(),
            }));
        }
        els.push_back(separator());
        els.push_back(hbox({
            text("   farm: "),
            text(Stats::formatRate(st.farmCurrentRate())),
            text(" current, "),
            text(Stats::formatRate(st.farmAvgRate())),
            text(" avg over "),
            text(Stats::formatDuration(std::chrono::steady_clock::now() - st.farm().start)),
            filler(),
        }));
        return vbox(els);
    }

    Element tabShares(Dashboard& d, Stats& st) {
        char pctbuf[16];
        snprintf(pctbuf, sizeof(pctbuf), "%.1f%%", st.acceptRatio());
        Elements els;
        els.push_back(hbox({ color(Color::Cyan, bold(text(" SHARES "))), filler(),
                             dim(text(std::string(" accept ") + pctbuf)) }));
        els.push_back(separator());
        els.push_back(hbox({
            text("  accepted "), color(Color::Green, bold(text(std::to_string(st.farmAccepted())))),
            text("   rejected "), color(Color::Red, bold(text(std::to_string(st.farmRejected())))),
            text("   wasted "), bold(text(std::to_string(st.farmWasted()))),
            text("   found "), bold(text(std::to_string(st.farmSharesFound()))),
            filler(),
        }));
        els.push_back(separator());

        els.push_back(color(Color::Cyan, bold(text(" EVENTS "))));
        auto ev = d.m_cfg.events ? d.m_cfg.events() : std::vector<std::string>{};
        if (ev.empty()) {
            els.push_back(hbox({ dim(text("   (none yet)")), filler() }));
        } else {
            for (const auto& e : ev) els.push_back(hbox({ text("   " + e), filler() }));
        }
        els.push_back(separator());

        // Hashrate history as text bars (last 48 samples).
        els.push_back(hbox({ color(Color::Cyan, bold(text(" RATE HISTORY "))), filler(),
                             dim(text(" last " + std::to_string(48) + " s ")) }));
        std::string sp = st.sparkline(48);
        if (!sp.empty())
            els.push_back(hbox({ text("  "), color(Color::RGB(0x00, 0xaf, 0xaf), text(sp)),
                                 filler() }));
        else
            els.push_back(hbox({ dim(text("  (sampling...)")), filler() }));
        return vbox(els);
    }

    // ---- Helpers --------------------------------------------------------
    static std::string maskWallet(const std::string& w) {
        if (w.empty()) return "-";
        return w.size() > 16 ? w.substr(0, 16) + "..." : w;
    }

    static std::string fmtDiff(uint64_t target) {
        if (!target) return "?";
        double diff = (target <= 1) ? 1.8446744073709552e19 : 1.8446744073709552e19 / (double)target;
        char buf[32];
        if (diff >= 1e9) snprintf(buf, sizeof(buf), "%.2fG", diff / 1e9);
        else if (diff >= 1e6) snprintf(buf, sizeof(buf), "%.2fM", diff / 1e6);
        else if (diff >= 1e3) snprintf(buf, sizeof(buf), "%.2fK", diff / 1e3);
        else snprintf(buf, sizeof(buf), "%.0f", diff);
        return buf;
    }

    // ---- Input ----------------------------------------------------------
    bool onEvent(Dashboard& d, Event e) {
        if (e == Event::Character('q') || e == Event::Character('Q') ||
            e == Event::Character('x') || e == Event::Escape) {
            if (d.m_cfg.onQuit) d.m_cfg.onQuit();
            d.requestStop();
            return true;
        }
        if (e == Event::Character('1')) { d.m_tab = 0; return true; }
        if (e == Event::Character('2')) { d.m_tab = 1; return true; }
        if (e == Event::Character('3')) { d.m_tab = 2; return true; }
        return false;
    }
};

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------
Dashboard::Dashboard(Stats& stats, Config cfg)
    : m_stats(stats), m_cfg(std::move(cfg)), m_impl(std::make_unique<Impl>()) {}

Dashboard::~Dashboard() = default;

void Dashboard::run() {
    ScreenInteractive screen = ScreenInteractive::Fullscreen();
    m_impl->screen.store(&screen);

    auto component = CatchEvent(Renderer([this] { return m_impl->render(*this); }),
                                [this](Event e) { return m_impl->onEvent(*this, e); });

    // 1 s refresh: sample the rolling window, run the per-tick callback, then
    // ask the loop to redraw. Benchmark completion also ends the loop here.
    std::thread refresher([this, &screen] {
        while (!m_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (m_stop.load()) break;
            m_stats.sample();
            if (m_cfg.onTick) m_cfg.onTick();
            if (m_cfg.benchmark && m_cfg.benchDone &&
                m_cfg.benchDone() >= m_cfg.benchTotal) {
                screen.Exit();
                break;
            }
            screen.PostEvent(Event::Custom);
        }
    });

    if (m_stop.load()) screen.Exit();  // stop was requested before Loop started
    screen.Loop(component);

    m_stop.store(true);
    refresher.join();
    m_impl->screen.store(nullptr);
}

void Dashboard::requestStop() {
    m_stop.store(true);
    m_impl->requestExit();
}
