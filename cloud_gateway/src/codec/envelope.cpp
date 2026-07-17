#include "envelope.hpp"

#include <cstring>
#include <map>

namespace cg {

namespace {

void put_u32_be(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >>  8) & 0xFF));
    v.push_back(static_cast<std::uint8_t>(x         & 0xFF));
}
void put_u64_be(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 7; i >= 0; --i) {
        v.push_back(static_cast<std::uint8_t>((x >> (i * 8)) & 0xFF));
    }
}
std::uint32_t get_u32_be(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16)
         | (std::uint32_t(p[2]) << 8)  |  std::uint32_t(p[3]);
}
std::uint64_t get_u64_be(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

} // namespace

// ============================================================================
//  Text envelope
// ============================================================================
std::string EnvelopeCodec::encode_text(const Envelope& e) {
    JsonNode root(std::map<std::string, JsonNode>{
        {"type", std::string(msg_type_str(e.type))},
        {"seq",  static_cast<std::int64_t>(e.seq)},
        {"ts",   static_cast<std::int64_t>(e.ts_ms)},
        {"body", JsonNode()},   // will be replaced below
    });

    // Replace placeholder body with parsed JSON if possible
    if (!e.body_json.empty()) {
        try {
            JsonNode parsed = parse_json(e.body_json);
            auto& obj = std::get<std::map<std::string, JsonNode>>(root.v);
            obj["body"] = parsed;
        } catch (...) {
            // Fall back to a string body
            auto& obj = std::get<std::map<std::string, JsonNode>>(root.v);
            obj["body"] = JsonNode(e.body_json);
        }
    }
    return to_json(root);
}

bool EnvelopeCodec::decode_text(const std::string& json, Envelope& out, std::string& err) {
    try {
        JsonNode root = parse_json(json);
        if (!root.is_object()) {
            err = "envelope must be a JSON object";
            return false;
        }
        const auto* t = root.find("type");
        if (!t || !t->is_string()) {
            err = "missing or non-string 'type'";
            return false;
        }
        if (!parse_msg_type(t->as_string(), out.type)) {
            err = "unknown message type: " + t->as_string();
            return false;
        }
        if (const auto* s = root.find("seq")) out.seq = static_cast<std::uint32_t>(s->as_int());
        if (const auto* ts = root.find("ts")) out.ts_ms = static_cast<std::uint64_t>(ts->as_int());

        // body: keep raw JSON if present, else empty object
        const auto* body = root.find("body");
        if (body) {
            JsonWriter w;
            w.write_value(*body);
            out.body_json = w.str();
        } else {
            out.body_json = "{}";
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

std::string EnvelopeCodec::encode(MsgType type, uint32_t seq, uint64_t ts_ms,
                                  const JsonNode& body) {
    Envelope e;
    e.type    = type;
    e.seq     = seq;
    e.ts_ms   = ts_ms;
    JsonWriter w;
    w.write_value(body);
    e.body_json = w.str();
    return encode_text(e);
}

// ============================================================================
//  Binary audio
// ============================================================================
std::vector<std::uint8_t> EnvelopeCodec::encode_audio(const AudioFrame& f) {
    std::vector<std::uint8_t> out;
    out.reserve(AUDIO_HEADER + f.pcm.size());
    out.push_back(static_cast<std::uint8_t>(f.op));
    put_u32_be(out, f.seq);
    put_u64_be(out, f.ts_ms);
    out.insert(out.end(), f.pcm.begin(), f.pcm.end());
    return out;
}

bool EnvelopeCodec::decode_audio(const std::uint8_t* data, std::size_t len,
                                  AudioFrame& out, std::string& err) {
    if (len < AUDIO_HEADER) {
        err = "audio frame too short";
        return false;
    }
    out.op    = static_cast<AudioOp>(data[0]);
    out.seq   = get_u32_be(data + 1);
    out.ts_ms = get_u64_be(data + 5);
    out.pcm.assign(data + AUDIO_HEADER, data + len);
    return true;
}

} // namespace cg