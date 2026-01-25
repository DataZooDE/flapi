#include "caching_file_provider.hpp"
#include <algorithm>
#include <crow/logging.h>

namespace flapi {

CachingFileProvider::CachingFileProvider(std::shared_ptr<IFileProvider> underlying,
                                           const FileCacheConfig& config)
    : _underlying(std::move(underlying)), _config(config) {
    if (!_underlying) {
        throw std::invalid_argument("CachingFileProvider requires a non-null underlying provider");
    }
    // // CROW_LOG_DEBUG << "CachingFileProvider created with TTL=" << _config.ttl.count()
    //                << "s, max_size=" << _config.max_size_bytes << " bytes";
}

bool CachingFileProvider::shouldCache(const std::string& path) const {
    // Only cache remote paths
    return _config.enabled && PathSchemeUtils::IsRemotePath(path);
}

bool CachingFileProvider::isExpired(const CacheEntry& entry) const {
    return std::chrono::steady_clock::now() >= entry.expires_at;
}

void CachingFileProvider::evictLRU(size_t needed_bytes) {
    // Collect entries sorted by last access time (oldest first)
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> entries;
    entries.reserve(_cache.size());

    for (const auto& [path, entry] : _cache) {
        entries.emplace_back(path, entry.last_access);
    }

    // Sort by last_access (oldest first)
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    size_t current_size = _stats.current_size_bytes.load();
    size_t target_size = _config.max_size_bytes > needed_bytes
                             ? _config.max_size_bytes - needed_bytes
                             : 0;

    for (const auto& [path, _] : entries) {
        if (current_size <= target_size) {
            break;
        }

        auto it = _cache.find(path);
        if (it != _cache.end()) {
            size_t entry_size = it->second.size_bytes;
            _cache.erase(it);
            current_size -= entry_size;
            _stats.evictions.fetch_add(1);
            _stats.current_entries.fetch_sub(1);
            _stats.current_size_bytes.fetch_sub(entry_size);
            // CROW_LOG_DEBUG << "Evicted cache entry: " << path << " (" << entry_size << " bytes)";
        }
    }
}

std::string CachingFileProvider::ReadFile(const std::string& path) {
    // Don't cache local files
    if (!shouldCache(path)) {
        return _underlying->ReadFile(path);
    }

    {
        std::lock_guard<std::mutex> lock(_cache_mutex);

        // Check cache
        auto it = _cache.find(path);
        if (it != _cache.end()) {
            if (!isExpired(it->second)) {
                // Cache hit
                it->second.last_access = std::chrono::steady_clock::now();
                _stats.hits.fetch_add(1);
                // CROW_LOG_DEBUG << "Cache hit: " << path;
                return it->second.content;
            }
            // Expired - remove from cache
            size_t entry_size = it->second.size_bytes;
            _cache.erase(it);
            _stats.current_entries.fetch_sub(1);
            _stats.current_size_bytes.fetch_sub(entry_size);
            // CROW_LOG_DEBUG << "Cache entry expired: " << path;
        }
    }

    // Cache miss - fetch from underlying
    _stats.misses.fetch_add(1);
    // CROW_LOG_DEBUG << "Cache miss: " << path;

    std::string content = _underlying->ReadFile(path);

    // Add to cache if within size limits
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);

        size_t content_size = content.size();

        // Check if this single entry exceeds max size
        if (content_size > _config.max_size_bytes) {
            // File too large to cache
            return content;
        }

        // Evict if necessary
        size_t current_size = _stats.current_size_bytes.load();
        if (current_size + content_size > _config.max_size_bytes) {
            evictLRU(content_size);
        }

        // Add to cache
        CacheEntry entry;
        entry.content = content;
        entry.expires_at = std::chrono::steady_clock::now() + _config.ttl;
        entry.last_access = std::chrono::steady_clock::now();
        entry.size_bytes = content_size;

        _cache[path] = std::move(entry);
        _stats.current_entries.fetch_add(1);
        _stats.current_size_bytes.fetch_add(content_size);
    }

    return content;
}

bool CachingFileProvider::FileExists(const std::string& path) {
    // FileExists is typically fast, so we don't cache this
    // This also ensures we get fresh existence checks
    return _underlying->FileExists(path);
}

std::vector<std::string> CachingFileProvider::ListFiles(const std::string& directory,
                                                          const std::string& pattern) {
    // Directory listings are not cached to ensure freshness
    return _underlying->ListFiles(directory, pattern);
}

bool CachingFileProvider::IsRemotePath(const std::string& path) const {
    return _underlying->IsRemotePath(path);
}

std::string CachingFileProvider::GetProviderName() const {
    return "caching(" + _underlying->GetProviderName() + ")";
}

bool CachingFileProvider::invalidate(const std::string& path) {
    std::lock_guard<std::mutex> lock(_cache_mutex);

    auto it = _cache.find(path);
    if (it != _cache.end()) {
        size_t entry_size = it->second.size_bytes;
        _cache.erase(it);
        _stats.current_entries.fetch_sub(1);
        _stats.current_size_bytes.fetch_sub(entry_size);
        // CROW_LOG_DEBUG << "Cache invalidated: " << path;
        return true;
    }
    return false;
}

void CachingFileProvider::clearCache() {
    std::lock_guard<std::mutex> lock(_cache_mutex);
    _cache.clear();
    _stats.current_entries.store(0);
    _stats.current_size_bytes.store(0);
    // CROW_LOG_DEBUG << "Cache cleared";
}

size_t CachingFileProvider::getCacheEntryCount() const {
    return _stats.current_entries.load();
}

size_t CachingFileProvider::getCacheSizeBytes() const {
    return _stats.current_size_bytes.load();
}

std::shared_ptr<CachingFileProvider> createCachingProvider(
    const std::string& path,
    const FileCacheConfig& config) {
    auto underlying = FileProviderFactory::CreateProvider(path);
    return std::make_shared<CachingFileProvider>(underlying, config);
}

} // namespace flapi
