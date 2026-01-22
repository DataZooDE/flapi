#include "path_validator.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace flapi {

PathValidator::PathValidator() : _config() {}

PathValidator::PathValidator(const Config& config) : _config(config) {}

PathValidator::ValidationResult PathValidator::ValidatePath(
    const std::string& user_path,
    const std::string& base_path) const {

    if (user_path.empty()) {
        return ValidationResult::Failure("Path cannot be empty");
    }

    // URL decode the path first to catch encoded traversal attempts
    std::string decoded_path = UrlDecode(user_path);

    // Check for traversal sequences after decoding
    if (ContainsTraversal(decoded_path)) {
        return ValidationResult::Failure("Path traversal not allowed");
    }

    // Check if it's a remote path
    if (IsRemotePath(decoded_path)) {
        return ValidateRemotePath(decoded_path);
    }

    // Handle local paths
    return ValidateLocalPath(decoded_path, base_path);
}

PathValidator::ValidationResult PathValidator::ValidateLocalPath(
    const std::string& path,
    const std::string& base_path) const {

    if (!_config.allow_local_paths) {
        return ValidationResult::Failure("Local paths not allowed");
    }

    std::string canonical;

    // Check if path is absolute
    bool is_absolute = !path.empty() && (path[0] == '/' ||
                       (path.length() > 1 && path[1] == ':'));  // Windows

    if (is_absolute) {
        canonical = NormalizeSeparators(path);
    } else {
        // Relative path - need a base
        if (!_config.allow_relative_paths) {
            return ValidationResult::Failure("Relative paths not allowed");
        }

        if (base_path.empty()) {
            return ValidationResult::Failure("Relative path requires a base path");
        }

        canonical = Canonicalize(base_path, path);
        if (canonical.empty()) {
            return ValidationResult::Failure("Path traversal not allowed");
        }
    }

    // If resolve_symlinks is enabled, use filesystem-based canonicalization
    // to resolve symlinks and get the real path. This prevents symlink escape attacks.
    if (_config.resolve_symlinks) {
        try {
            std::filesystem::path fs_path(canonical);
            // Use weakly_canonical so it works even if path doesn't fully exist
            // (it resolves what exists and normalizes the rest)
            std::filesystem::path real_path = std::filesystem::weakly_canonical(fs_path);
            canonical = real_path.string();

            // Re-normalize separators after filesystem operations
            canonical = NormalizeSeparators(canonical);
        } catch (const std::filesystem::filesystem_error& e) {
            return ValidationResult::Failure(
                std::string("Failed to resolve path: ") + e.what());
        }
    }

    // Check against allowed prefixes (after symlink resolution if enabled)
    if (!IsPathAllowed(canonical)) {
        return ValidationResult::Failure("Path not within allowed directory");
    }

    return ValidationResult::Success(canonical);
}

PathValidator::ValidationResult PathValidator::ValidateRemotePath(
    const std::string& path) const {

    std::string scheme = ExtractScheme(path);

    if (scheme.empty()) {
        return ValidationResult::Failure("Invalid URI format");
    }

    if (!IsSchemeAllowed(scheme)) {
        return ValidationResult::Failure("URL scheme not allowed: " + scheme);
    }

    // For remote paths, normalize separators but don't do filesystem-style
    // canonicalization (remote paths don't support '..' resolution the same way)
    std::string normalized = NormalizeSeparators(path);

    return ValidationResult::Success(normalized);
}

bool PathValidator::IsSchemeAllowed(const std::string& scheme) const {
    return _config.allowed_schemes.find(scheme) != _config.allowed_schemes.end();
}

std::string PathValidator::Canonicalize(const std::string& base,
                                        const std::string& relative) const {
    if (relative.empty()) {
        return NormalizeSeparators(base);
    }

    // Check for traversal in relative path
    if (ContainsTraversal(relative)) {
        return "";  // Traversal detected
    }

    std::string normalized_base = NormalizeSeparators(base);
    std::string normalized_rel = NormalizeSeparators(relative);

    // Ensure base ends with /
    if (!normalized_base.empty() && normalized_base.back() != '/') {
        normalized_base += '/';
    }

    // Handle ./ prefix in relative path
    if (normalized_rel.length() >= 2 &&
        normalized_rel[0] == '.' && normalized_rel[1] == '/') {
        normalized_rel = normalized_rel.substr(2);
    }

    // Simple concatenation (traversal already checked)
    std::string result = normalized_base + normalized_rel;

    // Clean up any // sequences
    std::string cleaned;
    bool last_was_slash = false;
    for (char c : result) {
        if (c == '/') {
            if (!last_was_slash) {
                cleaned += c;
            }
            last_was_slash = true;
        } else {
            cleaned += c;
            last_was_slash = false;
        }
    }

    return cleaned;
}

bool PathValidator::IsPathAllowed(const std::string& path) const {
    // If no prefixes configured, allow all paths
    if (_config.allowed_prefixes.empty()) {
        return true;
    }

    std::string normalized_path = NormalizeSeparators(path);

    for (const auto& prefix : _config.allowed_prefixes) {
        std::string normalized_prefix = NormalizeSeparators(prefix);

        // Ensure prefix ends with / for directory matching
        if (!normalized_prefix.empty() && normalized_prefix.back() != '/') {
            normalized_prefix += '/';
        }

        // Check if path starts with prefix, or path equals prefix without trailing /
        if (normalized_path.find(normalized_prefix) == 0) {
            return true;
        }

        // Also allow exact match without trailing slash
        std::string prefix_no_slash = normalized_prefix;
        if (!prefix_no_slash.empty() && prefix_no_slash.back() == '/') {
            prefix_no_slash.pop_back();
        }
        if (normalized_path == prefix_no_slash) {
            return true;
        }
    }

    return false;
}

void PathValidator::AddAllowedScheme(const std::string& scheme) {
    _config.allowed_schemes.insert(scheme);
}

void PathValidator::AddAllowedPrefix(const std::string& prefix) {
    _config.allowed_prefixes.push_back(prefix);
}

bool PathValidator::ContainsTraversal(const std::string& path) {
    // Check for '..' sequence which could be used for traversal
    // This catches: .., /../, \..\, etc.

    std::string normalized = NormalizeSeparators(path);

    // Check for standalone '..'
    if (normalized == "..") {
        return true;
    }

    // Check for '../' at start
    if (normalized.length() >= 3 && normalized.substr(0, 3) == "../") {
        return true;
    }

    // Check for '/..' followed by '/' or end of string anywhere in the path
    // This avoids false positives like /...file.txt (three dots)
    size_t pos = 0;
    while ((pos = normalized.find("/..", pos)) != std::string::npos) {
        // Check what follows the '/..': must be '/' or end of string
        size_t after_pos = pos + 3;
        if (after_pos >= normalized.length() || normalized[after_pos] == '/') {
            return true;
        }
        pos++;  // Move past this occurrence to check for more
    }

    return false;
}

std::string PathValidator::UrlDecodeSingle(const std::string& encoded) {
    std::string result;
    result.reserve(encoded.length());

    for (size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            // Check if next two chars are hex digits
            char h1 = encoded[i + 1];
            char h2 = encoded[i + 2];

            if (std::isxdigit(static_cast<unsigned char>(h1)) &&
                std::isxdigit(static_cast<unsigned char>(h2))) {
                // Convert hex to char
                int value = 0;
                std::istringstream iss(encoded.substr(i + 1, 2));
                iss >> std::hex >> value;
                result += static_cast<char>(value);
                i += 2;
                continue;
            }
        } else if (encoded[i] == '+') {
            // + is sometimes used for space in query strings
            result += ' ';
            continue;
        }
        result += encoded[i];
    }

    return result;
}

std::string PathValidator::UrlDecode(const std::string& encoded) {
    // Iteratively decode to catch double/triple-encoded traversal attempts
    // e.g., %252e%252e = %2e%2e = .. (after two decode passes)
    // Limit iterations to prevent infinite loops on malformed input
    constexpr int MAX_DECODE_ITERATIONS = 3;

    std::string current = encoded;
    for (int i = 0; i < MAX_DECODE_ITERATIONS; ++i) {
        std::string decoded = UrlDecodeSingle(current);
        if (decoded == current) {
            // No more decoding needed
            break;
        }
        current = decoded;
    }

    return current;
}

std::string PathValidator::ExtractScheme(const std::string& path) {
    // Look for "scheme://"
    size_t pos = path.find("://");
    if (pos == std::string::npos || pos == 0) {
        return "";
    }

    std::string scheme = path.substr(0, pos);

    // Validate scheme contains only valid characters (letters, digits, +, -, .)
    for (char c : scheme) {
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '+' && c != '-' && c != '.') {
            return "";
        }
    }

    // Convert to lowercase for comparison
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    return scheme;
}

bool PathValidator::IsRemotePath(const std::string& path) {
    std::string scheme = ExtractScheme(path);
    if (scheme.empty()) {
        return false;
    }

    // These schemes are considered "remote" (network-based)
    // Note: ExtractScheme already lowercases, so we only need lowercase here
    static const std::set<std::string> remote_schemes = {
        "s3", "gs", "az", "azure", "abfs", "abfss",  // Cloud storage
        "http", "https",                              // HTTP
        "ftp", "ftps", "sftp"                         // FTP
    };

    return remote_schemes.find(scheme) != remote_schemes.end();
}

std::string PathValidator::NormalizeSeparators(const std::string& path) {
    std::string result = path;
    // Replace backslashes with forward slashes (Windows compatibility)
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

} // namespace flapi
