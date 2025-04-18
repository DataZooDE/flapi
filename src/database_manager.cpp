#include <stdexcept>
#include <cstdio>
#include "crow/json.h"
#include <yaml-cpp/yaml.h>

#include "duckdb.hpp"
#include "duckdb/main/capi/capi_internal.hpp"

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

QueryExecutor DatabaseManager::createQueryExecutor() 
{
    if (db == nullptr) {
        throw std::runtime_error("Database not initialized");
    }
    return QueryExecutor(db);
}

void DatabaseManager::logDuckDBVersion() {
    CROW_LOG_DEBUG << "DuckDB Library Version: " << duckdb_library_version();

    auto executor = createQueryExecutor();
    executor.execute("SELECT version()");

    if (executor.rowCount() != 1 || executor.columnCount() != 1) {
        throw std::runtime_error("Unexpected result format for DuckDB version");
    }

    auto version = duckdb_value_varchar(&executor.result, 0, 0);
    CROW_LOG_DEBUG << "DuckDB DB Version: " << version;
    duckdb_free(version);
}

std::vector<std::string> DatabaseManager::getTableNames(const std::string& schema) {
    return getTableNames(schema, "", false);
}

std::vector<std::string> DatabaseManager::getTableNames(const std::string& schema, const std::string& table, bool prefixSearch) {
    std::string query = "SELECT table_name FROM information_schema.tables WHERE table_schema = '" + schema + "'";
    if (prefixSearch) {
        query += " AND table_name LIKE '" + table + "%'";
    }
    query += " ORDER BY table_name DESC";

    auto executor = createQueryExecutor();
    executor.execute(query);

    std::vector<std::string> table_names;
    for (idx_t row = 0; row < executor.rowCount(); row++) {
        const char* table_name = duckdb_value_varchar(&executor.result, 0, row);
        if (table_name) {
            table_names.push_back(table_name);
        }
        duckdb_free((void*)table_name);
    }

    return table_names;
}
        
bool DatabaseManager::tableExists(const std::string& schema, const std::string& table) {
    std::vector<std::string> tableNames = getTableNames(schema, table, true);
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

    // SET autoinstall_known_extensions=1; && SET autoload_known_extensions=1;
    if (duckdb_set_config(config, "autoinstall_known_extensions", "1") == DuckDBError) {
        duckdb_destroy_config(&config);
        throw std::runtime_error("Failed to set DuckDB configuration: autoinstall_known_extensions");
    }

    if (duckdb_set_config(config, "autoload_known_extensions", "1") == DuckDBError) {
        duckdb_destroy_config(&config);
        throw std::runtime_error("Failed to set DuckDB configuration: autoload_known_extensions");
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
    auto executor = createQueryExecutor();
    
    duckdb_extracted_statements stmts = nullptr;
    idx_t n_statements = duckdb_extract_statements(executor.conn, init_statement.c_str(), &stmts);
    if (n_statements == 0) {
        auto error_message = duckdb_extract_statements_error(stmts);
        throw std::runtime_error("Failed to extract statements from init statement: " + init_statement + "\n Error: " + error_message);
    }

    duckdb_prepared_statement prepared_stmt = nullptr;
    for (idx_t i = 0; i < n_statements; i++) {
        if (duckdb_prepare_extracted_statement(executor.conn, stmts, i, &prepared_stmt) == DuckDBError) {
            duckdb_destroy_extracted(&stmts);
            throw std::runtime_error("Failed to prepare statement: " + init_statement);
        }

        executor.executePrepared(prepared_stmt);
        duckdb_destroy_prepare(&prepared_stmt);
    }

    CROW_LOG_DEBUG << n_statements << " init statements executed successfully: \n" << init_statement;
    duckdb_destroy_extracted(&stmts);
}

void DatabaseManager::createSchemaIfNecessary(const std::string& schema) {
    auto executor = createQueryExecutor();
    executor.execute("CREATE SCHEMA IF NOT EXISTS '" + schema + "'");
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

QueryResult DatabaseManager::executeQuery(const EndpointConfig& endpoint, std::map<std::string, std::string>& params, bool with_pagination) 
{   
    if (cache_manager->shouldRefreshCache(config_manager, endpoint)) {
        cache_manager->refreshCache(config_manager, endpoint, params);
    }

    cache_manager->addQueryCacheParamsIfNecessary(config_manager, endpoint, params);
    std::string processedQuery = processTemplate(endpoint, params);
    return executeQuery(processedQuery, params, with_pagination);
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

QueryResult DatabaseManager::executeQuery(const std::string& query, 
                                          const std::map<std::string, std::string>& params,
                                          bool with_pagination) {
    auto executor = createQueryExecutor();
    std::string paginatedQuery = query;
    std::string countQuery = "SELECT COUNT(*) FROM (" + query + ") AS subquery";
    
    auto limit = 0;
    auto offset = 0;
    auto hasPagination = false;
    
    // Handle pagination parameters
    if (with_pagination && (params.find("limit") != params.end() || params.find("offset") != params.end())) {
        hasPagination = true;
        limit = std::stoll(params.find("limit")->second);
        offset = std::stoll(params.find("offset")->second);
        paginatedQuery = "SELECT * FROM (" + paginatedQuery + ") AS subquery " 
                        "LIMIT " + std::to_string(limit) + 
                        " OFFSET " + std::to_string(offset);
    }
    
    // Execute main query
    executor.execute(paginatedQuery);
    
    // Convert result to JSON using QueryResult
    QueryResult result;
    result.data = executor.toJson();
    
    // Handle pagination metadata
    if (with_pagination) {
        // Get total count
        auto countExecutor = createQueryExecutor();
        countExecutor.execute(countQuery);
        if (countExecutor.rowCount() > 0) {
            result.total_count = duckdb_value_int64(&countExecutor.result, 0, 0);
        }
        
        // Generate next link if needed
        if (hasPagination && offset + limit < result.total_count) {
            std::stringstream next_ss;
            next_ss << "?offset=" << (offset + limit);
            for (const auto& [key, value] : params) {
                if (key != "offset") {
                    next_ss << "&" << key << "=" << value;
                }
            }
            result.next = next_ss.str();
        }
    }
    
    return result;
}

YAML::Node DatabaseManager::describeSelectQuery(const EndpointConfig& endpoint) {
    YAML::Node properties;
    
    try {
        std::map<std::string, std::string> params;
        cache_manager->addQueryCacheParamsIfNecessary(config_manager, endpoint, params);
        std::string processedQuery = processTemplate(endpoint, params);
        
        std::string describeQuery = "DESCRIBE SELECT * FROM (" + processedQuery + ") AS subquery";
        
        auto executor = createQueryExecutor();
        executor.execute(describeQuery);
        
        for (idx_t i = 0; i < executor.rowCount(); i++) {
            // Get column name and type, storing pointers so we can free them
            const char* column_name_ptr = duckdb_value_varchar(&executor.result, 0, i);
            const char* column_type_ptr = duckdb_value_varchar(&executor.result, 1, i);
            
            // Create strings from the pointers before we free them
            std::string column_name(column_name_ptr);
            std::string column_type(column_type_ptr);
            
            // Free the memory allocated by duckdb_value_varchar
            duckdb_free((void*)column_name_ptr);
            duckdb_free((void*)column_type_ptr);
            
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
    }
    catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error in describeSelectQuery: " << e.what();
    }
    
    return properties;
}

void DatabaseManager::refreshSecretsTable(const std::string& secret_table, const std::string& secret_json) {
    auto executor = createQueryExecutor();

    auto create_table_stmt = "CREATE OR REPLACE TABLE " + secret_table + "(j JSON)";
    auto insert_stmt = "INSERT INTO " + secret_table + " VALUES ('" + secret_json + "')";

    try {
        executor.execute(create_table_stmt, "create table");
        executor.execute(insert_stmt, "insert data");
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to refresh JSON table '" + secret_table + "': " + e.what());
    }
}

std::optional<std::tuple<std::string, std::vector<std::string>>> DatabaseManager::findUserInSecretsTable(const std::string& secret_table, 
                                                                                                         const std::string& username) 
{
    std::stringstream query;

    query << "SELECT y->>'username' AS username, y->>'password' AS password, CAST(json_extract_string(y, '$.roles[*]') AS varchar[]) AS roles "
          << "FROM (SELECT unnest(cast(j.auth as JSON[])) AS y FROM " << secret_table << ") AS x "
          << "WHERE y->>'username' = '" << username << "' "
          << "LIMIT 1";

    auto executor = createQueryExecutor();
    executor.execute(query.str());

    while(true) 
    {
        auto chunk = duckdb_fetch_chunk(executor.result);
        if (chunk == nullptr) {
            break;
        }

        idx_t row_count = duckdb_data_chunk_get_size(chunk);
        if (row_count != 1) {
            duckdb_destroy_data_chunk(&chunk);
            return std::nullopt;
        }

        // get the username column
        auto username_col = duckdb_data_chunk_get_vector(chunk, 0);
        auto password_col = duckdb_data_chunk_get_vector(chunk, 1);
        auto roles_col = duckdb_data_chunk_get_vector(chunk, 2);

        duckdb_string_t *username_data = (duckdb_string_t *) duckdb_vector_get_data(username_col);
        std::string username;
        if (duckdb_string_is_inlined(username_data[0])) {
            username = std::string(username_data[0].value.inlined.inlined, username_data[0].value.inlined.length);
		} else {
            username = std::string(username_data[0].value.pointer.ptr, username_data[0].value.pointer.length);
        }

        duckdb_string_t *password_data = (duckdb_string_t *) duckdb_vector_get_data(password_col);
        std::string password;
        if (duckdb_string_is_inlined(password_data[0])) {
            password = std::string(password_data[0].value.inlined.inlined, password_data[0].value.inlined.length);
		} else {
            password = std::string(password_data[0].value.pointer.ptr, password_data[0].value.pointer.length);
        }

        duckdb_list_entry *list_data = (duckdb_list_entry *) duckdb_vector_get_data(roles_col);
        duckdb_vector list_child = duckdb_list_vector_get_child(roles_col);
        duckdb_string_t *child_data = (duckdb_string_t *) duckdb_vector_get_data(list_child);
        duckdb_list_entry roles_list = list_data[0];

        std::vector<std::string> roles;
        for (idx_t child_idx = roles_list.offset; child_idx < roles_list.offset + roles_list.length; child_idx++) 
        {
            std::string role;
            if (duckdb_string_is_inlined(child_data[child_idx])) {
                role = std::string(child_data[child_idx].value.inlined.inlined, child_data[child_idx].value.inlined.length);
            } else {
                role = std::string(child_data[child_idx].value.pointer.ptr, child_data[child_idx].value.pointer.length);
            }
            roles.push_back(role);
        }
        
        duckdb_destroy_data_chunk(&chunk);
        return std::make_tuple(password, roles);
    }

    return std::nullopt;
}

std::tuple<duckdb::SecretManager&, duckdb::CatalogTransaction> DatabaseManager::getSecretManagerAndTransaction() 
{
    auto wrapper = reinterpret_cast<duckdb::DatabaseWrapper *>(db);
    auto db_instance = wrapper->database->instance;
    auto &secret_manager = duckdb::SecretManager::Get(*db_instance);
    auto transaction = duckdb::CatalogTransaction::GetSystemTransaction(*db_instance);
    return std::tuple<duckdb::SecretManager&, duckdb::CatalogTransaction>(secret_manager, transaction);
}

std::vector<ColumnInfo> DatabaseManager::getTableColumns(const std::string& schema, const std::string& table) {
    std::vector<ColumnInfo> columns;
    
    try {
        auto query = "SELECT column_name, data_type, is_nullable "
                    "FROM information_schema.columns "
                    "WHERE table_schema = ? AND table_name = ? "
                    "ORDER BY ordinal_position";
        
        // Create parameter map with numbered placeholders
        std::map<std::string, std::string> params;
        params["1"] = schema;
        params["2"] = table;
        
        auto result = executeQuery(query, params, false);
        
        if (result.data.t() == crow::json::type::Null || result.data.size() == 0) {
            return columns;
        }

        for (std::size_t i = 0; i < result.data.size(); ++i) {
            ColumnInfo col;
            col.name = result.data[i]["column_name"].dump();
            col.type = result.data[i]["data_type"].dump();
            col.nullable = result.data[i]["is_nullable"].dump() == "YES";
            columns.push_back(std::move(col));
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error getting columns for table " << schema << "." << table << ": " << e.what();
    }
    
    return columns;
}

} // namespace flapi