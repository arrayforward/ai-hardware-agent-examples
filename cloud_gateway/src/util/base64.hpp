/*!
 * @file base64.hpp
 * @brief RFC 4648 Base64 encoder (used for WS handshake Sec-WebSocket-Accept).
 */
#pragma once

#include <string>
#include <string_view>

namespace cg {

std::string base64_encode(std::string_view data);

} // namespace cg