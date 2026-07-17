/*!
 * @file frame.hpp
 * @brief RFC 6455 WebSocket frame parser & serializer (server side).
 *
 * Server side: never masks outgoing frames, always allows client-masked frames.
 * Fragmentation is rejected (single-frame messages only).
 */
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cg {

// RFC 6455 opcodes
enum class WsOpcode : std::uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

// Parsed WebSocket frame
struct WsFrame {
    bool         fin   = true;
    WsOpcode     op    = WsOpcode::Continuation;
    std::vector<std::uint8_t> payload;
};

enum class ParseStatus {
    NeedMore,   // not enough data yet
    Ok,
    Error,      // protocol violation (will close connection)
};

struct ParseResult {
    ParseStatus status = ParseStatus::NeedMore;
    WsFrame     frame;
    std::string error;
};

// Streaming parser: feed bytes via push(); drain via pop().
class WsFrameParser {
public:
    // Returns the number of complete frames available after this push().
    // Caller should then drain them with pop().
    std::size_t push(const std::uint8_t* data, std::size_t len);

    bool        has_frame() const { return !ready_.empty(); }
    WsFrame     pop();

    void        reset() { buf_.clear(); ready_.clear(); error_.clear(); }
    bool        has_error() const { return !error_.empty(); }
    const std::string& error() const { return error_; }

    // Maximum allowed frame payload (default 64 KB)
    void set_max_payload(std::size_t n) { max_payload_ = n; }

private:
    void commit_error(const std::string& msg) { error_ = msg; }
    bool try_parse_one();

    std::vector<std::uint8_t> buf_;
    std::deque<WsFrame>       ready_;
    std::string               error_;
    std::size_t               max_payload_ = 64 * 1024;
};

// Frame serializer (no fragmentation, no masking on server side).
std::vector<std::uint8_t> ws_serialize(WsOpcode op, const std::uint8_t* data, std::size_t len);
std::vector<std::uint8_t> ws_serialize(WsOpcode op, std::string_view data);
std::vector<std::uint8_t> ws_close_frame(std::uint16_t code, std::string_view reason);
std::vector<std::uint8_t> ws_pong_frame(const std::uint8_t* data, std::size_t len);

} // namespace cg