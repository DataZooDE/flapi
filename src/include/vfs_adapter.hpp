#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "duckdb.h"

namespace flapi {

// Forward-declared so the factory can refer to bundle contents without
// pulling in the archive_io.hpp dependency.
using ArchiveEntries = std::map<std::string, std::vector<std::uint8_t>>;

/**
 * `text` with every URL's `user:pass@` and `?query` / `#fragment` replaced, so an
 * error or log line cannot carry a presigned signature or a password.
 */
std::string RedactUrlsInText(const std::string& text);

/**
 * An in-memory DuckDB instance of its own, never the main database that SQL
 * templates run on: what is created in it (secrets, tables) is invisible to them.
 * It honours FLAPI_EXTENSION_DIRECTORY (else a bundle's extensions, else the
 * extensions shipped next to the binary) and offline mode, like the main one.
 * With `allow_persistent_secrets` false it does not load ~/.duckdb/stored_secrets.
 * The caller owns the handle and closes it with duckdb_close. `purpose` names it
 * in errors. Throws FileOperationError when DuckDB cannot start.
 */
duckdb_database OpenPrivateDuckDb(bool allow_persistent_secrets, const std::string& purpose);

/**
 * While one of these is alive, DuckDBVFSProvider remembers what it read (and which files exist), so the several
 * passes of a configuration load (parse, validate, syntax check) fetch each remote file once instead of 3-5 times.
 * Scoped: the memo is dropped when the last scope ends, so nothing is ever served stale during normal operation.
 */
class VFSReadMemoScope {
public:
    VFSReadMemoScope();
    ~VFSReadMemoScope();
    VFSReadMemoScope(const VFSReadMemoScope&) = delete;
    VFSReadMemoScope& operator=(const VFSReadMemoScope&) = delete;
};

/**
 * Exception thrown when a file operation fails. The message is redacted: it ends up
 * in logs and in config-service responses.
 */
class FileOperationError : public std::runtime_error {
public:
    explicit FileOperationError(const std::string& message)
        : std::runtime_error(RedactUrlsInText(message)) {}
};

/** A file that exists but exceeds the read limit (distinct from "does not exist"). */
class FileTooLargeError : public FileOperationError {
public:
    explicit FileTooLargeError(const std::string& message) : FileOperationError(message) {}
};

/**
 * Abstract interface for file operations.
 * Provides a unified API for reading files from local filesystem or remote storage.
 *
 * Implementations:
 * - LocalFileProvider: Standard filesystem operations (std::filesystem)
 * - DuckDBVFSProvider: DuckDB's VFS for S3, GCS, Azure, HTTP
 * - EmbeddedArchiveFileProvider: in-memory ZIP appended to the binary
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
     * A URL safe to log: `user:pass@` and the query string (presigned-URL
     * signatures, tokens) are replaced. Non-URLs are returned unchanged.
     */
    static std::string RedactUrl(const std::string& url);

    /**
     * The URL without its `?query` / `#fragment` (the part that names the object).
     */
    static std::string StripQuery(const std::string& url);

    /**
     * True when `path` contains a `..` segment, also spelled with percent escapes
     * (`%2e%2e`, `.%2e`, `..%2f`) or a backslash separator.
     */
    static bool HasDotDotSegment(const std::string& path);

    /**
     * Join a relative file onto a remote base URL, keeping the base's `?query`
     * (a SAS token, a signed-URL parameter) at the END: `https://h/d?sig=1` +
     * `a.yaml` -> `https://h/d/a.yaml?sig=1`.
     */
    static std::string JoinUrl(const std::string& base, const std::string& rel);

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
class FileOpener;
class DatabaseInstance;
}

namespace flapi {

/**
 * File provider implementation using DuckDB's virtual file system.
 * Supports remote storage via httpfs extension (S3, GCS, Azure, HTTP/HTTPS).
 *
 * This provider requires that:
 * 1. the httpfs extension is available (INSTALLed or in the extension directory)
 * 2. Appropriate credentials are configured for cloud storage (S3, GCS, Azure)
 */
class DuckDBVFSProvider : public IFileProvider {
public:
    /**
     * Construct a DuckDBVFSProvider. It keeps no pointer into DuckDB: every
     * operation uses DatabaseManager's database when that is running and a
     * private bootstrap instance (in-memory, httpfs) before that, so it works
     * while the configuration itself is still being read.
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
    // Runs fn with a FileSystem that is valid for the duration of the call.
    void WithFileSystem(const std::string& path,
                        const std::function<void(duckdb::FileSystem&, duckdb::FileOpener*)>& fn) const;

    duckdb::FileSystem* _file_system;  // only set by the explicit-FileSystem constructor
};

/**
 * Factory for creating file providers based on path scheme.
 */
class FileProviderFactory {
public:
    /**
     * Create an appropriate file provider for the given path.
     *
     * Dispatch order:
     *   1. Remote schemes (s3://, gs://, az://, http(s)://) -> DuckDBVFSProvider
     *   2. Process has a bundle set via SetBundleContents() -> EmbeddedArchiveFileProvider
     *   3. Otherwise -> LocalFileProvider
     *
     * @throws FileOperationError if remote path requested but DatabaseManager not initialized
     */
    static std::shared_ptr<IFileProvider> CreateProvider(const std::string& path);

    /**
     * Create a local file provider.
     */
    static std::shared_ptr<IFileProvider> CreateLocalProvider();

    /**
     * Create a DuckDB VFS provider for remote file access.
     * Works before DatabaseManager exists (bootstrap instance) and after it.
     */
    static std::shared_ptr<IFileProvider> CreateDuckDBProvider();

    /**
     * Create an embedded provider backed by the currently set bundle.
     * @throws FileOperationError if no bundle is currently set.
     */
    static std::shared_ptr<IFileProvider> CreateEmbeddedProvider();

    /**
     * Set the process-wide ZIP-bundle contents that CreateProvider() and
     * CreateEmbeddedProvider() consult. Pass nullptr to clear.
     *
     * Called once at startup by main() after `LocateBundleInSelf()`
     * succeeds. Tests should pair set/clear with an RAII guard to keep
     * the static clean.
     */
    static void SetBundleContents(std::shared_ptr<const ArchiveEntries> entries);

    /**
     * Returns the currently set bundle contents, or nullptr if none.
     */
    static std::shared_ptr<const ArchiveEntries> GetBundleContents();
};

} // namespace flapi
