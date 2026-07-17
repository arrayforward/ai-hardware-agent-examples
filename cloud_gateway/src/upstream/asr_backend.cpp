#include "asr_backend.hpp"

#include "cloud_gateway/log.hpp"

#include <cmath>

namespace cg {

void StubAsr::feed(const std::uint8_t* g711a, std::size_t n_bytes,
                   int /*sample_rate*/, Callback cb) {
    if (!g711a || n_bytes == 0) return;

    // Crude energy proxy: G.711 A-law 0xD5 / 0x55 toggle indicates "active".
    int silent = 0;
    for (std::size_t i = 0; i < n_bytes; ++i) {
        if (g711a[i] == 0xD5 || g711a[i] == 0x55) ++silent;
    }
    double active_ratio = 1.0 - (static_cast<double>(silent) / n_bytes);

    if (active_ratio > 0.30) {
        GATEWAY_LOG_DEBUG("[StubAsr] voice detected ratio=%.2f", active_ratio);
        cb("stub-asr: detected speech", /*is_final=*/true);
    }
}

} // namespace cg