/*!
 * @file test_client.cpp
 * @brief Minimal C++ WebSocket client to exercise cloud_gateway.
 *
 * Behaviour:
 *   1. Connect to ws://host:port
 *   2. Perform RFC 6455 handshake with Sec-WebSocket-Protocol: convai.v1
 *   3. Send Hello JSON
 *   4. Send 50 fake audio frames (160 bytes each)
 *   5. Print every inbound frame until 5 s elapses, then close
 *
 * Usage:
 *   gateway_test_client [--host ADDR] [--port N] [--audio-frames N]
 *                       [--audio-bytes N]
 */
#include "cloud_gateway/log.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  define close      closesocket
#  define errno       WSAGetLastError()
#  define EAGAIN      WSAEWOULDBLOCK
#  define EWOULDBLOCK WSAEWOULDBLOCK
#  define EINTR       WSAEINTR
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <string>
#include <thread>
#include <vector>

#include "util/base64.hpp"
#include "util/sha1.hpp"

// ===========================================================================
//  Minimal WS client (text + binary, no fragmentation, server->client frames
//  may be masked per RFC 6455 - we must unmask).
// ===========================================================================

namespace {

constexpr std::string_view WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool set_nonblock(SOCKET fd, bool nb) {
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

bool send_all(SOCKET fd, const std::uint8_t* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        int n = ::send(fd, reinterpret_cast<const char*>(data + off),
                       static_cast<int>(len - off), 0);
        if (n > 0) off += static_cast<std::size_t>(n);
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
        else if (n < 0 && errno == EINTR) continue;
        else return false;
    }
    return true;
}

std::string build_handshake_request(const std::string& host,
                                   const std::string& port,
                                   const std::string& key_b64) {
    std::string r;
    r += "GET / HTTP/1.1\r\n";
    r += "Host: " + host + ":" + port + "\r\n";
    r += "Upgrade: websocket\r\n";
    r += "Connection: Upgrade\r\n";
    r += "Sec-WebSocket-Key: " + key_b64 + "\r\n";
    r += "Sec-WebSocket-Version: 13\r\n";
    r += "Sec-WebSocket-Protocol: convai.v1\r\n";
    r += "\r\n";
    return r;
}

bool recv_until(SOCKET fd, std::vector<std::uint8_t>& buf,
               const std::string& needle, int timeout_ms) {
    std::vector<std::uint8_t> needle_v(needle.begin(), needle.end());
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::uint8_t tmp[4096];
        int n = ::recv(fd, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
        if (n > 0) {
            buf.insert(buf.end(), tmp, tmp + n);
            if (std::search(buf.begin(), buf.end(),
                            needle_v.begin(), needle_v.end()) != buf.end()) {
                return true;
            }
        } else if (n == 0) {
            return false;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        } else if (errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return false;
}

void put_u16_be(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}
void put_u32_be(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >>  8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x         & 0xFF));
}
void put_u64_be(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 7; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(x >> (i*8)));
}

std::vector<std::uint8_t> ws_text(const std::string& s) {
    std::vector<std::uint8_t> out;
    out.push_back(0x81);
    if (s.size() < 126) {
        out.push_back(0x80 | static_cast<std::uint8_t>(s.size()));
    } else {
        out.push_back(0x80 | 126);
        put_u16_be(out, static_cast<std::uint16_t>(s.size()));
    }
    std::uint8_t mask[4] = {0x01,0x02,0x03,0x04};
    out.insert(out.end(), mask, mask + 4);
    for (std::size_t i = 0; i < s.size(); ++i) {
        out.push_back(static_cast<std::uint8_t>(s[i] ^ mask[i & 3]));
    }
    return out;
}

std::vector<std::uint8_t> ws_binary(const std::uint8_t* data, std::size_t n) {
    std::vector<std::uint8_t> out;
    out.push_back(0x82);
    if (n < 126) {
        out.push_back(0x80 | static_cast<std::uint8_t>(n));
    } else {
        out.push_back(0x80 | 126);
        put_u16_be(out, static_cast<std::uint16_t>(n));
    }
    std::uint8_t mask[4] = {0x01,0x02,0x03,0x04};
    out.insert(out.end(), mask, mask + 4);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<std::uint8_t>(data[i] ^ mask[i & 3]));
    }
    return out;
}

bool parse_server_frame(const std::uint8_t* data, std::size_t len,
                        std::uint8_t& op, std::vector<std::uint8_t>& payload,
                        std::string& err) {
    if (len < 2) { err = "short"; return false; }
    bool fin  = (data[0] & 0x80) != 0;
    bool masked = (data[1] & 0x80) != 0;
    op = data[0] & 0x0F;
    std::uint64_t plen = data[1] & 0x7F;
    std::size_t off = 2;
    if (plen == 126) {
        if (len < off + 2) { err = "short ext16"; return false; }
        plen = (std::uint64_t(data[off]) << 8) | data[off+1];
        off += 2;
    } else if (plen == 127) {
        if (len < off + 8) { err = "short ext64"; return false; }
        plen = 0;
        for (int i = 0; i < 8; ++i) plen = (plen << 8) | data[off + i];
        off += 8;
    }
    if (masked) {
        if (len < off + 4) { err = "short mask"; return false; }
        off += 4;
    }
    if (!fin) { err = "fragmented"; return false; }
    if (len < off + plen) { err = "short payload"; return false; }
    payload.assign(data + off, data + off + plen);
    if (masked && !payload.empty()) {
        // already moved past the mask key
    }
    return true;
}

void print_frame(const std::uint8_t op, const std::vector<std::uint8_t>& payload) {
    const char* name = "?";
    switch (op) {
        case 0x1: name = "TEXT"; break;
        case 0x2: name = "BINARY"; break;
        case 0x8: name = "CLOSE"; break;
        case 0x9: name = "PING"; break;
        case 0xA: name = "PONG"; break;
    }
    if (op == 0x1) {
        std::string s(payload.begin(), payload.end());
        std::printf("[recv] %-6s len=%zu  %s\n", name, payload.size(), s.c_str());
    } else if (op == 0x2) {
        if (payload.size() >= 13) {
            std::uint8_t aop = payload[0];
            std::uint32_t seq = (std::uint32_t(payload[1]) << 24)
                              | (std::uint32_t(payload[2]) << 16)
                              | (std::uint32_t(payload[3]) << 8)
                              |  std::uint32_t(payload[4]);
            std::printf("[recv] %-6s len=%zu  audio op=0x%02x seq=%u\n",
                        name, payload.size(), aop, seq);
        } else {
            std::printf("[recv] %-6s len=%zu  short\n", name, payload.size());
        }
    } else {
        std::printf("[recv] %-6s len=%zu\n", name, payload.size());
    }
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsad;
    WSAStartup(MAKEWORD(2, 2), &wsad);
#endif
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    int n_audio_frames = 50;
    int n_audio_bytes  = 160;

    static struct option longopts[] = {
        {"host",         required_argument, 0, 'h'},
        {"port",         required_argument, 0, 'p'},
        {"audio-frames", required_argument, 0, 'f'},
        {"audio-bytes",  required_argument, 0, 'b'},
        {0, 0, 0, 0},
    };
    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "h:p:f:b:", longopts, &idx)) != -1) {
        switch (opt) {
            case 'h': host = optarg; break;
            case 'p': port = static_cast<std::uint16_t>(std::atoi(optarg)); break;
            case 'f': n_audio_frames = std::atoi(optarg); break;
            case 'b': n_audio_bytes  = std::atoi(optarg); break;
            default: std::fprintf(stderr, "bad arg\n"); return 2;
        }
    }

    // generate a 16-byte key
    std::uint8_t key[16];
    for (int i = 0; i < 16; ++i) key[i] = static_cast<std::uint8_t>(i + 1);
    std::string key_b64 = cg::base64_encode(
        std::string_view(reinterpret_cast<const char*>(key), 16));

    // resolve + connect
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string port_s = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0) {
        std::fprintf(stderr, "getaddrinfo failed\n");
        return 1;
    }
    #ifdef _WIN32
    SOCKET fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == INVALID_SOCKET) { std::fprintf(stderr, "socket: %d\n", WSAGetLastError()); return 1; }
    if (::connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR) {
        std::fprintf(stderr, "connect: %d\n", WSAGetLastError());
        return 1;
    }
#else
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { std::fprintf(stderr, "socket: %s\n", std::strerror(errno)); return 1; }
    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        std::fprintf(stderr, "connect: %s\n", std::strerror(errno));
        return 1;
    }
#endif
    freeaddrinfo(res);
    set_nonblock(fd, false);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&one), sizeof(one));

    // ---- handshake ----
    auto req = build_handshake_request(host, port_s, key_b64);
    if (!send_all(fd, reinterpret_cast<const std::uint8_t*>(req.data()), req.size())) {
        std::fprintf(stderr, "send handshake: %s\n", std::strerror(errno));
        return 1;
    }
    std::vector<std::uint8_t> buf;
    if (!recv_until(fd, buf, "\r\n\r\n", 5000)) {
        std::fprintf(stderr, "handshake timeout\n");
        return 1;
    }
    std::string resp_hdr(buf.begin(), buf.end());
    if (resp_hdr.find("101 Switching Protocols") == std::string::npos) {
        std::fprintf(stderr, "handshake failed: %s\n", resp_hdr.c_str());
        return 1;
    }
    std::printf("[client] handshake ok, %s\n",
                resp_hdr.find("convai.v1") != std::string::npos ? "subproto ok" : "no subproto");

    // ---- send hello ----
    std::string hello = R"({"type":"hello","seq":1,"ts":1,"body":{"product_id":"demo","product_key":"k","product_secret":"s","device_name":"ws63-test","audio_codec":1,"sample_rate":8000}})";
    auto bytes = ws_text(hello);
    if (!send_all(fd, bytes.data(), bytes.size())) {
        std::fprintf(stderr, "send hello failed\n"); return 1;
    }
    std::printf("[client] hello sent\n");

    // ---- send audio frames (binary) ----
    // Use 0x80 mix so StubAsr sees "voice" energy (not the silent 0xD5/0x55 markers).
    std::vector<std::uint8_t> audio(n_audio_bytes);
    for (int i = 0; i < n_audio_bytes; ++i) {
        audio[i] = static_cast<std::uint8_t>(0x80 + (i & 0x1F));
    }

    // Send AUDIO_START (op=0x11) once
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(13);
        payload.push_back(0x11); // AUDIO_START
        put_u32_be(payload, 0);
        put_u64_be(payload, 0);
        auto frame = ws_binary(payload.data(), payload.size());
        if (!send_all(fd, frame.data(), frame.size())) return 1;
    }

    for (int i = 0; i < n_audio_frames; ++i) {
        std::vector<std::uint8_t> payload;
        payload.reserve(13 + audio.size());
        payload.push_back(0x10); // AUDIO_FRAME
        std::uint32_t seq = static_cast<std::uint32_t>(i + 1);
        put_u32_be(payload, seq);
        std::uint64_t ts = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        put_u64_be(payload, ts);
        payload.insert(payload.end(), audio.begin(), audio.end());
        auto frame = ws_binary(payload.data(), payload.size());
        if (!send_all(fd, frame.data(), frame.size())) return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Send AUDIO_END (op=0x12) to trigger ASR/LLM/TTS pipeline
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(13);
        payload.push_back(0x12); // AUDIO_END
        put_u32_be(payload, 0);
        put_u64_be(payload, 0);
        auto frame = ws_binary(payload.data(), payload.size());
        if (!send_all(fd, frame.data(), frame.size())) return 1;
    }
    std::printf("[client] sent %d audio frames (+start/end)\n", n_audio_frames);

    // ---- recv loop (5 s) ----
    set_nonblock(fd, true);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::uint8_t buf[16 * 1024];
        int n = ::recv(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n > 0) {
            // parse one frame at a time
            std::size_t off = 0;
            while (off < static_cast<std::size_t>(n)) {
                if (static_cast<std::size_t>(n) - off < 2) break;
                std::uint8_t op;
                std::vector<std::uint8_t> payload;
                std::string err;
                if (!parse_server_frame(buf + off, n - off, op, payload, err)) {
                    std::printf("[client] parse err: %s\n", err.c_str());
                    break;
                }
                std::size_t consumed = 2 + payload.size();
                if ((buf[off+1] & 0x7F) == 126) consumed += 2;
                else if ((buf[off+1] & 0x7F) == 127) consumed += 8;
                // server does not mask server->client frames, so no +4
                print_frame(op, payload);
                if (op == 0x8) { std::printf("[client] closing\n"); goto done; }
                off += consumed;
            }
        } else if (n == 0) {
            std::printf("[client] server closed\n");
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        } else {
            std::printf("[client] recv errno=%d\n", errno);
            break;
        }
    }
done:
    ::close(fd);
    return 0;
}