#include "extension_installer.hpp"
#include "external_access_policy.hpp"

#include <duckdb.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "archive_io.hpp"
#include <regex>
#include <set>

#include <crow/logging.h>

#include "duckdb.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "config_manager.hpp"
#include "selfpath.hpp"

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

long CurrentProcessId() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

const std::vector<std::string>& ImplicitExtensions() {
    static const std::vector<std::string> extensions = {"core_functions"};
    return extensions;
}

const std::vector<std::string>& DefaultExtensions() {
    static const std::vector<std::string> extensions = {"httpfs", "ducklake", "fts",    "json",
                                                        "postgres", "sqlite", "parquet"};
    return extensions;
}

std::set<std::string> StaticallyLinkedExtensions(duckdb_connection connection) {
    std::set<std::string> names;
    duckdb_result result;
    if (duckdb_query(connection,
                     "SELECT extension_name FROM duckdb_extensions() WHERE install_mode = 'STATICALLY_LINKED'",
                     &result) == DuckDBSuccess) {
        const idx_t rows = duckdb_row_count(&result);
        for (idx_t row = 0; row < rows; ++row) {
            char* name = duckdb_value_varchar(&result, 0, row);
            if (name != nullptr) {
                names.insert(name);
                duckdb_free(name);
            }
        }
    }
    duckdb_destroy_result(&result);
    return names;
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

namespace {

namespace fs = std::filesystem;

bool HasExtensionFiles(const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() == ".duckdb_extension") {
            return true;
        }
    }
    return false;
}

std::vector<fs::path> Subdirectories(const fs::path& dir) {
    std::vector<fs::path> result;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) {
            result.push_back(it->path());
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string SqlLiteral(const std::string& text) {
    std::string quoted = "'";
    for (const char c : text) {
        quoted += c;
        if (c == '\'') {
            quoted += '\'';
        }
    }
    return quoted + "'";
}

enum class ShippedCopy { NotShipped, NotNeeded, Copied, Failed };

// Copies `name` (and its .info install record) from the shipped directory into `target_root`
// in DuckDB's layout. The signed file is copied byte for byte, so its signature still holds.
ShippedCopy CopyShippedExtension(const fs::path& shipped_root, const std::string& name, const std::string& target_root,
                                 std::string& error) {
    const std::string version = duckdb::ExtensionHelper::GetVersionDirectoryName();
    const std::string platform = duckdb::DuckDB::Platform();
    const std::string file = duckdb::ExtensionHelper::ApplyExtensionAlias(name) + ".duckdb_extension";
    const fs::path source = shipped_root / version / platform / file;
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) {
        return ShippedCopy::NotShipped;
    }
    if (target_root.empty()) {
        return ShippedCopy::NotNeeded;
    }
    const fs::path target = fs::path(target_root) / version / platform;
    if (fs::exists(target / file, ec)) {
        return ShippedCopy::NotNeeded;  // already installed there; INSTALL would not replace it either
    }
    fs::create_directories(target, ec);
    for (const fs::path& from : {source, fs::path(source.string() + ".info")}) {
        if (!fs::exists(from, ec)) {
            continue;
        }
        const fs::path to = target / from.filename();
        if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
            error = "cannot copy " + from.string() + " to " + to.string() + ": " + ec.message();
            return ShippedCopy::Failed;
        }
        // The shipped copy may be read-only; the installed one is the user's to replace.
        fs::permissions(to, fs::perms::owner_write, fs::perm_options::add, ec);
    }
    return ShippedCopy::Copied;
}

}  // namespace

ShippedExtensions FindShippedExtensions(const std::filesystem::path& executable, const std::string& version,
                                        const std::string& platform) {
    ShippedExtensions result;
    const fs::path root = executable.parent_path() / "extensions";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return result;
    }
    if (HasExtensionFiles(root / version / platform)) {
        result.root = root;
        return result;
    }
    std::string found;
    for (const auto& version_dir : Subdirectories(root)) {
        for (const auto& platform_dir : Subdirectories(version_dir)) {
            if (HasExtensionFiles(platform_dir)) {
                found += (found.empty() ? "" : ", ") + version_dir.filename().string() + "/" +
                         platform_dir.filename().string();
            }
        }
    }
    result.ignored = "Ignoring the DuckDB extensions shipped in " + root.string() + ": " +
                     (found.empty() ? std::string("it holds none") : "they are for " + found) +
                     ", this flapi needs " + version + "/" + platform +
                     ". Extensions are installed into the user directory instead.";
    return result;
}

std::optional<std::filesystem::path> ShippedExtensionDirectory() {
    static const std::optional<fs::path> shipped = [] {
        std::optional<fs::path> found;
        try {
            // weakly_canonical resolves a symlink to the binary (e.g. /usr/local/bin/flapi) to
            // the file itself; Linux' /proc/self/exe already is that file.
            const fs::path self = fs::weakly_canonical(GetSelfPath());
            const auto lookup = FindShippedExtensions(self, duckdb::ExtensionHelper::GetVersionDirectoryName(),
                                                      duckdb::DuckDB::Platform());
            if (lookup.root) {
                CROW_LOG_INFO << "Using the DuckDB extensions shipped with flapi: "
                              << (*lookup.root / duckdb::ExtensionHelper::GetVersionDirectoryName() /
                                  duckdb::DuckDB::Platform())
                                     .string();
            } else if (!lookup.ignored.empty()) {
                CROW_LOG_WARNING << lookup.ignored;
            }
            found = lookup.root;
        } catch (const std::exception& e) {
            CROW_LOG_DEBUG << "Not looking for shipped DuckDB extensions: " << e.what();
        }
        return found;
    }();
    return shipped;
}

std::string ExtensionDirectoriesStatement(const std::vector<std::string>& directories) {
    std::string list;
    for (const auto& directory : directories) {
        list += (list.empty() ? "" : ", ") + SqlLiteral(directory);
    }
    return "SET GLOBAL extension_directories = [" + list + "]";
}

std::optional<std::string> ShippedExtensionSearchStatement() {
    const auto shipped = ShippedExtensionDirectory();
    if (!shipped) {
        return std::nullopt;
    }
    // DuckDB's own default (~/.duckdb/extensions; DuckDB expands the `~`, so `home_directory`
    // still applies) stays first: installs go there, never into the shipped directory.
    return ExtensionDirectoriesStatement({"~/.duckdb/extensions", shipped->string()});
}

bool ExternalAccessEnabled(duckdb_connection connection) {
    duckdb_result result;
    bool enabled = true;
    if (duckdb_query(connection, "SELECT current_setting('enable_external_access')::BOOLEAN", &result) ==
            DuckDBSuccess &&
        duckdb_row_count(&result) > 0 && !duckdb_value_is_null(&result, 0, 0)) {
        enabled = duckdb_value_boolean(&result, 0, 0);
    }
    duckdb_destroy_result(&result);
    return enabled;
}

std::string LoadOrInstallExtension(duckdb_connection connection, const std::string& name, bool offline) {
    std::string error;
    auto run = [&](const std::string& sql) {
        duckdb_result result;
        const bool ok = duckdb_query(connection, sql.c_str(), &result) == DuckDBSuccess;
        if (!ok) {
            const char* message = duckdb_result_error(&result);
            error = message ? message : "unknown error";
        }
        duckdb_destroy_result(&result);
        return ok;
    };
    if (run("LOAD " + name)) {
        return "";
    }
    // After duckdb-hardening switched external access off on THIS database, only an
    // extension that is already loaded can be "loaded"; say so instead of DuckDB's bare
    // permission error. Asked of the connection, not a process flag: the bootstrap
    // DuckDB that reads a remote config is never restricted.
    if (!ExternalAccessEnabled(connection)) {
        return ExtensionUnavailableUnderHardening(name);
    }
    if (offline) {
        return error;
    }
    if (run("INSTALL " + name) && run("LOAD " + name)) {
        return "";
    }
    return error;
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
    const auto built_in = StaticallyLinkedExtensions(con);
    const auto shipped = ShippedExtensionDirectory();
    for (const auto& spec : specs) {
        const std::string label = spec.repository.empty() ? spec.name : spec.name + "@" + spec.repository;
        // A repository-qualified spec is someone asking for that build on purpose: installed.
        if (spec.repository.empty() && built_in.count(spec.name) != 0) {
            log << "✓ " << label << " (built into flapi, nothing to install)\n";
            continue;
        }
        if (spec.repository.empty() && shipped) {
            std::string error;
            const auto outcome = CopyShippedExtension(*shipped, spec.name, extension_directory, error);
            if (outcome == ShippedCopy::Copied) {
                log << "✓ " << label << " (copied from the extensions shipped with flapi)\n";
                continue;
            }
            if (outcome == ShippedCopy::NotNeeded) {
                log << "✓ " << label << " (shipped with flapi, nothing to install)\n";
                continue;
            }
            if (outcome == ShippedCopy::Failed) {
                log << "✗ " << label << ": " << error << "\n";
                ++failures;
                continue;
            }
        }
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

bool ExtensionsOfflineRequested() {
    if (const char* flag = std::getenv("FLAPI_EXTENSIONS_OFFLINE"); flag != nullptr && *flag != '\0') {
        std::string value = flag;
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }
    return BundledExtensionDirectory().has_value();
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
    const fs::path staging = cache_root / (dir.filename().string() + ".tmp-" + std::to_string(CurrentProcessId()));
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
