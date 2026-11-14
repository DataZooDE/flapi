#pragma once

#include "cache_database_adapter.hpp"
#include <memory>

namespace flapi {

class DatabaseManager;

/// The row count DuckDB reports for a DML statement, from a QueryResult's
/// JSON (`[{"Count": n}]`). nullopt for any other shape. Exposed for tests.
std::optional<int64_t> extractAffectedRowCount(const std::string& result_json);

/**
 * Adapter that wraps DatabaseManager to implement ICacheDatabaseAdapter.
 * This allows CacheManager to work with either a real DatabaseManager
 * or a mock for unit testing.
 */
class DatabaseManagerCacheAdapter : public ICacheDatabaseAdapter {
public:
    explicit DatabaseManagerCacheAdapter(std::shared_ptr<DatabaseManager> db_manager);
    ~DatabaseManagerCacheAdapter() override = default;

    std::string renderCacheTemplate(const EndpointConfig& endpoint,
                                    const CacheConfig& cacheConfig,
                                    std::map<std::string, std::string>& params) override;

    void executeDuckLakeQuery(const std::string& query,
                              const std::map<std::string, std::string>& params = {}) override;

    QueryResult executeDuckLakeQueryWithResult(const std::string& query) override;

    std::optional<int64_t> executeDuckLakeQueryCountingRows(
        const std::string& query, const std::map<std::string, std::string>& params = {}) override;

private:
    std::shared_ptr<DatabaseManager> db_manager_;
};

} // namespace flapi
