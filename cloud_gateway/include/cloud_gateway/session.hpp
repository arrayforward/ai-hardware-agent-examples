/*!
 * @file session.hpp
 * @brief Public session-related types (forward decls only).
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace cg {

// Forward decl - the session itself is owned by SessionManager.
class Session;

// Opaque handle to a WebSocket connection (defined in ws/server.hpp).
class WsConnection;

// Opaque handles for thread-pool task submission.
class ThreadPool;

// Common time helpers.
inline std::int64_t now_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline std::uint64_t unix_ms() noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Session id (also used as WebSocket sub-session identifier).
using SessionId = std::string;

} // namespace cg