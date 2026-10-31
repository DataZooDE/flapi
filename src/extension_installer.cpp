#include "extension_installer.hpp"

#include "config_manager.hpp"

namespace flapi {

std::optional<ExtensionSpec> ParseExtensionSpec(const std::string& arg) {
    return std::nullopt;  // TODO(G2)
}

std::string BuildInstallStatement(const ExtensionSpec& spec) {
    return "";  // TODO(G2)
}

std::vector<ExtensionSpec> ExtractInstallSpecs(const std::string& init_sql) {
    return {};  // TODO(G2)
}

std::vector<ExtensionSpec> CollectExtensionSpecs(const ConfigManager& config_manager) {
    return {};  // TODO(G2)
}

int InstallExtensions(const std::vector<ExtensionSpec>& specs, const std::string& extension_directory,
                      std::ostream& log) {
    return 0;  // TODO(G2)
}

} // namespace flapi
