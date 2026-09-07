#include "Submitter.hpp"

#include <atomic>

Submitter::~Submitter() { stop(); }

void Submitter::start(SubmitFn submit, DropFn drop) {
    if (m_running.load()) return;
    m_submit = std::move(submit);
    m_drop = std::move(drop);
    m_running.store(true);
    m_thread = std::thread(&Submitter::run, this);
}

void Submitter::stop() {
    if (!m_running.exchange(false)) return;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_q.clear();
    }
}

bool Submitter::push(Share&& s) {
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (m_q.size() >= kMaxQueue) return false;
        m_q.push_back(std::move(s));
    }
    m_cv.notify_one();
    return true;
}

size_t Submitter::queueLen() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_q.size();
}

void Submitter::run() {
    while (true) {
        Share s;
        {
            std::unique_lock<std::mutex> lock(m_mu);
            m_cv.wait(lock, [&] { return !m_running.load() || !m_q.empty(); });
            if (!m_running.load() && m_q.empty()) return;
            s = std::move(m_q.front());
            m_q.pop_front();
        }
        if (m_submit) {
            bool sent = m_submit(s);
            if (!sent && m_drop) m_drop(s, "no connection");
        }
    }
}
