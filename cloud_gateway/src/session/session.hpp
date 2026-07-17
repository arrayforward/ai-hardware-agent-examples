/*!
 * @file session.hpp
 * @brief Per-connection session lifecycle: handshake -> ASR/LLM/TTS pipeline.
 */
#pragma once

#include "cloud_gateway/protocol.hpp"
#include "cloud_gateway/session.hpp"
#include "../ws/server.hpp"
#include "../upstream/llm_backend.hpp"   // for LlmChunk

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

namespace cg {

class SessionManager;
class AsrBackend;
class LlmBackend;
class TtsBackend;
class ThreadPool;

// Per-connection session. Created after a successful WS handshake.
//
// Threading: all callbacks are invoked from the WS server's io thread.
// CPU-bound work (ASR/LLM/TTS) is dispatched via the thread pool.
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(std::shared_ptr<WsConnection> conn,
            SessionManager* mgr,
            AsrBackend* asr,
            LlmBackend* llm,
            TtsBackend* tts,
            ThreadPool* pool);
    ~Session();

    const SessionId& id() const { return session_id_; }

    // Wire up callbacks on the underlying WsConnection.
    void bind();
    // Detach (server is shutting down).
    void detach();

    // ---- High-level inbound ----
    void on_hello(const JsonNode& body);
    void on_bye();
    void on_config_update(const JsonNode& body);
    void on_function_call_output(const JsonNode& body);
    void on_audio_binary(const AudioFrame& f);
    void on_close(std::uint16_t code, const std::string& reason);

private:
    void send_text(MsgType type, const JsonNode& body);
    void send_audio(const AudioFrame& f);
    void on_asr_done(std::string text);
    void on_llm_done(std::vector<LlmChunk> chunks);
    void on_tts_chunk(std::vector<AudioFrame> frames);
    void set_status(AgentStatus s);
    void send_event(SessionEventKind e, std::string details);

    std::shared_ptr<WsConnection> conn_;
    SessionManager*               mgr_  = nullptr;
    AsrBackend*                   asr_  = nullptr;
    LlmBackend*                   llm_  = nullptr;
    TtsBackend*                   tts_  = nullptr;
    ThreadPool*                   pool_ = nullptr;

    SessionId                     session_id_;
    bool                          hello_done_ = false;
    AgentStatus                   status_     = AgentStatus::Idle;
    std::atomic<std::uint32_t>    next_seq_{1};

    // Latest config snapshot.
    JsonNode                      current_config_;

    // Audio buffer between VAD window and ASR submission.
    std::vector<std::uint8_t>     audio_buf_;
    std::chrono::steady_clock::time_point last_audio_ms_{};
};

} // namespace cg