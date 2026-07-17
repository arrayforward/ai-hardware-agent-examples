#include "session.hpp"
#include "session_manager.hpp"

#include "../upstream/asr_backend.hpp"
#include "../upstream/llm_backend.hpp"
#include "../upstream/tts_backend.hpp"
#include "../server/thread_pool.hpp"
#include "../ws/server.hpp"
#include "../codec/envelope.hpp"
#include "cloud_gateway/log.hpp"

#include <chrono>
#include <random>

namespace cg {

namespace {

std::string gen_session_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t a = rng();
    std::uint64_t b = rng();
    char buf[40];
    std::snprintf(buf, sizeof(buf), "sess_%016llx%016llx",
                  (unsigned long long)a, (unsigned long long)b);
    return buf;
}

} // namespace

Session::Session(std::shared_ptr<WsConnection> conn, SessionManager* mgr,
                 AsrBackend* asr, LlmBackend* llm, TtsBackend* tts,
                 ThreadPool* pool)
    : conn_(std::move(conn)), mgr_(mgr), asr_(asr), llm_(llm), tts_(tts),
      pool_(pool), session_id_(gen_session_id()) {}

Session::~Session() = default;

void Session::bind() {
    auto self = shared_from_this();
    conn_->on_text([self](WsConnection* /*c*/, std::string payload) {
        Envelope env;
        std::string err;
        if (!EnvelopeCodec::decode_text(payload, env, err)) {
            GATEWAY_LOG_WARN("[%s] bad envelope: %s",
                             self->session_id_.c_str(), err.c_str());
            return;
        }
        JsonNode body;
        try { body = parse_json(env.body_json); } catch (...) {}
        switch (env.type) {
            case MsgType::Hello:
                self->on_hello(body); break;
            case MsgType::Bye:
                self->on_bye(); break;
            case MsgType::ConfigUpdate:
                self->on_config_update(body); break;
            case MsgType::FunctionCallOutput:
                self->on_function_call_output(body); break;
            default:
                GATEWAY_LOG_WARN("[%s] unhandled msg type %d",
                                 self->session_id_.c_str(),
                                 static_cast<int>(env.type));
        }
    });

    conn_->on_binary([self](WsConnection* /*c*/, const std::uint8_t* data, std::size_t len) {
        AudioFrame f;
        std::string err;
        if (!EnvelopeCodec::decode_audio(data, len, f, err)) {
            GATEWAY_LOG_WARN("[%s] bad audio frame: %s",
                             self->session_id_.c_str(), err.c_str());
            return;
        }
        if (f.op == AudioOp::Cancel) {
            // User barge-in: drop buffered audio.
            self->audio_buf_.clear();
            return;
        }
        if (f.op == AudioOp::End) {
            // VAD end: trigger ASR pipeline.
            self->on_audio_binary(f);
            return;
        }
        // Frame / Start: accumulate audio for later processing.
        if (f.op == AudioOp::Frame) {
            self->audio_buf_.insert(self->audio_buf_.end(),
                                    f.pcm.begin(), f.pcm.end());
        }
    });

    conn_->on_close([self](WsConnection* /*c*/, std::uint16_t code, std::string reason) {
        self->on_close(code, reason);
    });
}

void Session::detach() {
    conn_.reset();
}

void Session::send_text(MsgType type, const JsonNode& body) {
    if (!conn_) return;
    conn_->send_text(EnvelopeCodec::encode(type, next_seq_++, unix_ms(), body));
}

void Session::send_audio(const AudioFrame& f) {
    if (!conn_) return;
    auto bytes = EnvelopeCodec::encode_audio(f);
    conn_->send_binary(bytes.data(), bytes.size());
}

void Session::set_status(AgentStatus s) {
    if (status_ == s) return;
    status_ = s;
    JsonNode body(std::map<std::string, JsonNode>{
        {"status", std::string(agent_status_name(s))},
    });
    send_text(MsgType::Status, body);
}

void Session::send_event(SessionEventKind e, std::string details) {
    JsonNode body(std::map<std::string, JsonNode>{
        {"event",  std::string(session_event_name(e))},
        {"details", std::move(details)},
    });
    send_text(MsgType::Event, body);
}

// ----------------------------------------------------------------------------
//  Inbound handlers
// ----------------------------------------------------------------------------
void Session::on_hello(const JsonNode& body) {
    if (hello_done_) {
        GATEWAY_LOG_WARN("[%s] duplicate hello", session_id_.c_str());
        return;
    }
    hello_done_ = true;

    JsonNode ack_body(std::map<std::string, JsonNode>{
        {"session_id", session_id_},
        {"server_time", static_cast<std::int64_t>(unix_ms())},
        {"audio_config", JsonNode(std::map<std::string, JsonNode>{
            {"frame_ms", static_cast<std::int64_t>(20)},
            {"codec",    std::string("g711a")},
        })},
    });
    send_text(MsgType::HelloAck, ack_body);
    send_event(SessionEventKind::Connected, "session established");
    set_status(AgentStatus::Listening);
    GATEWAY_LOG_INFO("[%s] hello ok, peer=%s",
                     session_id_.c_str(),
                     conn_ ? conn_->peer_str().c_str() : "?");
}

void Session::on_bye() {
    GATEWAY_LOG_INFO("[%s] bye", session_id_.c_str());
    if (conn_) conn_->close(CLOSE_NORMAL, "bye");
}

void Session::on_config_update(const JsonNode& body) {
    current_config_ = body;
    GATEWAY_LOG_INFO("[%s] config updated", session_id_.c_str());

    JsonNode ack_body(std::map<std::string, JsonNode>{
        {"result", std::string("ok")},
        {"applied_at", static_cast<std::int64_t>(unix_ms())},
    });
    send_text(MsgType::ConfigUpdateAck, ack_body);
    send_event(SessionEventKind::Updated, "config applied");
}

void Session::on_function_call_output(const JsonNode& body) {
    if (body.is_object()) {
        if (const auto* items = body.find("items")) {
            if (items->is_array()) {
                for (const auto& it : items->as_array()) {
                    if (!it.is_object()) continue;
                    const auto* call_id = it.find("call_id");
                    const auto* out_v   = it.find("output");
                    if (!call_id || !out_v) continue;
                    std::string cid = call_id->is_string() ? call_id->as_string()
                                       : std::to_string(call_id->as_int());
                    std::string out = out_v->is_string() ? out_v->as_string() : "";
                    if (pool_ && llm_) {
                        auto self = shared_from_this();
                        auto fut = pool_->submit([self, cid, out]() {
                            self->llm_->feed_tool_result(cid, out,
                                [self](std::vector<LlmChunk> batch) {
                                    self->on_llm_done(batch);
                                });
                        });
                        (void)fut;
                    }
                }
                return;
            }
        }
    }
    GATEWAY_LOG_WARN("[%s] function_call_output ignored (bad shape)",
                     session_id_.c_str());
}

void Session::on_audio_binary(const AudioFrame& /*f*/) {
    if (!hello_done_) return;
    last_audio_ms_ = std::chrono::steady_clock::now();

    if (asr_ && pool_) {
        auto self = shared_from_this();
        // Take a snapshot of the accumulated audio for the worker thread.
        std::vector<std::uint8_t> snapshot = std::move(audio_buf_);
        audio_buf_.clear();
        int sr = 8000;
        auto fut = pool_->submit([self, snapshot = std::move(snapshot), sr]() mutable {
            self->asr_->feed(snapshot.data(),
                             snapshot.size(),
                             sr,
                             [self](std::string text, bool /*is_final*/) {
                                 if (!text.empty()) self->on_asr_done(text);
                             });
        });
        (void)fut;
    }
}

// ----------------------------------------------------------------------------
//  Outbound pipelines
// ----------------------------------------------------------------------------
void Session::on_asr_done(std::string text) {
    GATEWAY_LOG_INFO("[%s] ASR -> %s", session_id_.c_str(), text.c_str());
    set_status(AgentStatus::Thinking);

    if (llm_ && pool_) {
        auto self = shared_from_this();
        JsonNode cfg = current_config_;
        auto fut = pool_->submit([self, text, cfg]() {
            self->llm_->chat(text, cfg,
                [self](std::vector<LlmChunk> batch) {
                    self->on_llm_done(batch);
                });
        });
        (void)fut;
    }
}

void Session::on_llm_done(std::vector<LlmChunk> chunks) {
    for (auto& c : chunks) {
        if (c.kind == "text") {
            JsonNode body(std::map<std::string, JsonNode>{
                {"text", c.content},
            });
            send_text(MsgType::Text, body);
        } else if (c.kind == "function_call") {
            JsonNode body(std::map<std::string, JsonNode>{
                {"type", std::string("response.function_call_arguments.done")},
                {"calls", JsonNode(std::vector<JsonNode>{ JsonNode(c.content) })},
            });
            send_text(MsgType::FunctionCall, body);
        }
    }

    // Pick first text chunk for TTS
    std::string tts_text;
    for (auto& c : chunks) {
        if (c.kind == "text") { tts_text = c.content; break; }
    }
    if (!tts_text.empty() && tts_) {
        set_status(AgentStatus::Answering);
        const JsonNode* cfg = current_config_.find("config");
        std::string voice = "default";
        if (cfg) {
            const JsonNode* tts_cfg = cfg->find("tts_config");
            if (tts_cfg) {
                const JsonNode* pp = tts_cfg->find("provider_params");
                if (pp) {
                    const JsonNode* audio = pp->find("audio");
                    if (audio) {
                        const JsonNode* vt = audio->find("voice_type");
                        if (vt && vt->is_string()) voice = vt->as_string();
                    }
                }
            }
        }
        auto self = shared_from_this();
        std::string text = std::move(tts_text);
        auto fut = pool_->submit([self, text, voice]() {
            self->tts_->synth(text, voice,
                [self](std::vector<AudioFrame> batch) {
                    self->on_tts_chunk(batch);
                });
        });
        (void)fut;
    } else {
        set_status(AgentStatus::AnswerFinished);
        set_status(AgentStatus::Listening);
    }
}

void Session::on_tts_chunk(std::vector<AudioFrame> frames) {
    AudioFrame start;
    start.op = AudioOp::Start;
    start.seq = 0;
    start.ts_ms = unix_ms();
    send_audio(start);
    for (auto& f : frames) send_audio(f);
    AudioFrame end;
    end.op = AudioOp::End;
    end.seq = 0;
    end.ts_ms = unix_ms();
    send_audio(end);

    set_status(AgentStatus::AnswerFinished);
    set_status(AgentStatus::Listening);
}

void Session::on_close(std::uint16_t code, const std::string& reason) {
    GATEWAY_LOG_INFO("[%s] closed code=%u reason=%s",
                     session_id_.c_str(), code, reason.c_str());
    if (mgr_) mgr_->remove(session_id_);
}

} // namespace cg