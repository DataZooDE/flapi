#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace flapi {

using ArchiveEntries = std::map<std::string, std::vector<std::uint8_t>>;

struct ArchiveWriteOptions {
    std::optional<std::int64_t> source_date_epoch;
};

class ArchiveIOError : public std::runtime_error {
public:
    explicit ArchiveIOError(const std::string& message)
        : std::runtime_error(message) {}
};

// Writes the entries as an in-memory ZIP archive.
//
// Entries are emitted in std::map iteration order (sorted by key), so
// callers that hand in the same logical contents get a byte-identical
// output as long as `source_date_epoch` matches.
//
// Internally calls `archive_write_set_bytes_in_last_block(a, 1)` so
// the output is free of the 10240-byte tar-block zero padding that
// would otherwise confuse a downstream EOCD reverse scan -- a
// requirement of the self-packaging use case.
//
// Throws ArchiveIOError on libarchive failure.
std::vector<std::uint8_t> WriteArchive(const ArchiveEntries& entries,
                                       const ArchiveWriteOptions& options = {});

// True when `name` is a plain relative path that stays inside the directory it is
// extracted to: non-empty, no NUL, no backslash, not absolute (`/x`, `C:`), and no `..`
// component. A bundle entry failing this is a zip-slip attempt.
bool IsSafeArchiveEntryName(const std::string& name);

// Limits applied while a bundle is READ (startup, `info`, `unpack`): a few KiB of
// ZIP can inflate to gigabytes. Defaults: 1 GiB total, 100000 entries; override with
// FLAPI_BUNDLE_MAX_MIB / FLAPI_BUNDLE_MAX_ENTRIES.
//
// Reads an in-memory ZIP archive. Throws ArchiveIOError when the
// buffer is empty, not a recognised ZIP, or truncated.
ArchiveEntries ReadArchive(const std::vector<std::uint8_t>& buffer);

}  // namespace flapi
