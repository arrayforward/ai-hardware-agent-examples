#include "base64.hpp"

#include <array>
#include <cstdint>

namespace cg {

namespace {
constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<int8_t, 256> make_decode_table() {
    std::array<int8_t, 256> t{};
    for (int i = 0; i < 256; ++i) t[i] = -1;
    for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(kAlphabet[i])] = static_cast<int8_t>(i);
    return t;
}
} // namespace

std::string base64_encode(std::string_view data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        std::uint32_t v = (static_cast<std::uint8_t>(data[i]) << 16) |
                          (static_cast<std::uint8_t>(data[i + 1]) << 8) |
                          (static_cast<std::uint8_t>(data[i + 2]));
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
        out += kAlphabet[v & 0x3F];
    }
    if (i < data.size()) {
        std::uint32_t v = static_cast<std::uint8_t>(data[i]) << 16;
        if (i + 1 < data.size()) v |= static_cast<std::uint8_t>(data[i + 1]) << 8;
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += (i + 1 < data.size()) ? kAlphabet[(v >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

} // namespace cg