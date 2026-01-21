#include "vfs_adapter.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

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
// FileProviderFactory Implementation
// ============================================================================

std::shared_ptr<IFileProvider> FileProviderFactory::CreateProvider(const std::string& path) {
    if (PathSchemeUtils::IsRemotePath(path)) {
        // Future: Return DuckDBVFSProvider for remote paths
        // For now, throw an error since remote paths aren't supported yet
        throw FileOperationError(
            "Remote paths are not yet supported: " + path +
            ". DuckDBVFSProvider will be implemented in a future task.");
    }
    return CreateLocalProvider();
}

std::shared_ptr<IFileProvider> FileProviderFactory::CreateLocalProvider() {
    return std::make_shared<LocalFileProvider>();
}

} // namespace flapi
