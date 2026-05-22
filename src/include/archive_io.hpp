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

// Reads an in-memory ZIP archive. Throws ArchiveIOError when the
// buffer is empty, not a recognised ZIP, or truncated.
ArchiveEntries ReadArchive(const std::vector<std::uint8_t>& buffer);

}  // namespace flapi
