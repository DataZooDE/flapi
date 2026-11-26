#pragma once

#include <optional>
#include <string>
#include <vector>

namespace flapi {

class ConfigManager;

struct SecurityWarning {
    std::string code;
    std::string message;
    std::string location;
};

class SecurityAuditor {
public:
    std::vector<SecurityWarning> audit(const ConfigManager& config) const;

    static std::string classifyPassword(const std::string& password);

    /// `flapi --strict`: why the run is refused, or nullopt when there is no
    /// warning. Names each distinct code once, in the order first reported.
    static std::optional<std::string> strictRefusal(const std::vector<SecurityWarning>& warnings);
};

} // namespace flapi
