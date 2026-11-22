#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace flapi {

// total_count for paginated reads of DuckLake-cached endpoints (#242).
//
// The COUNT(*) behind a full page costs about as much as the page itself on a
// DuckLake table, and on a cached endpoint it cannot change until the DuckLake
// catalog does. So the count is kept, keyed by the rendered SQL plus the bound
// values, and tagged with the catalog's GENERATION: its newest snapshot id,
// read from the DuckLake metadata database just before the count would run.
// Every committed DuckLake change - a refresh, a full rebuild, a MERGE, a
// write endpoint, retention, a writer in another process - makes a new
// snapshot, so an entry is served only while the catalog is exactly as it was
// when the entry was counted.
//
// Only a query that provably reads nothing but DuckLake tables is eligible
// (CountableQuery): a single SELECT whose every table reference is
// <ducklake alias>.<schema>.<table> (or a CTE of the query), with no table
// function and no function DuckDB does not report as CONSISTENT. Time travel
// (`AT (VERSION => ...)`) is keyed by its rendered text and bound values like
// any other part of the query.
class CountCache {
public:
    static constexpr std::size_t kDefaultCapacity = 10000;

    explicit CountCache(std::size_t capacity = kDefaultCapacity);

    std::optional<std::int64_t> find(const std::string& key, std::int64_t generation);
    void store(const std::string& key, std::int64_t generation, std::int64_t count);

    // Memoised eligibility per rendered SQL (nullopt: not judged yet). Each
    // judged SQL gets an id that is never reused, so count keys carry the id
    // instead of the (possibly long) SQL text.
    struct Verdict {
        bool eligible = false;
        std::uint64_t id = 0;
    };
    std::optional<Verdict> verdict(const std::string& sql);
    Verdict storeVerdict(const std::string& sql, bool eligible);

    std::size_t size() const;
    void clear();

private:
    struct Entry {
        std::int64_t generation;
        std::int64_t count;
        std::list<std::string>::iterator lru;
    };

    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::list<std::string> lru_;   // front = most recently used
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, Verdict> verdicts_;
    std::uint64_t next_verdict_id_ = 1;
};

// What a paginated read needs to use the cache: the process-wide state, and
// the bound values of this request (part of the key).
struct CountCacheState {
    CountCache cache;
    std::string alias;            // the DuckLake catalog alias
    std::string generation_sql;   // newest snapshot id of that catalog
    // Cleared for good the first time the generation cannot be read: then
    // every count runs, as before.
    std::atomic<bool> usable{true};
};

struct CountCacheUse {
    CountCacheState* state = nullptr;
    std::string bound;
};

// The parse-tree half of the eligibility rule. `functions` receives the
// lower-cased names of every function the query calls, for the caller to check
// against duckdb_functions(). False when the query is not a single SELECT over
// tables of `ducklake_alias` only (or does not parse).
bool CountableQuery(const std::string& sql, const std::string& ducklake_alias,
                    std::vector<std::string>& functions);

}  // namespace flapi
