#pragma once

#include "mcp_types.hpp"
#include <string>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <chrono>

namespace flapi {

class MCPSessionManager {
public:
    MCPSessionManager();
    ~MCPSessionManager() = default;

    // Session management
    std::string createSession(const std::string& client_version = "",
                             const std::optional<MCPSession::AuthContext>& auth_context = std::nullopt);
    std::optional<MCPSession> getSession(const std::string& session_id);
    void updateSessionActivity(const std::string& session_id);
    void removeSession(const std::string& session_id);
    void cleanupExpiredSessions();

    // Session configuration
    void setSessionTimeout(std::chrono::minutes timeout);
    std::chrono::minutes getSessionTimeout() const;

    // Session utilities
    bool isSessionValid(const std::string& session_id) const;
    size_t getActiveSessionCount() const;

    // Authentication utilities
    bool isSessionAuthenticated(const std::string& session_id) const;
    std::optional<MCPSession::AuthContext> getAuthContext(const std::string& session_id) const;

private:
    std::unordered_map<std::string, MCPSession> sessions_;
    mutable std::mutex sessions_mutex_;
    std::chrono::minutes session_timeout_;

    // Helper methods
    std::string generateSessionId() const;
    bool isSessionExpired(const MCPSession& session) const;
};

} // namespace flapi
