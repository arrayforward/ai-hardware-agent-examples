/*!
 * @file asr_backend.hpp
 * @brief Speech-to-text backend interface.
 *
 * Real implementations wrap a cloud ASR API (e.g. iFlytek, Whisper).
 * Stub returns a deterministic string per N samples.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cg {

class AsrBackend {
public:
    using Callback = std::function<void(std::string text, bool is_final)>;

    virtual ~AsrBackend() = default;

    // Feed 20ms G.711A frame; may invoke cb() 0..N times.
    virtual void feed(const std::uint8_t* g711a, std::size_t n_bytes,
                      int sample_rate, Callback cb) = 0;
};

// Stub: detects "loud" frames via simple byte histogram, returns fixed text
// when energy threshold exceeded.
class StubAsr : public AsrBackend {
public:
    void feed(const std::uint8_t* g711a, std::size_t n_bytes,
              int sample_rate, Callback cb) override;
};

} // namespace cg