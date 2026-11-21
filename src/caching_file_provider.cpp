#include "caching_file_provider.hpp"
#include <optional>
#include <algorithm>
#include <crow/logging.h>
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

    using Content = std::shared_ptr<const std::string>;
    Content served;                                    // answered from the cache (fresh, or stale while refreshing)
    Content stale;                                     // the expired copy the refreshing reader falls back to
    bool refresher = false;                            // this reader re-fetches an expired entry
    std::shared_future<Content> follow;                // a cold miss another reader is already fetching
    std::shared_ptr<std::promise<Content>> lead;       // this reader fetches a cold miss for everyone

    {
        std::lock_guard<std::mutex> lock(_cache_mutex);
        const auto it = _cache.find(path);
        if (it != _cache.end()) {
            if (!isExpired(it->second)) {
                it->second.last_access = std::chrono::steady_clock::now();
                _stats.hits.fetch_add(1);
                served = it->second.content;
            } else if (it->second.refreshing) {
                // Somebody is already re-fetching it: serve the expired copy instead of piling more
                // reads onto the store (or blocking for as long as a slow store takes).
                _stats.hits.fetch_add(1);
                served = it->second.content;
            } else {
                it->second.refreshing = true;
                stale = it->second.content;
                refresher = true;
            }
        } else if (const auto flight = _inflight.find(path); flight != _inflight.end()) {
            _stats.hits.fetch_add(1);  // shares the leader's fetch: no extra store read
            follow = flight->second;
        } else {
            lead = std::make_shared<std::promise<Content>>();
            _inflight.emplace(path, lead->get_future().share());
        }
    }

    if (served) {
        return *served;  // copied outside the lock
    }
    if (follow.valid()) {
        return *follow.get();  // rethrows the leader's error
    }

    _stats.misses.fetch_add(1);
    std::string content;
    try {
        content = _underlying->ReadFile(path);
    } catch (...) {
        const auto failure = std::current_exception();
        if (refresher) {
            // A transient store error (an expired STS token, a 5xx, a blip) must not turn requests into
            // 500s the moment the TTL passes: keep serving the last good copy and retry in a few seconds.
            CROW_LOG_WARNING << "Remote read of " << PathSchemeUtils::RedactUrl(path)
                             << " failed, serving the cached copy past its TTL";
            std::lock_guard<std::mutex> lock(_cache_mutex);
            if (const auto it = _cache.find(path); it != _cache.end()) {
                it->second.refreshing = false;
                it->second.expires_at = std::chrono::steady_clock::now() +
                                        std::min<std::chrono::seconds>(_config.ttl, std::chrono::seconds(15));
            }
            return *stale;
        }
        {
            std::lock_guard<std::mutex> lock(_cache_mutex);
            _inflight.erase(path);
        }
        lead->set_exception(failure);
        std::rethrow_exception(failure);
    }

    auto fresh = std::make_shared<const std::string>(std::move(content));
    {
        std::lock_guard<std::mutex> lock(_cache_mutex);
        // Replace whatever expired entry was there (its bytes leave the accounting first).
        if (const auto old = _cache.find(path); old != _cache.end()) {
            _stats.current_size_bytes.fetch_sub(old->second.size_bytes);
            _stats.current_entries.fetch_sub(1);
            _cache.erase(old);
        }
        const size_t content_size = fresh->size();
        if (content_size <= _config.max_size_bytes) {
            if (_stats.current_size_bytes.load() + content_size > _config.max_size_bytes) {
                evictLRU(content_size);
            }
            CacheEntry entry;
            entry.content = fresh;
            entry.size_bytes = content_size;
            entry.expires_at = std::chrono::steady_clock::now() + _config.ttl;
            entry.last_access = std::chrono::steady_clock::now();
            _cache.emplace(path, std::move(entry));
            _stats.current_entries.fetch_add(1);
            _stats.current_size_bytes.fetch_add(content_size);
        }
        if (lead) {
            _inflight.erase(path);
        }
    }
    if (lead) {
        lead->set_value(fresh);
    }
    return *fresh;
}

bool CachingFileProvider::FileExists(const std::string& path) {
    // A file whose content is cached and not yet expired exists as far as the TTL is concerned:
    // without this every render still sent a HEAD to the store, and `storage.cache` saved nothing.
    // Absence is never cached, so a file that appears later is found at once.
    if (shouldCache(path)) {
        std::lock_guard<std::mutex> lock(_cache_mutex);
        const auto it = _cache.find(path);
        if (it != _cache.end() && !isExpired(it->second)) {
            return true;
        }
    }
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
