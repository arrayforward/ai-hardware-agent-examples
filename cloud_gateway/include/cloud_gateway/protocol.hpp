/*!
 * @file protocol.hpp
 * @brief ConvAI Cloud Gateway v1 message types & envelope model.
 *
 * The gateway speaks RFC 6455 WebSocket with sub-protocol `convai.v1`.
 * Two message classes share a single WS connection:
 *
 *   1. TEXT frames carry a JSON envelope of the form
 *      {
 *        "type": "<MsgType>",   // string form of MsgType
 *        "seq":  <uint32>,       // monotonically increasing per direction
 *        "ts":   <uint64>,       // ms since unix epoch
 *        "body": { ... }         // type-specific payload
 *      }
 *
 *   2. BINARY frames carry G.711A audio with a 13-byte mini-header:
 *      | AudioOp (1B) | Sequence (4B BE) | Timestamp (8B BE) | PCM data |
 *
 * Close codes follow RFC 6455 (1000 normal, 1001 going away, 1011 server
 * error, 4xxx application-specific).
 */
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace cg {

// ----------------------------------------------------------------------------
//  Constants
// ----------------------------------------------------------------------------
constexpr std::string_view WS_SUBPROTOCOL  = "convai.v1";
constexpr std::string_view WS_GUID         = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t      AUDIO_HEADER    = 13;
constexpr std::size_t      AUDIO_MAX_BYTES = 32 * 1024;   // 32 KB binary frame

// Audio codec identifiers (in HELLO.body.audio_codec)
constexpr int AUDIO_CODEC_PCM16 = 0;
constexpr int AUDIO_CODEC_G711A = 1;     // 8 kHz mono A-law (default)

// Close codes (application-specific range 4xxx)
constexpr std::uint16_t CLOSE_NORMAL            = 1000;
constexpr std::uint16_t CLOSE_GOING_AWAY        = 1001;
constexpr std::uint16_t CLOSE_PROTOCOL_ERROR    = 1002;
constexpr std::uint16_t CLOSE_AUTH_FAILED       = 4401;
constexpr std::uint16_t CLOSE_SESSION_NOT_FOUND = 4404;
constexpr std::uint16_t CLOSE_RATE_LIMITED      = 4429;
constexpr std::uint16_t CLOSE_INTERNAL_ERROR    = 1011;

// ----------------------------------------------------------------------------
//  Message types (text envelopes)
// ----------------------------------------------------------------------------
enum class MsgType : std::uint16_t {
    // handshake
    Hello              = 0x0001,
    HelloAck           = 0x0002,
    HelloErr           = 0x0003,
    Bye                = 0x0004,
    Ping               = 0x0006,
    Pong               = 0x0007,

    // agent state
    Status             = 0x0030,
    Event              = 0x0031,
    Text               = 0x0032,    // AI text reply (full sentence)
    TextDelta          = 0x0033,    // streaming text fragment

    // config / function call
    ConfigUpdate       = 0x0040,
    ConfigUpdateAck    = 0x0041,
    ConfigUpdateErr    = 0x0042,
    FunctionCall       = 0x0050,    // cloud -> device tool invocation
    FunctionCallOutput = 0x0051,    // device -> cloud tool result

    // misc
    Error              = 0x0060,
    Ack                = 0x007F,
};

const char*       msg_type_name(MsgType t);
std::string_view  msg_type_str(MsgType t) noexcept;
bool              parse_msg_type(std::string_view s, MsgType& out) noexcept;

// ----------------------------------------------------------------------------
//  Audio binary sub-protocol
// ----------------------------------------------------------------------------
enum class AudioOp : std::uint8_t {
    Frame    = 0x10,   // one 20ms G.711A frame
    Start    = 0x11,   // VAD begin (after this, audio frames flow)
    End      = 0x12,   // VAD end (LLM/TTS pipeline triggered)
    Cancel   = 0x13,   // user barge-in: discard buffered audio
};

// ----------------------------------------------------------------------------
//  Agent status (mirrors convai_status_e)
// ----------------------------------------------------------------------------
enum class AgentStatus : std::uint8_t {
    Idle            = 0,
    Listening       = 1,
    Thinking        = 2,
    Answering       = 3,
    Interrupted     = 4,
    AnswerFinished  = 5,
};
const char* agent_status_name(AgentStatus s);

// ----------------------------------------------------------------------------
//  Session event (mirrors convai_event_code_e)
// ----------------------------------------------------------------------------
enum class SessionEventKind : std::uint8_t {
    Connected     = 0,
    Disconnected  = 1,
    Failed        = 2,
    Updated       = 3,
};
const char* session_event_name(SessionEventKind e);

// ----------------------------------------------------------------------------
//  Text envelope (one WebSocket TEXT frame)
// ----------------------------------------------------------------------------
struct Envelope {
    MsgType      type     = MsgType::Hello;
    std::uint32_t seq     = 0;
    std::uint64_t ts_ms   = 0;
    std::string  body_json;   // raw JSON (kept opaque at this layer)
};

// ----------------------------------------------------------------------------
//  Binary audio frame
// ----------------------------------------------------------------------------
struct AudioFrame {
    AudioOp                 op        = AudioOp::Frame;
    std::uint32_t           seq       = 0;
    std::uint64_t           ts_ms     = 0;
    std::vector<std::uint8_t> pcm;       // G.711A bytes

    std::size_t wire_size() const noexcept { return AUDIO_HEADER + pcm.size(); }
};

// ----------------------------------------------------------------------------
//  Network byte-order helpers
// ----------------------------------------------------------------------------
inline std::uint16_t hton16(std::uint16_t v) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return static_cast<std::uint16_t>((v << 8) | (v >> 8));
#else
    return v;
#endif
}
inline std::uint16_t ntoh16(std::uint16_t v) noexcept { return hton16(v); }

inline std::uint32_t hton32(std::uint32_t v) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((v & 0x000000FFu) << 24)
         | ((v & 0x0000FF00u) << 8)
         | ((v & 0x00FF0000u) >> 8)
         | ((v & 0xFF000000u) >> 24);
#else
    return v;
#endif
}
inline std::uint32_t ntoh32(std::uint32_t v) noexcept { return hton32(v); }

inline std::uint64_t hton64(std::uint64_t v) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (static_cast<std::uint64_t>(hton32(static_cast<std::uint32_t>(v))) << 32)
         |  hton32(static_cast<std::uint32_t>(v >> 32));
#else
    return v;
#endif
}
inline std::uint64_t ntoh64(std::uint64_t v) noexcept { return hton64(v); }

} // namespace cg