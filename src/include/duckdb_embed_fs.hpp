#pragma once

#include "archive_io.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace flapi {

constexpr const char* kEmbedScheme = "embed://";

// DuckDB FileSystem implementation that serves entries from an
// in-memory ZIP via the `embed://` scheme. Sibling to
// `EmbeddedArchiveFileProvider` (#43) -- the two share one
// `std::shared_ptr<const ArchiveEntries>` so there is exactly one
// decompressed copy in memory.
//
// The spike's hard-won lesson is encoded in this override set:
// `OpenFile` / `Read` / `Seek` / `GetFileSize` alone are not enough
// for `read_csv()`. We also override `Glob` (path expansion happens
// before opening) and `SeekPosition` (the base class throws
// "not implemented" rather than no-oping for either).
class EmbeddedFileSystem : public ::duckdb::FileSystem {
public:
    explicit EmbeddedFileSystem(std::shared_ptr<const ArchiveEntries> entries);
    ~EmbeddedFileSystem() override = default;

    ::duckdb::unique_ptr<::duckdb::FileHandle> OpenFile(
        const std::string& path,
        ::duckdb::FileOpenFlags flags,
        ::duckdb::optional_ptr<::duckdb::FileOpener> opener = nullptr) override;

    int64_t Read(::duckdb::FileHandle& handle, void* buffer, int64_t nr_bytes) override;
    void Read(::duckdb::FileHandle& handle, void* buffer, int64_t nr_bytes,
              ::duckdb::idx_t location) override;

    int64_t GetFileSize(::duckdb::FileHandle& handle) override;

    void Seek(::duckdb::FileHandle& handle, ::duckdb::idx_t location) override;
    ::duckdb::idx_t SeekPosition(::duckdb::FileHandle& handle) override;

    ::duckdb::vector<::duckdb::OpenFileInfo> Glob(
        const std::string& path,
        ::duckdb::FileOpener* opener = nullptr) override;

    bool CanHandleFile(const std::string& fpath) override;
    bool FileExists(const std::string& filename,
                    ::duckdb::optional_ptr<::duckdb::FileOpener> opener = nullptr) override;
    bool OnDiskFile(::duckdb::FileHandle& handle) override;
    bool CanSeek() override { return true; }

    std::string GetName() const override { return "embed"; }

private:
    std::shared_ptr<const ArchiveEntries> _entries;
};

// Registers an EmbeddedFileSystem on the global DuckDB instance.
// No-op when no bundle has been set via FileProviderFactory or when
// DatabaseManager isn't initialised yet. Returns true if a filesystem
// was registered.
bool RegisterEmbeddedFileSystem();

}  // namespace flapi
