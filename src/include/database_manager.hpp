#pragma once

#include <crow.h>
#include <duckdb.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

#include "duckdb/main/secret/secret_manager.hpp"

#include "config_manager.hpp"
#include "cache_manager.hpp"
#include "sql_template_processor.hpp"

namespace flapi {

struct QueryExecutor {
    duckdb_connection conn;
    mutable duckdb_result result;
    bool has_result;
    
    QueryExecutor(duckdb_database db);
    ~QueryExecutor();
    
    void execute(const std::string& query, const std::string& context = "");
    void executePrepared(duckdb_prepared_statement stmt, const std::string& context = "");
    
    idx_t rowCount() const { return has_result ? duckdb_row_count(&result) : 0; }
    idx_t columnCount() const { return has_result ? duckdb_column_count(&result) : 0; }
};

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
    void executeInitStatement(const std::string& init_statement);
    void createSchemaIfNecessary(const std::string& schema);
    duckdb_connection getConnection(); 
    
    bool isCacheEnabled(const EndpointConfig& endpoint);
    bool invalidateCache(const EndpointConfig& endpoint);
    
    QueryResult executeQuery(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    QueryResult executeCacheQuery(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);
    QueryResult executeQuery(const std::string& query, const std::map<std::string, std::string>& params = {}, bool with_pagination = true);
    
    YAML::Node describeSelectQuery(const EndpointConfig& endpoint);

    bool tableExists(const std::string& schema, const std::string& table);
    std::vector<std::string> getTableNames(const std::string& schema, const std::string& table, bool prefixSearch = false);

    void refreshSecretsTable(const std::string& secret_table, const std::string& secret_json);
    std::optional<std::tuple<std::string, std::vector<std::string>>> findUserInSecretsTable(const std::string& secret_table, const std::string& username);
    std::tuple<duckdb::SecretManager&, duckdb::CatalogTransaction> getSecretManagerAndTransaction();

private:
    QueryExecutor createQueryExecutor();

    void logDuckDBVersion();
    
    void createAndInitializeDuckDBConfig(std::shared_ptr<ConfigManager> config_manager, duckdb_config& config);
   

    std::string processTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    std::string processCacheTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);

    crow::json::wvalue duckRowToJson(duckdb_result& result, idx_t row);

    duckdb_database db; // Database handle
    std::mutex db_mutex; // Mutex for thread safety
    std::shared_ptr<CacheManager> cache_manager;
    std::shared_ptr<SQLTemplateProcessor> sql_processor;
    std::shared_ptr<ConfigManager> config_manager;
};

} // namespace flapi