/*!
 * @file json_helper.hpp
 * @brief Minimal JSON encoder/decoder sufficient for protocol envelopes.
 *
 * This is NOT a general-purpose JSON library - it supports only the subset
 * used by the gateway: string, number, bool, null, object, array.
 *
 * Limits: nesting <= 32, total JSON size <= 256 KB (configurable).
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cg {

// ----------------------------------------------------------------------------
//  JSON value (AST)
// ----------------------------------------------------------------------------
enum class JsonNull { Value = 0 };

using JsonValue = std::variant<
    JsonNull,
    bool,
    double,
    std::int64_t,
    std::string,
    std::vector<class JsonNode>,
    std::map<std::string, class JsonNode>
>;

struct JsonNode {
    JsonValue v;
    JsonNode() : v(JsonNull::Value) {}
    JsonNode(std::nullptr_t) : v(JsonNull::Value) {}
    JsonNode(bool b) : v(b) {}
    JsonNode(int i) : v(static_cast<std::int64_t>(i)) {}
    JsonNode(long i) : v(static_cast<std::int64_t>(i)) {}
    JsonNode(long long i) : v(static_cast<std::int64_t>(i)) {}
    JsonNode(unsigned i) : v(static_cast<std::int64_t>(i)) {}
    JsonNode(unsigned long i) : v(static_cast<std::int64_t>(i)) {}
    JsonNode(unsigned long long i) : v(static_cast<std::int64_t>(i)) {}
    JsonNode(double d) : v(d) {}
    JsonNode(const char* s) : v(std::string(s)) {}
    JsonNode(std::string s) : v(std::move(s)) {}
    JsonNode(std::vector<JsonNode> a) : v(std::move(a)) {}
    JsonNode(std::map<std::string, JsonNode> o) : v(std::move(o)) {}

    bool is_null() const   { return std::holds_alternative<JsonNull>(v); }
    bool is_bool() const   { return std::holds_alternative<bool>(v); }
    bool is_number() const { return std::holds_alternative<std::int64_t>(v) || std::holds_alternative<double>(v); }
    bool is_string() const { return std::holds_alternative<std::string>(v); }
    bool is_array() const  { return std::holds_alternative<std::vector<JsonNode>>(v); }
    bool is_object() const { return std::holds_alternative<std::map<std::string, JsonNode>>(v); }

    bool as_bool() const   { return std::get<bool>(v); }
    std::int64_t as_int() const {
        if (std::holds_alternative<std::int64_t>(v)) return std::get<std::int64_t>(v);
        if (std::holds_alternative<double>(v))      return static_cast<std::int64_t>(std::get<double>(v));
        if (std::holds_alternative<bool>(v))       return std::get<bool>(v) ? 1 : 0;
        return 0;
    }
    double as_double() const {
        if (std::holds_alternative<double>(v))      return std::get<double>(v);
        if (std::holds_alternative<std::int64_t>(v)) return static_cast<double>(std::get<std::int64_t>(v));
        return 0.0;
    }
    const std::string& as_string() const { return std::get<std::string>(v); }
    const std::vector<JsonNode>& as_array() const { return std::get<std::vector<JsonNode>>(v); }
    std::vector<JsonNode>& as_array() { return std::get<std::vector<JsonNode>>(v); }
    const std::map<std::string, JsonNode>& as_object() const { return std::get<std::map<std::string, JsonNode>>(v); }
    std::map<std::string, JsonNode>& as_object() { return std::get<std::map<std::string, JsonNode>>(v); }

    const JsonNode* find(const std::string& key) const {
        if (!is_object()) return nullptr;
        const auto& o = as_object();
        auto it = o.find(key);
        return it == o.end() ? nullptr : &it->second;
    }
};

// ----------------------------------------------------------------------------
//  Encoder
// ----------------------------------------------------------------------------
class JsonWriter {
public:
    std::string str() const { return out_; }
    void clear() { out_.clear(); }

    void write_null()                       { out_ += "null"; }
    void write_bool(bool b)                 { out_ += b ? "true" : "false"; }
    void write_number(std::int64_t n);
    void write_number(double d);
    void write_string(std::string_view s);
    void write_array();
    void write_object();

    void write_value(const JsonNode& n);

private:
    std::string out_;
};

std::string to_json(const JsonNode& n);

// ----------------------------------------------------------------------------
//  Decoder
// ----------------------------------------------------------------------------
struct ParseError : public std::runtime_error {
    explicit ParseError(const std::string& m) : std::runtime_error(m) {}
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : s_(text) {}
    JsonNode parse();

private:
    void skip_ws();
    JsonNode parse_value();
    JsonNode parse_object();
    JsonNode parse_array();
    JsonNode parse_string();
    JsonNode parse_number();
    void expect(char c);
    [[noreturn]] void die(std::string_view msg);

    std::string_view s_;
    std::size_t pos_ = 0;
};

JsonNode parse_json(std::string_view text);

// ----------------------------------------------------------------------------
//  Builder sugar
// ----------------------------------------------------------------------------
inline JsonNode kv(const std::string& k, JsonNode v) {
    return JsonNode(std::map<std::string, JsonNode>{{k, std::move(v)}});
}

inline JsonNode arr(std::vector<JsonNode> items) {
    return JsonNode(std::move(items));
}

} // namespace cg