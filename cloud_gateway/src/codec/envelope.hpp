/*!
 * @file envelope.hpp
 * @brief Encoding/decoding for ConvAI v1 message types.
 *
 * Text envelopes:    Envelope <-> JSON
 * Binary audio:      AudioFrame <-> [13B header | G.711A bytes]
 *
 * The envelope JSON keeps body as raw JSON text to preserve sub-object
 * structure verbatim (the gateway layer does not inspect body fields).
 */
#pragma once

#include "cloud_gateway/protocol.hpp"
#include "../util/json_helper.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cg {

class EnvelopeCodec {
public:
    // Envelope -> JSON text.
    static std::string encode_text(const Envelope& e);

    // JSON text -> Envelope (preserves body_json verbatim).
    // Returns false on parse error; err is set.
    static bool decode_text(const std::string& json, Envelope& out, std::string& err);

    // Convenience: envelope with body as JsonNode.
    static std::string encode(MsgType type, uint32_t seq, uint64_t ts_ms,
                              const JsonNode& body);

    // Audio binary frame serialization (13B header + payload).
    static std::vector<std::uint8_t> encode_audio(const AudioFrame& f);

    // Audio binary frame parsing.
    // Returns false if buffer is too short or contains invalid header.
    static bool decode_audio(const std::uint8_t* data, std::size_t len,
                              AudioFrame& out, std::string& err);
};

} // namespace cg