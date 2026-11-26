#pragma once

#include <optional>
#include <string>
#include <vector>

namespace flapi {

class ConfigManager;
struct EndpointConfig;

struct SecurityWarning {
    std::string code;
    std::string message;
    std::string location;
};

class SecurityAuditor {
public:
    std::vector<SecurityWarning> audit(const ConfigManager& config) const;

    /// The findings that belong to one endpoint (its auth block, its MCP
    /// entities, its description, its unread or repeated keys), judged against
    /// `config`'s global settings. audit() reports these for every endpoint;
    /// ConfigManager uses it under --strict to refuse an endpoint created or
    /// reloaded later.
    std::vector<SecurityWarning> auditEndpoint(const EndpointConfig& endpoint,
                                               const ConfigManager& config) const;

    static std::string classifyPassword(const std::string& password);

    /// `flapi --strict`: why the run is refused, or nullopt when there is no
    /// warning. Names each distinct code once, in the order first reported.
    static std::optional<std::string> strictRefusal(const std::vector<SecurityWarning>& warnings);
};

} // namespace flapi
