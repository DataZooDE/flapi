#include "extension_installer.hpp"

#include <duckdb.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <unistd.h>

#include "archive_io.hpp"
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

namespace {

std::mutex& bundledMutex() {
    static std::mutex mutex;
    return mutex;
}

std::optional<std::filesystem::path>& bundledDirectory() {
    static std::optional<std::filesystem::path> dir;
    return dir;
}

// FNV-1a over names, sizes and bytes: names the extraction directory, so a re-packed
// binary with different extensions never reuses (or half-overwrites) an older one.
std::string contentHash(const std::vector<std::pair<std::string, const std::vector<std::uint8_t>*>>& files) {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](unsigned char byte) {
        h ^= byte;
        h *= 1099511628211ULL;
    };
    for (const auto& [name, data] : files) {
        for (const char c : name) {
            mix(static_cast<unsigned char>(c));
        }
        mix(0);
        for (const char c : std::to_string(data->size())) {
            mix(static_cast<unsigned char>(c));
        }
        mix(0);
        for (const auto byte : *data) {
            mix(byte);
        }
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
    return hex;
}

} // namespace

void SetBundledExtensionDirectory(std::optional<std::filesystem::path> dir) {
    std::lock_guard<std::mutex> lock(bundledMutex());
    bundledDirectory() = std::move(dir);
}

std::optional<std::filesystem::path> BundledExtensionDirectory() {
    std::lock_guard<std::mutex> lock(bundledMutex());
    return bundledDirectory();
}

std::optional<std::filesystem::path> MaterializeBundledExtensions(
    const std::map<std::string, std::vector<std::uint8_t>>& entries, const std::filesystem::path& cache_root) {
    namespace fs = std::filesystem;
    const std::string prefix = kBundledExtensionsPrefix;
    std::vector<std::pair<std::string, const std::vector<std::uint8_t>*>> files;
    for (const auto& [name, data] : entries) {
        if (name.rfind(prefix, 0) != 0) {
            continue;
        }
        const std::string rel = name.substr(prefix.size());
        const fs::path normal = fs::path(rel).lexically_normal();
        if (rel.empty() || normal.is_absolute() || normal.generic_string().rfind("..", 0) == 0) {
            throw std::runtime_error("bundled extension entry escapes its directory: " + name);
        }
        files.emplace_back(normal.generic_string(), &data);
    }
    if (files.empty()) {
        return std::nullopt;
    }

    const fs::path dir = cache_root / ("ext-" + contentHash(files));
    if (fs::exists(dir / ".complete")) {
        return dir;
    }
    // Written beside the target and renamed into place: a crash or a concurrent start
    // never leaves a half-written directory under the final name.
    const fs::path staging = cache_root / (dir.filename().string() + ".tmp-" + std::to_string(::getpid()));
    fs::remove_all(staging);
    for (const auto& [rel, data] : files) {
        const fs::path target = staging / rel;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
        if (!out) {
            throw std::runtime_error("cannot write bundled extension " + target.string());
        }
    }
    std::ofstream(staging / ".complete") << "ok\n";
    std::error_code ec;
    fs::rename(staging, dir, ec);
    if (ec) {
        // Another process got there first; its copy has the same contents.
        fs::remove_all(staging);
        if (!fs::exists(dir / ".complete")) {
            throw std::runtime_error("cannot place bundled extensions in " + dir.string() + ": " + ec.message());
        }
    }
    return dir;
}

} // namespace flapi
