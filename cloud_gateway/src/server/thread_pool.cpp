#include "thread_pool.hpp"

namespace cg {

ThreadPool::ThreadPool(std::size_t n_threads) {
    workers_.reserve(n_threads);
    for (std::size_t i = 0; i < n_threads; ++i) {
        workers_.emplace_back([this]() {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [this]() { return stop_.load() || !queue_.empty(); });
                    if (stop_.load() && queue_.empty()) return;
                    task = std::move(queue_.front());
                    queue_.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    stop_.store(true);
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

} // namespace cg