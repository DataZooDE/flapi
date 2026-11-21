#include "vfs_adapter.hpp"

#include "embedded_archive_file_provider.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <functional>
#include <vector>
#include <exception>
#include <mutex>
#include <cstring>
#include <cctype>
#include <crow/logging.h>

// DuckDB includes for VFS access
#include "duckdb.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"

// Include DatabaseManager for singleton access
#include "database_manager.hpp"
#include "credential_manager.hpp"
#include "extension_installer.hpp"

namespace flapi {

// ============================================================================
// PathSchemeUtils Implementation
// ============================================================================

namespace {
    // Helper to check if path starts with scheme (case-insensitive)
    bool StartsWithScheme(const std::string& path, const char* scheme) {
        size_t scheme_len = std::strlen(scheme);
        if (path.length() < scheme_len) {
            return false;
        }
        for (size_t i = 0; i < scheme_len; ++i) {
            if (std::tolower(static_cast<unsigned char>(path[i])) !=
                std::tolower(static_cast<unsigned char>(scheme[i]))) {
                return false;
            }
        }
        return true;
    }
}

bool PathSchemeUtils::IsRemotePath(const std::string& path) {
    return IsS3Path(path) || IsGCSPath(path) || IsAzurePath(path) || IsHttpPath(path);
}

std::string PathSchemeUtils::RedactUrl(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return url;
    }
    std::string out = url;
    if (const auto q = out.find('?', scheme_end); q != std::string::npos) {
        out.replace(q, std::string::npos, "?<redacted>");
    }
    const auto host_start = scheme_end + 3;
    const auto slash = out.find('/', host_start);
    const auto at = out.find('@', host_start);
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) {
        out.erase(host_start, at - host_start + 1);
    }
    return out;
}

std::string PathSchemeUtils::StripQuery(const std::string& url) {
    const auto cut = url.find_first_of("?#");
    return cut == std::string::npos ? url : url.substr(0, cut);
}

std::string PathSchemeUtils::JoinUrl(const std::string& base, const std::string& rel) {
    std::string path = StripQuery(base);
    const std::string suffix = base.substr(path.size());
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    std::string tail = rel;
    while (tail.rfind("./", 0) == 0) {
        tail.erase(0, 2);
    }
    while (!tail.empty() && tail.front() == '/') {
        tail.erase(0, 1);
    }
    return (tail.empty() ? path : path + "/" + tail) + suffix;
}

bool PathSchemeUtils::IsS3Path(const std::string& path) {
    return StartsWithScheme(path, SCHEME_S3);
}

bool PathSchemeUtils::IsGCSPath(const std::string& path) {
    return StartsWithScheme(path, SCHEME_GCS);
}

bool PathSchemeUtils::IsAzurePath(const std::string& path) {
    return StartsWithScheme(path, SCHEME_AZURE) || StartsWithScheme(path, SCHEME_AZURE_BLOB);
}

bool PathSchemeUtils::IsHttpPath(const std::string& path) {
    return StartsWithScheme(path, SCHEME_HTTP) || StartsWithScheme(path, SCHEME_HTTPS);
}

bool PathSchemeUtils::IsFilePath(const std::string& path) {
    return StartsWithScheme(path, SCHEME_FILE);
}

std::string PathSchemeUtils::GetScheme(const std::string& path) {
    // Check in order of specificity (longer schemes first for overlapping prefixes)
    if (StartsWithScheme(path, SCHEME_HTTPS)) {
        return SCHEME_HTTPS;
    }
    if (StartsWithScheme(path, SCHEME_HTTP)) {
        return SCHEME_HTTP;
    }
    if (IsS3Path(path)) {
        return SCHEME_S3;
    }
    if (IsGCSPath(path)) {
        return SCHEME_GCS;
    }
    if (StartsWithScheme(path, SCHEME_AZURE_BLOB)) {
        return SCHEME_AZURE_BLOB;
    }
    if (StartsWithScheme(path, SCHEME_AZURE)) {
        return SCHEME_AZURE;
    }
    if (IsFilePath(path)) {
        return SCHEME_FILE;
    }
    return "";
}

std::string PathSchemeUtils::StripFileScheme(const std::string& path) {
    if (IsFilePath(path)) {
        return path.substr(std::string(SCHEME_FILE).length());
    }
    return path;
}

// ============================================================================
// LocalFileProvider Implementation
// ============================================================================

std::string LocalFileProvider::ReadFile(const std::string& path) {
    // Handle file:// scheme
    std::string actual_path = PathSchemeUtils::StripFileScheme(path);

    // Check if file exists first
    if (!FileExists(actual_path)) {
        throw FileOperationError("File not found: " + path);
    }

    std::ifstream file(actual_path, std::ios::in | std::ios::binary);
    if (!file) {
        throw FileOperationError("Failed to open file: " + path);
    }

    std::ostringstream contents;
    contents << file.rdbuf();

    if (file.bad()) {
        throw FileOperationError("Error reading file: " + path);
    }

    return contents.str();
}

bool LocalFileProvider::FileExists(const std::string& path) {
    // Handle file:// scheme
    std::string actual_path = PathSchemeUtils::StripFileScheme(path);

    // Don't check remote paths
    if (PathSchemeUtils::IsRemotePath(actual_path)) {
        return false;
    }

    try {
        return std::filesystem::exists(actual_path) &&
               std::filesystem::is_regular_file(actual_path);
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
}

std::vector<std::string> LocalFileProvider::ListFiles(const std::string& directory,
                                                       const std::string& pattern) {
    // Handle file:// scheme
    std::string actual_dir = PathSchemeUtils::StripFileScheme(directory);

    std::vector<std::string> result;

    if (!std::filesystem::exists(actual_dir) ||
        !std::filesystem::is_directory(actual_dir)) {
        throw FileOperationError("Directory not found: " + directory);
    }

    // Convert glob pattern to regex
    // Simple conversion: * -> .*, ? -> ., escape other special chars
    std::string regex_pattern;
    for (char c : pattern) {
        switch (c) {
            case '*':
                regex_pattern += ".*";
                break;
            case '?':
                regex_pattern += ".";
                break;
            case '.':
            case '+':
            case '^':
            case '$':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '|':
            case '\\':
                regex_pattern += '\\';
                regex_pattern += c;
                break;
            default:
                regex_pattern += c;
                break;
        }
    }

    std::regex file_regex(regex_pattern, std::regex::icase);

    try {
        for (const auto& entry : std::filesystem::directory_iterator(actual_dir)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (std::regex_match(filename, file_regex)) {
                    result.push_back(entry.path().string());
                }
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        throw FileOperationError("Failed to list directory: " + directory + " - " + e.what());
    }

    // Sort for consistent ordering
    std::sort(result.begin(), result.end());

    return result;
}

bool LocalFileProvider::IsRemotePath(const std::string& path) const {
    return PathSchemeUtils::IsRemotePath(path);
}

// ============================================================================
// DuckDBVFSProvider Implementation
// ============================================================================

namespace {

// DuckDB's httpfs reports HTTP failures as a JSON blob (headers, response body,
// request ids). Users need the message: keep `exception_message` when present.
std::string CleanDuckError(const std::string& raw) {
    if (!raw.empty() && raw.front() == '{') {
        auto parsed = crow::json::load(raw);
        if (parsed && parsed.t() == crow::json::type::Object && parsed.has("exception_message") &&
            parsed["exception_message"].t() == crow::json::type::String) {
            std::string msg = parsed["exception_message"].s();
            const auto blank = msg.find("\n\n");
            if (blank != std::string::npos) {
                // First line is the HTTP status, next paragraph the store's own error code/message.
                const auto second = msg.find("\n\n", blank + 2);
                msg = msg.substr(0, second == std::string::npos ? msg.size() : second);
                std::replace(msg.begin(), msg.end(), '\n', ' ');
            }
            return msg;
        }
    }
    return raw;
}

struct BootstrapDb {
    duckdb_database db = nullptr;
    ~BootstrapDb() {
        if (db) {
            duckdb_close(&db);
        }
    }
};

// A DuckDB instance of its own, for reading a REMOTE configuration (a --config
// URL, a template.path in s3://) before DatabaseManager exists: the config
// decides what the database is, so it cannot be read through it. In-memory, with
// httpfs and the credentials found in the environment; created on first use.
// Every call opens its own connection, which keeps the instance alive for the
// call - nothing holds a FileSystem pointer across calls (#213).
std::shared_ptr<BootstrapDb> GetBootstrapDb() {
    static std::mutex mutex;
    static std::shared_ptr<BootstrapDb> instance;
    std::lock_guard<std::mutex> lock(mutex);
    if (instance) {
        return instance;
    }
    auto created = std::make_shared<BootstrapDb>();
    // The config file is not read yet, so only the environment (and a bundle's
    // embedded extensions) can say where extensions live - the same defaults
    // ConfigManager applies after `duckdb.extension_directory`.
    std::string extension_dir;
    if (const char* dir = std::getenv("FLAPI_EXTENSION_DIRECTORY"); dir != nullptr && *dir != '\0') {
        extension_dir = dir;
    } else if (const auto bundled = BundledExtensionDirectory()) {
        extension_dir = bundled->string();
    }
    bool offline = BundledExtensionDirectory().has_value();
    if (const char* flag = std::getenv("FLAPI_EXTENSIONS_OFFLINE"); flag != nullptr && *flag != '\0') {
        std::string v = flag;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
        offline = (v == "1" || v == "true" || v == "yes" || v == "on");
    }
    duckdb_config config = nullptr;
    if (duckdb_create_config(&config) == DuckDBSuccess) {
        if (!extension_dir.empty()) {
            duckdb_set_config(config, "extension_directory", extension_dir.c_str());
        }
        if (offline) {
            // LOAD of a known extension would otherwise auto-install (download) it.
            duckdb_set_config(config, "autoinstall_known_extensions", "false");
            duckdb_set_config(config, "autoload_known_extensions", "false");
        }
    }
    const auto opened = duckdb_open_ext(nullptr, &created->db, config, nullptr);
    if (config) {
        duckdb_destroy_config(&config);
    }
    if (opened != DuckDBSuccess) {
        throw FileOperationError("Could not start the bootstrap DuckDB used to read remote files.");
    }
    duckdb_connection conn = nullptr;
    if (duckdb_connect(created->db, &conn) != DuckDBSuccess) {
        throw FileOperationError("Could not connect to the bootstrap DuckDB.");
    }
    // Offline: never download; httpfs must already be in the extension directory.
    for (const char* sql : {"INSTALL httpfs", "LOAD httpfs"}) {
        if (offline && std::string(sql) == "INSTALL httpfs") {
            continue;
        }
        duckdb_result result;
        if (duckdb_query(conn, sql, &result) != DuckDBSuccess) {
            CROW_LOG_WARNING << "Bootstrap DuckDB: `" << sql << "` failed: "
                             << duckdb_result_error(&result)
                             << " (remote files cannot be read without httpfs"
                             << (offline ? "; offline mode: pre-install httpfs into FLAPI_EXTENSION_DIRECTORY"
                                         : "")
                             << ")";
        }
        duckdb_destroy_result(&result);
    }
    auto& credentials = getGlobalCredentialManager();
    credentials.loadFromEnvironment();
    credentials.configureConnection(conn);
    duckdb_disconnect(&conn);
    instance = created;
    return instance;
}

}  // namespace

DuckDBVFSProvider::DuckDBVFSProvider()
    : _file_system(nullptr) {
    // No FileSystem pointer is kept (#213). Each operation resolves one through
    // WithFileSystem: DatabaseManager's database when it is running, otherwise
    // the bootstrap instance, so a provider can be created - and used - before
    // the database is initialised.
}

DuckDBVFSProvider::DuckDBVFSProvider(::duckdb::FileSystem& fs)
    : _file_system(&fs) {
}

void DuckDBVFSProvider::WithFileSystem(const std::function<void(::duckdb::FileSystem&, ::duckdb::FileOpener*)>& fn) const {
    if (_file_system) {
        fn(*_file_system, nullptr);
        return;
    }

    duckdb_connection conn = nullptr;
    std::shared_ptr<BootstrapDb> bootstrap;  // keeps the instance alive for the call
    auto db_manager = DatabaseManager::getInstance();
    if (db_manager && db_manager->isInitialized()) {
        conn = db_manager->getConnection();
    } else {
        bootstrap = GetBootstrapDb();
        if (duckdb_connect(bootstrap->db, &conn) != DuckDBSuccess) {
            conn = nullptr;
        }
    }
    if (!conn) {
        throw FileOperationError("DuckDBVFSProvider could not get a DuckDB connection.");
    }

    auto* wrapper = reinterpret_cast<::duckdb::Connection*>(conn);
    if (!wrapper || !wrapper->context) {
        duckdb_disconnect(&conn);
        throw FileOperationError("Failed to get FileSystem from DuckDB connection.");
    }
    try {
        // GetFileSystem(context) already pushes the context's opener (httpfs
        // settings, secrets); passing one again is an INTERNAL error.
        // Remote file systems read settings and secrets through the context, which
        // needs an open transaction ("ActiveTransaction called without active
        // transaction" otherwise). A failure inside is rethrown after the
        // transaction has been rolled back by RunFunctionInTransaction.
        std::exception_ptr failure;
        wrapper->context->RunFunctionInTransaction([&]() {
            try {
                fn(::duckdb::FileSystem::GetFileSystem(*wrapper->context), nullptr);
            } catch (...) {
                failure = std::current_exception();
            }
        });
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (...) {
        duckdb_disconnect(&conn);
        throw;
    }
    duckdb_disconnect(&conn);
}

std::string DuckDBVFSProvider::ReadFile(const std::string& path) {
    // Maximum file size to read (10 MB). Config/SQL files should be small.
    // This prevents memory spikes from accidentally loading large files.
    constexpr int64_t MAX_FILE_SIZE = 10LL * 1024LL * 1024LL;  // 10 MB

    std::string content;
    try {
        WithFileSystem([&](::duckdb::FileSystem& fs, ::duckdb::FileOpener* opener) {
            auto handle = fs.OpenFile(path, ::duckdb::FileOpenFlags::FILE_FLAGS_READ, opener);
            if (!handle) {
                throw FileOperationError("Failed to open file: " + path);
            }

            auto file_size = fs.GetFileSize(*handle);
            if (file_size < 0) {
                throw FileOperationError("Failed to get file size: " + path);
            }
            if (file_size == 0) {
                // A server that sends no Content-Length (chunked http) reports 0 for a
                // file that has content: read until EOF instead of returning "".
                std::vector<char> chunk(64 * 1024);
                while (true) {
                    const auto got = handle->Read(chunk.data(), static_cast<::duckdb::idx_t>(chunk.size()));
                    if (got <= 0) {
                        break;
                    }
                    content.append(chunk.data(), static_cast<size_t>(got));
                    if (static_cast<int64_t>(content.size()) > MAX_FILE_SIZE) {
                        throw FileOperationError("File too large: " + PathSchemeUtils::RedactUrl(path) +
                                                 " (exceeds " + std::to_string(MAX_FILE_SIZE) + " byte limit)");
                    }
                }
                return;
            }
            if (file_size > MAX_FILE_SIZE) {
                throw FileOperationError(
                    "File too large: " + path + " (" + std::to_string(file_size) +
                    " bytes exceeds " + std::to_string(MAX_FILE_SIZE) + " byte limit)");
            }

            content.resize(static_cast<size_t>(file_size));
            auto bytes_read = handle->Read(content.data(), static_cast<::duckdb::idx_t>(file_size));
            if (bytes_read != file_size) {
                throw FileOperationError(
                    "Failed to read complete file: " + path +
                    " (read " + std::to_string(bytes_read) + " of " +
                    std::to_string(file_size) + " bytes)");
            }
        });
        return content;
    } catch (const FileOperationError&) {
        throw;
    } catch (const ::duckdb::Exception& e) {
        throw FileOperationError("DuckDB error reading file '" + PathSchemeUtils::RedactUrl(path) + "': " + CleanDuckError(e.what()));
    } catch (const std::exception& e) {
        throw FileOperationError("Error reading file '" + path + "': " + e.what());
    }
}

bool DuckDBVFSProvider::FileExists(const std::string& path) {
    try {
        bool exists = false;
        WithFileSystem([&](::duckdb::FileSystem& fs, ::duckdb::FileOpener* opener) {
            // httpfs does not implement FileExists (the base class answers false for
            // every URL), so ask the way a read would: open it (a missing file throws, which the
            // caller below turns into "does not exist").
            auto handle = fs.OpenFile(path, ::duckdb::FileOpenFlags::FILE_FLAGS_READ, opener);
            exists = handle != nullptr;
        });
        return exists;
    } catch (const std::exception& e) {
        // DuckDB may throw on network errors - log and treat as "file doesn't exist"
        // This could indicate credential issues or network problems
        CROW_LOG_DEBUG << "DuckDBVFSProvider::FileExists failed for '" << PathSchemeUtils::RedactUrl(path)
                         << "': " << e.what();
        return false;
    }
}

std::vector<std::string> DuckDBVFSProvider::ListFiles(const std::string& directory,
                                                       const std::string& pattern) {
    std::vector<std::string> result;
    try {
        // Build glob pattern
        std::string glob_path = directory;
        if (!glob_path.empty() && glob_path.back() != '/') {
            glob_path += '/';
        }
        glob_path += pattern;

        WithFileSystem([&](::duckdb::FileSystem& fs, ::duckdb::FileOpener* opener) {
            for (const auto& file_info : fs.Glob(glob_path, opener)) {
                result.push_back(file_info.path);
            }
        });
        std::sort(result.begin(), result.end());
        return result;
    } catch (const FileOperationError&) {
        throw;
    } catch (const ::duckdb::Exception& e) {
        throw FileOperationError(
            "DuckDB error listing files in '" + PathSchemeUtils::RedactUrl(directory) + "': " + CleanDuckError(e.what()));
    } catch (const std::exception& e) {
        throw FileOperationError(
            "Error listing files in '" + directory + "': " + e.what());
    }
}

bool DuckDBVFSProvider::IsRemotePath(const std::string& path) const {
    return PathSchemeUtils::IsRemotePath(path);
}

// ============================================================================
// FileProviderFactory Implementation
// ============================================================================

namespace {

// Process-wide bundle contents pointer. Set once by main() after
// LocateBundleInSelf() succeeds; nullptr means filesystem-mode is
// active.
std::shared_ptr<const ArchiveEntries> g_bundle_entries;

}  // namespace

std::shared_ptr<IFileProvider> FileProviderFactory::CreateProvider(const std::string& path) {
    if (PathSchemeUtils::IsRemotePath(path)) {
        return CreateDuckDBProvider();
    }
    if (g_bundle_entries != nullptr) {
        return CreateEmbeddedProvider();
    }
    return CreateLocalProvider();
}

std::shared_ptr<IFileProvider> FileProviderFactory::CreateLocalProvider() {
    return std::make_shared<LocalFileProvider>();
}

std::shared_ptr<IFileProvider> FileProviderFactory::CreateDuckDBProvider() {
    return std::make_shared<DuckDBVFSProvider>();
}

std::shared_ptr<IFileProvider> FileProviderFactory::CreateEmbeddedProvider() {
    if (g_bundle_entries == nullptr) {
        throw FileOperationError(
            "Cannot create embedded provider: no bundle is currently set");
    }
    return std::make_shared<EmbeddedArchiveFileProvider>(g_bundle_entries);
}

void FileProviderFactory::SetBundleContents(std::shared_ptr<const ArchiveEntries> entries) {
    g_bundle_entries = std::move(entries);
}

std::shared_ptr<const ArchiveEntries> FileProviderFactory::GetBundleContents() {
    return g_bundle_entries;
}

} // namespace flapi
