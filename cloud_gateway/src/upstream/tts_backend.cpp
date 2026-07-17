#include "tts_backend.hpp"

#include "cloud_gateway/log.hpp"
#include "cloud_gateway/session.hpp"

#include <cmath>
#ifndef _USE_MATH_DEFINES
#  define _USE_MATH_DEFINES
#endif
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace cg {

namespace {

// Encode one PCM16 sample to G.711 A-law.
std::uint8_t pcm16_to_alaw(std::int16_t pcm) {
    std::uint8_t mask = (pcm < 0) ? 0x55 : 0xD5;
    if (pcm < 0) pcm = static_cast<std::int16_t>(-pcm - 8);
    pcm = static_cast<std::int16_t>(pcm >> 3);
    if (pcm > 4095) pcm = 4095;
    int seg = 0;
    if (pcm >= 32)  { seg = 1; pcm >>= 1;
    if (pcm >= 32)  { seg = 2; pcm >>= 1;
    if (pcm >= 32)  { seg = 3; pcm >>= 1;
    if (pcm >= 32)  { seg = 4; pcm >>= 1;
    if (pcm >= 32)  { seg = 5; pcm >>= 1;
    if (pcm >= 32)  { seg = 6; pcm >>= 1;
    if (pcm >= 32)  { seg = 7; pcm >>= 1; }}}}}}}
    std::uint8_t v = static_cast<std::uint8_t>((seg << 4) | ((pcm >> 1) & 0x0F));
    return static_cast<std::uint8_t>(v ^ mask);
}

} // namespace

void StubTts::synth(const std::string& text, const std::string& voice_type,
                    Callback cb) {
    GATEWAY_LOG_INFO("[StubTts] synth text='%s' voice=%s",
                     text.c_str(), voice_type.c_str());

    // Generate ~40ms per character, 8000 Hz mono, 20ms frames = 160 samples
    const int sr = 8000;
    const int frame_ms = 20;
    const int samples_per_frame = sr * frame_ms / 1000;     // 160
    const double freq = 440.0;

    std::size_t total_frames = std::max<std::size_t>(1, text.size() * 2);
    std::uint32_t seq = 1;
    const std::uint64_t now = unix_ms();

    // Build ALL frames in one batch and emit in a single cb() call so the
    // session can wrap with exactly one Start/End pair.
    std::vector<AudioFrame> batch;
    batch.reserve(total_frames);
    for (std::size_t i = 0; i < total_frames; ++i) {
        AudioFrame f;
        f.op    = AudioOp::Frame;
        f.seq   = seq++;
        f.ts_ms = now + i * frame_ms;
        f.pcm.resize(samples_per_frame);
        double phase = 2.0 * M_PI * freq * (i * samples_per_frame) / sr;
        std::int16_t sample = static_cast<std::int16_t>(20000.0 * std::sin(phase));
        for (int j = 0; j < samples_per_frame; ++j) {
            double env = 0.5 + 0.5 * std::sin(2.0 * M_PI * 2.0 * (i * frame_ms / 1000.0 + j / static_cast<double>(sr)));
            std::int16_t s = static_cast<std::int16_t>(sample * env);
            f.pcm[j] = pcm16_to_alaw(s);
        }
        batch.push_back(std::move(f));
    }
    cb(std::move(batch));
}

} // namespace cg