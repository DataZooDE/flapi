#include <catch2/catch_test_macros.hpp>
#include "archive_io.hpp"
#include "duckdb_embed_fs.hpp"

#include "duckdb.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace flapi;

namespace {

std::vector<std::uint8_t> AsBytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::shared_ptr<const ArchiveEntries> SampleEntries() {
    auto entries = std::make_shared<ArchiveEntries>();
    (*entries)["data/people.csv"] =
        AsBytes("name,age\nAlice,30\nBob,25\nCarol,40\n");
    (*entries)["data/cities.csv"] =
        AsBytes("city\nBerlin\nMunich\nStuttgart\n");
    (*entries)["other/notes.txt"] = AsBytes("hi");
    return entries;
}

}  // namespace

TEST_CASE("EmbeddedFileSystem::CanHandleFile recognises embed:// paths",
          "[embed_fs]") {
    EmbeddedFileSystem fs(SampleEntries());
    REQUIRE(fs.CanHandleFile("embed://data/people.csv"));
    REQUIRE_FALSE(fs.CanHandleFile("/data/people.csv"));
    REQUIRE_FALSE(fs.CanHandleFile("s3://bucket/key"));
    REQUIRE_FALSE(fs.CanHandleFile("data/people.csv"));
}

TEST_CASE("EmbeddedFileSystem::OpenFile returns a handle for known entries",
          "[embed_fs]") {
    EmbeddedFileSystem fs(SampleEntries());

    auto handle = fs.OpenFile("embed://data/cities.csv",
                              ::duckdb::FileOpenFlags::FILE_FLAGS_READ);
    REQUIRE(handle != nullptr);
    REQUIRE(fs.GetFileSize(*handle) ==
            static_cast<int64_t>(SampleEntries()->at("data/cities.csv").size()));
}

TEST_CASE("EmbeddedFileSystem::OpenFile throws for missing entries",
          "[embed_fs]") {
    EmbeddedFileSystem fs(SampleEntries());
    REQUIRE_THROWS(fs.OpenFile("embed://data/missing.csv",
                                ::duckdb::FileOpenFlags::FILE_FLAGS_READ));
}

TEST_CASE("EmbeddedFileSystem::Read returns the entry bytes and advances pos",
          "[embed_fs]") {
    EmbeddedFileSystem fs(SampleEntries());
    auto handle = fs.OpenFile("embed://data/cities.csv",
                              ::duckdb::FileOpenFlags::FILE_FLAGS_READ);
    REQUIRE(handle != nullptr);

    std::vector<char> buf(1024, 0);
    int64_t n = fs.Read(*handle, buf.data(), static_cast<int64_t>(buf.size()));
    REQUIRE(n > 0);

    const std::string content(buf.data(), static_cast<std::size_t>(n));
    REQUIRE(content.find("Berlin") != std::string::npos);
    REQUIRE(content.find("Munich") != std::string::npos);

    // After consuming the entire entry, further reads return zero.
    int64_t n2 = fs.Read(*handle, buf.data(), static_cast<int64_t>(buf.size()));
    REQUIRE(n2 == 0);
}

TEST_CASE("EmbeddedFileSystem positional Read does not move the cursor",
          "[embed_fs]") {
    EmbeddedFileSystem fs(SampleEntries());
    auto handle = fs.OpenFile("embed://data/cities.csv",
                              ::duckdb::FileOpenFlags::FILE_FLAGS_READ);
    REQUIRE(handle != nullptr);

    std::vector<char> buf(5, 0);
    fs.Read(*handle, buf.data(), 5, 5);  // skip "city\n"
    REQUIRE(std::string(buf.data(), 5) == "Berli");

    // Stream position should still be 0 (positional read doesn't move it).
    REQUIRE(fs.SeekPosition(*handle) == 0);

    // Streaming read starts at position 0.
    std::vector<char> buf2(4, 0);
    int64_t n = fs.Read(*handle, buf2.data(), 4);
    REQUIRE(n == 4);
    REQUIRE(std::string(buf2.data(), 4) == "city");
}

TEST_CASE("EmbeddedFileSystem::Seek and SeekPosition agree", "[embed_fs]") {
    EmbeddedFileSystem fs(SampleEntries());
    auto handle = fs.OpenFile("embed://data/people.csv",
                              ::duckdb::FileOpenFlags::FILE_FLAGS_READ);
    REQUIRE(handle != nullptr);

    fs.Seek(*handle, 9);
    REQUIRE(fs.SeekPosition(*handle) == 9);
}

TEST_CASE("EmbeddedFileSystem::Glob expands wildcards inside embed scheme",
          "[embed_fs]") {
    EmbeddedFileSystem fs(SampleEntries());
    auto matches = fs.Glob("embed://data/*.csv");
    REQUIRE(matches.size() == 2);

    std::vector<std::string> paths;
    paths.reserve(matches.size());
    for (auto& m : matches) {
        paths.push_back(m.path);
    }
    std::sort(paths.begin(), paths.end());

    REQUIRE(paths[0] == "embed://data/cities.csv");
    REQUIRE(paths[1] == "embed://data/people.csv");
}

TEST_CASE("EmbeddedFileSystem::Glob returns exact match for non-wildcard paths",
          "[embed_fs]") {
    EmbeddedFileSystem fs(SampleEntries());
    auto matches = fs.Glob("embed://data/cities.csv");
    REQUIRE(matches.size() == 1);
    REQUIRE(matches[0].path == "embed://data/cities.csv");

    auto none = fs.Glob("embed://data/missing.csv");
    REQUIRE(none.empty());
}

TEST_CASE("EmbeddedFileSystem proof-of-life: read_csv('embed://...') returns rows",
          "[embed_fs][duckdb_e2e]") {
    ::duckdb::DuckDB db(nullptr);  // in-memory

    auto& dbi = *db.instance;
    auto& vfs = ::duckdb::FileSystem::GetFileSystem(dbi);
    vfs.RegisterSubSystem(::duckdb::make_uniq<EmbeddedFileSystem>(SampleEntries()));

    ::duckdb::Connection conn(db);
    auto result = conn.Query(
        "SELECT name FROM read_csv('embed://data/people.csv') ORDER BY name");
    if (result->HasError()) {
        FAIL("read_csv failed: " << result->GetError());
    }

    REQUIRE(result->RowCount() == 3);
    REQUIRE(result->GetValue(0, 0).ToString() == "Alice");
    REQUIRE(result->GetValue(0, 1).ToString() == "Bob");
    REQUIRE(result->GetValue(0, 2).ToString() == "Carol");
}

TEST_CASE("EmbeddedFileSystem proof-of-life: read_csv with embed:// glob",
          "[embed_fs][duckdb_e2e]") {
    ::duckdb::DuckDB db(nullptr);

    auto& dbi = *db.instance;
    auto& vfs = ::duckdb::FileSystem::GetFileSystem(dbi);
    vfs.RegisterSubSystem(::duckdb::make_uniq<EmbeddedFileSystem>(SampleEntries()));

    ::duckdb::Connection conn(db);
    // Glob over both CSVs - schemas differ, so use union_by_name=true.
    auto result = conn.Query(
        "SELECT COUNT(*) FROM read_csv('embed://data/*.csv', union_by_name=true)");
    if (result->HasError()) {
        FAIL("read_csv glob failed: " << result->GetError());
    }
    REQUIRE(result->RowCount() == 1);
    // 3 rows in people.csv + 3 rows in cities.csv = 6.
    REQUIRE(result->GetValue(0, 0).GetValue<int64_t>() == 6);
}
