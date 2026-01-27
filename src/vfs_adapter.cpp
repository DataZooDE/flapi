#include "vfs_adapter.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
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

DuckDBVFSProvider::DuckDBVFSProvider()
    : _file_system(nullptr) {
    // TODO(flapi-b33): This implementation uses DuckDB internal APIs (capi_internal.hpp)
    // which are ABI-unstable and may break on DuckDB upgrades. Future refactoring should:
    // 1. Expose FileSystem access through DatabaseManager via a stable public API, OR
    // 2. Use stable DuckDB C API if/when it provides FileSystem access
    //
    // TODO(flapi-p9h): The _file_system pointer is obtained once and cached. If the
    // DatabaseManager is reset or the underlying DB instance changes, this pointer
    // can become dangling. For now, we assume DatabaseManager has stable lifetime.
    // Future improvement: re-fetch FileSystem per operation or tie to DatabaseManager lifetime.

    try {
        // Get DatabaseManager singleton
        auto db_manager = DatabaseManager::getInstance();
        if (!db_manager) {
            throw FileOperationError(
                "DuckDBVFSProvider requires DatabaseManager to be initialized. "
                "Call DatabaseManager::initializeDBManagerFromConfig() first.");
        }

        // Check if DatabaseManager has been initialized with a database
        // This prevents crashes when trying to get a connection from an uninitialized manager
        if (!db_manager->isInitialized()) {
            throw FileOperationError(
                "DatabaseManager is not initialized. DuckDBVFSProvider requires "
                "a running database. Call DatabaseManager::initializeDBManagerFromConfig() first.");
        }

        // Get the DuckDB database handle from DatabaseManager
        // We need to get a connection to access the database instance
        auto conn = db_manager->getConnection();
        if (!conn) {
            throw FileOperationError(
                "Failed to get DuckDB connection from DatabaseManager.");
        }

        // Get the database wrapper from the connection
        // The connection's internal structure gives us access to the database
        // WARNING: This uses internal DuckDB APIs that are not ABI-stable
        auto* conn_wrapper = reinterpret_cast<::duckdb::Connection*>(conn);
        if (conn_wrapper && conn_wrapper->context) {
            _file_system = &::duckdb::FileSystem::GetFileSystem(*conn_wrapper->context);
        } else {
            duckdb_disconnect(&conn);
            throw FileOperationError(
                "Failed to get FileSystem from DuckDB connection.");
        }

        // Release the connection since we only needed it to get the FileSystem
        duckdb_disconnect(&conn);
    } catch (const FileOperationError&) {
        // Re-throw our own exceptions
        throw;
    } catch (const std::exception& e) {
        // Wrap other exceptions (like from DatabaseManager) in FileOperationError
        throw FileOperationError(
            std::string("DuckDBVFSProvider initialization failed: ") + e.what());
    }
}

DuckDBVFSProvider::DuckDBVFSProvider(::duckdb::FileSystem& fs)
    : _file_system(&fs) {
}

std::string DuckDBVFSProvider::ReadFile(const std::string& path) {
    if (!_file_system) {
        throw FileOperationError("DuckDBVFSProvider not properly initialized.");
    }

    // Maximum file size to read (10 MB). Config/SQL files should be small.
    // This prevents memory spikes from accidentally loading large files.
    constexpr int64_t MAX_FILE_SIZE = 10LL * 1024LL * 1024LL;  // 10 MB

    try {
        // Open the file for reading
        auto flags = ::duckdb::FileOpenFlags::FILE_FLAGS_READ;
        auto handle = _file_system->OpenFile(path, flags);

        if (!handle) {
            throw FileOperationError("Failed to open file: " + path);
        }

        // Get file size
        auto file_size = _file_system->GetFileSize(*handle);
        if (file_size < 0) {
            throw FileOperationError("Failed to get file size: " + path);
        }

        if (file_size == 0) {
            return "";
        }

        // Check file size limit
        if (file_size > MAX_FILE_SIZE) {
            throw FileOperationError(
                "File too large: " + path + " (" + std::to_string(file_size) +
                " bytes exceeds " + std::to_string(MAX_FILE_SIZE) + " byte limit)");
        }

        // Read entire file content
        std::string content;
        content.resize(static_cast<size_t>(file_size));

        auto bytes_read = handle->Read(content.data(), static_cast<::duckdb::idx_t>(file_size));
        if (bytes_read != file_size) {
            throw FileOperationError(
                "Failed to read complete file: " + path +
                " (read " + std::to_string(bytes_read) + " of " +
                std::to_string(file_size) + " bytes)");
        }

        return content;
    } catch (const ::duckdb::Exception& e) {
        throw FileOperationError("DuckDB error reading file '" + path + "': " + e.what());
    } catch (const std::exception& e) {
        throw FileOperationError("Error reading file '" + path + "': " + e.what());
    }
}

bool DuckDBVFSProvider::FileExists(const std::string& path) {
    if (!_file_system) {
        CROW_LOG_WARNING << "DuckDBVFSProvider::FileExists called without initialized file system";
        return false;
    }

    try {
        return _file_system->FileExists(path);
    } catch (const ::duckdb::Exception& e) {
        // DuckDB may throw on network errors - log and treat as "file doesn't exist"
        // This could indicate credential issues or network problems
        CROW_LOG_WARNING << "DuckDBVFSProvider::FileExists failed for '" << path
                         << "': " << e.what();
        return false;
    } catch (const std::exception& e) {
        CROW_LOG_WARNING << "DuckDBVFSProvider::FileExists failed for '" << path
                         << "': " << e.what();
        return false;
    }
}

std::vector<std::string> DuckDBVFSProvider::ListFiles(const std::string& directory,
                                                       const std::string& pattern) {
    if (!_file_system) {
        throw FileOperationError("DuckDBVFSProvider not properly initialized.");
    }

    std::vector<std::string> result;

    try {
        // Build glob pattern
        std::string glob_path = directory;
        if (!glob_path.empty() && glob_path.back() != '/') {
            glob_path += '/';
        }
        glob_path += pattern;

        // Use DuckDB's Glob function
        auto files = _file_system->Glob(glob_path);

        for (const auto& file_info : files) {
            result.push_back(file_info.path);
        }

        // Sort for consistent ordering
        std::sort(result.begin(), result.end());

        return result;
    } catch (const ::duckdb::Exception& e) {
        throw FileOperationError(
            "DuckDB error listing files in '" + directory + "': " + e.what());
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

std::shared_ptr<IFileProvider> FileProviderFactory::CreateProvider(const std::string& path) {
    if (PathSchemeUtils::IsRemotePath(path)) {
        return CreateDuckDBProvider();
    }
    return CreateLocalProvider();
}

std::shared_ptr<IFileProvider> FileProviderFactory::CreateLocalProvider() {
    return std::make_shared<LocalFileProvider>();
}

std::shared_ptr<IFileProvider> FileProviderFactory::CreateDuckDBProvider() {
    return std::make_shared<DuckDBVFSProvider>();
}

} // namespace flapi
