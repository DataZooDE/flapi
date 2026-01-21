#pragma once

#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

namespace flapi {

/**
 * Exception thrown when a file operation fails.
 */
class FileOperationError : public std::runtime_error {
public:
    explicit FileOperationError(const std::string& message)
        : std::runtime_error(message) {}
};

/**
 * Abstract interface for file operations.
 * Provides a unified API for reading files from local filesystem or remote storage.
 *
 * Implementations:
 * - LocalFileProvider: Standard filesystem operations (std::filesystem)
 * - DuckDBVFSProvider: DuckDB's VFS for S3, GCS, Azure, HTTP (future)
 */
class IFileProvider {
public:
    virtual ~IFileProvider() = default;

    /**
     * Read the entire contents of a file.
     *
     * @param path File path (local path or URI like s3://bucket/key)
     * @return File contents as string
     * @throws FileOperationError if file cannot be read
     */
    virtual std::string ReadFile(const std::string& path) = 0;

    /**
     * Check if a file exists.
     *
     * @param path File path to check
     * @return true if file exists and is readable
     */
    virtual bool FileExists(const std::string& path) = 0;

    /**
     * List files in a directory matching a pattern.
     *
     * @param directory Directory path to search
     * @param pattern Glob pattern (e.g., "*.yaml", "*.sql")
     * @return Vector of matching file paths
     * @throws FileOperationError if directory cannot be accessed
     */
    virtual std::vector<std::string> ListFiles(const std::string& directory,
                                                const std::string& pattern = "*") = 0;

    /**
     * Check if a path refers to a remote resource (S3, GCS, Azure, HTTP).
     *
     * @param path Path to check
     * @return true if path uses a remote URI scheme
     */
    virtual bool IsRemotePath(const std::string& path) const = 0;

    /**
     * Get the provider name for debugging/logging.
     *
     * @return Provider name (e.g., "local", "duckdb-vfs")
     */
    virtual std::string GetProviderName() const = 0;
};

/**
 * Utility functions for path scheme detection.
 */
class PathSchemeUtils {
public:
    /**
     * Supported URI schemes for remote storage.
     */
    static constexpr const char* SCHEME_S3 = "s3://";
    static constexpr const char* SCHEME_GCS = "gs://";
    static constexpr const char* SCHEME_AZURE = "az://";
    static constexpr const char* SCHEME_AZURE_BLOB = "azure://";
    static constexpr const char* SCHEME_HTTP = "http://";
    static constexpr const char* SCHEME_HTTPS = "https://";
    static constexpr const char* SCHEME_FILE = "file://";

    /**
     * Check if a path is a remote URI.
     *
     * @param path Path to check
     * @return true if path starts with a remote scheme (s3://, gs://, az://, http://, https://)
     */
    static bool IsRemotePath(const std::string& path);

    /**
     * Check if a path uses the S3 scheme.
     */
    static bool IsS3Path(const std::string& path);

    /**
     * Check if a path uses the GCS scheme.
     */
    static bool IsGCSPath(const std::string& path);

    /**
     * Check if a path uses the Azure scheme.
     */
    static bool IsAzurePath(const std::string& path);

    /**
     * Check if a path uses HTTP/HTTPS scheme.
     */
    static bool IsHttpPath(const std::string& path);

    /**
     * Check if a path uses the file:// scheme.
     */
    static bool IsFilePath(const std::string& path);

    /**
     * Extract the scheme from a path.
     *
     * @param path Path to extract scheme from
     * @return Scheme string (e.g., "s3://", "https://") or empty if local path
     */
    static std::string GetScheme(const std::string& path);

    /**
     * Remove the file:// scheme prefix if present.
     *
     * @param path Path potentially with file:// prefix
     * @return Path without file:// prefix
     */
    static std::string StripFileScheme(const std::string& path);
};

/**
 * File provider implementation using the local filesystem.
 * Uses std::filesystem for all operations.
 */
class LocalFileProvider : public IFileProvider {
public:
    LocalFileProvider() = default;
    ~LocalFileProvider() override = default;

    std::string ReadFile(const std::string& path) override;
    bool FileExists(const std::string& path) override;
    std::vector<std::string> ListFiles(const std::string& directory,
                                        const std::string& pattern = "*") override;
    bool IsRemotePath(const std::string& path) const override;
    std::string GetProviderName() const override { return "local"; }
};

} // namespace flapi

// Forward declarations for DuckDB types (in global duckdb namespace)
namespace duckdb {
class FileSystem;
class DatabaseInstance;
}

namespace flapi {

/**
 * File provider implementation using DuckDB's virtual file system.
 * Supports remote storage via httpfs extension (S3, GCS, Azure, HTTP/HTTPS).
 *
 * This provider requires that:
 * 1. DatabaseManager is initialized
 * 2. httpfs extension is loaded (for HTTP/HTTPS support)
 * 3. Appropriate credentials are configured for cloud storage (S3, GCS, Azure)
 */
class DuckDBVFSProvider : public IFileProvider {
public:
    /**
     * Construct a DuckDBVFSProvider using the global DatabaseManager instance.
     * Throws FileOperationError if DatabaseManager is not initialized.
     */
    DuckDBVFSProvider();

    /**
     * Construct a DuckDBVFSProvider with an explicit FileSystem reference.
     * Useful for testing or when FileSystem is obtained from a different source.
     */
    explicit DuckDBVFSProvider(duckdb::FileSystem& fs);

    ~DuckDBVFSProvider() override = default;

    std::string ReadFile(const std::string& path) override;
    bool FileExists(const std::string& path) override;
    std::vector<std::string> ListFiles(const std::string& directory,
                                        const std::string& pattern = "*") override;
    bool IsRemotePath(const std::string& path) const override;
    std::string GetProviderName() const override { return "duckdb-vfs"; }

private:
    duckdb::FileSystem* _file_system;
};

/**
 * Factory for creating file providers based on path scheme.
 */
class FileProviderFactory {
public:
    /**
     * Create an appropriate file provider for the given path.
     * Returns LocalFileProvider for local paths (no scheme or file://).
     * Returns DuckDBVFSProvider for remote paths (s3://, gs://, az://, http://, https://).
     *
     * @param path Path to create provider for (used to detect scheme)
     * @return Shared pointer to appropriate file provider
     * @throws FileOperationError if remote path requested but DatabaseManager not initialized
     */
    static std::shared_ptr<IFileProvider> CreateProvider(const std::string& path);

    /**
     * Create a local file provider.
     */
    static std::shared_ptr<IFileProvider> CreateLocalProvider();

    /**
     * Create a DuckDB VFS provider for remote file access.
     * Requires DatabaseManager to be initialized.
     *
     * @throws FileOperationError if DatabaseManager is not initialized
     */
    static std::shared_ptr<IFileProvider> CreateDuckDBProvider();
};

} // namespace flapi
