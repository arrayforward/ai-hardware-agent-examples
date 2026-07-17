/*!
 * @file config.hpp
 * @brief Runtime configuration for cloud_gateway.
 *
 * Configuration sources (priority high → low):
 *   1. Command-line flags
 *   2. Environment variables
 *   3. Built-in defaults
 */
#pragma once

#include <cstdint>
#include <string>

namespace cg {

struct Config {
    // ---- TCP listener ----
    std::string  bind_address   = "0.0.0.0";
    std::uint16_t port          = 9000;
    int          listen_backlog = 128;

    // ---- I/O threading ----
    int          io_threads      = 1;     // epoll event loop count (typically 1)
    int          worker_threads  = 4;     // CPU-bound pool (ASR/LLM/TTS)
    int          heartbeat_seconds = 30;

    // ---- Frame codec ----
    std::uint32_t max_payload_size = 64 * 1024;   // 64 KB per frame

    // ---- Session ----
    int          session_ttl_seconds    = 300;     // 5 min idle then close
    int          audio_buffer_ms        = 100;     // 100ms VAD window

    // ---- Upstream backends (stub by default) ----
    std::string  asr_backend = "stub";
    std::string  llm_backend = "stub";
    std::string  tts_backend = "stub";

    // ---- Misc ----
    bool         verbose = false;

    static Config from_args(int argc, char** argv);
    void print() const;
};

} // namespace cg