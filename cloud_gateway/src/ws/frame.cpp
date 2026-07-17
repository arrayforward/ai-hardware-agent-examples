#include "frame.hpp"

namespace cg {

// ============================================================================
//  Parser
// ============================================================================
namespace {

inline void apply_mask(std::uint8_t* dst, const std::uint8_t* src,
                       std::size_t len, const std::uint8_t mask[4]) {
    for (std::size_t i = 0; i < len; ++i) {
        dst[i] = src[i] ^ mask[i & 3];
    }
}

} // namespace

std::size_t WsFrameParser::push(const std::uint8_t* data, std::size_t len) {
    if (!error_.empty()) return 0;
    buf_.insert(buf_.end(), data, data + len);

    std::size_t produced = 0;
    while (try_parse_one()) {
        ++produced;
    }
    return produced;
}

bool WsFrameParser::try_parse_one() {
    if (buf_.size() < 2) return false;

    const std::uint8_t* p = buf_.data();
    bool fin     = (p[0] & 0x80) != 0;
    bool masked  = (p[1] & 0x80) != 0;
    std::uint8_t op_v = p[0] & 0x0F;
    std::uint64_t plen = p[1] & 0x7F;

    std::size_t header_len = 2;
    if (plen == 126) {
        if (buf_.size() < 4) return false;
        plen = (std::uint64_t(p[2]) << 8) | std::uint64_t(p[3]);
        header_len = 4;
    } else if (plen == 127) {
        if (buf_.size() < 10) return false;
        plen = 0;
        for (int i = 0; i < 8; ++i) {
            plen = (plen << 8) | std::uint64_t(p[2 + i]);
        }
        header_len = 10;
    }

    if (masked) header_len += 4;
    if (plen > max_payload_) {
        commit_error("frame payload too large");
        return false;
    }

    if (buf_.size() < header_len + plen) return false;

    WsFrame f;
    f.fin = fin;
    switch (op_v) {
        case 0x0: f.op = WsOpcode::Continuation; break;
        case 0x1: f.op = WsOpcode::Text;         break;
        case 0x2: f.op = WsOpcode::Binary;       break;
        case 0x8: f.op = WsOpcode::Close;        break;
        case 0x9: f.op = WsOpcode::Ping;         break;
        case 0xA: f.op = WsOpcode::Pong;         break;
        default:
            commit_error("reserved opcode");
            return false;
    }
    if (!fin) {
        commit_error("fragmented frames not supported");
        return false;
    }

    const std::uint8_t* payload_ptr = p + header_len;
    if (masked) {
        const std::uint8_t* mask = p + header_len - 4;
        f.payload.resize(static_cast<std::size_t>(plen));
        apply_mask(f.payload.data(), payload_ptr, static_cast<std::size_t>(plen), mask);
    } else {
        f.payload.assign(payload_ptr, payload_ptr + plen);
    }

    buf_.erase(buf_.begin(), buf_.begin() + header_len + plen);
    ready_.push_back(std::move(f));
    return true;
}

WsFrame WsFrameParser::pop() {
    WsFrame f = std::move(ready_.front());
    ready_.pop_front();
    return f;
}

// ============================================================================
//  Serializer
// ============================================================================
namespace {

inline void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}

} // namespace

std::vector<std::uint8_t> ws_serialize(WsOpcode op, const std::uint8_t* data, std::size_t len) {
    std::vector<std::uint8_t> out;
    out.reserve(len + 10);

    std::uint8_t b0 = 0x80 | (static_cast<std::uint8_t>(op) & 0x0F);
    out.push_back(b0);

    if (len < 126) {
        out.push_back(static_cast<std::uint8_t>(len) & 0x7F);
    } else if (len <= 0xFFFF) {
        out.push_back(126);
        put_u16(out, static_cast<std::uint16_t>(len));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<std::uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }
    out.insert(out.end(), data, data + len);
    return out;
}

std::vector<std::uint8_t> ws_serialize(WsOpcode op, std::string_view data) {
    return ws_serialize(op, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::vector<std::uint8_t> ws_close_frame(std::uint16_t code, std::string_view reason) {
    std::vector<std::uint8_t> payload;
    put_u16(payload, code);
    payload.insert(payload.end(), reason.begin(), reason.end());
    return ws_serialize(WsOpcode::Close, payload.data(), payload.size());
}

std::vector<std::uint8_t> ws_pong_frame(const std::uint8_t* data, std::size_t len) {
    return ws_serialize(WsOpcode::Pong, data, len);
}

} // namespace cg