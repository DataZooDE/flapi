#pragma once

#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace flapi {

class ConfigManager;

// A DuckDB extension and the repository it comes from (empty = the core repository).
struct ExtensionSpec {
    std::string name;
    std::string repository;
};

// Parses `name` or `name@repository`. Both parts must be identifiers: they end up in SQL.
std::optional<ExtensionSpec> ParseExtensionSpec(const std::string& arg);

std::string BuildInstallStatement(const ExtensionSpec& spec);

// The `[FORCE] INSTALL <name> [FROM <repository>]` statements in a block of init SQL.
std::vector<ExtensionSpec> ExtractInstallSpecs(const std::string& init_sql);

// duckdb.default-extensions, then every INSTALL in connections.*.init; first occurrence wins.
std::vector<ExtensionSpec> CollectExtensionSpecs(const ConfigManager& config_manager);

// Installs into `extension_directory` (empty = DuckDB's default). Returns the number of
// extensions that failed; every failure is written to `log`, so a build step reports all at once.
int InstallExtensions(const std::vector<ExtensionSpec>& specs, const std::string& extension_directory,
                      std::ostream& log);

} // namespace flapi
