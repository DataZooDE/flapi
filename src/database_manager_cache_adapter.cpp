#include "database_manager_cache_adapter.hpp"
#include "database_manager.hpp"

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

} // namespace flapi
