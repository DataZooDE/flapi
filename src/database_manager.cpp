#include <stdexcept>
#include <cstdio>
#include "crow/json.h"
#include <yaml-cpp/yaml.h>

#include "sql_template_processor.hpp"
#include "database_manager.hpp"

namespace flapi {

std::shared_ptr<DatabaseManager> DatabaseManager::getInstance() {
    static std::shared_ptr<DatabaseManager> instance = std::make_shared<DatabaseManager>();
    return instance;
}

DatabaseManager::DatabaseManager() {
    // Initialize the database handle to nullptr
    db = nullptr;
}

DatabaseManager::~DatabaseManager() {
    if (db) {
        duckdb_close(&db);
    }
}

void DatabaseManager::initializeDBManagerFromConfig(std::shared_ptr<ConfigManager> config_manager) {
    std::lock_guard<std::mutex> lock(db_mutex);
    if (!db) {
        duckdb_config config;
        std::string db_path = config_manager->getDuckDBPath();
        createAndInitializeDuckDBConfig(config_manager, config);
        
        // Open the database using the configuration
        char* error = nullptr;
        if (duckdb_open_ext(db_path.c_str(), &db, config, &error) == DuckDBError) {
            std::string error_message = error ? error : "Unknown error";
            duckdb_free(error);
            duckdb_destroy_config(&config);
            throw std::runtime_error("Failed to open database: " + error_message);
        }

        // Cleanup the configuration object
        duckdb_destroy_config(&config);

        logDuckDBVersion();
        initializeConnections(config_manager);

        // Initialize the sql processor, done before cache manager.
        this->config_manager = config_manager;
        sql_processor = std::make_shared<SQLTemplateProcessor>(config_manager);

        // Initialize the cache manager
        cache_manager = std::make_unique<CacheManager>(shared_from_this());
        cache_manager->warmUpCaches(config_manager);
    }
}

void DatabaseManager::logDuckDBVersion() {
    CROW_LOG_DEBUG << "DuckDB Library Version: " << duckdb_library_version();

    duckdb_connection conn;
    if (duckdb_connect(db, &conn) == DuckDBError) {
        throw std::runtime_error("Failed to create database connection for initialization");
    }

    duckdb_result result;
    if (duckdb_query(conn, "SELECT version()", &result) == DuckDBError) {
        throw std::runtime_error("Failed to query DuckDB version");
    }

    idx_t row_count = duckdb_row_count(&result);
    idx_t column_count = duckdb_column_count(&result);
    if (row_count != 1 || column_count != 1) {
        throw std::runtime_error("Unexpected result format for DuckDB version");
    }

    auto version = duckdb_value_varchar(&result, 0, 0);
    CROW_LOG_DEBUG << "DuckDB DB Version: " << version;
    duckdb_free(version);

    duckdb_destroy_result(&result);
    duckdb_disconnect(&conn);    
}

std::vector<std::string> DatabaseManager::getTableNames(const std::string& schema, const std::string& table, bool prefixSearch) {
    duckdb_connection conn;
    if (duckdb_connect(db, &conn) == DuckDBError) {
        throw std::runtime_error("Failed to create database connection to get table names");
    }

    std::string query = "SELECT table_name FROM information_schema.tables WHERE table_schema = '" + schema + "'";
    if (prefixSearch) {
        query += " OR table_name LIKE '" + table + "%'";
    }
    query += " ORDER BY table_name DESC";

    duckdb_result result;
    if (duckdb_query(conn, query.c_str(), &result) == DuckDBError) {
        duckdb_destroy_result(&result);
        duckdb_disconnect(&conn);
        throw std::runtime_error("Failed to get table names");
    }

    std::vector<std::string> table_names;
    idx_t row_count = duckdb_row_count(&result);
    for (idx_t row = 0; row < row_count; row++) {
        const char* table_name = duckdb_value_varchar(&result, 0, row);
        if (table_name) {
            table_names.push_back(table_name);
        }
        duckdb_free((void*)table_name);
    }

    duckdb_destroy_result(&result);
    duckdb_disconnect(&conn);

    return table_names;
}
    
bool DatabaseManager::tableExists(const std::string& schema, const std::string& table, bool prefixSearch) {
    std::vector<std::string> tableNames = getTableNames(schema, table, prefixSearch);
    return !tableNames.empty();
}

bool DatabaseManager::isCacheEnabled(const EndpointConfig& endpoint) {
    return !endpoint.cache.cacheTableName.empty();
}

bool DatabaseManager::invalidateCache(const EndpointConfig& endpoint) {
    auto params = std::map<std::string, std::string>();
    try {
        cache_manager->refreshCache(config_manager, endpoint, params);
    }
    catch (const std::exception& e) {
        CROW_LOG_ERROR << "Failed to invalidate cache: " << e.what();
        return false;
    }
    return true;
}

void DatabaseManager::createAndInitializeDuckDBConfig(std::shared_ptr<ConfigManager> config_manager, duckdb_config& config) {
    duckdb_create_config(&config);

    // Allow unsigned extensions
    if (duckdb_set_config(config, "allow_unsigned_extensions", "true") == DuckDBError) {
        duckdb_destroy_config(&config);
        throw std::runtime_error("Failed to set DuckDB configuration: allow_unsigned_extensions");
    }

    // Apply settings from the configuration
    const auto& duckdb_settings = config_manager->getDuckDBConfig().settings;
    for (const auto& [key, value] : duckdb_settings) {
        if (duckdb_set_config(config, key.c_str(), value.c_str()) == DuckDBError) {
            duckdb_destroy_config(&config);
            throw std::runtime_error("Failed to set DuckDB configuration: " + key);
        }
    }
}

void DatabaseManager::initializeConnections(std::shared_ptr<ConfigManager> config_manager) {
    const auto& connections = config_manager->getConnections();
    for (const auto& [connection_name, connection_config] : connections) {
        const auto& init = connection_config.getInit();
        if (!init.empty()) {
            CROW_LOG_DEBUG << "Executing init statement for connection: " << connection_name;
            executeInitStatement(init);
        }
    }
}

void DatabaseManager::executeInitStatement(const std::string& init_statement) {
    duckdb_connection conn;
    if (duckdb_connect(db, &conn) == DuckDBError) {
        throw std::runtime_error("Failed to create database connection for initialization");
    }

    duckdb_extracted_statements stmts = nullptr;
    idx_t n_statements = duckdb_extract_statements(conn, init_statement.c_str(), &stmts);
    if (n_statements == 0) {
        auto error_message = duckdb_extract_statements_error(stmts);
        throw std::runtime_error("Failed to extract statements from init statement: " + init_statement + "\n Error: " + error_message);
    }

    duckdb_state status;
    duckdb_prepared_statement prepared_stmt = nullptr;
    duckdb_result result;
    for (idx_t i = 0; i < n_statements; i++) {
        if (duckdb_prepare_extracted_statement(conn, stmts, i, &prepared_stmt) == DuckDBError) {
            duckdb_destroy_extracted(&stmts);
            throw std::runtime_error("Failed to prepare statement: " + init_statement);
        }

        if (duckdb_execute_prepared(prepared_stmt, &result) == DuckDBError) 
        {
            std::string error_message = duckdb_result_error(&result);
            duckdb_destroy_extracted(&stmts);
            duckdb_destroy_result(&result);
            duckdb_disconnect(&conn);
        throw std::runtime_error("Failed to execute init statement " + init_statement + "\n Error: " + error_message);
        }

        duckdb_destroy_prepare(&prepared_stmt);
        duckdb_destroy_result(&result);
    }

    CROW_LOG_DEBUG << n_statements << " init statements executed successfully: \n" << init_statement;

    duckdb_destroy_extracted(&stmts);
    duckdb_disconnect(&conn);
}

duckdb_connection DatabaseManager::getConnection() {
    std::lock_guard<std::mutex> lock(db_mutex); // Lock for thread safety
    duckdb_connection conn;

    if (db == nullptr) {
        throw std::runtime_error("Database not initialized, ensure createDB is called before getConnection");
    }

    if (duckdb_connect(db, &conn) == DuckDBError) { // Check for connection errors
        throw std::runtime_error("Failed to create database connection");
    }
    return conn; // Return the connection handle
}

QueryResult DatabaseManager::executeQuery(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) 
{   
    if (cache_manager->shouldRefreshCache(config_manager, endpoint)) {
        cache_manager->refreshCache(config_manager, endpoint, params);
    }

    cache_manager->addQueryCacheParamsIfNecessary(config_manager, endpoint, params);
    std::string processedQuery = processTemplate(endpoint, params);
    return executeQuery(processedQuery, params, true);
}

QueryResult DatabaseManager::executeCacheQuery(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params) 
{   
    std::string processedQuery = processCacheTemplate(endpoint, cacheConfig, params);
    return executeQuery(processedQuery, params, false);
}

std::string DatabaseManager::processTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    return sql_processor->loadAndProcessTemplate(endpoint, params);
}

std::string DatabaseManager::processCacheTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params) {
    return sql_processor->loadAndProcessTemplate(endpoint, cacheConfig, params);
}

QueryResult DatabaseManager::executeQuery(const std::string& query, const std::map<std::string, std::string>& params, bool with_pagination)
{
    duckdb_connection conn;
    if (duckdb_connect(db, &conn) == DuckDBError) {
        throw std::runtime_error("Failed to create database connection");
    }

    std::string paginatedQuery = query;
    std::string countQuery = "SELECT COUNT(*) FROM (" + query + ") AS subquery";

    auto limit = 0;
    auto offset = 0;
    auto hasPagination = false;

    if (with_pagination && (params.find("limit") != params.end() || params.find("offset") != params.end())) {
        hasPagination = true;
        limit = std::stoll(params.find("limit")->second);
        offset = std::stoll(params.find("offset")->second);
        paginatedQuery = "SELECT * FROM (" + paginatedQuery + ") AS subquery " 
                       +"LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);
    }

    duckdb_result result;
    if (duckdb_query(conn, paginatedQuery.c_str(), &result) == DuckDBError) {
        std::string error_message = duckdb_result_error(&result);
        duckdb_destroy_result(&result);
        duckdb_disconnect(&conn);
        std::string pagination_message = with_pagination ? "(with pagination)" : "";
        error_message = "Query execution failed " + pagination_message + ": " + error_message;
        throw std::runtime_error(error_message);
    }

    crow::json::wvalue json_result;
    std::vector<crow::json::wvalue> rows;

    idx_t row_count = duckdb_row_count(&result);
    idx_t column_count = duckdb_column_count(&result);

    for (idx_t row = 0; row < row_count; row++) {
        crow::json::wvalue json_row;
        for (idx_t col = 0; col < column_count; col++) {
            std::string column_name = duckdb_column_name(&result, col);
            duckdb_type type = duckdb_column_type(&result, col);

            switch (type) {
                case DUCKDB_TYPE_VARCHAR: {
                    const char* str_val = duckdb_value_varchar(&result, col, row);
                    json_row[column_name] = str_val ? str_val : "";
                    duckdb_free((void*)str_val);
                    break;
                }
                case DUCKDB_TYPE_INTEGER: {
                    int32_t int_val = duckdb_value_int32(&result, col, row);
                    json_row[column_name] = int_val;
                    break;
                }
                case DUCKDB_TYPE_BIGINT: {
                    int64_t bigint_val = duckdb_value_int64(&result, col, row);
                    json_row[column_name] = bigint_val;
                    break;
                }
                case DUCKDB_TYPE_DOUBLE: {
                    double double_val = duckdb_value_double(&result, col, row);
                    json_row[column_name] = double_val;
                    break;
                }
                case DUCKDB_TYPE_BOOLEAN: {
                    bool bool_val = duckdb_value_boolean(&result, col, row);
                    json_row[column_name] = bool_val;
                    break;
                }
                // Add more types as needed
                default: {
                    const char* str_val = duckdb_value_varchar(&result, col, row);
                    json_row[column_name] = str_val ? str_val : "";
                    duckdb_free((void*)str_val);
                    break;
                }
            }
        }
        rows.push_back(std::move(json_row));
    }

    json_result = std::move(rows);

    duckdb_destroy_result(&result);

    // Execute count query
    int64_t total_count = 0;
    if (with_pagination && duckdb_query(conn, countQuery.c_str(), &result) == DuckDBSuccess) {
        total_count = duckdb_value_int64(&result, 0, 0);
    }
    duckdb_destroy_result(&result);
    duckdb_disconnect(&conn);

    std::string next = "";
    if (hasPagination && offset + limit < total_count) {
        next = "?offset=" + std::to_string(offset + limit);
        for (const auto& [key, value] : params) {
            if (key != "offset") {
                next += "&" + key + "=" + value;
            }
        }
    }

    return QueryResult{std::move(json_result), next, total_count};
}

YAML::Node DatabaseManager::describeSelectQuery(const EndpointConfig& endpoint)
{
    YAML::Node properties;
    
    try {
        // Process the SQL template
        std::map<std::string, std::string> params;
        cache_manager->addQueryCacheParamsIfNecessary(config_manager, endpoint, params);
        std::string processedQuery = processTemplate(endpoint, params);
        
        // Construct the DESCRIBE query
        std::string describeQuery = "DESCRIBE SELECT * FROM (" + processedQuery + ") AS subquery";
        
        duckdb_connection conn;
        if (duckdb_connect(db, &conn) == DuckDBError) {
            throw std::runtime_error("Failed to create database connection");
        }

        duckdb_result result;
        if (duckdb_query(conn, describeQuery.c_str(), &result) == DuckDBError) {
            std::string error_message = duckdb_result_error(&result);
            duckdb_destroy_result(&result);
            duckdb_disconnect(&conn);
            throw std::runtime_error("Query execution failed: " + error_message);
        }
        
        idx_t row_count = duckdb_row_count(&result);
        for (idx_t i = 0; i < row_count; i++) {
            std::string column_name = duckdb_value_varchar(&result, 0, i);
            std::string column_type = duckdb_value_varchar(&result, 1, i);
            
            YAML::Node property;
            if (column_type == "INTEGER" || column_type == "BIGINT") {
                property["type"] = "integer";
            } else if (column_type == "DOUBLE" || column_type == "FLOAT") {
                property["type"] = "number";
            } else if (column_type == "VARCHAR") {
                property["type"] = "string";
            } else if (column_type == "BOOLEAN") {
                property["type"] = "boolean";
            } else if (column_type == "DATE" || column_type == "TIME" || column_type == "TIMESTAMP") {
                property["type"] = "string";
                property["format"] = "date-time";
            } else {
                property["type"] = "string";  // Default to string for unknown types
            }
            
            properties[column_name] = property;
        }
        
        duckdb_destroy_result(&result);
        duckdb_disconnect(&conn);
    }
    catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error in describeSelectQuery: " << e.what();
    }
    
    return properties;
}

} // namespace flapi