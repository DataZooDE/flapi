#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace flapi {

// The reserved Mach-O segment+section we use to host the bundled
// ZIP on macOS. Allocated at link time with a placeholder of
// FLAPI_RESERVED_BUNDLE_MIB MiB and overwritten by `flapi pack`
// at packaging time so the binary signature stays valid after a
// re-`codesign`.
constexpr const char* kFlapiSegName = "__FLAPI";
constexpr const char* kFlapiSectName = "__bundle";

struct MachOSection {
    // File offset of the section's first byte, suitable for seek().
    std::uint64_t file_offset = 0;
    // Allocated section size in bytes (== reserved capacity).
    std::uint64_t size = 0;
};

// Returns true if the bytes at `magic_bytes` (read from the head of
// a file) are a Mach-O magic value we recognise. Cheap pre-check.
bool IsMachOMagic(const std::uint8_t magic_bytes[4]);

// Locate the __FLAPI/__bundle section in a Mach-O file on disk.
// Returns nullopt if:
//   - the file isn't a thin (non-fat) Mach-O,
//   - the file is malformed,
//   - the section doesn't exist (e.g., on a non-macOS build).
//
// Fat / universal binaries are currently not supported -- a follow-up
// can iterate slices. macOS releases produced by this repo are thin
// per-architecture, so the gap is acceptable for now.
std::optional<MachOSection> LocateFlapiSection(const std::filesystem::path& path);

// Overload that scans a buffer instead of opening a file. Used by
// unit tests against synthetic Mach-O fixtures.
std::optional<MachOSection> LocateFlapiSectionInBuffer(
    const std::vector<std::uint8_t>& buffer);

// Overwrite the reserved section at `binary` with `payload`. The
// trailing capacity beyond payload.size() is zero-padded so that the
// EOCD of the embedded ZIP still reverse-scans cleanly from segment
// EOF. Throws ArchiveIOError when the payload is bigger than the
// reserved capacity (see PackOptions::host_binary_override for the
// override knob).
//
// Caller is responsible for re-signing the binary afterwards
// (CodesignBinary), since the signature covers the section bytes.
void OverwriteFlapiSection(const std::filesystem::path& binary,
                           const MachOSection& section,
                           const std::vector<std::uint8_t>& payload);

// Result of a codesign attempt.
struct CodesignResult {
    int exit_code = 0;
    std::string identity;     // "-" for ad-hoc; otherwise the CODESIGN_IDENTITY value
    std::string stderr_tail;  // last few KB of codesign stderr, for diagnostics
};

// Invoke `codesign --force --sign <identity> <binary>` where identity
// is taken from the CODESIGN_IDENTITY env var, defaulting to "-"
// (ad-hoc). On non-Darwin builds this is a no-op that returns
// exit_code = 0 -- the caller doesn't need a platform guard.
CodesignResult CodesignBinary(const std::filesystem::path& binary);

}  // namespace flapi
