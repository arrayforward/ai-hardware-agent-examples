/*!
 * @file thread_pool.hpp
 * @brief Fixed-size thread pool for CPU-bound tasks (ASR/LLM/TTS).
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace cg {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t n_threads);
    ~ThreadPool();

    // Submit a callable; returns a future for the result.
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(m_);
            queue_.emplace([task]() { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    std::size_t size() const noexcept { return workers_.size(); }

private:
    std::vector<std::thread>     workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex                   m_;
    std::condition_variable      cv_;
    std::atomic<bool>            stop_{false};
};

} // namespace cg