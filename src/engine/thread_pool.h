#pragma once

// A tiny persistent thread pool for data-parallel loops. Threads are created
// once, so a parallel scan per keystroke costs microseconds of overhead
// instead of thread start-up time.

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "core/hardware.h"

namespace engine {

class ThreadPool {
public:
    explicit ThreadPool(unsigned threads = 0) {
        if (threads == 0) threads = hw::search_threads();  // every core this machine has
        for (unsigned i = 1; i < threads; ++i) workers_.emplace_back([this] { worker(); });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned size() const noexcept { return static_cast<unsigned>(workers_.size()) + 1; }

    // Runs fn(i) for every i in [0, n), on the pool plus the calling thread.
    // Returns when all calls have finished.
    void parallel_for(size_t n, const std::function<void(size_t)>& fn) {
        if (n == 0) return;
        if (n == 1 || workers_.empty()) {
            for (size_t i = 0; i < n; ++i) fn(i);
            return;
        }
        std::lock_guard<std::mutex> run_lock(run_m_);  // one loop at a time
        {
            std::lock_guard<std::mutex> lk(m_);
            job_ = &fn;
            job_n_ = n;
            next_.store(0, std::memory_order_relaxed);
            pending_ = workers_.size();
            ++generation_;
        }
        cv_.notify_all();
        for (size_t i; (i = next_.fetch_add(1, std::memory_order_relaxed)) < n;) fn(i);
        std::unique_lock<std::mutex> lk(m_);
        done_cv_.wait(lk, [this] { return pending_ == 0; });
        job_ = nullptr;
    }

private:
    void worker() {
        uint64_t seen = 0;
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            const auto* fn = job_;
            const size_t n = job_n_;
            lk.unlock();
            for (size_t i; (i = next_.fetch_add(1, std::memory_order_relaxed)) < n;) (*fn)(i);
            lk.lock();
            if (--pending_ == 0) done_cv_.notify_one();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex m_, run_m_;
    std::condition_variable cv_, done_cv_;
    const std::function<void(size_t)>* job_ = nullptr;
    size_t job_n_ = 0;
    std::atomic<size_t> next_{0};
    size_t pending_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

}  // namespace engine
