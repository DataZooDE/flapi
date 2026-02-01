#pragma once

#include <string>
#include <map>

namespace flapi {

// Forward declarations
struct EndpointConfig;
struct CacheConfig;
class QueryResult;

/**
 * Interface for database operations used by CacheManager.
 * This abstraction enables unit testing CacheManager without touching DuckDB.
 */
class ICacheDatabaseAdapter {
public:
    virtual ~ICacheDatabaseAdapter() = default;

    /**
     * Render a cache template with given parameters.
     * @param endpoint The endpoint configuration
     * @param cacheConfig The cache configuration
     * @param params Template parameters to render
     * @return The rendered SQL string
     */
    virtual std::string renderCacheTemplate(const EndpointConfig& endpoint,
                                            const CacheConfig& cacheConfig,
                                            std::map<std::string, std::string>& params) = 0;

    /**
     * Execute a DuckLake query (with no result processing needed).
     * @param query The SQL query to execute
     * @param params Optional parameters
     */
    virtual void executeDuckLakeQuery(const std::string& query,
                                      const std::map<std::string, std::string>& params = {}) = 0;

    /**
     * Execute a DuckLake query and return results.
     * @param query The SQL query to execute
     * @return QueryResult containing the query results
     */
    virtual QueryResult executeDuckLakeQueryWithResult(const std::string& query) = 0;
};

} // namespace flapi
