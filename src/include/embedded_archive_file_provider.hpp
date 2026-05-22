#pragma once

#include "archive_io.hpp"
#include "vfs_adapter.hpp"

#include <memory>
#include <string>
#include <vector>

namespace flapi {

// IFileProvider implementation backed by a decompressed in-memory ZIP.
//
// Used when the running binary has a bundle appended (#42). Entries
// live in a `std::shared_ptr<const ArchiveEntries>` shared with the
// DuckDB-side `embed://` filesystem (#44) so we hold one in-memory map,
// not two.
//
// Path normalisation rules (input -> entry key):
//   - leading `file://` stripped (parity with LocalFileProvider)
//   - leading `./` stripped, repeatedly
//   - leading `/` stripped
//   - trailing `/` stripped on directories
class EmbeddedArchiveFileProvider : public IFileProvider {
public:
    explicit EmbeddedArchiveFileProvider(std::shared_ptr<const ArchiveEntries> entries);
    ~EmbeddedArchiveFileProvider() override = default;

    std::string ReadFile(const std::string& path) override;
    bool FileExists(const std::string& path) override;
    std::vector<std::string> ListFiles(const std::string& directory,
                                       const std::string& pattern = "*") override;
    bool IsRemotePath(const std::string& path) const override;
    std::string GetProviderName() const override { return "embedded"; }

private:
    std::shared_ptr<const ArchiveEntries> _entries;
};

}  // namespace flapi
