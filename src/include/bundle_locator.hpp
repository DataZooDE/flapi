#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace flapi {

// Location of an appended ZIP archive within a host binary.
//
// `offset` is the file offset of the first ZIP local file header --
// the byte you would seek to when slicing the file for libarchive.
// `size`   is the total bytes from `offset` through (and including)
// the End-of-Central-Directory record plus its comment, excluding any
// trailing zero padding that the locator tolerates.
struct BundleLocation {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

// Reverse-scans the file at `path` for a ZIP End-of-Central-Directory
// record. Returns the bundle's location, or nullopt if no valid
// bundle is detected.
//
// Tolerates trailing zero padding after the EOCD record -- the
// spike-caught case of libarchive's default 10240-byte tar-block
// rounding pushing the EOCD off file-EOF.
std::optional<BundleLocation> LocateBundle(const std::filesystem::path& path);

// Convenience: scan the currently running executable. Returns nullopt
// if either the self-path lookup or the EOCD scan fails.
std::optional<BundleLocation> LocateBundleInSelf();

}  // namespace flapi
