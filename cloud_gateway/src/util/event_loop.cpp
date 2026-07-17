#include "event_loop.hpp"
#include "cloud_gateway/log.hpp"

#include <cstring>
#include <unordered_map>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
using epoll_event  = void;        // unused on Windows
constexpr std::uint32_t EPOLLIN  = 0x001;
constexpr std::uint32_t EPOLLOUT = 0x004;
constexpr std::uint32_t EPOLLERR = 0x008;
constexpr std::uint32_t EPOLLHUP = 0x010;
#else
#  include <sys/epoll.h>
#  include <unistd.h>
#endif

namespace cg {

struct EventLoop::Impl {
#ifdef _WIN32
    SOCKET              wakeup_send = INVALID_SOCKET;
    SOCKET              wakeup_recv = INVALID_SOCKET;
    std::unordered_map<int, std::uint32_t> watch;   // fd -> events mask
#else
    int                 epfd = -1;
#endif
};

EventLoop::EventLoop()  { impl_ = new Impl(); }
EventLoop::~EventLoop() { shutdown(); delete impl_; }

bool EventLoop::init() {
#ifdef _WIN32
    static bool wsa_inited = false;
    if (!wsa_inited) {
        WSADATA wsad;
        if (WSAStartup(MAKEWORD(2, 2), &wsad) != 0) {
            GATEWAY_LOG_ERROR("WSAStartup failed");
            return false;
        }
        wsa_inited = true;
    }
    // Use socketpair-like trick to wake the loop with select().
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        GATEWAY_LOG_ERROR("socket failed: %d", WSAGetLastError());
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listener, 1);

    socklen_t alen = sizeof(addr);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &alen);
    SOCKET c = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr2 = addr;
    ::connect(c, reinterpret_cast<sockaddr*>(&addr2), sizeof(addr2));
    SOCKET s = ::accept(listener, nullptr, nullptr);
    ::closesocket(listener);

    impl_->wakeup_send = c;
    impl_->wakeup_recv = s;
    u_long nb = 1;
    ioctlsocket(impl_->wakeup_recv, FIONBIO, &nb);
    return true;
#else
    impl_->epfd = ::epoll_create1(0);
    if (impl_->epfd < 0) {
        GATEWAY_LOG_ERROR("epoll_create1 failed: %s", std::strerror(errno));
        return false;
    }
    return true;
#endif
}

void EventLoop::shutdown() {
#ifdef _WIN32
    if (impl_->wakeup_send != INVALID_SOCKET) ::closesocket(impl_->wakeup_send);
    if (impl_->wakeup_recv != INVALID_SOCKET) ::closesocket(impl_->wakeup_recv);
    impl_->wakeup_send = impl_->wakeup_recv = INVALID_SOCKET;
    impl_->watch.clear();
#else
    if (impl_->epfd >= 0) ::close(impl_->epfd);
    impl_->epfd = -1;
#endif
}

void EventLoop::add(int fd, std::uint32_t events) {
#ifdef _WIN32
    impl_->watch[fd] = events;
#else
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    ::epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, fd, &ev);
#endif
}

void EventLoop::mod(int fd, std::uint32_t events) {
#ifdef _WIN32
    impl_->watch[fd] = events;
#else
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    ::epoll_ctl(impl_->epfd, EPOLL_CTL_MOD, fd, &ev);
#endif
}

void EventLoop::del(int fd) {
#ifdef _WIN32
    impl_->watch.erase(fd);
#else
    ::epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, fd, nullptr);
#endif
}

void EventLoop::run_once(int timeout_ms, Callback cb) {
#ifdef _WIN32
    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
    SOCKET maxfd = 0;
    for (auto& [fd, ev] : impl_->watch) {
        if (ev & EPOLLIN)  FD_SET(fd, &rfds);
        if (ev & EPOLLOUT) FD_SET(fd, &wfds);
        FD_SET(fd, &efds);
        if (fd > (int)maxfd) maxfd = (SOCKET)fd;
    }
    timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int n = ::select((int)maxfd + 1, &rfds, &wfds, &efds, &tv);
    if (n < 0) {
        int e = WSAGetLastError();
        if (e != WSAEINTR) GATEWAY_LOG_WARN("select errno=%d", e);
        return;
    }
    if (n == 0) return;
    std::vector<int> fds;
    fds.reserve(impl_->watch.size());
    for (auto& [fd, ev] : impl_->watch) fds.push_back(fd);
    for (int fd : fds) {
        std::uint32_t ev = 0;
        if (FD_ISSET(fd, &rfds)) ev |= EPOLLIN;
        if (FD_ISSET(fd, &wfds)) ev |= EPOLLOUT;
        if (FD_ISSET(fd, &efds)) ev |= EPOLLERR | EPOLLHUP;
        if (ev) cb(fd, ev);
    }
#else
    epoll_event events[64];
    int n = ::epoll_wait(impl_->epfd, events, 64, timeout_ms);
    if (n < 0) {
        if (errno == EINTR) return;
        GATEWAY_LOG_ERROR("epoll_wait failed: %s", std::strerror(errno));
        return;
    }
    for (int i = 0; i < n; ++i) cb(events[i].data.fd, events[i].events);
#endif
}

void EventLoop::run(Callback cb) {
    while (true) run_once(500, cb);
}

} // namespace cg