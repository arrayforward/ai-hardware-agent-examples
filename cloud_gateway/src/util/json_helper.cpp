#include "json_helper.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace cg {

// ============================================================================
//  Writer
// ============================================================================
void JsonWriter::write_number(std::int64_t n) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
    out_ += buf;
}

void JsonWriter::write_number(double d) {
    if (std::isnan(d) || std::isinf(d)) {
        out_ += "null";
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    out_ += buf;
}

void JsonWriter::write_string(std::string_view s) {
    out_ += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out_ += "\\\""; break;
            case '\\': out_ += "\\\\"; break;
            case '\b': out_ += "\\b";  break;
            case '\f': out_ += "\\f";  break;
            case '\n': out_ += "\\n";  break;
            case '\r': out_ += "\\r";  break;
            case '\t': out_ += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                   static_cast<unsigned>(c));
                    out_ += buf;
                } else {
                    out_ += c;
                }
        }
    }
    out_ += '"';
}

void JsonWriter::write_array()  { out_ += "[]"; }
void JsonWriter::write_object() { out_ += "{}"; }

void JsonWriter::write_value(const JsonNode& n) {
    if (n.is_null())        write_null();
    else if (n.is_bool())   write_bool(n.as_bool());
    else if (std::holds_alternative<std::int64_t>(n.v)) write_number(n.as_int());
    else if (std::holds_alternative<double>(n.v))      write_number(n.as_double());
    else if (n.is_string())  write_string(n.as_string());
    else if (n.is_array()) {
        out_ += '[';
        bool first = true;
        for (const auto& x : n.as_array()) {
            if (!first) out_ += ',';
            first = false;
            write_value(x);
        }
        out_ += ']';
    }
    else if (n.is_object()) {
        out_ += '{';
        bool first = true;
        for (const auto& [k, v] : n.as_object()) {
            if (!first) out_ += ',';
            first = false;
            write_string(k);
            out_ += ':';
            write_value(v);
        }
        out_ += '}';
    }
}

std::string to_json(const JsonNode& n) {
    JsonWriter w;
    w.write_value(n);
    return w.str();
}

// ============================================================================
//  Parser
// ============================================================================
void JsonParser::skip_ws() {
    while (pos_ < s_.size()) {
        char c = s_[pos_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
        else break;
    }
}

void JsonParser::expect(char c) {
    skip_ws();
    if (pos_ >= s_.size() || s_[pos_] != c) {
        die(std::string("expected '") + c + "'");
    }
    ++pos_;
}

void JsonParser::die(std::string_view msg) {
    throw ParseError(std::string(msg) + " at offset " + std::to_string(pos_));
}

JsonNode JsonParser::parse_string() {
    expect('"');
    std::string out;
    while (pos_ < s_.size()) {
        char c = s_[pos_++];
        if (c == '"') return JsonNode(std::move(out));
        if (c == '\\') {
            if (pos_ >= s_.size()) die("unterminated escape");
            char e = s_[pos_++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (pos_ + 4 > s_.size()) die("short \\u escape");
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s_[pos_++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                        else die("bad hex digit");
                    }
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: die("unknown escape");
            }
        } else {
            out += c;
        }
    }
    die("unterminated string");
}

JsonNode JsonParser::parse_number() {
    std::size_t start = pos_;
    if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
    while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
    bool is_double = false;
    if (pos_ < s_.size() && s_[pos_] == '.') {
        is_double = true;
        ++pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
        is_double = true;
        ++pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
    }
    std::string tok(s_.substr(start, pos_ - start));
    if (is_double) {
        return JsonNode(std::stod(tok));
    }
    return JsonNode(static_cast<std::int64_t>(std::stoll(tok)));
}

JsonNode JsonParser::parse_array() {
    expect('[');
    std::vector<JsonNode> items;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return JsonNode(std::move(items)); }
    while (true) {
        items.push_back(parse_value());
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
        expect(']');
        return JsonNode(std::move(items));
    }
}

JsonNode JsonParser::parse_object() {
    expect('{');
    std::map<std::string, JsonNode> obj;
    skip_ws();
    if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return JsonNode(std::move(obj)); }
    while (true) {
        skip_ws();
        if (pos_ >= s_.size() || s_[pos_] != '"') die("expected string key");
        JsonNode k = parse_string();
        expect(':');
        obj.emplace(std::move(k.as_string()), parse_value());
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
        expect('}');
        return JsonNode(std::move(obj));
    }
}

JsonNode JsonParser::parse_value() {
    skip_ws();
    if (pos_ >= s_.size()) die("unexpected end");
    char c = s_[pos_];
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return parse_string();
    if (c == 't' || c == 'f') {
        if (s_.substr(pos_, 4) == "true")  { pos_ += 4; return JsonNode(true); }
        if (s_.substr(pos_, 5) == "false") { pos_ += 5; return JsonNode(false); }
        die("bad literal");
    }
    if (c == 'n') {
        if (s_.substr(pos_, 4) == "null") { pos_ += 4; return JsonNode(nullptr); }
        die("bad literal");
    }
    return parse_number();
}

JsonNode JsonParser::parse() {
    JsonNode root = parse_value();
    skip_ws();
    if (pos_ != s_.size()) die("trailing data");
    return root;
}

JsonNode parse_json(std::string_view text) {
    JsonParser p(text);
    return p.parse();
}

} // namespace cg