/*!
 * @file server.hpp
 * @brief epoll-based WebSocket server with per-connection state machine.
 *
 * Threading model:
 *   - One io thread runs the epoll loop
 *   - Per-connection callbacks are invoked from the io thread
 *   - CPU-bound work (ASR/LLM/TTS) is dispatched via ThreadPool
 *
 * States per connection:
 *   HANDSHAKING  -> reading HTTP upgrade request
 *   OPEN         -> framing/decoding WebSocket frames
 *   CLOSING      -> close handshake
 *   CLOSED       -> removed from epoll
 */
#pragma once

#include "cloud_gateway/log.hpp"
#include "frame.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
struct sockaddr_in;
#else
#  include <netinet/in.h>
#endif

namespace cg { class EventLoop; }  // forward decl from util/event_loop.hpp

#include <deque>

namespace cg {

class ThreadPool;

// Forward decl
class WsServer;
class WsConnection;

// Frame callback (set per connection once handshaked).
using OnTextFn   = std::function<void(WsConnection*, std::string)>;
using OnBinaryFn = std::function<void(WsConnection*, const std::uint8_t*, std::size_t)>;
using OnCloseFn  = std::function<void(WsConnection*, std::uint16_t code, std::string reason)>;

class WsConnection : public std::enable_shared_from_this<WsConnection> {
public:
    enum class State {
        Handshaking,
        Open,
        Closing,
        Closed,
    };

    WsConnection(int fd, const sockaddr_in& peer, WsServer* server);
    ~WsConnection();

    int            fd() const      { return fd_; }
    State          state() const   { return state_; }
    const sockaddr_in& peer() const { return peer_; }
    std::string    peer_str() const;

    // Set callbacks (called from io thread).
    void on_text(OnTextFn cb)   { on_text_   = std::move(cb); }
    void on_binary(OnBinaryFn cb){ on_binary_ = std::move(cb); }
    void on_close(OnCloseFn cb) { on_close_  = std::move(cb); }

    // Send a WS frame. Thread-safe (queues into write buffer).
    void send_text(std::string_view data);
    void send_binary(const std::uint8_t* data, std::size_t len);
    void send_binary(std::string_view data) {
        send_binary(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    }
    void close(std::uint16_t code, std::string_view reason);

    // Called by server when socket becomes readable/writable.
    void on_readable();
    void on_writable();

    // Mark connection for destruction after this iteration.
    void schedule_close();

    // Heartbeat timestamp (visible to server for timeouts).
    std::int64_t last_pong_ms_ = 0;

private:
    void try_handshake();
    void try_parse_frames();
    void flush_write_queue();
    void begin_closing(std::uint16_t code, std::string_view reason);
    void close_fd_now();

    int                       fd_;
    sockaddr_in               peer_;
    WsServer*                 server_;
    State                     state_ = State::Handshaking;
    WsFrameParser             parser_;
    std::vector<std::uint8_t> read_buf_;
    std::vector<std::uint8_t> write_buf_;
    std::deque<std::vector<std::uint8_t>> write_queue_;   // pending serialized frames
    std::size_t               write_pos_ = 0;
    bool                      wants_write_ = false;
    bool                      close_after_write_ = false;
    bool                      pending_close_ = false;
    OnTextFn                  on_text_;
    OnBinaryFn                on_binary_;
    OnCloseFn                 on_close_;

    // Serialises send_text/send_binary (worker threads) and
    // flush_write_queue (io thread) access to write_queue_.
    std::mutex                write_mu_;
};

class WsServer {
public:
    WsServer(std::string bind_addr, std::uint16_t port, ThreadPool* pool);
    ~WsServer();

    bool start();
    void stop();
    void run();   // blocking; returns on stop()

    void set_on_connect(std::function<void(std::shared_ptr<WsConnection>)> cb) {
        on_connect_ = std::move(cb);
    }

    // Called by Connection when it wants to send data.
    void enable_write(WsConnection* c);
    // Called by Connection when it should be closed.
    void close_connection(WsConnection* c);

    int listen_fd() const { return listen_fd_; }
    ThreadPool* pool() const { return pool_; }

private:
    void on_accept();
    void on_event(int fd, std::uint32_t events);
    void tick_heartbeat();
    void set_nonblock(int fd);
    void del_fd(int fd);

    std::string                bind_addr_;
    std::uint16_t              port_;
    ThreadPool*                pool_;
    int                        listen_fd_  = -1;
    bool                       running_     = false;
    std::unique_ptr<class EventLoop> loop_;
    std::function<void(std::shared_ptr<WsConnection>)> on_connect_;
    std::unordered_map<int, std::shared_ptr<WsConnection>> conns_;
};

} // namespace cg