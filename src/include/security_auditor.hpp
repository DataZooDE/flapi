#pragma once

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
};

} // namespace flapi
