#include "llm_backend.hpp"

#include "cloud_gateway/log.hpp"

namespace cg {

void StubLlm::chat(const std::string& asr_text,
                   const JsonNode& /*session_config*/,
                   Callback cb) {
    GATEWAY_LOG_INFO("[StubLlm] chat asr=%s", asr_text.c_str());

    // Stream a few text chunks then a function call.
    cb({
        {"text", "好的，听到你说："},
        {"text", asr_text},
        {"text", "，我帮你查一下……"},
        {"function_call",
         R"({"call_id":"call_stub_1","name":"emotion","arguments":"{\"emotion\":\"happy\"}"})"},
        {"text", " 哈哈，真是个有趣的问题！"},
    });
}

void StubLlm::feed_tool_result(const std::string& call_id,
                               const std::string& output,
                               Callback cb) {
    GATEWAY_LOG_INFO("[StubLlm] tool result %s -> %s",
                     call_id.c_str(), output.c_str());
    cb({
        {"text", "（已收到工具结果）"},
    });
}

} // namespace cg