#include <catch2/catch_test_macros.hpp>
#include "archive_io.hpp"
#include "bundle_locator.hpp"
#include "selfpath.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using namespace flapi;

namespace {

class TempBinary {
public:
    explicit TempBinary(const std::vector<std::uint8_t>& bytes) {
        path_ = std::filesystem::temp_directory_path() /
                ("bundle_locator_test_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".bin");
        std::ofstream f(path_, std::ios::binary);
        if (!bytes.empty()) {
            f.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        }
    }
    ~TempBinary() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempBinary(const TempBinary&) = delete;
    TempBinary& operator=(const TempBinary&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> RandomBytes(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> bytes(n);
    for (auto& b : bytes) {
        b = static_cast<std::uint8_t>(rng() & 0xff);
    }
    return bytes;
}

std::vector<std::uint8_t> SampleZip() {
    ArchiveEntries entries;
    entries["flapi.yaml"] = {'p', 'r', 'o', 'j', 'e', 'c', 't'};
    entries["sqls/customers.sql"] = {'S', 'E', 'L', 'E', 'C', 'T'};
    entries["data/cities.csv"] = {'B', 'e', 'r', 'l', 'i', 'n'};

    ArchiveWriteOptions opts;
    opts.source_date_epoch = 1'700'000'000;
    return WriteArchive(entries, opts);
}

void Append(std::vector<std::uint8_t>& dst, const std::vector<std::uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// Removes any incidental occurrence of the EOCD signature in random bytes so
// the "no bundle" test isn't undermined by a 1-in-4-billion lottery win.
void ScrubEocdSignature(std::vector<std::uint8_t>& bytes) {
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] == 0x50 && bytes[i + 1] == 0x4b &&
            bytes[i + 2] == 0x05 && bytes[i + 3] == 0x06) {
            bytes[i] = 0x00;
        }
    }
}

}  // namespace

TEST_CASE("bundle_locator finds ZIP appended to random leading bytes", "[bundle_locator]") {
    auto leading = RandomBytes(4096, 0xc0ffee01);
    ScrubEocdSignature(leading);
    auto zip = SampleZip();

    std::vector<std::uint8_t> file_bytes = leading;
    Append(file_bytes, zip);

    TempBinary tb{file_bytes};
    auto loc = LocateBundle(tb.path());

    REQUIRE(loc.has_value());
    REQUIRE(loc->offset == leading.size());
    REQUIRE(loc->size == zip.size());
}

TEST_CASE("bundle_locator tolerates 10 KiB of trailing zero padding", "[bundle_locator]") {
    auto leading = RandomBytes(2048, 0xbadcafe);
    ScrubEocdSignature(leading);
    auto zip = SampleZip();

    std::vector<std::uint8_t> file_bytes = leading;
    Append(file_bytes, zip);
    file_bytes.insert(file_bytes.end(), 10240, std::uint8_t{0});

    TempBinary tb{file_bytes};
    auto loc = LocateBundle(tb.path());

    REQUIRE(loc.has_value());
    REQUIRE(loc->offset == leading.size());
    REQUIRE(loc->size == zip.size());
}

TEST_CASE("bundle_locator returns nullopt when no EOCD signature is present",
          "[bundle_locator]") {
    auto bytes = RandomBytes(8192, 0xdecafbad);
    ScrubEocdSignature(bytes);

    TempBinary tb{bytes};
    auto loc = LocateBundle(tb.path());

    REQUIRE_FALSE(loc.has_value());
}

TEST_CASE("bundle_locator rejects an EOCD with implausible fields", "[bundle_locator]") {
    // 2 KiB of filler with a fake EOCD signature at the very end whose
    // central-directory size/offset can't possibly fit in the file.
    std::vector<std::uint8_t> bytes(2048, std::uint8_t{0xab});
    const std::size_t eocd_off = bytes.size() - 22;

    auto put_u16 = [&](std::size_t off, std::uint16_t v) {
        bytes[off] = static_cast<std::uint8_t>(v & 0xff);
        bytes[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    };
    auto put_u32 = [&](std::size_t off, std::uint32_t v) {
        bytes[off]     = static_cast<std::uint8_t>(v & 0xff);
        bytes[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xff);
        bytes[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xff);
        bytes[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xff);
    };

    put_u32(eocd_off + 0, 0x06054b50);   // signature
    put_u16(eocd_off + 4, 0);             // disk number
    put_u16(eocd_off + 6, 0);             // disk where CD starts
    put_u16(eocd_off + 8, 1);             // entries on this disk
    put_u16(eocd_off + 10, 1);            // total entries
    put_u32(eocd_off + 12, 0xffffffffu);  // central directory size
    put_u32(eocd_off + 16, 0xffffffffu);  // central directory offset
    put_u16(eocd_off + 20, 0);            // comment length

    TempBinary tb{bytes};
    auto loc = LocateBundle(tb.path());
    REQUIRE_FALSE(loc.has_value());
}

TEST_CASE("bundle_locator returns nullopt for a truncated bundle", "[bundle_locator]") {
    auto leading = RandomBytes(2048, 0xfeedbeef);
    ScrubEocdSignature(leading);
    auto zip = SampleZip();

    std::vector<std::uint8_t> file_bytes = leading;
    Append(file_bytes, zip);

    REQUIRE(file_bytes.size() > 1024);
    file_bytes.resize(file_bytes.size() - 1024);  // chop EOCD off the tail

    TempBinary tb{file_bytes};
    auto loc = LocateBundle(tb.path());
    REQUIRE_FALSE(loc.has_value());
}

TEST_CASE("selfpath returns an existing executable path", "[selfpath]") {
    auto path = GetSelfPath();

    REQUIRE_FALSE(path.empty());
    REQUIRE(std::filesystem::exists(path));
}

TEST_CASE("LocateBundleInSelf returns nullopt when no bundle is present",
          "[bundle_locator]") {
    // The test binary itself has no appended ZIP, so this should be nullopt.
    auto loc = LocateBundleInSelf();
    REQUIRE_FALSE(loc.has_value());
}
