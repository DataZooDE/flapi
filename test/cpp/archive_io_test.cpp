#include <catch2/catch_test_macros.hpp>
#include "archive_io.hpp"

#include <cstdint>
#include <vector>

using namespace flapi;

namespace {

ArchiveEntries SampleEntries() {
    ArchiveEntries entries;
    entries["flapi.yaml"] = {'p', 'r', 'o', 'j', 'e', 'c', 't', '-', 'n', 'a', 'm', 'e', ':', ' ', 'x'};
    entries["sqls/customers.sql"] = {'S', 'E', 'L', 'E', 'C', 'T', ' ', '1'};
    entries["data/cities.csv"] = {'c', 'i', 't', 'y', '\n', 'B', 'e', 'r', 'l', 'i', 'n'};
    return entries;
}

}  // namespace

TEST_CASE("archive_io round-trip preserves entry contents", "[archive_io]") {
    auto input = SampleEntries();

    auto bytes = WriteArchive(input);
    REQUIRE_FALSE(bytes.empty());

    auto roundtripped = ReadArchive(bytes);
    REQUIRE(roundtripped == input);
}

TEST_CASE("archive_io is reproducible with a fixed source-date-epoch", "[archive_io]") {
    auto input = SampleEntries();

    ArchiveWriteOptions opts;
    opts.source_date_epoch = 1'700'000'000;

    auto a = WriteArchive(input, opts);
    auto b = WriteArchive(input, opts);

    REQUIRE_FALSE(a.empty());
    REQUIRE(a == b);
}

TEST_CASE("archive_io emits entries in deterministic (sorted) order", "[archive_io]") {
    // Two maps with the same logical contents but different construction
    // orders. std::map already sorts, so wire order is map-iteration order.
    // We assert that's stable -- the contract callers will rely on.
    ArchiveEntries first;
    first["zeta"] = {1};
    first["alpha"] = {2};
    first["mike"] = {3};

    ArchiveEntries second;
    second["alpha"] = {2};
    second["mike"] = {3};
    second["zeta"] = {1};

    ArchiveWriteOptions opts;
    opts.source_date_epoch = 0;

    auto a = WriteArchive(first, opts);
    auto b = WriteArchive(second, opts);

    REQUIRE_FALSE(a.empty());
    REQUIRE(a == b);
}

TEST_CASE("archive_io rejects malformed input without crashing", "[archive_io]") {
    SECTION("empty buffer throws ArchiveIOError") {
        std::vector<uint8_t> empty;
        REQUIRE_THROWS_AS(ReadArchive(empty), ArchiveIOError);
    }

    SECTION("random garbage throws ArchiveIOError") {
        std::vector<uint8_t> garbage(256);
        for (size_t i = 0; i < garbage.size(); ++i) {
            garbage[i] = static_cast<uint8_t>((i * 37 + 11) & 0xff);
        }
        REQUIRE_THROWS_AS(ReadArchive(garbage), ArchiveIOError);
    }

    SECTION("truncated valid zip throws ArchiveIOError") {
        auto bytes = WriteArchive(SampleEntries());
        REQUIRE(bytes.size() > 16);
        bytes.resize(bytes.size() / 2);
        REQUIRE_THROWS_AS(ReadArchive(bytes), ArchiveIOError);
    }
}

TEST_CASE("archive_io has no trailing tar-block padding", "[archive_io]") {
    // The spike caught libarchive's default 10240-byte tar-block rounding
    // confusing the EOCD scan. The contract here: the last 4 bytes of a
    // freshly written archive contain either the EOCD signature or, at
    // minimum, no extraneous trailing zero padding that pushes the EOCD
    // record more than 22 bytes from EOF on a comment-less archive.
    auto bytes = WriteArchive(SampleEntries());
    REQUIRE(bytes.size() >= 22);

    // Locate the EOCD signature (0x06054b50, little-endian) by reverse scan.
    // For a comment-less archive, it should sit exactly 22 bytes from EOF.
    const std::uint32_t kEocdSig = 0x06054b50;
    auto eocd_offset_from_end = [&bytes]() -> size_t {
        for (size_t back = 22; back <= bytes.size() && back < 22 + 65536; ++back) {
            size_t i = bytes.size() - back;
            std::uint32_t v = std::uint32_t(bytes[i])
                            | (std::uint32_t(bytes[i + 1]) << 8)
                            | (std::uint32_t(bytes[i + 2]) << 16)
                            | (std::uint32_t(bytes[i + 3]) << 24);
            if (v == kEocdSig) return back;
        }
        return 0;
    }();

    REQUIRE(eocd_offset_from_end == 22);
}
