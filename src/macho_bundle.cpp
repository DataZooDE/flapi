#include "macho_bundle.hpp"

#include "archive_io.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace flapi {

namespace {

// Magic constants from <mach-o/loader.h>. Duplicated here so the
// parser is portable to Linux/Windows builds (the integration tests
// for Mach-O parsing on those platforms run against synthetic
// fixtures).
constexpr std::uint32_t kMachOMagic32   = 0xFEEDFACEu;
constexpr std::uint32_t kMachOCigam32   = 0xCEFAEDFEu;  // byte-swapped
constexpr std::uint32_t kMachOMagic64   = 0xFEEDFACFu;
constexpr std::uint32_t kMachOCigam64   = 0xCFFAEDFEu;
constexpr std::uint32_t kFatMagic       = 0xCAFEBABEu;
constexpr std::uint32_t kFatCigam       = 0xBEBAFECAu;
constexpr std::uint32_t kFatMagic64     = 0xCAFEBABFu;
constexpr std::uint32_t kFatCigam64     = 0xBFBAFECAu;

constexpr std::uint32_t kLcSegment      = 0x01;
constexpr std::uint32_t kLcSegment64    = 0x19;

// Apple CPU type constants (from <mach/machine.h>). Only the ones we
// care about for slice selection on hosts we actually run on. Marked
// maybe_unused because the host-arch #ifdef below only picks one.
[[maybe_unused]] constexpr std::uint32_t kCpuTypeX86_64 = 0x01000007u;
[[maybe_unused]] constexpr std::uint32_t kCpuTypeArm64  = 0x0100000Cu;

// What slice to prefer when the input is a fat / universal binary.
// Resolved at compile time from the host arch so the recursive
// LocateFlapiSection call picks the slice that matches the binary
// doing the lookup -- which, in the self-packaging case, is the
// binary being looked at. 0 means "no preference; first slice".
#if defined(__aarch64__) || defined(__arm64__)
constexpr std::uint32_t kPreferredCpuType = kCpuTypeArm64;
#elif defined(__x86_64__) || defined(_M_X64)
constexpr std::uint32_t kPreferredCpuType = kCpuTypeX86_64;
#else
constexpr std::uint32_t kPreferredCpuType = 0u;
#endif

std::uint32_t ReadU32LE(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16)
         | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64LE(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(ReadU32LE(p))
         | (static_cast<std::uint64_t>(ReadU32LE(p + 4)) << 32);
}

// Fat header + fat_arch records are stored big-endian on disk
// regardless of host endianness (Apple's universal-binary spec).
std::uint32_t ReadU32BE(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24)
         | (static_cast<std::uint32_t>(p[1]) << 16)
         | (static_cast<std::uint32_t>(p[2]) << 8)
         |  static_cast<std::uint32_t>(p[3]);
}

std::uint64_t ReadU64BE(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(ReadU32BE(p)) << 32)
         |  static_cast<std::uint64_t>(ReadU32BE(p + 4));
}

bool NameEquals(const std::uint8_t* fixed, std::size_t cap, const char* expected) {
    // Mach-O segment/section names are NUL-padded fixed-length fields.
    // We compare up to cap bytes, treating NUL as terminator on the
    // `fixed` side. The expected string must not exceed cap.
    const std::size_t exp_len = std::strlen(expected);
    if (exp_len > cap) {
        return false;
    }
    for (std::size_t i = 0; i < exp_len; ++i) {
        if (fixed[i] != static_cast<std::uint8_t>(expected[i])) {
            return false;
        }
    }
    // After the expected string, the rest must be NUL.
    for (std::size_t i = exp_len; i < cap; ++i) {
        if (fixed[i] != 0) {
            return false;
        }
    }
    return true;
}

// Parses load commands of a 64-bit Mach-O whose mach_header_64 starts
// at buffer[base]. ncmds + sizeofcmds were read from the header.
// Returns the matching section or nullopt.
std::optional<MachOSection> FindInLoadCommands64(
        const std::vector<std::uint8_t>& buffer,
        std::size_t base,
        std::uint32_t ncmds,
        std::uint32_t sizeofcmds) {
    // mach_header_64 is 32 bytes; load commands start right after.
    constexpr std::size_t kHeader64Size = 32;
    if (base + kHeader64Size > buffer.size()) {
        return std::nullopt;
    }

    std::size_t cursor = base + kHeader64Size;
    const std::size_t end = cursor + sizeofcmds;
    if (end > buffer.size()) {
        return std::nullopt;
    }

    for (std::uint32_t i = 0; i < ncmds; ++i) {
        if (cursor + 8 > end) {
            return std::nullopt;
        }
        const std::uint32_t cmd = ReadU32LE(buffer.data() + cursor);
        const std::uint32_t cmdsize = ReadU32LE(buffer.data() + cursor + 4);
        if (cmdsize < 8 || cursor + cmdsize > end) {
            return std::nullopt;
        }

        if (cmd == kLcSegment64) {
            // segment_command_64 layout (72 bytes before sections):
            //   uint32 cmd, uint32 cmdsize,
            //   char segname[16],
            //   uint64 vmaddr, uint64 vmsize,
            //   uint64 fileoff, uint64 filesize,
            //   int32 maxprot, int32 initprot,
            //   uint32 nsects, uint32 flags
            constexpr std::size_t kSeg64Size = 72;
            if (cmdsize < kSeg64Size) {
                return std::nullopt;
            }
            const std::uint8_t* segname_p = buffer.data() + cursor + 8;
            const std::uint32_t nsects = ReadU32LE(buffer.data() + cursor + 64);

            if (NameEquals(segname_p, 16, kFlapiSegName)) {
                // section_64 layout (80 bytes each):
                //   char sectname[16],
                //   char segname[16],
                //   uint64 addr, uint64 size,
                //   uint32 offset, uint32 align,
                //   uint32 reloff, uint32 nreloc,
                //   uint32 flags, uint32 reserved1,
                //   uint32 reserved2, uint32 reserved3
                constexpr std::size_t kSect64Size = 80;
                std::size_t sect_cursor = cursor + kSeg64Size;
                for (std::uint32_t s = 0; s < nsects; ++s) {
                    if (sect_cursor + kSect64Size > cursor + cmdsize) {
                        return std::nullopt;
                    }
                    const std::uint8_t* sn = buffer.data() + sect_cursor;
                    if (NameEquals(sn, 16, kFlapiSectName)) {
                        MachOSection out;
                        out.size = ReadU64LE(buffer.data() + sect_cursor + 40);
                        out.file_offset = ReadU32LE(buffer.data() + sect_cursor + 48);
                        return out;
                    }
                    sect_cursor += kSect64Size;
                }
            }
        } else if (cmd == kLcSegment) {
            // 32-bit segment. Layout differs from segment_command_64.
            //   uint32 cmd, uint32 cmdsize,
            //   char segname[16],
            //   uint32 vmaddr, uint32 vmsize,
            //   uint32 fileoff, uint32 filesize,
            //   int32 maxprot, int32 initprot,
            //   uint32 nsects, uint32 flags
            constexpr std::size_t kSeg32Size = 56;
            if (cmdsize < kSeg32Size) {
                return std::nullopt;
            }
            const std::uint8_t* segname_p = buffer.data() + cursor + 8;
            const std::uint32_t nsects = ReadU32LE(buffer.data() + cursor + 48);

            if (NameEquals(segname_p, 16, kFlapiSegName)) {
                constexpr std::size_t kSect32Size = 68;
                std::size_t sect_cursor = cursor + kSeg32Size;
                for (std::uint32_t s = 0; s < nsects; ++s) {
                    if (sect_cursor + kSect32Size > cursor + cmdsize) {
                        return std::nullopt;
                    }
                    const std::uint8_t* sn = buffer.data() + sect_cursor;
                    if (NameEquals(sn, 16, kFlapiSectName)) {
                        MachOSection out;
                        out.size = ReadU32LE(buffer.data() + sect_cursor + 36);
                        out.file_offset = ReadU32LE(buffer.data() + sect_cursor + 40);
                        return out;
                    }
                    sect_cursor += kSect32Size;
                }
            }
        }

        cursor += cmdsize;
    }
    return std::nullopt;
}

// Parses a fat / universal Mach-O header at buffer[0] and returns the
// absolute file offset of the slice we want to recurse into. The
// caller is responsible for the magic dispatch and for adding the
// returned offset to any per-slice section offsets it computes.
//
// Selection rule: first slice whose cputype matches kPreferredCpuType;
// else the first slice. The fallback gives deterministic behaviour on
// hosts whose arch isn't represented in the file (e.g., a PPC-only
// fat binary inspected on x86_64, or a test fixture built on a host
// arch we don't compile-time match).
//
// Returns nullopt on truncation, an absurd nfat_arch (we cap at 64
// slices -- real universal binaries top out at 4-5), or a slice whose
// declared extent exceeds the buffer.
struct FatSlice {
    std::uint64_t file_offset = 0;
    std::uint64_t size = 0;
};

std::optional<FatSlice> ParseFatHeader(
        const std::vector<std::uint8_t>& buffer,
        std::uint32_t magic) {
    constexpr std::size_t kFatHeaderSize = 8;
    if (buffer.size() < kFatHeaderSize) {
        return std::nullopt;
    }
    const bool is_64 = (magic == kFatMagic64 || magic == kFatCigam64);
    const std::size_t arch_size = is_64 ? 32u : 20u;
    const std::uint32_t nfat_arch = ReadU32BE(buffer.data() + 4);
    constexpr std::uint32_t kMaxSlices = 64u;
    if (nfat_arch == 0 || nfat_arch > kMaxSlices) {
        return std::nullopt;
    }
    if (buffer.size() < kFatHeaderSize + nfat_arch * arch_size) {
        return std::nullopt;
    }

    auto read_slice = [&](std::uint32_t i) -> FatSlice {
        const std::size_t off = kFatHeaderSize + i * arch_size;
        FatSlice s;
        // Layout (fat_arch):    cputype, cpusubtype, offset, size, align
        // Layout (fat_arch_64): cputype, cpusubtype, offset(64), size(64),
        //                       align, reserved
        if (is_64) {
            s.file_offset = ReadU64BE(buffer.data() + off + 8);
            s.size        = ReadU64BE(buffer.data() + off + 16);
        } else {
            s.file_offset = ReadU32BE(buffer.data() + off + 8);
            s.size        = ReadU32BE(buffer.data() + off + 12);
        }
        return s;
    };
    auto read_cputype = [&](std::uint32_t i) -> std::uint32_t {
        const std::size_t off = kFatHeaderSize + i * arch_size;
        return ReadU32BE(buffer.data() + off);
    };

    // Pass 1: preferred cputype.
    if (kPreferredCpuType != 0) {
        for (std::uint32_t i = 0; i < nfat_arch; ++i) {
            if (read_cputype(i) == kPreferredCpuType) {
                FatSlice s = read_slice(i);
                if (s.file_offset > buffer.size() ||
                    s.size > buffer.size() ||
                    s.file_offset + s.size > buffer.size()) {
                    return std::nullopt;
                }
                return s;
            }
        }
    }
    // Pass 2: first slice as fallback.
    FatSlice s = read_slice(0);
    if (s.file_offset > buffer.size() ||
        s.size > buffer.size() ||
        s.file_offset + s.size > buffer.size()) {
        return std::nullopt;
    }
    return s;
}

// Inner overload: parse a Mach-O whose first byte lives at
// buffer[base] and produce a section file_offset that is absolute
// within the original (possibly fat-wrapping) buffer.
std::optional<MachOSection> LocateFlapiSectionAt(
        const std::vector<std::uint8_t>& buffer,
        std::uint64_t base) {
    if (base + 32 > buffer.size()) {
        return std::nullopt;
    }
    const std::uint32_t magic = ReadU32LE(buffer.data() + base);

    // 32-bit Mach-O: we don't ship 32-bit artifacts; cigam (byte-swapped)
    // is also out of scope for this parser. A future PR can extend if a
    // legit use-case appears.
    if (magic != kMachOMagic64) {
        return std::nullopt;
    }

    // mach_header_64 layout:
    //   uint32 magic, cputype, cpusubtype, filetype,
    //   uint32 ncmds, sizeofcmds, flags, reserved
    const std::uint32_t ncmds      = ReadU32LE(buffer.data() + base + 16);
    const std::uint32_t sizeofcmds = ReadU32LE(buffer.data() + base + 20);

    auto inner = FindInLoadCommands64(buffer, base, ncmds, sizeofcmds);
    if (!inner.has_value()) {
        return std::nullopt;
    }
    // The section's offset field is recorded relative to its slice's
    // base in the on-disk Mach-O, so callers further out need the
    // absolute file offset. base is the slice's absolute offset in the
    // outer (potentially fat) file; adding it gives the absolute byte
    // position seek() should land on.
    inner->file_offset += base;
    return inner;
}

}  // namespace

bool IsMachOMagic(const std::uint8_t magic_bytes[4]) {
    const std::uint32_t m = ReadU32LE(magic_bytes);
    return m == kMachOMagic32 || m == kMachOCigam32 ||
           m == kMachOMagic64 || m == kMachOCigam64 ||
           m == kFatMagic     || m == kFatCigam ||
           m == kFatMagic64   || m == kFatCigam64;
}

std::optional<MachOSection> LocateFlapiSectionInBuffer(
        const std::vector<std::uint8_t>& buffer) {
    if (buffer.size() < 32) {
        return std::nullopt;
    }
    const std::uint32_t magic = ReadU32LE(buffer.data());

    // Fat / universal binary: pick the slice that matches the host
    // arch (or the first slice as a deterministic fallback) and recurse
    // into the inner thin Mach-O. The returned file_offset is absolute
    // within the fat file, which is what OverwriteFlapiSection and
    // LocateBundleInRange both expect.
    if (magic == kFatMagic || magic == kFatCigam ||
        magic == kFatMagic64 || magic == kFatCigam64) {
        auto slice = ParseFatHeader(buffer, magic);
        if (!slice.has_value()) {
            return std::nullopt;
        }
        return LocateFlapiSectionAt(buffer, slice->file_offset);
    }

    return LocateFlapiSectionAt(buffer, /*base=*/0);
}

std::optional<MachOSection> LocateFlapiSection(const std::filesystem::path& path) {
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size < 32) {
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    // Read the header + a reasonable load-command budget (4 MiB cap so
    // we don't slurp huge binaries -- normal load-cmd table is < 64 KiB).
    constexpr std::size_t kReadCap = 4u * 1024u * 1024u;
    const auto to_read =
        static_cast<std::size_t>(std::min<std::uint64_t>(file_size, kReadCap));
    std::vector<std::uint8_t> buf(to_read);
    in.read(reinterpret_cast<char*>(buf.data()),
            static_cast<std::streamsize>(to_read));
    if (!in) {
        return std::nullopt;
    }
    return LocateFlapiSectionInBuffer(buf);
}

void OverwriteFlapiSection(const std::filesystem::path& binary,
                           const MachOSection& section,
                           const std::vector<std::uint8_t>& payload) {
    if (payload.size() > section.size) {
        throw ArchiveIOError(
            "embedded archive (" + std::to_string(payload.size()) +
            " bytes) exceeds reserved __FLAPI/__bundle section (" +
            std::to_string(section.size) +
            " bytes); rebuild flapi with a larger FLAPI_RESERVED_BUNDLE_MIB");
    }

    std::fstream out(binary, std::ios::in | std::ios::out | std::ios::binary);
    if (!out.is_open()) {
        throw ArchiveIOError("cannot open for in-place write: " + binary.string());
    }

    out.seekp(static_cast<std::streamoff>(section.file_offset), std::ios::beg);
    if (!payload.empty()) {
        out.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
    }

    // Zero-pad the trailing slack so re-packs are reproducible AND so
    // the EOCD reverse-scan inside the section finds the record cleanly
    // without sniffing leftover bytes from a prior bundle.
    const std::uint64_t slack = section.size - payload.size();
    if (slack > 0) {
        std::vector<std::uint8_t> zeros(
            static_cast<std::size_t>(std::min<std::uint64_t>(slack, 64u * 1024u)), 0);
        std::uint64_t remaining = slack;
        while (remaining > 0) {
            const auto chunk =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, zeros.size()));
            out.write(reinterpret_cast<const char*>(zeros.data()),
                      static_cast<std::streamsize>(chunk));
            remaining -= chunk;
        }
    }

    out.flush();
    if (!out) {
        throw ArchiveIOError("write failure on " + binary.string());
    }
}

CodesignResult CodesignBinary(const std::filesystem::path& binary) {
    CodesignResult r;
#ifdef __APPLE__
    const char* env_id = std::getenv("CODESIGN_IDENTITY");
    r.identity = (env_id != nullptr && *env_id != '\0') ? env_id : "-";

    // Use a popen-based invocation so we can capture stderr for the
    // diagnostic tail. We pass arguments via execvp-style argv when
    // we have it; for popen we shell-quote conservatively.
    auto shell_quote = [](const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out.push_back(c);
            }
        }
        out.push_back('\'');
        return out;
    };

    const std::string cmd =
        "codesign --force --sign " + shell_quote(r.identity) + " " +
        shell_quote(binary.string()) + " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        r.exit_code = -1;
        r.stderr_tail = "failed to invoke codesign (popen)";
        return r;
    }

    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        r.stderr_tail.append(buf.data());
        if (r.stderr_tail.size() > 4096) {
            // Keep only the tail so we don't drag a ton of output around.
            r.stderr_tail.erase(0, r.stderr_tail.size() - 4096);
        }
    }
    const int status = pclose(pipe);
    if (status == -1) {
        r.exit_code = -1;
    } else if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
    } else {
        r.exit_code = -1;
    }
#else
    (void)binary;
    r.exit_code = 0;
    r.identity = "n/a";
#endif
    return r;
}

}  // namespace flapi
