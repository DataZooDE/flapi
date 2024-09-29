#pragma once

#include <crow.h>
#include <duckdb.h> // Use the C API header
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "config_manager.hpp"
#include "cache_manager.hpp"
#include "sql_template_processor.hpp"

namespace flapi {

struct QueryResult {
    crow::json::wvalue data;
    std::string next;
    int64_t total_count;
};

class DatabaseManager : public std::enable_shared_from_this<DatabaseManager> {
public:

    DatabaseManager();
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager&) = delete; 
    DatabaseManager& operator=(const DatabaseManager&) = delete; 

    static std::shared_ptr<DatabaseManager> getInstance(); // Singleton access method
    void initializeDBManagerFromConfig(std::shared_ptr<ConfigManager> config_manager);
    void initializeConnections(std::shared_ptr<ConfigManager> config_manager);
    duckdb_connection getConnection(); // Return connection handle for thread-specific connections
    
    bool isCacheEnabled(const EndpointConfig& endpoint);
    bool invalidateCache(const EndpointConfig& endpoint);
    
    QueryResult executeQuery(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    QueryResult executeCacheQuery(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);
    
     YAML::Node describeSelectQuery(const EndpointConfig& endpoint);

    bool tableExists(const std::string& schema, const std::string& table, bool prefixSearch = false);
    std::vector<std::string> getTableNames(const std::string& schema, const std::string& table, bool prefixSearch = false);

private:
    void logDuckDBVersion();
    void executeInitStatement(const std::string& init_statement);
    void createAndInitializeDuckDBConfig(std::shared_ptr<ConfigManager> config_manager, duckdb_config& config);

    std::string processTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    std::string processCacheTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);

    QueryResult executeQuery(const std::string& query, const std::map<std::string, std::string>& params = {});

    duckdb_database db; // Database handle
    std::mutex db_mutex; // Mutex for thread safety
    std::shared_ptr<CacheManager> cache_manager;
    std::shared_ptr<SQLTemplateProcessor> sql_processor;
    std::shared_ptr<ConfigManager> config_manager;
};

} // namespace flapi