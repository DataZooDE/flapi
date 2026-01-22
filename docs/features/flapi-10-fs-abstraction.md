# Designing a Filesystem Abstraction Layer for flAPI

**flAPI can leverage DuckDB's existing VFS to expose cloud storage through Crow's HTTP layer with minimal new code.** The key insight is that DuckDB already provides a mature, production-tested filesystem abstraction with support for S3, Azure, GCS, and HTTP—flAPI needs only thin "glue" to bridge configuration loading and response streaming. This approach minimizes development effort while enabling cloud-native deployments on serverless platforms with read-only filesystems.

## Current architecture reveals natural integration points

flAPI is a C++ API framework built on **DuckDB v1.4.1** and **Crow** that generates REST and MCP endpoints from SQL templates. Currently, all configuration files (YAML endpoints, SQL templates) are loaded from the local filesystem via Docker volume mounts. The architecture consists of three layers that a filesystem abstraction must bridge:

The **configuration layer** uses yaml-cpp to parse endpoint definitions from a `template.path` directory, with support for includes (`{{include from path/to/file.yaml}}`) and environment variable substitution. The **query layer** leverages DuckDB's SQL engine and its extension ecosystem—BigQuery, Postgres, Parquet, Iceberg—already using DuckDB's internal VFS for data access. The **HTTP layer** uses Crow for request handling, response serialization, and static file serving with automatic MIME detection.

DuckDB's `FileSystem` class provides the core abstraction needed:

```cpp
class FileSystem {
    virtual unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags);
    virtual void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location);
    virtual void Write(FileHandle &handle, void *data, int64_t nr_bytes, idx_t location);
    virtual void RegisterSubSystem(unique_ptr<FileSystem> sub_fs);
    virtual vector<OpenFileInfo> GlobFiles(const string &pattern, FileGlobOptions options);
};
```

The httpfs and azure extensions already implement this interface for **S3-compatible storage** (AWS, GCS, MinIO, R2), **Azure Blob Storage**, and **HTTP/HTTPS URLs**. Rather than building new filesystem implementations, flAPI should reuse these.

## DuckDB's VFS provides mature cloud storage integration

DuckDB's filesystem architecture is designed for exactly this use case. The `VirtualFileSystem` container routes requests based on URL prefix—`s3://` to S3FileSystem, `az://` to AzureFileSystem, `https://` to HTTPFileSystem—with automatic fallback to LocalFileSystem. Key capabilities include:

**Chunked I/O with configurable buffering**: The Azure extension defaults to **1MB chunks** with parallel transfer threads. S3 uses multi-part uploads for writes. Both support partial reads for range requests, critical for streaming large query results.

**Unified credentials management**: The Secrets Manager pattern handles authentication across providers:

```sql
CREATE SECRET my_aws (
    TYPE s3,
    KEY_ID 'AKIAIOSFODNN7EXAMPLE',
    SECRET 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY',
    REGION 'us-east-1',
    SCOPE 's3://my-bucket/'
);
```

This model maps naturally to flAPI's existing connection configuration, where secrets could be defined once and referenced by template paths.

**Extension-based registration**: Custom filesystems register during extension load via `RegisterSubSystem()`, enabling flAPI to add new storage backends without core changes. The pattern from httpfs shows how to handle URL scheme matching, credential injection, and error propagation.

## Crow requires adaptation for VFS-backed streaming

Crow's file serving is simpler than DuckDB's VFS. Static files use POSIX `stat()` and `std::ifstream` with **16KB chunks** written synchronously. The framework lacks:

- **No virtual filesystem abstraction**: Direct filesystem access only
- **No HTTP Range request support**: No 206 Partial Content handling
- **No async file I/O**: Blocking reads in the request thread

However, Crow provides hooks that flAPI can use. The `stream_threshold` setting (default 1MB) automatically streams responses exceeding that size. The `set_static_file_info_unsafe()` method bypasses path sanitization when flAPI controls the path. The middleware system enables authentication and authorization before file access.

The integration pattern requires a custom response handler that reads from DuckDB's VFS and writes to Crow's response:

```cpp
CROW_ROUTE(app, "/files/<path>")
([&db_instance](const crow::request& req, crow::response& res, std::string path) {
    auto& fs = db_instance.GetFileSystem();
    std::string vfs_path = resolve_to_vfs_url(path); // e.g., "s3://bucket/path"
    
    auto handle = fs.OpenFile(vfs_path, FileFlags::READ);
    int64_t file_size = fs.GetFileSize(*handle);
    
    res.set_header("Content-Length", std::to_string(file_size));
    res.set_header("Accept-Ranges", "bytes");
    
    // Stream in 1MB chunks
    std::vector<char> buffer(1024 * 1024);
    int64_t offset = 0;
    while (offset < file_size) {
        int64_t to_read = std::min((int64_t)buffer.size(), file_size - offset);
        fs.Read(*handle, buffer.data(), to_read, offset);
        res.write(std::string(buffer.data(), to_read));
        offset += to_read;
    }
    res.end();
});
```

## Serverless constraints shape the configuration strategy

Serverless platforms impose consistent limitations that the abstraction must address:

| Platform | Writable Storage | Temp Limit | Persistent Options |
|----------|------------------|------------|-------------------|
| AWS Lambda | `/tmp` only | 512MB-10GB | EFS (requires VPC), S3 |
| Google Cloud Run | In-memory | Shares container RAM | GCS FUSE, NFS |
| Azure Functions | `C:\local\Temp` | ~500MB | Azure Files (5TB) |
| Fly.io | Ephemeral root | 2000 IOPS limit | Volumes (500GB max) |

The critical insight: **configuration files should be read from object storage, not mounted volumes.** This eliminates the need for EFS (which adds cold-start latency) or volume mounts (which complicate deployment).

Environment-based configuration is already idiomatic for serverless:

```yaml
template:
  path: '{{env.CONFIG_PATH}}'  # s3://my-bucket/config/ or ./local/
  environment-whitelist:
    - '^FLAPI_.*'
    - '^AWS_.*'

storage:
  default: '{{env.STORAGE_TYPE}}'  # s3, azure, gcs, local
  backends:
    s3:
      bucket: '{{env.S3_BUCKET}}'
      region: '{{env.AWS_REGION}}'
```

For serverless deployment, the startup sequence becomes: load bootstrap config from environment or embedded defaults → initialize DuckDB with httpfs/azure extensions → read full configuration from remote VFS → serve requests.

## Proposed minimal-glue architecture

The filesystem abstraction should be a thin wrapper, not a new implementation:

**Configuration loading adapter**: Modify flAPI's template loader to accept VFS URLs. When `template.path` starts with `s3://`, `gs://`, `az://`, or `https://`, use DuckDB's FileSystem instead of std::filesystem. The existing include resolution (`{{include from path}}`) would work unchanged since DuckDB handles path resolution within each filesystem.

```cpp
class ConfigLoader {
    duckdb::FileSystem& vfs_;
    
    std::string LoadFile(const std::string& path) {
        auto handle = vfs_.OpenFile(path, FileFlags::READ);
        std::string content;
        content.resize(vfs_.GetFileSize(*handle));
        vfs_.Read(*handle, content.data(), content.size(), 0);
        return content;
    }
    
    std::vector<std::string> ListEndpoints(const std::string& directory) {
        return vfs_.GlobFiles(directory + "/*.yaml", {});
    }
};
```

**HTTP response streaming bridge**: Create a Crow response writer that reads from VFS FileHandle. For large files (exceeding `stream_threshold`), write chunks directly to the socket. For small files, buffer in memory. Add Range request support for resumable downloads:

```cpp
void StreamVFSFile(crow::response& res, duckdb::FileHandle& handle, 
                   optional<pair<int64_t, int64_t>> range) {
    int64_t file_size = handle.GetFileSize();
    int64_t start = range ? range->first : 0;
    int64_t end = range ? range->second : file_size - 1;
    
    if (range) {
        res.code = 206;
        res.set_header("Content-Range", 
            fmt::format("bytes {}-{}/{}", start, end, file_size));
    }
    res.set_header("Content-Length", std::to_string(end - start + 1));
    // ... stream chunks
}
```

**Security layer**: Path validation must happen before VFS access. Implement canonicalization and scope checking:

```cpp
std::optional<std::string> ValidatePath(const std::string& user_path, 
                                         const std::string& allowed_prefix) {
    // Reject traversal attempts
    if (user_path.find("..") != std::string::npos) return std::nullopt;
    
    std::string resolved = resolve_relative_path(allowed_prefix, user_path);
    if (!resolved.starts_with(allowed_prefix)) return std::nullopt;
    
    return resolved;
}
```

## Configuration pattern unifies local and cloud deployment

The proposed YAML structure extends flAPI's existing patterns:

```yaml
project_name: cloud-native-flapi

storage:
  config_path: 's3://my-bucket/flapi-config/'  # or ./local/
  cache_path: 's3://my-bucket/flapi-cache/'
  
  credentials:
    s3:
      type: environment  # reads AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY
    azure:
      type: managed_identity
    gcs:
      type: service_account
      key_file: '/secrets/gcs-key.json'

template:
  path: '{{storage.config_path}}sqls/'
  
duckdb:
  db_path: ':memory:'  # serverless-friendly
  extensions:
    - httpfs
    - azure

connections:
  data-lake:
    properties:
      base_path: 's3://data-lake-bucket/'
```

For local development, paths remain unchanged (`./sqls/`). For cloud deployment, only `storage.config_path` changes. The same SQL templates work in both environments because they reference connection properties, not absolute paths.

## Implementation requires three focused changes

**Phase 1: VFS-aware configuration loading** (~2 weeks)
- Modify `TemplateLoader` to detect URL schemes and route to DuckDB FileSystem
- Add `storage` section to root config schema
- Implement credential initialization from environment or secrets
- Ensure `{{include}}` resolution works across VFS boundaries

**Phase 2: HTTP streaming bridge** (~1 week)
- Create `VFSResponseWriter` that chunks FileHandle reads to Crow responses
- Add Range request parsing and 206 response generation
- Integrate with existing authentication middleware
- Add Content-Type detection for streamed files

**Phase 3: Serverless packaging** (~1 week)
- Create minimal bootstrap configuration embedded in binary
- Document Lambda/Cloud Run/Azure Functions deployment patterns
- Add health check that verifies VFS connectivity
- Implement graceful degradation when remote storage unavailable

## Security requires defense in depth

Exposing filesystem operations through HTTP demands multiple protection layers:

**Path validation**: All user-provided paths must be validated against allowed prefixes. The existing DuckDB secrets SCOPE feature provides per-path credential isolation. flAPI should add its own allowlist of accessible paths per endpoint.

**Read-only by default**: Serverless environments assume read-only access. flAPI should default to read-only VFS operations, requiring explicit configuration to enable writes (for cache updates).

**Query blacklisting**: The existing `preventSqlInjection` validator should be extended to block filesystem-related SQL injection (e.g., `COPY TO`, `EXPORT DATABASE`).

**Rate limiting**: The current per-endpoint rate limiting applies naturally to file downloads, preventing abuse of cloud egress.

## Conclusion

The filesystem abstraction for flAPI should be minimal glue between DuckDB's mature VFS and Crow's HTTP layer. DuckDB already handles the complex work of cloud storage protocols, authentication, chunked I/O, and error handling. Crow provides the HTTP framework with streaming support. flAPI's contribution is the configuration bridge and security layer—estimated at **500-1000 lines of focused C++ code** rather than a new filesystem implementation.

This approach achieves the stated goal: developer-friendly cloud-native deployment that reuses existing implementations. Local development uses file paths; production uses S3 URLs. The same SQL templates, same endpoint definitions, same authentication—only the storage backend changes.
