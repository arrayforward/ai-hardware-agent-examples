/*!
 * @file session_manager.hpp
 * @brief Owns active sessions and dispatches incoming frames.
 */
#pragma once

#include "session.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace cg {

class SessionManager {
public:
    void add(std::shared_ptr<Session> s);
    void remove(const SessionId& id);
    std::shared_ptr<Session> get(const SessionId& id);
    std::size_t count() const;

private:
    mutable std::mutex                                  m_;
    std::unordered_map<SessionId, std::shared_ptr<Session>> map_;
};

} // namespace cg