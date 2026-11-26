#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "config_manager.hpp"
#include "oidc_auth_handler.hpp"

namespace flapi {

/**
 * The OIDC token handlers of the process, one per effective configuration.
 *
 * REST (AuthMiddleware) and MCP (MCPAuthHandler) each kept a map of their own,
 * without a lock, keyed on `issuer:client_id` (#281, #284). So:
 *
 * - concurrent first requests inserted into an unsynchronised unordered_map;
 * - two endpoints with the same issuer and client but different audiences or
 *   claims shared whichever handler was built first, and one of them checked
 *   tokens against the other's audience;
 * - REST and MCP fetched discovery and JWKS separately.
 *
 * Here a handler is keyed on the whole configuration as requests see it (the
 * provider preset applied), the map is locked, and handlers for the same
 * issuer share one OIDCProviderKeys, so discovery and JWKS are fetched once
 * per issuer for the whole process.
 *
 * Handlers are never evicted: there is one per distinct OIDC block a config
 * has ever loaded, and a reload that changes a block adds one.
 */
class OIDCHandlerRegistry {
public:
    static OIDCHandlerRegistry& instance();

    /// The handler for `config` as written in YAML (the preset is applied here).
    std::shared_ptr<OIDCAuthHandler> handlerFor(const OIDCConfig& config);

    /// `config` with its preset applied: what a request is checked against.
    static OIDCConfig effectiveConfig(const OIDCConfig& config);
    /// The registry key of an effective config: every field that decides who
    /// is admitted or how keys are fetched, length-prefixed.
    static std::string keyFor(const OIDCConfig& effective);
    static OIDCAuthHandler::Config handlerConfigFor(const OIDCConfig& effective);

    std::size_t handlerCount() const;
    std::size_t providerCount() const;
    void clear();   // tests

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<OIDCAuthHandler>> handlers_;
    std::unordered_map<std::string, std::shared_ptr<OIDCProviderKeys>> providers_;
};

}  // namespace flapi
