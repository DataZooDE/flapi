#include "include/mcp_session_manager.hpp"
#include "include/mcp_constants.hpp"
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace flapi {

MCPSessionManager::MCPSessionManager()
    : session_timeout_(flapi::mcp::constants::DEFAULT_SESSION_TIMEOUT_MINUTES) {
}

std::string MCPSessionManager::createSession(
    const std::string& client_version,
    const std::optional<MCPSession::AuthContext>& auth_context) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    std::string session_id = generateSessionId();

    MCPSession session;
    session.session_id = session_id;
    session.client_version = client_version;
    session.auth_context = auth_context;

    sessions_[session_id] = session;
    return session_id;
}

std::optional<MCPSession> MCPSessionManager::getSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return std::nullopt;
    }

    if (isSessionExpired(it->second)) {
        sessions_.erase(it);
        return std::nullopt;
    }

    return it->second;
}

void MCPSessionManager::updateSessionActivity(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second.last_activity = std::chrono::steady_clock::now();
    }
}

void MCPSessionManager::removeSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(session_id);
}

void MCPSessionManager::cleanupExpiredSessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto now = std::chrono::steady_clock::now();
    auto timeout_duration = std::chrono::minutes(session_timeout_);

    for (auto it = sessions_.begin(); it != sessions_.end();) {
        auto session_duration = now - it->second.last_activity;
        if (session_duration > timeout_duration) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void MCPSessionManager::setSessionTimeout(std::chrono::minutes timeout) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    session_timeout_ = timeout;
}

std::chrono::minutes MCPSessionManager::getSessionTimeout() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return session_timeout_;
}

bool MCPSessionManager::isSessionValid(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }

    if (isSessionExpired(it->second)) {
        return false;
    }

    return true;
}

size_t MCPSessionManager::getActiveSessionCount() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_.size();
}

std::string MCPSessionManager::generateSessionId() const {
    // Generate a random session ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis;

    uint32_t part1 = dis(gen);
    uint32_t part2 = dis(gen);
    uint32_t part3 = dis(gen);

    std::stringstream ss;
    ss << std::hex << std::setw(8) << std::setfill('0') << part1
       << std::hex << std::setw(8) << std::setfill('0') << part2
       << std::hex << std::setw(8) << std::setfill('0') << part3;

    return ss.str();
}

bool MCPSessionManager::isSessionExpired(const MCPSession& session) const {
    auto now = std::chrono::steady_clock::now();
    auto timeout_duration = std::chrono::minutes(session_timeout_);
    return (now - session.last_activity) > timeout_duration;
}

bool MCPSessionManager::isSessionAuthenticated(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return false;
    }

    if (isSessionExpired(it->second)) {
        return false;
    }

    return it->second.isAuthenticated();
}

std::optional<MCPSession::AuthContext> MCPSessionManager::getAuthContext(
    const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return std::nullopt;
    }

    if (isSessionExpired(it->second)) {
        return std::nullopt;
    }

    return it->second.auth_context;
}

} // namespace flapi
