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
#include "query_executor.hpp"
#include <optional>

namespace flapi {

struct ColumnInfo {
    std::string name;
    std::string type;
    bool nullable;
};

struct WriteResult {
    int64_t rows_affected = 0;
    std::optional<crow::json::wvalue> returned_data;  // For RETURNING clauses in INSERT/UPDATE/DELETE
    std::string last_insert_id;  // For cases where we need to track the last inserted ID
};

class DatabaseManager : public std::enable_shared_from_this<DatabaseManager> {
public:

    DatabaseManager();
    ~DatabaseManager();
    DatabaseManager(const DatabaseManager&) = delete; 
    DatabaseManager& operator=(const DatabaseManager&) = delete; 

    static std::shared_ptr<DatabaseManager> getInstance(); // Singleton access method
    
    void initializeDBManagerFromConfig(std::shared_ptr<ConfigManager> config_manager);
    void loadDefaultExtensions(std::shared_ptr<ConfigManager> config_manager);
    void initializeConnections(std::shared_ptr<ConfigManager> config_manager);
    void executeInitStatement(const std::string& init_statement);
    void createSchemaIfNecessary(const std::string& schema);
    duckdb_connection getConnection(); 
    
    bool isCacheEnabled(const EndpointConfig& endpoint);
    bool invalidateCache(const EndpointConfig& endpoint);
    
    QueryResult executeQuery(const EndpointConfig& endpoint, std::map<std::string, std::string>& params, bool with_pagination = true);
    QueryResult executeCacheQuery(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);
    QueryResult executeQuery(const std::string& query, const std::map<std::string, std::string>& params = {}, bool with_pagination = true);

    // Execute query and return raw QueryExecutor for Arrow serialization
    // The executor owns the duckdb_result and must remain alive while accessing it
    std::unique_ptr<QueryExecutor> executeQueryRaw(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    QueryResult executeDuckLakeQuery(const std::string& query, const std::map<std::string, std::string>& params = {});
    std::string renderCacheTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);
    
    // Write operation methods
    WriteResult executeWrite(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    WriteResult executeWriteInTransaction(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    
    YAML::Node describeSelectQuery(const EndpointConfig& endpoint);

    bool tableExists(const std::string& schema, const std::string& table);
    std::vector<std::string> getTableNames(const std::string& schema, const std::string& table, bool prefixSearch = false);
    std::vector<std::string> getTableNames(const std::string& schema);
    std::vector<ColumnInfo> getTableColumns(const std::string& schema, const std::string& table);

    void refreshSecretsTable(const std::string& secret_table, const std::string& secret_json);
    std::optional<std::tuple<std::string, std::vector<std::string>>> findUserInSecretsTable(const std::string& secret_table, const std::string& username);
    std::tuple<duckdb::SecretManager&, duckdb::CatalogTransaction> getSecretManagerAndTransaction();
    
    std::shared_ptr<CacheManager> getCacheManager() const { return cache_manager; }

private:
    QueryExecutor createQueryExecutor();

    void logDuckDBVersion();
    
    void createAndInitializeDuckDBConfig(std::shared_ptr<ConfigManager> config_manager, duckdb_config& config);
   

    std::string processTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    std::string processCacheTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);
    
    // Internal helper for write operations (used by executeWriteInTransaction)
    WriteResult executeWrite(QueryExecutor& executor, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);

    duckdb_database db; // Database handle
    std::mutex db_mutex; // Mutex for thread safety
    std::shared_ptr<CacheManager> cache_manager;
    std::shared_ptr<SQLTemplateProcessor> sql_processor;
    std::shared_ptr<ConfigManager> config_manager;
};

} // namespace flapi