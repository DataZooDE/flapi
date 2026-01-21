#pragma once

#include <optional>
#include <string>
#include <vector>
#include <set>

namespace flapi {

/**
 * Security-focused path validation to prevent traversal attacks and
 * enforce access controls on file paths.
 *
 * This class provides:
 * - Path traversal attack prevention (blocking '..' sequences)
 * - Prefix-based access control (paths must be under allowed prefixes)
 * - URL scheme whitelisting (only configured schemes are allowed)
 * - Path canonicalization for consistent validation
 */
class PathValidator {
public:
    /**
     * Configuration for path validation.
     */
    struct Config {
        // Allowed URL schemes (default: file, https)
        std::set<std::string> allowed_schemes = {"file", "https"};

        // Whether to allow local file paths (paths without scheme)
        bool allow_local_paths = true;

        // Allowed path prefixes for local paths (empty = all allowed)
        std::vector<std::string> allowed_prefixes;

        // Whether to allow relative paths
        bool allow_relative_paths = true;
    };

    /**
     * Result of path validation.
     */
    struct ValidationResult {
        bool valid = false;
        std::string canonical_path;
        std::string error_message;

        static ValidationResult Success(const std::string& path) {
            return {true, path, ""};
        }

        static ValidationResult Failure(const std::string& error) {
            return {false, "", error};
        }
    };

    /**
     * Create a PathValidator with default configuration.
     */
    PathValidator();

    /**
     * Create a PathValidator with custom configuration.
     */
    explicit PathValidator(const Config& config);

    /**
     * Validate a user-provided path.
     *
     * @param user_path The path to validate (can be absolute, relative, or URI)
     * @param base_path Optional base path for resolving relative paths
     * @return ValidationResult with canonical path if valid, error message if not
     */
    ValidationResult ValidatePath(const std::string& user_path,
                                  const std::string& base_path = "") const;

    /**
     * Check if a URL scheme is allowed by configuration.
     *
     * @param scheme The scheme to check (e.g., "s3", "https", "file")
     * @return true if the scheme is allowed
     */
    bool IsSchemeAllowed(const std::string& scheme) const;

    /**
     * Canonicalize a path by resolving relative components.
     * Does NOT check filesystem - purely string-based canonicalization.
     *
     * @param base The base path
     * @param relative The relative path to resolve
     * @return Canonicalized path, or empty string if traversal detected
     */
    std::string Canonicalize(const std::string& base,
                             const std::string& relative) const;

    /**
     * Check if a path is within an allowed prefix.
     *
     * @param path The canonical path to check
     * @return true if path starts with an allowed prefix (or no prefixes configured)
     */
    bool IsPathAllowed(const std::string& path) const;

    /**
     * Add an allowed scheme to the configuration.
     */
    void AddAllowedScheme(const std::string& scheme);

    /**
     * Add an allowed path prefix.
     */
    void AddAllowedPrefix(const std::string& prefix);

    /**
     * Get current configuration.
     */
    const Config& GetConfig() const { return _config; }

    // Static utility methods

    /**
     * Check if a path contains path traversal sequences.
     * Checks for '..' after URL decoding.
     *
     * @param path The path to check
     * @return true if path contains traversal sequences
     */
    static bool ContainsTraversal(const std::string& path);

    /**
     * URL-decode a string (handles %2e%2e for .. etc).
     *
     * @param encoded The URL-encoded string
     * @return Decoded string
     */
    static std::string UrlDecode(const std::string& encoded);

    /**
     * Extract scheme from a path/URI.
     *
     * @param path The path or URI
     * @return Scheme (e.g., "s3", "https") or empty if no scheme
     */
    static std::string ExtractScheme(const std::string& path);

    /**
     * Check if a path is a remote URI (has a network scheme).
     *
     * @param path The path to check
     * @return true if path starts with s3://, gs://, https://, etc.
     */
    static bool IsRemotePath(const std::string& path);

private:
    Config _config;

    // Validate a local filesystem path
    ValidationResult ValidateLocalPath(const std::string& path,
                                       const std::string& base_path) const;

    // Validate a remote URI
    ValidationResult ValidateRemotePath(const std::string& path) const;

    // Normalize path separators to forward slashes
    static std::string NormalizeSeparators(const std::string& path);
};

} // namespace flapi
