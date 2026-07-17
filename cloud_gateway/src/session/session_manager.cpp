#include "session_manager.hpp"

namespace cg {

void SessionManager::add(std::shared_ptr<Session> s) {
    std::lock_guard<std::mutex> lk(m_);
    map_.emplace(s->id(), std::move(s));
}

void SessionManager::remove(const SessionId& id) {
    std::lock_guard<std::mutex> lk(m_);
    map_.erase(id);
}

std::shared_ptr<Session> SessionManager::get(const SessionId& id) {
    std::lock_guard<std::mutex> lk(m_);
    auto it = map_.find(id);
    return it == map_.end() ? nullptr : it->second;
}

std::size_t SessionManager::count() const {
    std::lock_guard<std::mutex> lk(m_);
    return map_.size();
}

} // namespace cg