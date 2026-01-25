#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "caching_file_provider.hpp"
#include <filesystem>
#include <fstream>
#include <thread>

using namespace flapi;

// Mock file provider for testing caching behavior
class CacheMockFileProvider : public IFileProvider {
public:
    mutable int read_count = 0;
    mutable int exists_count = 0;
    mutable int list_count = 0;
    std::string content_to_return = "mock content";
    bool exists_result = true;
    std::vector<std::string> list_result;
    bool throw_on_read = false;

    std::string ReadFile(const std::string& /* path */) override {
        read_count++;
        if (throw_on_read) {
            throw FileOperationError("Mock read error");
        }
        return content_to_return;
    }

    bool FileExists(const std::string& /* path */) override {
        exists_count++;
        return exists_result;
    }

    std::vector<std::string> ListFiles(const std::string& /* directory */,
                                        const std::string& /* pattern */) override {
        list_count++;
        return list_result;
    }

    bool IsRemotePath(const std::string& path) const override {
        return PathSchemeUtils::IsRemotePath(path);
    }

    std::string GetProviderName() const override {
        return "mock";
    }
};

// Helper to create temporary test files
class TempTestFile {
public:
    explicit TempTestFile(const std::string& content = "") {
        path_ = std::filesystem::temp_directory_path() /
                ("vfs_cache_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".txt");
        std::ofstream file(path_);
        file << content;
    }

    ~TempTestFile() {
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
        }
    }

    std::filesystem::path path() const { return path_; }
    std::string pathString() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

// ============================================================================
// CachingFileProvider Basic Tests
// ============================================================================

TEST_CASE("CachingFileProvider construction", "[vfs][cache]") {
    SECTION("Constructor requires non-null underlying provider") {
        REQUIRE_THROWS_AS(
            CachingFileProvider(nullptr),
            std::invalid_argument
        );
    }

    SECTION("Constructor with valid provider succeeds") {
        auto mock = std::make_shared<CacheMockFileProvider>();
        FileCacheConfig config;
        config.ttl = std::chrono::seconds(60);

        REQUIRE_NOTHROW(CachingFileProvider(mock, config));
    }

    SECTION("Provider name includes underlying provider") {
        auto mock = std::make_shared<CacheMockFileProvider>();
        CachingFileProvider cached(mock);

        REQUIRE(cached.GetProviderName() == "caching(mock)");
    }
}

// ============================================================================
// Cache Hit/Miss Tests
// ============================================================================

TEST_CASE("CachingFileProvider cache behavior", "[vfs][cache]") {
    auto mock = std::make_shared<CacheMockFileProvider>();
    mock->content_to_return = "cached content";

    FileCacheConfig config;
    config.enabled = true;
    config.ttl = std::chrono::seconds(60);
    config.max_size_bytes = 1024 * 1024;

    CachingFileProvider cached(mock, config);

    SECTION("Local files are NOT cached") {
        TempTestFile temp_file("local content");

        // First read - should go to underlying (local file provider)
        // Note: We're using mock which always returns mock content
        // The key point is that local paths should NOT be cached
        std::string result = cached.ReadFile(temp_file.pathString());
        REQUIRE(mock->read_count == 1);

        // Second read - should ALSO go to underlying (no caching for local)
        result = cached.ReadFile(temp_file.pathString());
        REQUIRE(mock->read_count == 2);

        // No cache entries for local files
        REQUIRE(cached.getCacheEntryCount() == 0);
    }

    SECTION("Remote files are cached") {
        std::string remote_path = "s3://bucket/key/file.yaml";

        // First read - cache miss
        std::string result1 = cached.ReadFile(remote_path);
        REQUIRE(mock->read_count == 1);
        REQUIRE(result1 == "cached content");
        REQUIRE(cached.getStats().misses.load() == 1);

        // Second read - cache hit
        std::string result2 = cached.ReadFile(remote_path);
        REQUIRE(mock->read_count == 1);  // Still 1, served from cache
        REQUIRE(result2 == "cached content");
        REQUIRE(cached.getStats().hits.load() == 1);

        // Cache should have 1 entry
        REQUIRE(cached.getCacheEntryCount() == 1);
    }

    SECTION("Different remote paths are cached separately") {
        std::string path1 = "s3://bucket/file1.yaml";
        std::string path2 = "s3://bucket/file2.yaml";

        cached.ReadFile(path1);
        cached.ReadFile(path2);

        REQUIRE(mock->read_count == 2);
        REQUIRE(cached.getCacheEntryCount() == 2);

        // Read again - both should hit cache
        cached.ReadFile(path1);
        cached.ReadFile(path2);

        REQUIRE(mock->read_count == 2);  // No additional reads
        REQUIRE(cached.getStats().hits.load() == 2);
    }
}

// ============================================================================
// TTL Expiration Tests
// ============================================================================

TEST_CASE("CachingFileProvider TTL expiration", "[vfs][cache]") {
    auto mock = std::make_shared<CacheMockFileProvider>();
    mock->content_to_return = "content v1";

    FileCacheConfig config;
    config.enabled = true;
    config.ttl = std::chrono::seconds(1);  // Short TTL for testing
    config.max_size_bytes = 1024 * 1024;

    CachingFileProvider cached(mock, config);

    SECTION("Expired entries are refetched") {
        std::string path = "s3://bucket/file.yaml";

        // First read - cache miss
        cached.ReadFile(path);
        REQUIRE(mock->read_count == 1);

        // Read again immediately - cache hit
        cached.ReadFile(path);
        REQUIRE(mock->read_count == 1);

        // Wait for TTL to expire
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        // Update mock content
        mock->content_to_return = "content v2";

        // Read after expiration - should refetch
        std::string result = cached.ReadFile(path);
        REQUIRE(mock->read_count == 2);
        REQUIRE(result == "content v2");
    }
}

// ============================================================================
// Cache Size Limit Tests
// ============================================================================

TEST_CASE("CachingFileProvider size limits", "[vfs][cache]") {
    auto mock = std::make_shared<CacheMockFileProvider>();

    FileCacheConfig config;
    config.enabled = true;
    config.ttl = std::chrono::seconds(300);
    config.max_size_bytes = 100;  // Very small limit for testing

    CachingFileProvider cached(mock, config);

    SECTION("LRU eviction when max size exceeded") {
        mock->content_to_return = std::string(40, 'a');  // 40 bytes

        // Add first entry
        cached.ReadFile("s3://bucket/file1.yaml");
        REQUIRE(cached.getCacheEntryCount() == 1);

        // Add second entry
        cached.ReadFile("s3://bucket/file2.yaml");
        REQUIRE(cached.getCacheEntryCount() == 2);

        // Add third entry - should trigger eviction
        cached.ReadFile("s3://bucket/file3.yaml");

        // Cache should have evicted at least one entry
        REQUIRE(cached.getCacheSizeBytes() <= 100);
        REQUIRE(cached.getStats().evictions.load() > 0);
    }

    SECTION("Single file exceeding max size is not cached") {
        mock->content_to_return = std::string(200, 'x');  // 200 bytes > 100 max

        cached.ReadFile("s3://bucket/large.yaml");

        // File should not be cached (too large)
        REQUIRE(cached.getCacheEntryCount() == 0);
    }
}

// ============================================================================
// Cache Invalidation Tests
// ============================================================================

TEST_CASE("CachingFileProvider cache invalidation", "[vfs][cache]") {
    auto mock = std::make_shared<CacheMockFileProvider>();
    mock->content_to_return = "content";

    FileCacheConfig config;
    config.enabled = true;
    config.ttl = std::chrono::seconds(300);
    config.max_size_bytes = 1024 * 1024;

    CachingFileProvider cached(mock, config);

    SECTION("invalidate removes specific entry") {
        std::string path1 = "s3://bucket/file1.yaml";
        std::string path2 = "s3://bucket/file2.yaml";

        cached.ReadFile(path1);
        cached.ReadFile(path2);
        REQUIRE(cached.getCacheEntryCount() == 2);

        // Invalidate first entry
        bool removed = cached.invalidate(path1);
        REQUIRE(removed == true);
        REQUIRE(cached.getCacheEntryCount() == 1);

        // Reading path1 should miss
        cached.ReadFile(path1);
        REQUIRE(mock->read_count == 3);  // 2 initial + 1 re-read

        // Invalidating non-existent entry returns false
        REQUIRE(cached.invalidate("s3://bucket/nonexistent.yaml") == false);
    }

    SECTION("clearCache removes all entries") {
        cached.ReadFile("s3://bucket/file1.yaml");
        cached.ReadFile("s3://bucket/file2.yaml");
        cached.ReadFile("s3://bucket/file3.yaml");
        REQUIRE(cached.getCacheEntryCount() == 3);

        cached.clearCache();

        REQUIRE(cached.getCacheEntryCount() == 0);
        REQUIRE(cached.getCacheSizeBytes() == 0);
    }
}

// ============================================================================
// Cache Stats Tests
// ============================================================================

TEST_CASE("CachingFileProvider statistics", "[vfs][cache]") {
    auto mock = std::make_shared<CacheMockFileProvider>();
    mock->content_to_return = "content";

    FileCacheConfig config;
    config.enabled = true;
    config.ttl = std::chrono::seconds(300);
    config.max_size_bytes = 1024 * 1024;

    CachingFileProvider cached(mock, config);

    SECTION("Stats track hits and misses") {
        std::string path = "s3://bucket/file.yaml";

        // Initial state
        REQUIRE(cached.getStats().hits.load() == 0);
        REQUIRE(cached.getStats().misses.load() == 0);

        // First read - miss
        cached.ReadFile(path);
        REQUIRE(cached.getStats().misses.load() == 1);
        REQUIRE(cached.getStats().hits.load() == 0);

        // Second read - hit
        cached.ReadFile(path);
        REQUIRE(cached.getStats().misses.load() == 1);
        REQUIRE(cached.getStats().hits.load() == 1);

        // Third read - hit
        cached.ReadFile(path);
        REQUIRE(cached.getStats().hits.load() == 2);
    }

    SECTION("Stats track size correctly") {
        mock->content_to_return = "12345";  // 5 bytes

        cached.ReadFile("s3://bucket/file.yaml");

        REQUIRE(cached.getStats().current_entries.load() == 1);
        REQUIRE(cached.getStats().current_size_bytes.load() == 5);
    }
}

// ============================================================================
// Disabled Cache Tests
// ============================================================================

TEST_CASE("CachingFileProvider with caching disabled", "[vfs][cache]") {
    auto mock = std::make_shared<CacheMockFileProvider>();
    mock->content_to_return = "content";

    FileCacheConfig config;
    config.enabled = false;  // Disabled
    config.ttl = std::chrono::seconds(300);

    CachingFileProvider cached(mock, config);

    SECTION("All reads go to underlying when disabled") {
        std::string path = "s3://bucket/file.yaml";

        cached.ReadFile(path);
        cached.ReadFile(path);
        cached.ReadFile(path);

        // All reads should go to underlying
        REQUIRE(mock->read_count == 3);
        REQUIRE(cached.getCacheEntryCount() == 0);
    }

    SECTION("isCachingEnabled returns false") {
        REQUIRE(cached.isCachingEnabled() == false);
    }
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_CASE("CachingFileProvider error handling", "[vfs][cache]") {
    auto mock = std::make_shared<CacheMockFileProvider>();

    FileCacheConfig config;
    config.enabled = true;
    config.ttl = std::chrono::seconds(300);
    config.max_size_bytes = 1024 * 1024;

    CachingFileProvider cached(mock, config);

    SECTION("Errors from underlying provider propagate") {
        mock->throw_on_read = true;

        REQUIRE_THROWS_AS(
            cached.ReadFile("s3://bucket/file.yaml"),
            FileOperationError
        );

        // Nothing should be cached on error
        REQUIRE(cached.getCacheEntryCount() == 0);
    }
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST_CASE("CachingFileProvider thread safety", "[vfs][cache]") {
    auto mock = std::make_shared<CacheMockFileProvider>();
    mock->content_to_return = "concurrent content";

    FileCacheConfig config;
    config.enabled = true;
    config.ttl = std::chrono::seconds(300);
    config.max_size_bytes = 1024 * 1024;

    auto cached = std::make_shared<CachingFileProvider>(mock, config);

    SECTION("Concurrent reads from same path") {
        const int num_threads = 10;
        const int reads_per_thread = 100;
        std::vector<std::thread> threads;

        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([cached, reads_per_thread]() {
                for (int j = 0; j < reads_per_thread; ++j) {
                    auto content = cached->ReadFile("s3://bucket/shared.yaml");
                    REQUIRE(content == "concurrent content");
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        // Should have exactly 1 cache entry
        REQUIRE(cached->getCacheEntryCount() == 1);

        // Total operations should equal num_threads * reads_per_thread
        auto total_ops = cached->getStats().hits.load() + cached->getStats().misses.load();
        REQUIRE(total_ops == num_threads * reads_per_thread);
    }
}
