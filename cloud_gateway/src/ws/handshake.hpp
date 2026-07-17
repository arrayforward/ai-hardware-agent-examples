/*!
 * @file handshake.hpp
 * @brief Minimal RFC 6455 server-side handshake.
 *
 * Parses the HTTP Upgrade request, computes Sec-WebSocket-Accept,
 * and serializes the 101 Switching Protocols response.
 *
 * Restrictions:
 *   - Only HTTP/1.1 is supported
 *   - Sec-WebSocket-Version MUST be 13
 *   - Sec-WebSocket-Protocol: only "convai.v1" is accepted
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cg {

struct HandshakeRequest {
    std::string method;          // "GET"
    std::string path;            // "/"
    std::string version;         // "HTTP/1.1"
    std::string sec_key;         // Sec-WebSocket-Key
    std::string sec_protocol;    // Sec-WebSocket-Protocol (optional)
    std::string host;            // Host header
};

struct HandshakeResult {
    bool        ok = false;
    std::string error;
    HandshakeRequest req;
};

// Parse an HTTP request from a buffer. Returns OK with the parsed fields
// when a complete request (ending with \r\n\r\n) has been received.
// Returns NeedMore if more bytes are required.
struct HandshakeParseResult {
    enum Status { NeedMore, Ok, Error };
    Status      status = NeedMore;
    std::string error;
    HandshakeRequest req;
    std::size_t consumed = 0;   // bytes used by this request
};

HandshakeParseResult parse_handshake(std::string_view buf);

std::string build_handshake_response(const HandshakeRequest& req);

} // namespace cg