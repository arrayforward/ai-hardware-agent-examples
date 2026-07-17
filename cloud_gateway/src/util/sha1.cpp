#include "sha1.hpp"

#include <cstring>

namespace cg {

namespace {
inline std::uint32_t rotl(std::uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}
} // namespace

Sha1::Sha1() : bit_count_(0), buffer_len_(0) {
    state_[0] = 0x67452301u;
    state_[1] = 0xEFCDAB89u;
    state_[2] = 0x98BADCFEu;
    state_[3] = 0x10325476u;
    state_[4] = 0xC3D2E1F0u;
}

void Sha1::transform(const std::uint8_t block[64]) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t(block[i * 4 + 0]) << 24)
             | (std::uint32_t(block[i * 4 + 1]) << 16)
             | (std::uint32_t(block[i * 4 + 2]) << 8)
             |  std::uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2],
                  d = state_[3], e = state_[4];

    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20)       { f = (b & c) | ((~b) & d);                k = 0x5A827999u; }
        else if (i < 40)  { f = b ^ c ^ d;                           k = 0x6ED9EBA1u; }
        else if (i < 60)  { f = (b & c) | (b & d) | (c & d);         k = 0x8F1BBCDCu; }
        else              { f = b ^ c ^ d;                           k = 0xCA62C1D6u; }

        std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }

    state_[0] += a; state_[1] += b; state_[2] += c;
    state_[3] += d; state_[4] += e;
}

void Sha1::update(const std::uint8_t* data, std::size_t len) {
    bit_count_ += static_cast<std::uint64_t>(len) * 8;
    while (len > 0) {
        std::size_t take = std::min<std::size_t>(64 - buffer_len_, len);
        std::memcpy(buffer_ + buffer_len_, data, take);
        buffer_len_ += take;
        data        += take;
        len         -= take;
        if (buffer_len_ == 64) {
            transform(buffer_);
            buffer_len_ = 0;
        }
    }
}

void Sha1::finalize(std::uint8_t out[20]) {
    std::uint64_t final_bits = bit_count_;
    buffer_[buffer_len_++] = 0x80;
    if (buffer_len_ > 56) {
        while (buffer_len_ < 64) buffer_[buffer_len_++] = 0;
        transform(buffer_);
        buffer_len_ = 0;
    }
    while (buffer_len_ < 56) buffer_[buffer_len_++] = 0;
    for (int i = 7; i >= 0; --i) {
        buffer_[buffer_len_++] = static_cast<std::uint8_t>((final_bits >> (i * 8)) & 0xFF);
    }
    transform(buffer_);

    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >>  8) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i]        & 0xFF);
    }
}

void sha1(const std::uint8_t* data, std::size_t len, std::uint8_t out[20]) {
    Sha1 h;
    h.update(data, len);
    h.finalize(out);
}

std::string sha1_hex(const std::string& data) {
    std::uint8_t out[20];
    sha1(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), out);
    static const char hex[] = "0123456789abcdef";
    std::string r(40, '0');
    for (int i = 0; i < 20; ++i) {
        r[i * 2 + 0] = hex[(out[i] >> 4) & 0xF];
        r[i * 2 + 1] = hex[out[i] & 0xF];
    }
    return r;
}

} // namespace cg