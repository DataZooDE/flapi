#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
// function, no sample, and no function that may answer differently between
// requests. DuckDB's own stability report is not enough for the last part: a
// name passes when no overload is VOLATILE and not every overload is
// CONSISTENT_WITHIN_QUERY (date_part and friends read the fixed TimeZone in
// their TIMESTAMPTZ overloads), but age(TIMESTAMP) is reported CONSISTENT and
// reads the transaction start; such names are refused by name
// (UnstableFunctionName). Each three-part reference must also be a TABLE in the
// catalog, not a VIEW, whose body may read anything: checked against
// duckdb_tables() whenever the generation changes. Time travel
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

    // Whether every table the SQL names is a DuckLake TABLE (not a view), as
    // judged at `generation`; nullopt when not judged at that generation.
    std::optional<bool> tablesVerdict(const std::string& sql, std::int64_t generation);
    void storeTablesVerdict(const std::string& sql, std::int64_t generation, bool tables_ok);

    std::size_t size() const;
    void clear();

private:
    struct Entry {
        std::int64_t generation;
        std::int64_t count;
        std::list<std::string>::iterator lru;
    };
    struct StoredVerdict {
        Verdict verdict;
        std::int64_t tables_generation = -1;   // -1: tables not judged yet
        bool tables_ok = false;
    };

    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::list<std::string> lru_;   // front = most recently used
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, StoredVerdict> verdicts_;
    std::uint64_t next_verdict_id_ = 1;
};

// Count keys are built from length-prefixed parts, so no value can be spliced
// into another's: "3:abc" never reads as anything but the three bytes abc.
void AppendKeyPart(std::string& key, std::string_view part);
// A key longer than this (long bound values) is not cached: one entry must not
// hold megabytes, and the LRU bounds the number of entries only.
constexpr std::size_t kMaxCountKeyBytes = 4096;

// When the generation cannot be read, the cache is off for a while and then
// tried again; transitions are reported so each is logged once.
class CountCacheSwitch {
public:
    static constexpr std::int64_t kRetryAfterMs = 30000;

    // True when the cache may be used at `now_ms` (steady clock): on, or off
    // long enough to try again.
    bool usable(std::int64_t now_ms) const;
    // The generation could not be read. True when this turned the cache off.
    bool failed(std::int64_t now_ms);
    // The generation was read. True when this turned the cache back on.
    bool succeeded();

private:
    std::atomic<bool> off_{false};
    std::atomic<std::int64_t> retry_at_ms_{0};
};

// What a paginated read needs to use the cache: the process-wide state, and
// the bound values of this request (part of the key).
struct CountCacheState {
    CountCache cache;
    std::string alias;            // the DuckLake catalog alias
    std::string generation_sql;   // newest snapshot id of that catalog
    CountCacheSwitch power;
};

struct CountCacheUse {
    CountCacheState* state = nullptr;
    std::string bound;            // AppendKeyPart-encoded name/value pairs
    bool too_long = false;        // the key would exceed kMaxCountKeyBytes
};

struct TableName {
    std::string catalog;
    std::string schema;
    std::string name;
};

// Names refused whatever duckdb_functions() reports (lower-case).
bool UnstableFunctionName(const std::string& lower_name);

// The parse-tree half of the eligibility rule. `functions` receives the
// lower-cased names of every function the query calls, for the caller to check
// against duckdb_functions(); `tables` every three-part table reference, for
// the caller to check against duckdb_tables(). False when the query is not a
// single SELECT over tables of `ducklake_alias` only, samples, calls an
// UnstableFunctionName (or does not parse).
bool CountableQuery(const std::string& sql, const std::string& ducklake_alias,
                    std::vector<std::string>& functions, std::vector<TableName>& tables);
bool CountableQuery(const std::string& sql, const std::string& ducklake_alias,
                    std::vector<std::string>& functions);

}  // namespace flapi
