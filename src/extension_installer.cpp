#include "extension_installer.hpp"

#include <duckdb.h>
#include <algorithm>
#include <regex>
#include <set>

#include "config_manager.hpp"

namespace flapi {

namespace {

const std::regex& identifier() {
    static const std::regex re("^[A-Za-z_][A-Za-z0-9_]*$");
    return re;
}

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

} // namespace

const std::vector<std::string>& ImplicitExtensions() {
    static const std::vector<std::string> extensions = {"core_functions"};
    return extensions;
}

std::optional<ExtensionSpec> ParseExtensionSpec(const std::string& arg) {
    const auto at = arg.find('@');
    ExtensionSpec spec;
    spec.name = arg.substr(0, at);
    if (at != std::string::npos) {
        spec.repository = arg.substr(at + 1);
        if (!std::regex_match(spec.repository, identifier())) {
            return std::nullopt;
        }
    }
    if (!std::regex_match(spec.name, identifier())) {
        return std::nullopt;
    }
    return spec;
}

std::string BuildInstallStatement(const ExtensionSpec& spec) {
    std::string sql = "INSTALL \"" + spec.name + "\"";
    if (!spec.repository.empty()) {
        sql += " FROM " + spec.repository;
    }
    return sql + ";";
}

std::vector<ExtensionSpec> ExtractInstallSpecs(const std::string& init_sql) {
    // Statement by statement, anchored at the start, so `SELECT 'INSTALL x'` is not one.
    static const std::regex install(
        R"(^(?:FORCE\s+)?INSTALL\s+(['"]?)([A-Za-z_][A-Za-z0-9_]*)\1(?:\s+FROM\s+(['"]?)([A-Za-z_][A-Za-z0-9_]*)\3)?$)",
        std::regex::icase);
    std::vector<ExtensionSpec> specs;
    std::size_t start = 0;
    while (start <= init_sql.size()) {
        auto end = init_sql.find(';', start);
        if (end == std::string::npos) {
            end = init_sql.size();
        }
        const std::string statement = trim(init_sql.substr(start, end - start));
        std::smatch match;
        if (std::regex_match(statement, match, install)) {
            specs.push_back({match[2].str(), match[4].str()});
        }
        start = end + 1;
    }
    return specs;
}

std::vector<ExtensionSpec> CollectExtensionSpecs(const ConfigManager& config_manager) {
    std::vector<ExtensionSpec> specs;
    std::set<std::string> seen;
    auto add = [&](const ExtensionSpec& spec) {
        if (seen.insert(spec.name).second) {
            specs.push_back(spec);
        }
    };
    for (const auto& name : ImplicitExtensions()) {
        add({name, ""});
    }
    for (const auto& name : config_manager.getDuckDBConfig().default_extensions) {
        add({name, ""});
    }
    // Sorted by connection name: the map is unordered, and the output should not be.
    const auto& connections = config_manager.getConnections();
    std::vector<std::string> names;
    for (const auto& [name, connection] : connections) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        for (const auto& spec : ExtractInstallSpecs(connections.at(name).getInit())) {
            add(spec);
        }
    }
    return specs;
}

int InstallExtensions(const std::vector<ExtensionSpec>& specs, const std::string& extension_directory,
                      std::ostream& log) {
    duckdb_config config;
    duckdb_create_config(&config);
    if (!extension_directory.empty() &&
        duckdb_set_config(config, "extension_directory", extension_directory.c_str()) == DuckDBError) {
        duckdb_destroy_config(&config);
        log << "cannot use extension directory " << extension_directory << "\n";
        return static_cast<int>(specs.size());
    }
    duckdb_database db = nullptr;
    char* open_error = nullptr;
    if (duckdb_open_ext(nullptr, &db, config, &open_error) == DuckDBError) {
        log << "cannot open DuckDB: " << (open_error ? open_error : "unknown error") << "\n";
        duckdb_free(open_error);
        duckdb_destroy_config(&config);
        return static_cast<int>(specs.size());
    }
    duckdb_destroy_config(&config);
    duckdb_connection con = nullptr;
    duckdb_connect(db, &con);

    int failures = 0;
    for (const auto& spec : specs) {
        const std::string label = spec.repository.empty() ? spec.name : spec.name + "@" + spec.repository;
        duckdb_result result;
        if (duckdb_query(con, BuildInstallStatement(spec).c_str(), &result) == DuckDBError) {
            const char* error = duckdb_result_error(&result);
            log << "✗ " << label << ": " << (error ? error : "unknown error") << "\n";
            ++failures;
        } else {
            log << "✓ " << label << "\n";
        }
        duckdb_destroy_result(&result);
    }
    duckdb_disconnect(&con);
    duckdb_close(&db);
    return failures;
}

} // namespace flapi
