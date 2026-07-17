/*!
 * @file llm_backend.hpp
 * @brief LLM backend interface (chat + tool/function calling).
 */
#pragma once

#include "../util/json_helper.hpp"

#include <functional>
#include <string>
#include <vector>

namespace cg {

// One piece of LLM output. Either a text fragment or a function call.
struct LlmChunk {
    std::string kind;     // "text" or "function_call"
    std::string content;  // raw content (text body, or JSON for function call)
};

class LlmBackend {
public:
    virtual ~LlmBackend() = default;

    // ASR transcript -> ordered list of chunks. Callback may be invoked
    // multiple times for streaming models.
    using Callback = std::function<void(std::vector<LlmChunk> batch)>;

    virtual void chat(const std::string& asr_text,
                      const JsonNode& session_config,
                      Callback cb) = 0;

    // Inject tool/function output so the model can continue.
    virtual void feed_tool_result(const std::string& call_id,
                                  const std::string& output,
                                  Callback cb) = 0;
};

// Deterministic stub: returns canned response + one emotion function call.
class StubLlm : public LlmBackend {
public:
    void chat(const std::string& asr_text,
              const JsonNode& session_config,
              Callback cb) override;
    void feed_tool_result(const std::string& call_id,
                          const std::string& output,
                          Callback cb) override;
};

} // namespace cg