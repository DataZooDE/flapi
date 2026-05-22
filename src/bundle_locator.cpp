#include "bundle_locator.hpp"
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

// We accept padding well in excess of the 10 KiB spike default; the
// total tail buffer is EOCD + max comment + 64 KiB pad budget.
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
    if (tail.size() < kEocdRecordSize) {
        return std::nullopt;
    }

    // Reverse-scan from the latest valid signature position. The latest
    // (largest-offset) EOCD wins, since any earlier signature byte
    // sequence in random leading data is a false positive.
    const std::size_t max_start = tail.size() - kEocdRecordSize;
    for (std::size_t i = max_start + 1; i-- > 0; ) {
        if (tail[i]     != 0x50 ||
            tail[i + 1] != 0x4b ||
            tail[i + 2] != 0x05 ||
            tail[i + 3] != 0x06) {
            continue;
        }

        const std::uint8_t* p = tail.data() + i;
        const std::uint16_t this_disk        = ReadU16(p + 4);
        const std::uint16_t cd_start_disk    = ReadU16(p + 6);
        const std::uint16_t entries_this     = ReadU16(p + 8);
        const std::uint16_t entries_total    = ReadU16(p + 10);
        const std::uint32_t cd_size          = ReadU32(p + 12);
        const std::uint32_t cd_offset_arch   = ReadU32(p + 16);
        const std::uint16_t comment_len      = ReadU16(p + 20);

        // Multi-disk archives are not supported.
        if (this_disk != 0 || cd_start_disk != 0) {
            continue;
        }
        if (entries_this != entries_total) {
            continue;
        }

        // The comment must fit in the tail.
        const std::size_t comment_end = i + kEocdRecordSize + comment_len;
        if (comment_end > tail.size()) {
            continue;
        }

        // Anything after the comment up to file-EOF must be zero
        // padding -- the libarchive tar-block rounding tolerance.
        bool padding_ok = true;
        for (std::size_t j = comment_end; j < tail.size(); ++j) {
            if (tail[j] != 0) {
                padding_ok = false;
                break;
            }
        }
        if (!padding_ok) {
            continue;
        }

        const std::uint64_t eocd_file_offset = tail_start + i;

        // The central directory sits immediately before the EOCD.
        if (cd_size > eocd_file_offset) {
            continue;
        }
        const std::uint64_t cd_file_offset = eocd_file_offset - cd_size;

        if (cd_offset_arch > cd_file_offset) {
            continue;
        }
        const std::uint64_t bundle_start = cd_file_offset - cd_offset_arch;

        const std::uint64_t bundle_end = eocd_file_offset + kEocdRecordSize + comment_len;
        if (bundle_end < bundle_start) {
            continue;  // overflow paranoia
        }

        BundleLocation loc;
        loc.offset = bundle_start;
        loc.size = bundle_end - bundle_start;
        return loc;
    }

    return std::nullopt;
}

std::optional<BundleLocation> LocateBundleInSelf() {
    try {
        return LocateBundle(GetSelfPath());
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace flapi
