#include <catch2/catch_test_macros.hpp>
#include "archive_io.hpp"
#include "bundle_locator.hpp"
#include "pack.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace flapi;

namespace {

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("flapi_pack_test_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    std::filesystem::path WriteFile(const std::string& rel,
                                    const std::string& content) {
        auto full = path_ / rel;
        std::filesystem::create_directories(full.parent_path());
        std::ofstream f(full, std::ios::binary | std::ios::trunc);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        return full;
    }

private:
    std::filesystem::path path_;
};

class TempFilePath {
public:
    explicit TempFilePath(const std::string& suffix = ".bin") {
        path_ = std::filesystem::temp_directory_path() /
                ("flapi_pack_test_out_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)) + suffix);
    }
    ~TempFilePath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempFilePath(const TempFilePath&) = delete;
    TempFilePath& operator=(const TempFilePath&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::vector<std::uint8_t> ReadAllBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
}

void PopulateSampleTree(TempDir& dir) {
    dir.WriteFile("flapi.yaml", "project-name: bundled\nversion: 1\n");
    dir.WriteFile("sqls/customers.yaml", "url-path: /customers\n");
    dir.WriteFile("sqls/customers.sql", "SELECT * FROM customers");
    dir.WriteFile("data/cities.csv", "city\nBerlin\nMunich\n");
}

// A tiny fake "host binary": 4 KiB of pseudo-random bytes with any
// incidental EOCD signatures scrubbed so the locator can't mistake the
// host for a bundle. Stays the same across runs (fixed seed).
std::filesystem::path WriteFakeHost(TempDir& dir, std::uint32_t seed = 0xc0ffee) {
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> bytes(4096);
    for (auto& b : bytes) {
        b = static_cast<std::uint8_t>(rng() & 0xff);
    }
    for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] == 0x50 && bytes[i + 1] == 0x4b &&
            bytes[i + 2] == 0x05 && bytes[i + 3] == 0x06) {
            bytes[i] = 0x00;
        }
    }
    auto p = dir.path() / "fake_host.bin";
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return p;
}

}  // namespace

TEST_CASE("IsSecretExcluded matches the default deny list", "[pack][secrets]") {
    REQUIRE(IsSecretExcluded("config/.env"));
    REQUIRE(IsSecretExcluded(".env"));
    REQUIRE(IsSecretExcluded("secrets/api.token"));
    REQUIRE(IsSecretExcluded("nested/secrets/api.token"));
    REQUIRE(IsSecretExcluded("certs/server.pem"));
    REQUIRE(IsSecretExcluded("certs/key.key"));

    REQUIRE_FALSE(IsSecretExcluded("flapi.yaml"));
    REQUIRE_FALSE(IsSecretExcluded("sqls/customers.sql"));
    REQUIRE_FALSE(IsSecretExcluded("data/notsecrets/file.txt"));
    REQUIRE_FALSE(IsSecretExcluded("envoy.conf"));
    REQUIRE_FALSE(IsSecretExcluded("foo.pemmed"));
}

TEST_CASE("Pack produces a binary with an appended ZIP bundle", "[pack]") {
    TempDir host_dir;
    TempDir input;
    PopulateSampleTree(input);

    PackOptions opts;
    opts.host_binary_override = WriteFakeHost(host_dir);
    opts.source_date_epoch = 0;

    TempFilePath out;
    auto result = Pack(input.path(), out.path(), opts);

    REQUIRE(result.entry_count == 4);
    REQUIRE(std::filesystem::exists(out.path()));

    auto loc = LocateBundle(out.path());
    REQUIRE(loc.has_value());
    REQUIRE(loc->size == result.archive_size);
}

TEST_CASE("Pack bundle round-trips through ReadArchive", "[pack]") {
    TempDir host_dir;
    TempDir input;
    PopulateSampleTree(input);

    PackOptions opts;
    opts.host_binary_override = WriteFakeHost(host_dir);
    opts.source_date_epoch = 0;

    TempFilePath out;
    Pack(input.path(), out.path(), opts);

    auto loc = LocateBundle(out.path());
    REQUIRE(loc.has_value());

    auto bytes = ReadAllBytes(out.path());
    std::vector<std::uint8_t> zip(
        bytes.begin() + static_cast<std::ptrdiff_t>(loc->offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(loc->offset + loc->size));
    auto entries = ReadArchive(zip);

    REQUIRE(entries.size() == 4);
    REQUIRE(entries.count("flapi.yaml") == 1);
    REQUIRE(entries.count("sqls/customers.yaml") == 1);
    REQUIRE(entries.count("sqls/customers.sql") == 1);
    REQUIRE(entries.count("data/cities.csv") == 1);

    const auto& customers_sql = entries["sqls/customers.sql"];
    REQUIRE(std::string(customers_sql.begin(), customers_sql.end()) ==
            "SELECT * FROM customers");
}

TEST_CASE("Pack refuses files matching the secrets exclude list",
          "[pack][secrets]") {
    TempDir host_dir;
    TempDir input;
    PopulateSampleTree(input);
    input.WriteFile(".env", "DB_PASSWORD=hunter2\n");

    PackOptions opts;
    opts.host_binary_override = WriteFakeHost(host_dir);

    TempFilePath out;
    REQUIRE_THROWS_AS(Pack(input.path(), out.path(), opts), PackError);
}

TEST_CASE("Pack with --allow-secrets bundles secret files",
          "[pack][secrets]") {
    TempDir host_dir;
    TempDir input;
    PopulateSampleTree(input);
    input.WriteFile(".env", "DB_PASSWORD=hunter2\n");

    PackOptions opts;
    opts.host_binary_override = WriteFakeHost(host_dir);
    opts.source_date_epoch = 0;
    opts.allow_secrets = true;

    TempFilePath out;
    auto result = Pack(input.path(), out.path(), opts);
    REQUIRE(result.entry_count == 5);
}

TEST_CASE("Pack is idempotent: re-packing a bundled binary doesn't grow it",
          "[pack][idempotence]") {
    TempDir host_dir;
    TempDir input;
    PopulateSampleTree(input);

    PackOptions opts;
    opts.host_binary_override = WriteFakeHost(host_dir);
    opts.source_date_epoch = 0;

    TempFilePath first;
    Pack(input.path(), first.path(), opts);
    const auto size1 = std::filesystem::file_size(first.path());

    // Use the produced bundled binary as the host for a second pack.
    // The pack code should strip the trailing ZIP from the copy before
    // appending the new one, so the output size stays stable.
    PackOptions opts2 = opts;
    opts2.host_binary_override = first.path();

    TempFilePath second;
    Pack(input.path(), second.path(), opts2);
    const auto size2 = std::filesystem::file_size(second.path());

    REQUIRE(size1 == size2);

    // And a third round, just to be sure.
    PackOptions opts3 = opts2;
    opts3.host_binary_override = second.path();
    TempFilePath third;
    Pack(input.path(), third.path(), opts3);
    REQUIRE(std::filesystem::file_size(third.path()) == size1);
}

TEST_CASE("Pack output preserves the host binary bytes prefix",
          "[pack][host_prefix]") {
    TempDir host_dir;
    auto host = WriteFakeHost(host_dir);

    TempDir input;
    PopulateSampleTree(input);

    PackOptions opts;
    opts.host_binary_override = host;
    opts.source_date_epoch = 0;

    TempFilePath out;
    Pack(input.path(), out.path(), opts);

    auto host_bytes = ReadAllBytes(host);
    auto out_bytes = ReadAllBytes(out.path());

    REQUIRE(out_bytes.size() > host_bytes.size());
    for (std::size_t i = 0; i < host_bytes.size(); ++i) {
        if (out_bytes[i] != host_bytes[i]) {
            FAIL("host prefix diverges at byte " << i);
        }
    }
}

TEST_CASE("PrintBundleInfo reports 'none' when no bundle is present",
          "[pack][info]") {
    std::ostringstream os;
    int rc = PrintBundleInfo(os);
    REQUIRE(rc != 0);
    const std::string out = os.str();
    REQUIRE(out.find("none") != std::string::npos);
}

TEST_CASE("PrintBundleInfoForPath lists entries from a bundled binary",
          "[pack][info]") {
    TempDir host_dir;
    TempDir input;
    PopulateSampleTree(input);

    PackOptions opts;
    opts.host_binary_override = WriteFakeHost(host_dir);
    opts.source_date_epoch = 0;

    TempFilePath out;
    Pack(input.path(), out.path(), opts);

    std::ostringstream os;
    int rc = PrintBundleInfoForPath(out.path(), os);
    REQUIRE(rc == 0);
    const std::string text = os.str();
    REQUIRE(text.find("flapi.yaml") != std::string::npos);
    REQUIRE(text.find("sqls/customers.sql") != std::string::npos);
    REQUIRE(text.find("data/cities.csv") != std::string::npos);
}

TEST_CASE("UnpackBundleFromPath restores all entries to dst",
          "[pack][unpack]") {
    TempDir host_dir;
    TempDir input;
    PopulateSampleTree(input);

    PackOptions opts;
    opts.host_binary_override = WriteFakeHost(host_dir);
    opts.source_date_epoch = 0;

    TempFilePath out;
    Pack(input.path(), out.path(), opts);

    TempDir dst;
    auto r = UnpackBundleFromPath(out.path(), dst.path());
    REQUIRE(r.entries_written == 4);

    REQUIRE(std::filesystem::exists(dst.path() / "flapi.yaml"));
    REQUIRE(std::filesystem::exists(dst.path() / "sqls/customers.sql"));
    REQUIRE(std::filesystem::exists(dst.path() / "data/cities.csv"));

    std::ifstream src(input.path() / "sqls/customers.sql", std::ios::binary);
    std::ifstream restored(dst.path() / "sqls/customers.sql", std::ios::binary);
    std::string a((std::istreambuf_iterator<char>(src)),
                  std::istreambuf_iterator<char>());
    std::string b((std::istreambuf_iterator<char>(restored)),
                  std::istreambuf_iterator<char>());
    REQUIRE(a == b);
}
