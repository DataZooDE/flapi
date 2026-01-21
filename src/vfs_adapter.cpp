#include "vfs_adapter.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

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

bool PathSchemeUtils::IsRemotePath(const std::string& path) {
    return IsS3Path(path) || IsGCSPath(path) || IsAzurePath(path) || IsHttpPath(path);
}

bool PathSchemeUtils::IsS3Path(const std::string& path) {
    return path.rfind(SCHEME_S3, 0) == 0;
}

bool PathSchemeUtils::IsGCSPath(const std::string& path) {
    return path.rfind(SCHEME_GCS, 0) == 0;
}

bool PathSchemeUtils::IsAzurePath(const std::string& path) {
    return path.rfind(SCHEME_AZURE, 0) == 0 || path.rfind(SCHEME_AZURE_BLOB, 0) == 0;
}

bool PathSchemeUtils::IsHttpPath(const std::string& path) {
    return path.rfind(SCHEME_HTTP, 0) == 0 || path.rfind(SCHEME_HTTPS, 0) == 0;
}

bool PathSchemeUtils::IsFilePath(const std::string& path) {
    return path.rfind(SCHEME_FILE, 0) == 0;
}

std::string PathSchemeUtils::GetScheme(const std::string& path) {
    if (IsS3Path(path)) {
        return SCHEME_S3;
    }
    if (IsGCSPath(path)) {
        return SCHEME_GCS;
    }
    if (path.rfind(SCHEME_AZURE_BLOB, 0) == 0) {
        return SCHEME_AZURE_BLOB;
    }
    if (path.rfind(SCHEME_AZURE, 0) == 0) {
        return SCHEME_AZURE;
    }
    if (path.rfind(SCHEME_HTTPS, 0) == 0) {
        return SCHEME_HTTPS;
    }
    if (path.rfind(SCHEME_HTTP, 0) == 0) {
        return SCHEME_HTTP;
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
    try {
        // Get DatabaseManager singleton
        auto db_manager = DatabaseManager::getInstance();
        if (!db_manager) {
            throw FileOperationError(
                "DuckDBVFSProvider requires DatabaseManager to be initialized. "
                "Call DatabaseManager::initializeDBManagerFromConfig() first.");
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
        return false;
    }

    try {
        return _file_system->FileExists(path);
    } catch (const ::duckdb::Exception&) {
        // DuckDB may throw on network errors - treat as "file doesn't exist"
        return false;
    } catch (const std::exception&) {
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
