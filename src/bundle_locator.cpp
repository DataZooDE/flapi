#include "bundle_locator.hpp"
#include "macho_bundle.hpp"
#include "selfpath.hpp"

#include <crow/logging.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <vector>

namespace flapi {

namespace {

constexpr std::size_t kEocdRecordSize = 22;
constexpr std::size_t kMaxCommentLen = 0xffffu;

// Total tail buffer = EOCD + max comment + 64 KiB pad budget. Generous
// vs. the spike's 10 KiB libarchive tar-block default; cheap to read.
constexpr std::size_t kPaddingBudget = 65536;
constexpr std::size_t kScanBudget = kEocdRecordSize + kMaxCommentLen + kPaddingBudget;

std::uint16_t ReadU16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t ReadU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16)
         | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Scan a buffer for the most-recent valid EOCD record. `buf_start_in_file`
// is the absolute file offset of buf[0]; used to translate the result.
// `logical_eof_in_buf` is the buf-relative position to treat as EOF for
// padding-tolerance purposes (== buf.size() in the common case).
std::optional<BundleLocation> ScanBufferForEocd(
        const std::vector<std::uint8_t>& buf,
        std::uint64_t buf_start_in_file,
        std::size_t logical_eof_in_buf) {
    if (logical_eof_in_buf > buf.size() || logical_eof_in_buf < kEocdRecordSize) {
        return std::nullopt;
    }
    const std::size_t max_start = logical_eof_in_buf - kEocdRecordSize;
    for (std::size_t i = max_start + 1; i-- > 0; ) {
        if (buf[i]     != 0x50 ||
            buf[i + 1] != 0x4b ||
            buf[i + 2] != 0x05 ||
            buf[i + 3] != 0x06) {
            continue;
        }
        const std::uint8_t* p = buf.data() + i;
        const std::uint16_t this_disk     = ReadU16(p + 4);
        const std::uint16_t cd_start_disk = ReadU16(p + 6);
        const std::uint16_t entries_this  = ReadU16(p + 8);
        const std::uint16_t entries_total = ReadU16(p + 10);
        const std::uint32_t cd_size       = ReadU32(p + 12);
        const std::uint32_t cd_offset_arch = ReadU32(p + 16);
        const std::uint16_t comment_len   = ReadU16(p + 20);

        if (this_disk != 0 || cd_start_disk != 0) {
            continue;
        }
        // ZIP64 sentinels (#65): when a ZIP exceeds 4 GiB or 65535 entries,
        // the EOCD's size / offset / entry-count fields are pinned to
        // 0xffffffff / 0xffff and the real values live in a ZIP64 EOCD
        // record we don't parse. Surface this explicitly -- the implicit
        // range-check rejection below would otherwise return nullopt with
        // no log line, making the failure indistinguishable from "no
        // bundle present" in operator logs.
        constexpr std::uint16_t kZip64SentinelU16 = 0xffffu;
        constexpr std::uint32_t kZip64SentinelU32 = 0xffffffffu;
        if (cd_size == kZip64SentinelU32 || cd_offset_arch == kZip64SentinelU32 ||
            entries_this == kZip64SentinelU16 || entries_total == kZip64SentinelU16) {
            CROW_LOG_WARNING
                << "bundle_locator: ZIP64-shaped EOCD detected at file offset "
                << (buf_start_in_file + i)
                << " (cd_size=0x" << std::hex << cd_size
                << " cd_offset=0x" << cd_offset_arch
                << " entries=" << std::dec << entries_this
                << "); ZIP64 is not supported -- treating as no bundle present";
            return std::nullopt;
        }
        if (entries_this != entries_total) {
            continue;
        }

        const std::size_t comment_end = i + kEocdRecordSize + comment_len;
        if (comment_end > logical_eof_in_buf) {
            continue;
        }

        // Anything after the comment up to logical EOF must be zero.
        bool padding_ok = true;
        for (std::size_t j = comment_end; j < logical_eof_in_buf; ++j) {
            if (buf[j] != 0) {
                padding_ok = false;
                break;
            }
        }
        if (!padding_ok) {
            continue;
        }

        const std::uint64_t eocd_file_offset = buf_start_in_file + i;
        if (cd_size > eocd_file_offset) {
            continue;
        }
        const std::uint64_t cd_file_offset = eocd_file_offset - cd_size;
        if (cd_offset_arch > cd_file_offset) {
            continue;
        }
        const std::uint64_t bundle_start = cd_file_offset - cd_offset_arch;
        const std::uint64_t bundle_end =
            eocd_file_offset + kEocdRecordSize + comment_len;
        if (bundle_end < bundle_start) {
            continue;
        }

        BundleLocation loc;
        loc.offset = bundle_start;
        loc.size = bundle_end - bundle_start;
        return loc;
    }
    return std::nullopt;
}

}  // namespace

std::optional<BundleLocation> LocateBundle(const std::filesystem::path& path) {
    std::error_code ec;
    // macOS reserved-segment path (#48): try the __FLAPI/__bundle section
    // first. Linux/Windows binaries don't have the section, so this is a
    // cheap miss and we fall through to the EOF tail scan below. Required
    // so `flapi info` / `flapi unpack` (which call LocateBundle directly,
    // not LocateBundleInSelf) find macOS-packed bundles.
    if (auto sect = LocateFlapiSection(path); sect.has_value()) {
        if (auto loc = LocateBundleInRange(path, sect->file_offset, sect->size);
            loc.has_value()) {
            return loc;
        }
        // Section present but unpopulated -- fall through to EOF scan
        // so --macos-append bundles are still discoverable.
    }

    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size < kEocdRecordSize) {
        return std::nullopt;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    const std::size_t tail_bytes =
        static_cast<std::size_t>(std::min<std::uint64_t>(file_size, kScanBudget));
    const std::uint64_t tail_start = file_size - tail_bytes;

    std::vector<std::uint8_t> tail(tail_bytes);
    in.seekg(static_cast<std::streamoff>(tail_start), std::ios::beg);
    in.read(reinterpret_cast<char*>(tail.data()),
            static_cast<std::streamsize>(tail_bytes));
    if (!in) {
        return std::nullopt;
    }
    return ScanBufferForEocd(tail, tail_start, tail.size());
}

std::optional<BundleLocation> LocateBundleInRange(
        const std::filesystem::path& path,
        std::uint64_t range_offset,
        std::uint64_t range_size) {
    if (range_size < kEocdRecordSize) {
        return std::nullopt;
    }
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(path, ec);
    if (ec || range_offset + range_size > file_size) {
        return std::nullopt;
    }
    // Cap range_size at something sensible -- a runaway section_size
    // value from a malformed Mach-O could otherwise allocate gigabytes.
    constexpr std::uint64_t kMaxRangeBytes = 64ull * 1024ull * 1024ull;
    const std::size_t to_read =
        static_cast<std::size_t>(std::min<std::uint64_t>(range_size, kMaxRangeBytes));

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> buf(to_read);
    in.seekg(static_cast<std::streamoff>(range_offset), std::ios::beg);
    in.read(reinterpret_cast<char*>(buf.data()),
            static_cast<std::streamsize>(to_read));
    if (!in) {
        return std::nullopt;
    }
    return ScanBufferForEocd(buf, range_offset, buf.size());
}

std::optional<BundleLocation> LocateBundleInSelf() {
    std::filesystem::path self_path;
    try {
        self_path = GetSelfPath();
    } catch (...) {
        return std::nullopt;
    }
    // LocateBundle now tries section-mode first, then EOF tail.
    try {
        return LocateBundle(self_path);
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace flapi
