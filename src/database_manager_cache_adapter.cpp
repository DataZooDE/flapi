#include "database_manager_cache_adapter.hpp"
#include "database_manager.hpp"

#include <crow/json.h>

namespace flapi {

DatabaseManagerCacheAdapter::DatabaseManagerCacheAdapter(std::shared_ptr<DatabaseManager> db_manager)
    : db_manager_(std::move(db_manager)) {}

std::string DatabaseManagerCacheAdapter::renderCacheTemplate(const EndpointConfig& endpoint,
                                                              const CacheConfig& cacheConfig,
                                                              std::map<std::string, std::string>& params) {
    return db_manager_->renderCacheTemplate(endpoint, cacheConfig, params);
}

void DatabaseManagerCacheAdapter::executeDuckLakeQuery(const std::string& query,
                                                        const std::map<std::string, std::string>& params) {
    // DatabaseManager::executeDuckLakeQuery returns QueryResult, but we ignore it here
    db_manager_->executeDuckLakeQuery(query, params);
}

QueryResult DatabaseManagerCacheAdapter::executeDuckLakeQueryWithResult(const std::string& query) {
    std::map<std::string, std::string> emptyParams;
    return db_manager_->executeDuckLakeQuery(query, emptyParams);
}

std::optional<int64_t> DatabaseManagerCacheAdapter::executeDuckLakeQueryCountingRows(
    const std::string& query, const std::map<std::string, std::string>& params) {
    (void)params;  // a rendered cache template has no remaining parameters
    return db_manager_->executeDuckLakeScript(query);
}

std::optional<int64_t> DatabaseManagerCacheAdapter::executePopulateScript(
    const std::string& script, const PopulateTarget& target, bool rebuild) {
    return db_manager_->executeDuckLakeScript(script, &target, rebuild);
}

std::optional<int64_t> extractAffectedRowCount(const std::string& result_json) {
    // A DML statement's result is exactly one row with exactly one column,
    // `Count`. Anything else is not a row count and is not reported as one.
    const auto parsed = crow::json::load(result_json);
    if (!parsed || parsed.t() != crow::json::type::List || parsed.size() != 1) {
        return std::nullopt;
    }
    const auto& row = parsed[0];
    if (row.t() != crow::json::type::Object || row.size() != 1 || !row.has("Count")) {
        return std::nullopt;
    }
    const auto& count = row["Count"];
    if (count.t() != crow::json::type::Number) {
        return std::nullopt;
    }
    return static_cast<int64_t>(count.i());
}

} // namespace flapi
