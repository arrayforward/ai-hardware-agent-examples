/*!
 * @file tts_backend.hpp
 * @brief Text-to-speech backend interface.
 *
 * Produces 20 ms G.711A frames at 8 kHz mono. Stub synthesizes a distinctive
 * tone so audio pipeline can be verified by ear or waveform inspection.
 */
#pragma once

#include "cloud_gateway/protocol.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cg {

class TtsBackend {
public:
    using Callback = std::function<void(std::vector<AudioFrame> batch)>;

    virtual ~TtsBackend() = default;

    // Convert text to G.711A frames. Callback may be invoked multiple times
    // for streaming synthesis.
    virtual void synth(const std::string& text, const std::string& voice_type,
                       Callback cb) = 0;
};

// Stub: emits a 440 Hz tone (encoded as G.711A) lasting ~ text.length() * 50 ms.
class StubTts : public TtsBackend {
public:
    void synth(const std::string& text, const std::string& voice_type,
               Callback cb) override;
};

} // namespace cg