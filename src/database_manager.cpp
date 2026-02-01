#include <stdexcept>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include "crow/json.h"
#include <yaml-cpp/yaml.h>

#include "duckdb.hpp"
#include "duckdb/main/capi/capi_internal.hpp"

#include "sql_template_processor.hpp"
#include "database_manager.hpp"
#include "duckdb_raii.hpp"

namespace flapi {


std::shared_ptr<DatabaseManager> DatabaseManager::getInstance() {
    static std::shared_ptr<DatabaseManager> instance = std::make_shared<DatabaseManager>();
    return instance;
}

DatabaseManager::DatabaseManager() {
    // Initialize the database handle to nullptr
    db = nullptr;
}

bool DatabaseManager::isInitialized() const {
    return db != nullptr;
}

void DatabaseManager::reset() {
    std::lock_guard<std::mutex> lock(db_mutex);

    // Close the database if open
    if (db) {
        // Try graceful shutdown first
        if (config_manager) {
            try {
                const auto& ducklake_config = config_manager->getDuckLakeConfig();
                if (ducklake_config.enabled) {
                    try {
                        std::string detach_stmt = "DETACH " + ducklake_config.alias + ";";
                        // Need to execute without lock since we already hold it
                        duckdb_connection conn;
                        if (duckdb_connect(db, &conn) == DuckDBSuccess) {
                            duckdb_result result;
                            duckdb_query(conn, detach_stmt.c_str(), &result);
                            duckdb_destroy_result(&result);
                            duckdb_disconnect(&conn);
                        }
                    } catch (...) {
                        // Ignore errors during cleanup
                    }
                }
            } catch (...) {
                // Ignore errors during cleanup
            }
        }

        duckdb_close(&db);
        db = nullptr;
        CROW_LOG_DEBUG << "DatabaseManager reset: closed database";
    }

    // Clear all internal state
    cache_manager.reset();
    sql_processor.reset();
    config_manager.reset();

    CROW_LOG_DEBUG << "DatabaseManager reset: cleared all state";
}

DatabaseManager::~DatabaseManager() {
    try {
        // Graceful shutdown: Detach DuckLake and flush WAL before closing
        if (db && config_manager) {
            try {
                const auto& ducklake_config = config_manager->getDuckLakeConfig();
                if (ducklake_config.enabled) {
                    try {
                        // Detach DuckLake catalog to release locks
                        std::string detach_stmt = "DETACH " + ducklake_config.alias + ";";
                        executeInitStatement(detach_stmt);
                        CROW_LOG_INFO << "Detached DuckLake catalog: " << ducklake_config.alias;
                    } catch (const std::exception& e) {
                        CROW_LOG_WARNING << "Failed to detach DuckLake catalog: " << e.what();
                    }
                }

                // Checkpoint to flush WAL before closing
                try {
                    executeInitStatement("CHECKPOINT;");
                    CROW_LOG_DEBUG << "Checkpointed database WAL";
                } catch (const std::exception& e) {
                    CROW_LOG_WARNING << "Failed to checkpoint WAL: " << e.what();
                }
            } catch (const std::exception& e) {
                CROW_LOG_WARNING << "Error during database graceful shutdown: " << e.what();
            }
        }
    } catch (const std::exception& e) {
        // Catch-all to prevent exceptions from propagating in destructor
        CROW_LOG_ERROR << "Unexpected error during database cleanup: " << e.what();
    }

    // Close the database connection
    if (db) {
        duckdb_close(&db);
        CROW_LOG_DEBUG << "Closed DuckDB connection";
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
            DuckDBString error_wrapper(error);
            std::string error_message = error_wrapper.to_string();
            if (error_message.empty()) {
                error_message = "Unknown error";
            }
            duckdb_destroy_config(&config);
            throw std::runtime_error("Failed to open database: " + error_message);
        }

        // Cleanup the configuration object
        duckdb_destroy_config(&config);

        logDuckDBVersion();
        loadDefaultExtensions(config_manager);
        initializeConnections(config_manager);

        this->config_manager = config_manager;

        const auto& ducklake_config = config_manager->getDuckLakeConfig();
        if (ducklake_config.enabled) {
            CROW_LOG_INFO << "Attaching DuckLake catalog at alias '" << ducklake_config.alias << "'";
            std::string attach_stmt = "ATTACH 'ducklake:" + ducklake_config.metadata_path + "' AS " + ducklake_config.alias + " (DATA_PATH '" + ducklake_config.data_path + "'";

            // Add data inlining configuration if specified
            if (ducklake_config.data_inlining_row_limit) {
                attach_stmt += ", DATA_INLINING_ROW_LIMIT " + std::to_string(ducklake_config.data_inlining_row_limit.value());
                CROW_LOG_INFO << "DuckLake data inlining enabled with row limit: " << ducklake_config.data_inlining_row_limit.value();
            }

            if (ducklake_config.override_data_path) {
                attach_stmt += ", OVERRIDE_DATA_PATH TRUE";
            }

            attach_stmt += ");";

            try {
                executeInitStatement(attach_stmt);
            } catch (const std::exception& e) {
                throw std::runtime_error(std::string("Failed to attach DuckLake catalog: ") + e.what());
            }
        }

        sql_processor = std::make_shared<SQLTemplateProcessor>(config_manager);

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

    DuckDBString version_wrapper(duckdb_value_varchar(&executor.result, 0, 0));
    CROW_LOG_DEBUG << "DuckDB DB Version: " << version_wrapper.to_string();
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
        DuckDBString table_name_wrapper(duckdb_value_varchar(&executor.result, 0, row));
        if (!table_name_wrapper.is_null()) {
            table_names.push_back(table_name_wrapper.to_string());
        }
    }

    return table_names;
}
        
bool DatabaseManager::tableExists(const std::string& schema, const std::string& table) {
    std::vector<std::string> tableNames = getTableNames(schema, table, true);
    return !tableNames.empty();
}

bool DatabaseManager::isCacheEnabled(const EndpointConfig& endpoint) {
    return endpoint.cache.enabled && !endpoint.cache.table.empty();
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

void DatabaseManager::loadDefaultExtensions(std::shared_ptr<ConfigManager> config_manager) {
    auto extensions = config_manager->getDuckDBConfig().default_extensions;

    // Install the extensions (try individually to handle failures gracefully)
    CROW_LOG_INFO << "Installing default " << extensions.size() << " extensions";
    for (const auto& extension : extensions) {
        std::string install_stmt = "INSTALL " + extension + ";";
        try {
            executeInitStatement(install_stmt);
            CROW_LOG_DEBUG << "Successfully installed extension: " << extension;
        } catch (const std::exception& e) {
            CROW_LOG_WARNING << "Failed to install extension '" << extension << "': " << e.what() << ". Skipping.";
        }
    }
    CROW_LOG_INFO << "Finished installing default extensions";

    // Load the extensions (try individually to handle failures gracefully)
    CROW_LOG_INFO << "Loading default " << extensions.size() << " extensions";
    int loaded_count = 0;
    for (const auto& extension : extensions) {
        std::string load_stmt = "LOAD " + extension + ";";
        try {
            executeInitStatement(load_stmt);
            CROW_LOG_DEBUG << "Successfully loaded extension: " << extension;
            loaded_count++;
        } catch (const std::exception& e) {
            CROW_LOG_WARNING << "Failed to load extension '" << extension << "': " << e.what() << ". Skipping.";
        }
    }
    CROW_LOG_INFO << "Successfully loaded " << loaded_count << " out of " << extensions.size() << " extensions";
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
    cache_manager->addQueryCacheParamsIfNecessary(config_manager, endpoint, params);
    std::string processedQuery = processTemplate(endpoint, params);
    return executeQuery(processedQuery, params, with_pagination);
}

QueryResult DatabaseManager::executeCacheQuery(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params) 
{   
    std::string processedQuery = processCacheTemplate(endpoint, cacheConfig, params);
    return executeQuery(processedQuery, params, false);
}

QueryResult DatabaseManager::executeDuckLakeQuery(const std::string& query, const std::map<std::string, std::string>& params) {
    return executeQuery(query, params, false);
}

std::string DatabaseManager::processTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    return sql_processor->loadAndProcessTemplate(endpoint, params);
}

std::string DatabaseManager::processCacheTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params) {
    return sql_processor->loadAndProcessTemplate(endpoint, cacheConfig, params);
}

std::string DatabaseManager::renderCacheTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params) {
    return processCacheTemplate(endpoint, cacheConfig, params);
}

std::unique_ptr<QueryExecutor> DatabaseManager::executeQueryRaw(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    cache_manager->addQueryCacheParamsIfNecessary(config_manager, endpoint, params);
    std::string processedQuery = processTemplate(endpoint, params);

    auto executor = std::make_unique<QueryExecutor>(db);
    executor->execute(processedQuery);
    return executor;
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
        if (params.find("limit") != params.end()) {
            limit = std::stoll(params.find("limit")->second);
        }
        if (params.find("offset") != params.end()) {
            offset = std::stoll(params.find("offset")->second);
        }
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

WriteResult DatabaseManager::executeWrite(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    auto executor = createQueryExecutor();
    return executeWrite(executor, endpoint, params);
}

WriteResult DatabaseManager::executeWrite(QueryExecutor& executor, const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    WriteResult result;
    
    // Process SQL template (reuse existing template processor)
    std::string processedQuery = processTemplate(endpoint, params);
    
    // Helper function to trim whitespace
    auto trim = [](const std::string& str) -> std::string {
        size_t first = str.find_first_not_of(" \t\n\r");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\n\r");
        return str.substr(first, (last - first + 1));
    };
    
    // Helper function to check if a string starts with another (case-insensitive)
    auto startsWith = [](const std::string& str, const std::string& prefix) -> bool {
        if (str.length() < prefix.length()) return false;
        std::string strLower = str;
        std::string prefixLower = prefix;
        std::transform(strLower.begin(), strLower.end(), strLower.begin(), ::tolower);
        std::transform(prefixLower.begin(), prefixLower.end(), prefixLower.begin(), ::tolower);
        return strLower.compare(0, prefixLower.length(), prefixLower) == 0;
    };
    
    // Split query by semicolons to support multiple statements
    std::vector<std::string> statements;
    std::istringstream queryStream(processedQuery);
    std::string statement;
    
    while (std::getline(queryStream, statement, ';')) {
        std::string trimmed = trim(statement);
        if (!trimmed.empty()) {
            statements.push_back(trimmed);
        }
    }
    
    // If no statements found (shouldn't happen), treat entire query as single statement
    if (statements.empty()) {
        statements.push_back(trim(processedQuery));
    }
    
    bool hasReturningData = false;
    
    // Execute all statements except potentially the last one
    // The last statement might be a SELECT if returns-data is true and no RETURNING clause worked
    size_t statementsToExecute = statements.size();
    bool lastStatementIsSelect = false;
    
    if (statements.size() > 1 && endpoint.operation.returns_data) {
        // Check if last statement is a SELECT
        std::string lastStatement = trim(statements.back());
        lastStatementIsSelect = startsWith(lastStatement, "SELECT");
        
        if (lastStatementIsSelect) {
            // Execute all but the last statement first
            statementsToExecute = statements.size() - 1;
        }
    }
    
    // Execute statements
    for (size_t i = 0; i < statementsToExecute; ++i) {
        executor.execute(statements[i], "write operation statement " + std::to_string(i + 1));
        
        // Track rows affected from the last write statement (INSERT/UPDATE/DELETE)
        // For multi-statement queries, we want the total rows affected
        int64_t currentRows = executor.rowCount();
        if (currentRows > 0) {
            result.rows_affected = currentRows;
        }
        
        // Check if there's a RETURNING clause - if so, capture the returned data
        // We detect RETURNING by:
        // 1. Checking if the statement contains "RETURNING" (case-insensitive)
        // 2. AND checking if there are columns AND rows in the result
        std::string currentStatement = trim(statements[i]);
        std::string upperStatement = currentStatement;
        std::transform(upperStatement.begin(), upperStatement.end(), upperStatement.begin(), ::toupper);
        bool hasReturningClause = upperStatement.find("RETURNING") != std::string::npos;
        
        if (hasReturningClause && executor.columnCount() > 0 && executor.rowCount() > 0) {
            // There's a RETURNING clause with data
            result.returned_data = executor.toJson();
            hasReturningData = true;
        }
    }
    
    // If we need to return data but didn't get it from RETURNING clause,
    // and the last statement is a SELECT, execute it and capture the result
    if (endpoint.operation.returns_data && !hasReturningData && lastStatementIsSelect && statements.size() > 1) {
        std::string selectStatement = trim(statements.back());
        executor.execute(selectStatement, "select returning data");
        
        if (executor.columnCount() > 0 && executor.rowCount() > 0) {
            result.returned_data = executor.toJson();
        }
    }
    
    return result;
}

WriteResult DatabaseManager::executeWriteInTransaction(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    WriteResult result;
    
    auto executor = createQueryExecutor();
    
    try {
        // Begin transaction
        executor.execute("BEGIN TRANSACTION", "begin transaction");
        
        // Execute the write operation using the same executor (same connection)
        result = executeWrite(executor, endpoint, params);
        
        // Commit transaction
        executor.execute("COMMIT", "commit transaction");
        
    } catch (const std::exception& e) {
        // Rollback on error
        try {
            executor.execute("ROLLBACK", "rollback transaction");
        } catch (const std::exception& rollback_error) {
            CROW_LOG_ERROR << "Failed to rollback transaction: " << rollback_error.what();
        }
        // Re-throw the original error
        throw;
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
            // Get column name and type using RAII wrappers
            DuckDBString column_name_wrapper(duckdb_value_varchar(&executor.result, 0, i));
            DuckDBString column_type_wrapper(duckdb_value_varchar(&executor.result, 1, i));

            // Create strings from the wrappers (memory freed automatically on scope exit)
            std::string column_name = column_name_wrapper.to_string();
            std::string column_type = column_type_wrapper.to_string();
            
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
        CROW_LOG_ERROR << "Error in describeSelectQuery for " << endpoint.getIdentifier() << ": " << e.what();
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
