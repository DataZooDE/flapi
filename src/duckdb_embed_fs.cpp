#include "duckdb_embed_fs.hpp"

#include "database_manager.hpp"
#include <crow.h>
#include "vfs_adapter.hpp"

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/main/capi/capi_internal.hpp"

#include <algorithm>
#include <cstring>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>

namespace flapi {

namespace {

// File handle holding a non-owning pointer to bytes in ArchiveEntries
// plus a streaming position. The owning shared_ptr lives in the
// EmbeddedFileSystem instance, so the data is valid as long as the
// filesystem outlives all its handles -- guaranteed for DuckDB
// sub-systems registered with RegisterSubSystem.
class EmbeddedFileHandle : public ::duckdb::FileHandle {
public:
    EmbeddedFileHandle(EmbeddedFileSystem& fs,
                       std::string path,
                       ::duckdb::FileOpenFlags flags,
                       const std::vector<std::uint8_t>* data)
        : ::duckdb::FileHandle(fs, std::move(path), flags),
          data_(data),
          position_(0) {}

    void Close() override { /* in-memory, nothing to release */ }

    const std::vector<std::uint8_t>* data() const { return data_; }
    ::duckdb::idx_t position() const { return position_; }
    void set_position(::duckdb::idx_t p) { position_ = p; }

private:
    const std::vector<std::uint8_t>* data_;
    ::duckdb::idx_t position_;
};

EmbeddedFileHandle& AsEmbedded(::duckdb::FileHandle& h) {
    return h.Cast<EmbeddedFileHandle>();
}

bool HasEmbedScheme(const std::string& path) {
    const std::size_t scheme_len = std::strlen(kEmbedScheme);
    return path.size() >= scheme_len &&
           path.compare(0, scheme_len, kEmbedScheme) == 0;
}

std::string StripEmbedScheme(const std::string& path) {
    const std::size_t scheme_len = std::strlen(kEmbedScheme);
    if (HasEmbedScheme(path)) {
        return path.substr(scheme_len);
    }
    return path;
}

bool ContainsGlobMeta(const std::string& s) {
    return s.find('*') != std::string::npos ||
           s.find('?') != std::string::npos ||
           s.find('[') != std::string::npos;
}

// Convert a basic glob (just *, ?, and literal chars) into a regex.
std::regex GlobToRegex(const std::string& pattern) {
    std::string out;
    out.reserve(pattern.size() * 2);
    for (char c : pattern) {
        switch (c) {
            case '*':
                out += "[^/]*";
                break;
            case '?':
                out += "[^/]";
                break;
            case '.': case '+': case '^': case '$':
            case '(': case ')': case '[': case ']':
            case '{': case '}': case '|': case '\\':
                out += '\\';
                out += c;
                break;
            default:
                out += c;
                break;
        }
    }
    return std::regex(out);
}

}  // namespace

EmbeddedFileSystem::EmbeddedFileSystem(std::shared_ptr<const ArchiveEntries> entries)
    : _entries(std::move(entries)) {
    if (_entries == nullptr) {
        throw std::invalid_argument(
            "EmbeddedFileSystem requires non-null ArchiveEntries");
    }
}

bool EmbeddedFileSystem::CanHandleFile(const std::string& fpath) {
    return HasEmbedScheme(fpath);
}

::duckdb::unique_ptr<::duckdb::FileHandle> EmbeddedFileSystem::OpenFile(
        const std::string& path,
        ::duckdb::FileOpenFlags flags,
        ::duckdb::optional_ptr<::duckdb::FileOpener> /*opener*/) {
    const std::string key = StripEmbedScheme(path);
    auto it = _entries->find(key);
    if (it == _entries->end()) {
        throw ::duckdb::IOException("embed://: no such entry: " + path);
    }
    return ::duckdb::make_uniq<EmbeddedFileHandle>(*this, path, flags, &it->second);
}

int64_t EmbeddedFileSystem::Read(::duckdb::FileHandle& handle, void* buffer, int64_t nr_bytes) {
    auto& h = AsEmbedded(handle);
    if (h.data() == nullptr) {
        return 0;
    }
    const auto& bytes = *h.data();
    const ::duckdb::idx_t pos = h.position();
    if (pos >= bytes.size()) {
        return 0;
    }
    const ::duckdb::idx_t remaining = bytes.size() - pos;
    const int64_t to_copy =
        std::min<int64_t>(nr_bytes, static_cast<int64_t>(remaining));
    std::memcpy(buffer, bytes.data() + pos, static_cast<std::size_t>(to_copy));
    h.set_position(pos + static_cast<::duckdb::idx_t>(to_copy));
    return to_copy;
}

void EmbeddedFileSystem::Read(::duckdb::FileHandle& handle, void* buffer,
                              int64_t nr_bytes, ::duckdb::idx_t location) {
    auto& h = AsEmbedded(handle);
    if (h.data() == nullptr) {
        throw ::duckdb::IOException("embed://: handle has no data");
    }
    const auto& bytes = *h.data();
    if (location > bytes.size() ||
        nr_bytes < 0 ||
        location + static_cast<::duckdb::idx_t>(nr_bytes) > bytes.size()) {
        throw ::duckdb::IOException(
            "embed://: positional read out of bounds for " + h.GetPath());
    }
    std::memcpy(buffer, bytes.data() + location, static_cast<std::size_t>(nr_bytes));
    // Positional Read intentionally does not move the cursor (mirrors
    // the documented FileSystem::Read(handle, buf, n, location) contract).
}

int64_t EmbeddedFileSystem::GetFileSize(::duckdb::FileHandle& handle) {
    auto& h = AsEmbedded(handle);
    if (h.data() == nullptr) {
        return -1;
    }
    return static_cast<int64_t>(h.data()->size());
}

::duckdb::timestamp_t EmbeddedFileSystem::GetLastModifiedTime(::duckdb::FileHandle& /*handle*/) {
    // Bundle entries are immutable in-memory bytes. Report the epoch so
    // readers that key caches on mtime (e.g. parquet) get a stable value;
    // anything else would break `SOURCE_DATE_EPOCH` reproducibility.
    return ::duckdb::timestamp_t(0);
}

void EmbeddedFileSystem::Seek(::duckdb::FileHandle& handle, ::duckdb::idx_t location) {
    auto& h = AsEmbedded(handle);
    if (h.data() == nullptr) {
        throw ::duckdb::IOException("embed://: handle has no data");
    }
    if (location > h.data()->size()) {
        throw ::duckdb::IOException(
            "embed://: seek beyond EOF for " + h.GetPath());
    }
    h.set_position(location);
}

::duckdb::idx_t EmbeddedFileSystem::SeekPosition(::duckdb::FileHandle& handle) {
    return AsEmbedded(handle).position();
}

bool EmbeddedFileSystem::OnDiskFile(::duckdb::FileHandle& /*handle*/) {
    return false;
}

bool EmbeddedFileSystem::FileExists(const std::string& filename,
                                    ::duckdb::optional_ptr<::duckdb::FileOpener> /*opener*/) {
    const std::string key = StripEmbedScheme(filename);
    return _entries->find(key) != _entries->end();
}

::duckdb::vector<::duckdb::OpenFileInfo> EmbeddedFileSystem::Glob(
        const std::string& path,
        ::duckdb::FileOpener* /*opener*/) {
    ::duckdb::vector<::duckdb::OpenFileInfo> result;

    // Non-glob path: just check existence.
    if (!ContainsGlobMeta(path)) {
        const std::string key = StripEmbedScheme(path);
        if (_entries->find(key) != _entries->end()) {
            result.emplace_back(path);
        }
        return result;
    }

    // We only support glob inside the embed:// scheme. Mixing schemes
    // here would be a bug in the caller.
    if (!HasEmbedScheme(path)) {
        return result;
    }

    const std::string key_pattern = StripEmbedScheme(path);
    const std::regex re = GlobToRegex(key_pattern);

    for (const auto& [name, _data] : *_entries) {
        if (std::regex_match(name, re)) {
            result.emplace_back(std::string(kEmbedScheme) + name);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

DirectoryEmbedFileSystem::DirectoryEmbedFileSystem(std::filesystem::path root)
    : _root(std::filesystem::weakly_canonical(std::move(root))),
      _local(::duckdb::FileSystem::CreateLocal()) {}

bool DirectoryEmbedFileSystem::CanHandleFile(const std::string& fpath) {
    return HasEmbedScheme(fpath);
}

std::string DirectoryEmbedFileSystem::MapPath(const std::string& path) const {
    // The bundle holds the config tree, so `embed://<rel>` names <config dir>/<rel>.
    // Nothing outside it: a packed binary could not reach `embed://../x` either.
    const std::string rel = StripEmbedScheme(path);
    const std::filesystem::path relative(rel);
    if (rel.empty() || relative.is_absolute()) {
        throw ::duckdb::IOException("embed://: not a path inside the configuration directory: " + path);
    }
    const auto mapped = (_root / relative).lexically_normal();
    const auto back = mapped.lexically_relative(_root);
    if (back.empty() || *back.begin() == "..") {
        throw ::duckdb::IOException("embed://: path escapes the configuration directory: " + path);
    }
    return mapped.string();
}

::duckdb::unique_ptr<::duckdb::FileHandle> DirectoryEmbedFileSystem::OpenFile(
        const std::string& path,
        ::duckdb::FileOpenFlags flags,
        ::duckdb::optional_ptr<::duckdb::FileOpener> opener) {
    // The handle belongs to the local filesystem, so every read, seek and size
    // call on it goes there directly (VirtualFileSystem dispatches by handle).
    return _local->OpenFile(MapPath(path), flags, opener);
}

::duckdb::vector<::duckdb::OpenFileInfo> DirectoryEmbedFileSystem::Glob(
        const std::string& path,
        ::duckdb::FileOpener* opener) {
    return _local->Glob(MapPath(path), opener);
}

bool DirectoryEmbedFileSystem::FileExists(const std::string& filename,
                                          ::duckdb::optional_ptr<::duckdb::FileOpener> opener) {
    return _local->FileExists(MapPath(filename), opener);
}

bool RegisterEmbeddedFileSystem(const std::string& config_dir) {
    auto entries = FileProviderFactory::GetBundleContents();
    if (entries == nullptr && config_dir.empty()) {
        return false;
    }

    auto dbm = DatabaseManager::getInstance();
    if (!dbm || !dbm->isInitialized()) {
        return false;
    }

    auto conn = dbm->getConnection();
    if (conn == nullptr) {
        return false;
    }

    auto* conn_wrapper = reinterpret_cast<::duckdb::Connection*>(conn);
    if (conn_wrapper == nullptr || conn_wrapper->context == nullptr) {
        duckdb_disconnect(&conn);
        return false;
    }

    auto& fs = ::duckdb::FileSystem::GetFileSystem(*conn_wrapper->context);
    if (entries != nullptr) {
        fs.RegisterSubSystem(::duckdb::make_uniq<EmbeddedFileSystem>(entries));
    } else {
        // No bundle: a config written for `flapi pack` (read_csv('embed://data/x.csv'))
        // must also run from its source tree - a one-shot refresh there failed
        // "No files found that match embed://...". Same paths, read from disk.
        fs.RegisterSubSystem(::duckdb::make_uniq<DirectoryEmbedFileSystem>(config_dir));
        CROW_LOG_INFO << "No bundle in this binary: embed:// paths resolve against the configuration directory "
                      << config_dir;
    }

    duckdb_disconnect(&conn);
    return true;
}

}  // namespace flapi
