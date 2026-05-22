#include "bundle_locator.hpp"
#include "macho_bundle.hpp"
#include "selfpath.hpp"

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

    // Prefer the reserved Mach-O section if present (#48). Linux/Windows
    // binaries lack the section, so this returns nullopt and we fall
    // through to the EOF-tail scan unchanged.
    if (auto sect = LocateFlapiSection(self_path); sect.has_value()) {
        if (auto loc = LocateBundleInRange(self_path, sect->file_offset, sect->size);
            loc.has_value()) {
            return loc;
        }
        // Section is present but empty / unpopulated (e.g., un-packed
        // build): fall through to the EOF-tail scan.
    }

    try {
        return LocateBundle(self_path);
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace flapi
