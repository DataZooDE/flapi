#include <catch2/catch_test_macros.hpp>
#include "archive_io.hpp"
#include "embedded_archive_file_provider.hpp"
#include "vfs_adapter.hpp"

#include <algorithm>
#include <memory>

using namespace flapi;

namespace {

std::shared_ptr<const ArchiveEntries> SampleEntries() {
    auto entries = std::make_shared<ArchiveEntries>();
    (*entries)["flapi.yaml"] =
        {'p', 'r', 'o', 'j', 'e', 'c', 't', '-', 'n', 'a', 'm', 'e'};
    (*entries)["sqls/customers.yaml"] = {'c', 'y'};
    (*entries)["sqls/customers.sql"] = {'S', 'E', 'L', 'E', 'C', 'T'};
    (*entries)["sqls/orders.yaml"] = {'o', 'y'};
    (*entries)["sqls/orders.sql"] = {'I', 'N', 'S', 'E', 'R', 'T'};
    (*entries)["data/cities.csv"] = {'B', 'e', 'r', 'l', 'i', 'n'};
    return entries;
}

// RAII guard so factory-dispatch tests can't leak global state.
struct BundleGuard {
    explicit BundleGuard(std::shared_ptr<const ArchiveEntries> e) {
        FileProviderFactory::SetBundleContents(std::move(e));
    }
    ~BundleGuard() { FileProviderFactory::SetBundleContents(nullptr); }
    BundleGuard(const BundleGuard&) = delete;
    BundleGuard& operator=(const BundleGuard&) = delete;
};

}  // namespace

TEST_CASE("EmbeddedArchiveFileProvider returns bytes for a known entry",
          "[embedded_provider]") {
    EmbeddedArchiveFileProvider provider(SampleEntries());
    REQUIRE(provider.ReadFile("sqls/customers.sql") == "SELECT");
    REQUIRE(provider.ReadFile("data/cities.csv") == "Berlin");
}

TEST_CASE("EmbeddedArchiveFileProvider normalises ./ and file:// prefixes",
          "[embedded_provider]") {
    EmbeddedArchiveFileProvider provider(SampleEntries());
    REQUIRE(provider.ReadFile("./flapi.yaml") == "project-name");
    REQUIRE(provider.ReadFile("file://sqls/customers.sql") == "SELECT");
    REQUIRE(provider.ReadFile("./sqls/orders.sql") == "INSERT");
}

TEST_CASE("EmbeddedArchiveFileProvider throws on a missing entry",
          "[embedded_provider]") {
    EmbeddedArchiveFileProvider provider(SampleEntries());
    REQUIRE_THROWS_AS(provider.ReadFile("sqls/missing.sql"), FileOperationError);
}

TEST_CASE("EmbeddedArchiveFileProvider::FileExists matches present and absent",
          "[embedded_provider]") {
    EmbeddedArchiveFileProvider provider(SampleEntries());
    REQUIRE(provider.FileExists("flapi.yaml"));
    REQUIRE(provider.FileExists("./flapi.yaml"));
    REQUIRE(provider.FileExists("sqls/customers.sql"));
    REQUIRE_FALSE(provider.FileExists("sqls/missing.sql"));
    REQUIRE_FALSE(provider.FileExists("nope/at/all.yaml"));
}

TEST_CASE("EmbeddedArchiveFileProvider::ListFiles applies a glob pattern",
          "[embedded_provider]") {
    EmbeddedArchiveFileProvider provider(SampleEntries());

    auto yamls = provider.ListFiles("sqls", "*.yaml");
    REQUIRE(yamls.size() == 2);
    REQUIRE(yamls[0] == "sqls/customers.yaml");
    REQUIRE(yamls[1] == "sqls/orders.yaml");

    auto sqls = provider.ListFiles("sqls", "*.sql");
    REQUIRE(sqls.size() == 2);
    REQUIRE(sqls[0] == "sqls/customers.sql");
    REQUIRE(sqls[1] == "sqls/orders.sql");

    auto all_in_sqls = provider.ListFiles("sqls", "*");
    REQUIRE(all_in_sqls.size() == 4);
}

TEST_CASE("EmbeddedArchiveFileProvider::ListFiles is non-recursive",
          "[embedded_provider]") {
    auto entries = std::make_shared<ArchiveEntries>();
    (*entries)["sqls/a.sql"] = {};
    (*entries)["sqls/sub/b.sql"] = {};
    (*entries)["sqls/sub/c.sql"] = {};

    EmbeddedArchiveFileProvider provider(entries);
    auto list = provider.ListFiles("sqls", "*.sql");

    REQUIRE(list.size() == 1);
    REQUIRE(list[0] == "sqls/a.sql");
}

TEST_CASE("EmbeddedArchiveFileProvider::IsRemotePath always returns false",
          "[embedded_provider]") {
    EmbeddedArchiveFileProvider provider(SampleEntries());
    REQUIRE_FALSE(provider.IsRemotePath("flapi.yaml"));
    REQUIRE_FALSE(provider.IsRemotePath("s3://bucket/key"));
}

TEST_CASE("EmbeddedArchiveFileProvider::GetProviderName is 'embedded'",
          "[embedded_provider]") {
    EmbeddedArchiveFileProvider provider(SampleEntries());
    REQUIRE(provider.GetProviderName() == "embedded");
}

TEST_CASE("FileProviderFactory picks EmbeddedArchiveFileProvider when a bundle is set",
          "[vfs][factory][embedded_provider]") {
    BundleGuard guard{SampleEntries()};

    auto provider = FileProviderFactory::CreateProvider("flapi.yaml");
    REQUIRE(provider != nullptr);
    REQUIRE(provider->GetProviderName() == "embedded");

    auto rel = FileProviderFactory::CreateProvider("sqls/customers.sql");
    REQUIRE(rel != nullptr);
    REQUIRE(rel->GetProviderName() == "embedded");
}

TEST_CASE("FileProviderFactory still picks the local provider when no bundle is set",
          "[vfs][factory][embedded_provider]") {
    // Defensive: clear any leakage from earlier tests.
    FileProviderFactory::SetBundleContents(nullptr);

    auto provider = FileProviderFactory::CreateProvider("/tmp/x.yaml");
    REQUIRE(provider != nullptr);
    REQUIRE(provider->GetProviderName() == "local");
}

TEST_CASE("FileProviderFactory routes remote paths to DuckDB even with a bundle",
          "[vfs][factory][embedded_provider]") {
    BundleGuard guard{SampleEntries()};

    // DuckDBVFSProvider needs DatabaseManager. If a previous test
    // initialised it, we should see "duckdb-vfs"; otherwise a
    // FileOperationError mentioning the database is acceptable.
    try {
        auto provider = FileProviderFactory::CreateProvider("s3://bucket/key");
        REQUIRE(provider != nullptr);
        REQUIRE(provider->GetProviderName() == "duckdb-vfs");
    } catch (const FileOperationError& e) {
        const std::string msg(e.what());
        const bool mentions_db =
            msg.find("Database") != std::string::npos ||
            msg.find("database") != std::string::npos ||
            msg.find("initialized") != std::string::npos;
        REQUIRE(mentions_db);
    }
}
