#pragma once

#include "archive_io.hpp"

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>

namespace flapi {

class PackError : public std::runtime_error {
public:
    explicit PackError(const std::string& m) : std::runtime_error(m) {}
};

struct PackOptions {
    // Bundle files even when their relative path matches the default
    // secret-exclude list. Intended for tests; production users must
    // not flip this.
    bool allow_secrets = false;

    // mtime stamped on every archive entry. nullopt resolves to the
    // SOURCE_DATE_EPOCH environment variable if set, else 0. Set
    // explicitly to make tests deterministic.
    std::optional<std::int64_t> source_date_epoch;

    // Override the source binary whose host bytes get copied into
    // `out_path` before the archive is appended. Defaults to
    // `GetSelfPath()`. Tests use this to feed a small fake host
    // binary -- avoiding the cost of copying the multi-GB
    // sanitiser-instrumented test binary on every test case.
    std::optional<std::filesystem::path> host_binary_override;
};

struct PackResult {
    std::filesystem::path output;
    std::size_t entry_count = 0;
    std::uint64_t archive_size = 0;
};

// Pack `in_dir` (recursively) into a self-contained executable at
// `out_path`.
//
// Steps:
//  1. Copies the running binary's host bytes to `out_path`. If the
//     running binary itself already has a trailing ZIP, only the
//     pre-bundle prefix is copied -- so re-pack is idempotent.
//  2. Walks `in_dir` recursively. Files matching the default secret
//     deny list (*.env, secrets/*, *.pem, *.key) cause a PackError
//     unless `options.allow_secrets` is true.
//  3. Writes an archive (via archive_io::WriteArchive) and appends it.
//  4. Copies executable bits from the running binary onto the output
//     (best-effort, Unix only).
//
// Throws PackError on any I/O or policy failure. Leaves `out_path` in
// an undefined state on throw -- callers are expected to delete it.
PackResult Pack(const std::filesystem::path& in_dir,
                const std::filesystem::path& out_path,
                const PackOptions& options = {});

// Print bundle info for a binary on disk. Returns 0 when a bundle is
// found and listed; 1 when no bundle is present.
int PrintBundleInfoForPath(const std::filesystem::path& binary, std::ostream& os);

// Print bundle info for the running binary (convenience).
int PrintBundleInfo(std::ostream& os);

struct UnpackResult {
    std::size_t entries_written = 0;
};

// Unpack the bundle from `binary` into `dst_dir`. Creates `dst_dir`
// (and intermediate dirs per entry) if missing. Throws PackError if
// the binary has no bundle or any entry can't be written.
UnpackResult UnpackBundleFromPath(const std::filesystem::path& binary,
                                  const std::filesystem::path& dst_dir);

// Unpack the running binary's bundle (convenience).
UnpackResult UnpackBundle(const std::filesystem::path& dst_dir);

// Whether `rel_path` matches the default secret exclude list. The
// patterns:
//   - `*.env`     at any depth                (e.g. `.env`, `cfg/.env`)
//   - `secrets/`  segment at any depth        (e.g. `secrets/x`, `a/secrets/x`)
//   - `*.pem`     at any depth
//   - `*.key`     at any depth
bool IsSecretExcluded(const std::string& rel_path);

}  // namespace flapi
