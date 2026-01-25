#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <atomic>
#include "vfs_adapter.hpp"

namespace flapi {

/**
 * Cache statistics for monitoring.
 */
struct CacheStats {
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> evictions{0};
    std::atomic<uint64_t> current_size_bytes{0};
    std::atomic<uint64_t> current_entries{0};
};

/**
 * Configuration for the caching file provider.
 */
struct FileCacheConfig {
    bool enabled = true;
    std::chrono::seconds ttl{300};          // Default 5 minutes
    size_t max_size_bytes = 50UL * 1024UL * 1024UL;  // Default 50 MB
};

/**
 * Caching decorator for IFileProvider.
 * Implements TTL-based caching for remote files with LRU eviction.
 *
 * Design:
 * - Local files are NEVER cached (always fresh from disk)
 * - Remote files (s3://, gs://, az://, https://) are cached with TTL
 * - LRU eviction when max_size_bytes is exceeded
 * - Thread-safe for concurrent access
 *
 * Usage:
 *   auto underlying = FileProviderFactory::CreateDuckDBProvider();
 *   FileCacheConfig config;
 *   config.ttl = std::chrono::seconds(300);
 *   config.max_size_bytes = 50 * 1024 * 1024;
 *   auto cached = std::make_shared<CachingFileProvider>(underlying, config);
 *   std::string content = cached->ReadFile("s3://bucket/file.yaml");
 */
class CachingFileProvider : public IFileProvider {
public:
    /**
     * Create a caching decorator around an existing file provider.
     *
     * @param underlying The underlying file provider to cache
     * @param config Cache configuration (TTL, max size)
     */
    CachingFileProvider(std::shared_ptr<IFileProvider> underlying,
                         const FileCacheConfig& config = FileCacheConfig());

    ~CachingFileProvider() override = default;

    // IFileProvider interface
    std::string ReadFile(const std::string& path) override;
    bool FileExists(const std::string& path) override;
    std::vector<std::string> ListFiles(const std::string& directory,
                                        const std::string& pattern = "*") override;
    bool IsRemotePath(const std::string& path) const override;
    std::string GetProviderName() const override;

    // Cache management
    /**
     * Invalidate a specific cache entry.
     *
     * @param path Path to invalidate
     * @return true if entry was found and removed
     */
    bool invalidate(const std::string& path);

    /**
     * Clear entire cache.
     */
    void clearCache();

    /**
     * Get cache statistics.
     */
    const CacheStats& getStats() const { return _stats; }

    /**
     * Check if caching is enabled.
     */
    bool isCachingEnabled() const { return _config.enabled; }

    /**
     * Get current cache entry count.
     */
    size_t getCacheEntryCount() const;

    /**
     * Get current cache size in bytes.
     */
    size_t getCacheSizeBytes() const;

private:
    struct CacheEntry {
        std::string content;
        std::chrono::steady_clock::time_point expires_at;
        std::chrono::steady_clock::time_point last_access;
        size_t size_bytes;
    };

    std::shared_ptr<IFileProvider> _underlying;
    FileCacheConfig _config;
    mutable std::mutex _cache_mutex;
    std::unordered_map<std::string, CacheEntry> _cache;
    CacheStats _stats;

    /**
     * Check if a cache entry is expired.
     */
    bool isExpired(const CacheEntry& entry) const;

    /**
     * Evict entries to make room for new content.
     * Uses LRU (Least Recently Used) strategy.
     *
     * @param needed_bytes Space needed for new entry
     */
    void evictLRU(size_t needed_bytes);

    /**
     * Should this path be cached?
     * Only remote paths are cached.
     */
    bool shouldCache(const std::string& path) const;
};

/**
 * Factory method to create a caching provider.
 * Creates appropriate underlying provider based on path.
 */
std::shared_ptr<CachingFileProvider> createCachingProvider(
    const std::string& path,
    const FileCacheConfig& config = FileCacheConfig());

} // namespace flapi
