/*!
 * @file event_loop.hpp
 * @brief Cross-platform event loop abstraction.
 *
 * On Linux/macOS this wraps epoll. On Windows it wraps WSAEventSelect /
 * select(). The interface (add/mod/del/run) is identical so the rest of
 * the gateway does not care.
 */
#pragma once

#include <cstdint>
#include <functional>

namespace cg {

class EventLoop {
public:
    using Callback = std::function<void(int fd, std::uint32_t events)>;

    EventLoop();
    ~EventLoop();

    // Returns false on fatal init failure.
    bool init();
    void shutdown();

    // Add a fd with the given events (EPOLLIN / EPOLLOUT equivalents).
    // The user-data pointer is opaque (we use fd itself as the key).
    void add(int fd, std::uint32_t events);
    void mod(int fd, std::uint32_t events);
    void del(int fd);

    // Block up to timeout_ms and dispatch events to cb.
    void run_once(int timeout_ms, Callback cb);

    // Convenience: run forever.
    void run(Callback cb);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace cg