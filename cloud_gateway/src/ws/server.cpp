#include "server.hpp"

#include "handshake.hpp"
#include "../server/thread_pool.hpp"
#include "../util/event_loop.hpp"
#include "cloud_gateway/protocol.hpp"
#include "cloud_gateway/session.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
// rename POSIX close to avoid clashing with WsConnection::close method
#  define cg_close   closesocket
#  define errno      WSAGetLastError()
#  define EAGAIN     WSAEWOULDBLOCK
#  define EWOULDBLOCK WSAEWOULDBLOCK
#  define EINPROGRESS WSAEINPROGRESS
#  define EINTR       WSAEINTR
#  define MSG_NOSIGNAL 0
#else
#  define cg_close   close
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <sys/epoll.h>
#  include <unistd.h>
#endif

namespace cg {

// ============================================================================
//  WsConnection
// ============================================================================
WsConnection::WsConnection(int fd, const sockaddr_in& peer, WsServer* server)
    : fd_(fd), peer_(peer), server_(server) {
    last_pong_ms_ = now_ms();
}

WsConnection::~WsConnection() {
    if (fd_ >= 0) ::cg_close(fd_);
}

std::string WsConnection::peer_str() const {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_.sin_addr, ip, sizeof(ip));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s:%u", ip, ntohs(peer_.sin_port));
    return buf;
}

void WsConnection::send_text(std::string_view data) {
    auto bytes = ws_serialize(WsOpcode::Text, data);
    {
        std::lock_guard<std::mutex> lk(write_mu_);
        write_queue_.push_back(std::move(bytes));
    }
    server_->enable_write(this);
}

void WsConnection::send_binary(const std::uint8_t* data, std::size_t len) {
    auto bytes = ws_serialize(WsOpcode::Binary, data, len);
    {
        std::lock_guard<std::mutex> lk(write_mu_);
        write_queue_.push_back(std::move(bytes));
    }
    server_->enable_write(this);
}

void WsConnection::close(std::uint16_t code, std::string_view reason) {
    if (state_ == State::Closed) return;
    begin_closing(code, reason);
}

void WsConnection::schedule_close() {
    pending_close_ = true;
}

void WsConnection::begin_closing(std::uint16_t code, std::string_view reason) {
    if (state_ == State::Closing || state_ == State::Closed) return;
    state_ = State::Closing;
    auto bytes = ws_close_frame(code, reason);
    {
        std::lock_guard<std::mutex> lk(write_mu_);
        write_queue_.push_back(std::move(bytes));
    }
    close_after_write_ = true;
    server_->enable_write(this);
}

void WsConnection::close_fd_now() {
    if (fd_ >= 0) {
        ::cg_close(fd_);
        fd_ = -1;
    }
    state_ = State::Closed;
}

void WsConnection::try_handshake() {
    std::string_view sv(reinterpret_cast<const char*>(read_buf_.data()), read_buf_.size());
    auto r = parse_handshake(sv);
    if (r.status == HandshakeParseResult::NeedMore) return;
    if (r.status == HandshakeParseResult::Error) {
        GATEWAY_LOG_WARN("[%s] handshake error: %s", peer_str().c_str(), r.error.c_str());
        begin_closing(CLOSE_PROTOCOL_ERROR, r.error);
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + r.consumed);
        return;
    }
auto resp = build_handshake_response(r.req);
    std::vector<std::uint8_t> resp_bytes(resp.begin(), resp.end());
    {
        std::lock_guard<std::mutex> lk(write_mu_);
        write_queue_.push_back(std::move(resp_bytes));
    }

    read_buf_.erase(read_buf_.begin(), read_buf_.begin() + r.consumed);
    state_ = State::Open;
    GATEWAY_LOG_INFO("[%s] handshake ok, subproto=%s",
                     peer_str().c_str(),
                     r.req.sec_protocol.empty() ? "(none)" : r.req.sec_protocol.c_str());
    server_->enable_write(this);
}

void WsConnection::try_parse_frames() {
    std::size_t produced = parser_.push(read_buf_.data(), read_buf_.size());
    read_buf_.clear();
    (void)produced;

    if (parser_.has_error()) {
        GATEWAY_LOG_WARN("[%s] frame parse error: %s", peer_str().c_str(),
                         parser_.error().c_str());
        begin_closing(CLOSE_PROTOCOL_ERROR, "bad frame");
        return;
    }

    while (parser_.has_frame()) {
        auto f = parser_.pop();
        switch (f.op) {
            case WsOpcode::Text:
                if (on_text_) {
                    std::string s(f.payload.begin(), f.payload.end());
                    on_text_(this, std::move(s));
                }
                break;
            case WsOpcode::Binary:
                if (on_binary_) {
                    on_binary_(this, f.payload.data(), f.payload.size());
                }
                break;
case WsOpcode::Ping: {
                auto pong = ws_pong_frame(f.payload.data(), f.payload.size());
                {
                    std::lock_guard<std::mutex> lk(write_mu_);
                    write_queue_.push_back(std::move(pong));
                }
                server_->enable_write(this);
                break;
            }
            case WsOpcode::Pong:
                last_pong_ms_ = now_ms();
                break;
            case WsOpcode::Close:
                GATEWAY_LOG_INFO("[%s] peer close", peer_str().c_str());
                begin_closing(CLOSE_NORMAL, "bye");
                return;
            default:
                break;
        }
    }
}

void WsConnection::on_readable() {
    if (state_ == State::Closed) return;

    while (true) {
        std::uint8_t buf[16 * 1024];
#ifdef _WIN32
        int n = ::recv(fd_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
#else
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
#endif
        if (n > 0) {
            read_buf_.insert(read_buf_.end(), buf, buf + n);
        } else if (n == 0) {
            GATEWAY_LOG_INFO("[%s] peer closed", peer_str().c_str());
            server_->close_connection(this);
            return;
        } else {
#ifdef _WIN32
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) break;
            if (e == WSAEINTR) continue;
            GATEWAY_LOG_WARN("[%s] recv errno=%d", peer_str().c_str(), e);
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            GATEWAY_LOG_WARN("[%s] recv errno=%d", peer_str().c_str(), errno);
#endif
            server_->close_connection(this);
            return;
        }
    }

    if (state_ == State::Handshaking) {
        try_handshake();
    }
    if (state_ == State::Open) {
        try_parse_frames();
    }
}

void WsConnection::on_writable() {
    if (state_ == State::Closed) return;
    flush_write_queue();

    if (write_queue_.empty() && close_after_write_) {
        GATEWAY_LOG_DEBUG("[%s] close after write complete", peer_str().c_str());
        server_->close_connection(this);
    }
}

void WsConnection::flush_write_queue() {
    // Hold the write_mu_ only long enough to dequeue; release before
    // the actual send() so a slow peer doesn't block worker threads.
    while (true) {
        std::vector<std::uint8_t> front;
        std::size_t pos;
        {
            std::lock_guard<std::mutex> lk(write_mu_);
            if (write_queue_.empty()) {
                write_pos_ = 0;
                return;
            }
            front = std::move(write_queue_.front());
            write_queue_.pop_front();
            pos = write_pos_;
        }

        while (pos < front.size()) {
#ifdef _WIN32
            int n = ::send(fd_, reinterpret_cast<const char*>(front.data() + pos),
                           (int)(front.size() - pos), 0);
#else
            ssize_t n = ::send(fd_, front.data() + pos,
                                front.size() - pos, MSG_NOSIGNAL);
#endif
            if (n > 0) {
                pos += static_cast<std::size_t>(n);
            } else if (n < 0
#ifdef _WIN32
                       && WSAGetLastError() == WSAEWOULDBLOCK
#else
                       && (errno == EAGAIN || errno == EWOULDBLOCK)
#endif
            ) {
                // Put the partial frame back at the front and exit.
                std::lock_guard<std::mutex> lk(write_mu_);
                write_pos_ = pos;
                write_queue_.push_front(std::move(front));
                return;
            } else if (n < 0
#ifdef _WIN32
                       && WSAGetLastError() == WSAEINTR
#else
                       && errno == EINTR
#endif
            ) {
                continue;
            } else {
                GATEWAY_LOG_WARN("[%s] send errno=%d", peer_str().c_str(),
#ifdef _WIN32
                                WSAGetLastError()
#else
                                errno
#endif
                );
                server_->close_connection(this);
                return;
            }
        }
    }
}

// ============================================================================
//  WsServer
// ============================================================================
WsServer::WsServer(std::string bind_addr, std::uint16_t port, ThreadPool* pool)
    : bind_addr_(std::move(bind_addr)), port_(port), pool_(pool) {}

WsServer::~WsServer() {
    stop();
}

bool WsServer::start() {
    loop_ = std::make_unique<EventLoop>();
    if (!loop_->init()) return false;

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        GATEWAY_LOG_ERROR("socket() failed");
        return false;
    }
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = hton16(port_);
    if (inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr) != 1) {
        GATEWAY_LOG_ERROR("invalid bind address: %s", bind_addr_.c_str());
        ::cg_close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        GATEWAY_LOG_ERROR("bind() failed");
        ::cg_close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 128) < 0) {
        GATEWAY_LOG_ERROR("listen() failed");
        ::cg_close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    set_nonblock(listen_fd_);
    loop_->add(listen_fd_, 0x001 | 0x004);  // EPOLLIN | EPOLLOUT
    running_ = true;
    GATEWAY_LOG_INFO("listening on %s:%u (fd=%d)",
                     bind_addr_.c_str(), port_, listen_fd_);
    return true;
}

void WsServer::stop() {
    if (!running_) return;
    running_ = false;
    if (loop_) loop_->shutdown();
    if (listen_fd_ >= 0) ::cg_close(listen_fd_);
    listen_fd_ = -1;
    loop_.reset();
}

void WsServer::run() {
    while (running_) {
        loop_->run_once(500, [this](int fd, std::uint32_t ev) {
            if (fd == listen_fd_) on_accept();
            else                  on_event(fd, ev);
        });
        tick_heartbeat();
    }
}

void WsServer::set_nonblock(int fd) {
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

void WsServer::on_accept() {
    while (true) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
#ifdef _WIN32
        SOCKET cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd == INVALID_SOCKET) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) break;
            if (e == WSAEINTR) continue;
            GATEWAY_LOG_WARN("accept failed: %d", e);
            break;
        }
#else
        int cfd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen,
                            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            GATEWAY_LOG_WARN("accept failed: %s", std::strerror(errno));
            break;
        }
#endif
        int one = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), sizeof(one));

        auto conn = std::make_shared<WsConnection>(
#ifdef _WIN32
            static_cast<int>(cfd),
#else
            cfd,
#endif
            peer, this);
        conns_.emplace(cfd, conn);
        loop_->add(cfd, 0x001);   // EPOLLIN
        if (on_connect_) on_connect_(conn);
    }
}

void WsServer::on_event(int fd, std::uint32_t ev) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    auto& conn = it->second;

    if (ev & (0x008 | 0x010)) {   // EPOLLERR | EPOLLHUP
        GATEWAY_LOG_INFO("[%s] epoll error/hup", conn->peer_str().c_str());
        close_connection(conn.get());
        return;
    }
    if (ev & 0x001) conn->on_readable();   // EPOLLIN
    if (ev & 0x004) conn->on_writable();  // EPOLLOUT

    if (conn->state() == WsConnection::State::Closed) {
        del_fd(fd);
        conns_.erase(it);
    }
}

void WsServer::tick_heartbeat() {
    const std::int64_t now = now_ms();
    const std::int64_t timeout = 60 * 1000;
    for (auto it = conns_.begin(); it != conns_.end();) {
        auto& c = it->second;
        if (now - c->last_pong_ms_ > timeout) {
            GATEWAY_LOG_WARN("[%s] heartbeat timeout, closing", c->peer_str().c_str());
            c->close(CLOSE_GOING_AWAY, "heartbeat timeout");
            del_fd(c->fd());
            it = conns_.erase(it);
        } else {
            ++it;
        }
    }
}

void WsServer::enable_write(WsConnection* c) {
    loop_->mod(c->fd(), 0x001 | 0x004);
}

void WsServer::close_connection(WsConnection* c) {
    if (c->fd() >= 0) del_fd(c->fd());
    c->schedule_close();
    conns_.erase(c->fd());
}

void WsServer::del_fd(int fd) {
    loop_->del(fd);
}

} // namespace cg

