#pragma once

#include <crow.h>
#include <duckdb.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "duckdb/main/secret/secret_manager.hpp"

#include "config_manager.hpp"
#include "cache_manager.hpp"
#include "sql_template_processor.hpp"
#include "query_executor.hpp"
#include <optional>

namespace flapi {

// True when a backend error is transient lock contention rather than a fault in
// the request. Exposed so the HTTP and MCP layers can map it to 503 instead of
// 500 without duplicating the string matching.
bool isLockContentionMessage(const std::string& message);

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

    // Check if the database has been initialized
    bool isInitialized() const;

    // Reset the database manager to uninitialized state (for testing)
    // This closes any open database and clears all internal state
    void reset();

    // The graceful end of the database: DETACH the DuckLake catalog, CHECKPOINT,
    // close. Called by main() once the handler pool is drained and the warmup
    // thread joined - never from the destructor, which runs inside exit()
    // where executing SQL is undefined (#147). Idempotent.
    void shutdown();

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
    /// Run a (possibly multi-statement) DuckLake script - a rendered cache
    /// populate template - in ONE transaction, statement by statement, and
    /// return the sum of the row counts its DML statements report (nullopt if
    /// none reports one). Any failure rolls the whole script back, so a failed
    /// refresh leaves the cache exactly as it was. A script that manages its own
    /// transaction (contains BEGIN/COMMIT/ROLLBACK) is run as written.
    ///
    /// With a `target` it is a cache populate template, and two more rules hold:
    /// a SELECT statement anywhere in it is refused before anything runs (a bare
    /// SELECT builds nothing, and on a large source materialises the whole
    /// result only to discard it), and the script must leave `target` existing,
    /// checked before COMMIT so a failure rolls back. `rebuild` drops `target`
    /// at the start of the same transaction (POST .../cache/refresh?full=true).
    std::optional<int64_t> executeDuckLakeScript(const std::string& script,
                                                 const PopulateTarget* target = nullptr,
                                                 bool rebuild = false);
    /// 1-based index of the first SELECT statement in `script`, or nullopt
    /// (also when the script does not parse - execution reports that). Pure.
    /// `statement_count`, when given, receives the number of statements parsed.
    static std::optional<std::size_t> FindSelectStatement(const std::string& script,
                                                          std::size_t* statement_count = nullptr);
    /// The statements of `script`, split on top-level `;` (string literals,
    /// quoted identifiers, dollar quotes and comments respected), each without
    /// its terminator; whitespace/comment-only pieces are dropped. Splitting is
    /// lexical only, so a statement that does not parse still gets its own
    /// number - the parser rejects the whole script at its first error. Pure.
    static std::vector<std::string> SplitStatements(const std::string& script);

    /// One finding of validateScript, `statement` 1-based (0: the script as a whole).
    struct ScriptIssue {
        std::size_t statement = 0;
        std::string type;      // parse | bare_select | bind | cache_not_built | not_checked
        std::string message;
    };
    struct ScriptValidation {
        std::size_t statements = 0;
        std::vector<ScriptIssue> errors;
        std::vector<ScriptIssue> warnings;
    };
    /// Checks a rendered script WITHOUT running it (template/expand?validate_only):
    /// each statement is parsed by the connection's parser, then prepared - bound
    /// and planned, which is what EXPLAIN checks: unknown tables and columns,
    /// type errors - inside a transaction that is always rolled back. Only a
    /// plain CREATE TABLE/SCHEMA without AS (no data) is executed there, so a
    /// later statement that writes the table it creates can be bound; no DML and
    /// no query ever runs. With `populate` it is a cache populate template and
    /// the bare-SELECT rule applies. A table that does not exist because an
    /// earlier statement would create it is a warning (not_checked), as is the
    /// cache table `cache_table` before its first build (cache_not_built).
    ScriptValidation validateScript(const std::string& script, const PopulateTarget* populate,
                                    const PopulateTarget* cache_table);
    /// The error a populate template containing a SELECT is refused with.
    // A script that is exactly one SELECT, as `CREATE OR REPLACE TABLE <target>
    // AS <select>`; nullopt for anything else. Used for a cache with no
    // template-file, whose populate SQL is the endpoint's serving template.
    static std::optional<std::string> MaterialiseSelect(const std::string& script, const PopulateTarget& target);
    /// `statement_count` (0: unknown) tells a bare SELECT before the last
    /// statement - a guard whose result is discarded - from one that is the
    /// whole build.
    static std::string PopulateSelectError(const PopulateTarget& target, std::size_t statement,
                                           std::size_t statement_count = 0);
    /// Create the local metadata-path parent and data-path directories DuckLake
    /// needs (a read-only replica only checks them). The problem, or nullopt.
    static std::optional<std::string> PrepareDuckLakeDirectories(const DuckLakeConfig& ducklake);
    /// The DuckLake attach failure as reported: credentials redacted, and a lock
    /// conflict on a DuckDB-file catalog explained.
    static std::string DuckLakeAttachErrorMessage(const DuckLakeConfig& ducklake, const std::string& raw);
    std::string renderCacheTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);
    
    // Write operation methods
    WriteResult executeWrite(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    WriteResult executeWriteInTransaction(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);

private:
    // The single-attempt body; executeQuery wraps it in lock-contention retry.
    QueryResult executeQueryOnce(const EndpointConfig& endpoint, std::map<std::string, std::string>& params, bool with_pagination);
public:
    
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

    // W3.1 PR B: prepared-statement path counterpart to `executeQuery(string, ...)`.
    // `sql` carries `?` placeholders, `bindings` records the per-placeholder
    // type and field name, `bind_values` is the user param map (pre-Mustache
    // strip) from which values are pulled. `for_template_params` is the
    // post-Mustache map used for pagination + next-link composition.
    QueryResult executeQueryWithBindings(const std::string& sql,
                                         const std::vector<PreparedBindingSpec>& bindings,
                                         const std::map<std::string, std::string>& bind_values,
                                         const std::map<std::string, std::string>& for_template_params,
                                         bool with_pagination);

    void logDuckDBVersion();
    
    void createAndInitializeDuckDBConfig(std::shared_ptr<ConfigManager> config_manager, duckdb_config& config);
   

    std::string processTemplate(const EndpointConfig& endpoint, std::map<std::string, std::string>& params);
    std::string processCacheTemplate(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params);
    
    // Internal helper for write operations (used by executeWriteInTransaction)
    WriteResult executeWrite(QueryExecutor& executor, const EndpointConfig& endpoint, std::map<std::string, std::string>& params);

    // #116: a concurrent read and write against a DuckDB SQLite attachment
    // deadlock each other, and the attachment never answers again. One mutex
    // per connection that needs it, so parquet / BigQuery / DuckLake traffic
    // keeps running in parallel and only the affected backend pays.
    // Locked in sorted name order, so an endpoint naming two serialised
    // connections cannot deadlock against one naming them the other way round.
    using AccessLocks = std::vector<std::unique_lock<std::mutex>>;
    AccessLocks lockSerialisedAccess(const EndpointConfig& endpoint);
    /// The locks a READ of this endpoint needs: none when it is served from its
    /// DuckLake cache (cache.enabled with a template-file, so the serving
    /// template reads the cache table and not the source), else the same as
    /// lockSerialisedAccess. Writes always take lockSerialisedAccess.
    AccessLocks lockSerialisedReadAccess(const EndpointConfig& endpoint);

    std::mutex access_mutexes_guard;
    std::unordered_map<std::string, std::unique_ptr<std::mutex>> access_mutexes;

    // Shared by reset() and shutdown(). Requires db_mutex to be held.
    void closeDatabaseLocked(bool graceful);

    duckdb_database db; // Database handle

    // duckdb.default_order, applied per connection when DuckLake is enabled
    // (see createAndInitializeDuckDBConfig); empty otherwise.
    std::optional<std::string> session_default_order_;

    // True only once initializeDBManagerFromConfig() has run to completion.
    // shutdown() detaches and checkpoints only when it is set: a startup that
    // failed part-way wrote nothing, so there is nothing to flush (#145).
    bool initialized = false;
    // The DuckLake catalog was attached (so shutdown detaches it).
    bool ducklake_attached_ = false;
    std::mutex db_mutex; // Mutex for thread safety
    std::shared_ptr<CacheManager> cache_manager;
    std::shared_ptr<SQLTemplateProcessor> sql_processor;
    std::shared_ptr<ConfigManager> config_manager;
};

} // namespace flapi