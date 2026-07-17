#include "handshake.hpp"

#include "../util/base64.hpp"
#include "../util/sha1.hpp"
#include "cloud_gateway/log.hpp"
#include "cloud_gateway/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace cg {

namespace {

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::string to_lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return r;
}

} // namespace

HandshakeParseResult parse_handshake(std::string_view buf) {
    HandshakeParseResult res;

    auto hdr_end = buf.find("\r\n\r\n");
    if (hdr_end == std::string_view::npos) {
        return res;
    }
    res.consumed = hdr_end + 4;

    std::string_view headers_block = buf.substr(0, hdr_end);
    std::size_t line_start = 0;
    bool first = true;
    while (line_start <= headers_block.size()) {
        std::size_t line_end = headers_block.find("\r\n", line_start);
        if (line_end == std::string_view::npos) {
            line_end = headers_block.size();
        }
        std::string_view line = headers_block.substr(line_start, line_end - line_start);
        if (line.empty()) { line_start = line_end + 2; continue; }

        if (first) {
            // Request line: METHOD SP PATH SP HTTP/VERSION
            std::istringstream iss{std::string(line)};
            iss >> res.req.method >> res.req.path >> res.req.version;
            if (res.req.method.empty() || res.req.path.empty() || res.req.version.empty()) {
                res.status = HandshakeParseResult::Error;
                res.error = "malformed request line";
                return res;
            }
            if (res.req.method != "GET") {
                res.status = HandshakeParseResult::Error;
                res.error = "method not GET";
                return res;
            }
            first = false;
        } else {
            // Header line: "Key: value"
            auto colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            std::string key = to_lower(trim(line.substr(0, colon)));
            std::string val = trim(line.substr(colon + 1));

            if      (key == "host")                    res.req.host = val;
            else if (key == "sec-websocket-key")       res.req.sec_key = val;
            else if (key == "sec-websocket-protocol")  res.req.sec_protocol = val;
            else if (key == "upgrade")                 ;
            else if (key == "connection")               ;
            else if (key == "sec-websocket-version") {
                if (val != "13") {
                    res.status = HandshakeParseResult::Error;
                    res.error = "unsupported WS version " + val;
                    return res;
                }
            }
        }
        if (line_end >= headers_block.size()) break;
        line_start = line_end + 2;
    }

    if (res.req.sec_key.empty()) {
        res.status = HandshakeParseResult::Error;
        res.error = "missing Sec-WebSocket-Key";
        return res;
    }
    if (!res.req.sec_protocol.empty() && res.req.sec_protocol != "convai.v1") {
        res.status = HandshakeParseResult::Error;
        res.error = "unsupported subprotocol " + res.req.sec_protocol;
        return res;
    }

    res.status = HandshakeParseResult::Ok;
    return res;
}

std::string build_handshake_response(const HandshakeRequest& req) {
    // accept = base64( sha1( key + GUID ) )
    std::uint8_t digest[20];
    std::string joined = req.sec_key + std::string(cg::WS_GUID);
    sha1(reinterpret_cast<const std::uint8_t*>(joined.data()), joined.size(), digest);
    std::string accept = base64_encode(
        std::string_view(reinterpret_cast<const char*>(digest), 20));

    std::ostringstream r;
    r << "HTTP/1.1 101 Switching Protocols\r\n"
      << "Upgrade: websocket\r\n"
      << "Connection: Upgrade\r\n"
      << "Sec-WebSocket-Accept: " << accept << "\r\n"
      << "Sec-WebSocket-Protocol: convai.v1\r\n"
      << "Server: cloud_gateway/0.1\r\n"
      << "\r\n";
    return r.str();
}

} // namespace cg
