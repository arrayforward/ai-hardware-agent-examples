#include "cloud_gateway/protocol.hpp"

#include <cstring>

namespace cg {

// ============================================================================
//  msg_type name <-> enum
// ============================================================================
struct MsgTypeEntry {
    MsgType     type;
    const char* name;
};

static constexpr MsgTypeEntry kMsgTable[] = {
    {MsgType::Hello,              "hello"},
    {MsgType::HelloAck,           "hello_ack"},
    {MsgType::HelloErr,           "hello_err"},
    {MsgType::Bye,                "bye"},
    {MsgType::Ping,               "ping"},
    {MsgType::Pong,               "pong"},
    {MsgType::Status,             "status"},
    {MsgType::Event,              "event"},
    {MsgType::Text,               "text"},
    {MsgType::TextDelta,          "text_delta"},
    {MsgType::ConfigUpdate,       "config_update"},
    {MsgType::ConfigUpdateAck,    "config_update_ack"},
    {MsgType::ConfigUpdateErr,    "config_update_err"},
    {MsgType::FunctionCall,       "function_call"},
    {MsgType::FunctionCallOutput, "function_call_output"},
    {MsgType::Error,              "error"},
    {MsgType::Ack,                "ack"},
};

const char* msg_type_name(MsgType t) {
    for (const auto& e : kMsgTable) {
        if (e.type == t) return e.name;
    }
    return "unknown";
}

std::string_view msg_type_str(MsgType t) noexcept {
    return std::string_view(msg_type_name(t));
}

bool parse_msg_type(std::string_view s, MsgType& out) noexcept {
    for (const auto& e : kMsgTable) {
        if (s == e.name) { out = e.type; return true; }
    }
    return false;
}

// ============================================================================
//  enum -> string helpers
// ============================================================================
const char* agent_status_name(AgentStatus s) {
    switch (s) {
        case AgentStatus::Idle:           return "idle";
        case AgentStatus::Listening:      return "listening";
        case AgentStatus::Thinking:       return "thinking";
        case AgentStatus::Answering:      return "answering";
        case AgentStatus::Interrupted:    return "interrupted";
        case AgentStatus::AnswerFinished: return "answer_finished";
    }
    return "unknown";
}

const char* session_event_name(SessionEventKind e) {
    switch (e) {
        case SessionEventKind::Connected:    return "connected";
        case SessionEventKind::Disconnected: return "disconnected";
        case SessionEventKind::Failed:       return "failed";
        case SessionEventKind::Updated:      return "updated";
    }
    return "unknown";
}

} // namespace cg