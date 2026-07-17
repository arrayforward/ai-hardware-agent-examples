/*!
 * @file sha1.hpp
 * @brief RFC 3174 SHA-1 implementation (used for WS handshake).
 *
 * Minimal, self-contained. Suitable only for protocol use - not for crypto.
 */
#pragma once

#include <cstdint>
#include <string>

namespace cg {

class Sha1 {
public:
    Sha1();
    void update(const std::uint8_t* data, std::size_t len);
    void update(const char* data, std::size_t len) {
        update(reinterpret_cast<const std::uint8_t*>(data), len);
    }
    void finalize(std::uint8_t out[20]);

private:
    std::uint32_t state_[5];
    std::uint64_t bit_count_;
    std::uint8_t  buffer_[64];
    std::size_t   buffer_len_;
    void transform(const std::uint8_t block[64]);
};

// One-shot helper.
void sha1(const std::uint8_t* data, std::size_t len, std::uint8_t out[20]);

inline void sha1(const char* data, std::size_t len, std::uint8_t out[20]) {
    sha1(reinterpret_cast<const std::uint8_t*>(data), len, out);
}

std::string sha1_hex(const std::string& data);

} // namespace cg