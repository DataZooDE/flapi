#include <stdexcept>
#include <filesystem>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <cctype>
#include "crow/json.h"
#include <yaml-cpp/yaml.h>

#include "duckdb.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

#include "sql_template_processor.hpp"
#include "sql_utils.hpp"
#include "database_manager.hpp"
#include "cache_refresh_command.hpp"
#include "extension_installer.hpp"
#include <chrono>
#include <thread>
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

    // Graceful whenever there is a config to read the DuckLake alias from -
    // the behaviour reset() always had, so tests that re-initialise between
    // cases still release the catalog lock before reopening it.
    closeDatabaseLocked(/*graceful=*/config_manager != nullptr);

    // Clear all internal state
    cache_manager.reset();
    sql_processor.reset();
    config_manager.reset();

    CROW_LOG_DEBUG << "DatabaseManager reset: cleared all state";
}

void DatabaseManager::shutdown() {
    std::lock_guard<std::mutex> lock(db_mutex);
    // Only a manager that finished initialising has anything to detach or
    // flush. A failed startup wrote nothing (#145), and closing is enough.
    closeDatabaseLocked(/*graceful=*/initialized);
}

void DatabaseManager::closeDatabaseLocked(bool graceful) {
    if (!db) {
        return;
    }

    // Raw connections, deliberately - not executeInitStatement(). That goes
    // through QueryExecutor and the active-executor registry, takes db_mutex
    // (already held here), and is the path both #145 backtraces ran through.
    // Detach and checkpoint need none of it.
    auto run = [this](const char* sql, std::string& error) -> bool {
        duckdb_connection conn;
        if (duckdb_connect(db, &conn) != DuckDBSuccess) {
            error = "could not open a connection";
            return false;
        }
        duckdb_result result;
        const bool ok = duckdb_query(conn, sql, &result) == DuckDBSuccess;
        if (!ok) {
            const char* message = duckdb_result_error(&result);
            error = message ? message : "unknown error";
        }
        duckdb_destroy_result(&result);
        duckdb_disconnect(&conn);
        return ok;
    };

    if (graceful && config_manager) {
        std::string error;
        const auto& ducklake_config = config_manager->getDuckLakeConfig();
        // Only what was attached: a one-shot refresh that lost the lock race
        // printed "Failed to detach DuckLake catalog ... database not found".
        if (ducklake_config.enabled && ducklake_attached_) {
            const std::string detach = "DETACH " + ducklake_config.alias + ";";
            if (run(detach.c_str(), error)) {
                CROW_LOG_INFO << "Detached DuckLake catalog: " << ducklake_config.alias;
            } else {
                CROW_LOG_WARNING << "Failed to detach DuckLake catalog "
                                 << ducklake_config.alias << ": " << error;
            }
        }
        if (run("CHECKPOINT;", error)) {
            CROW_LOG_INFO << "Checkpointed database";
        } else {
            CROW_LOG_WARNING << "Failed to checkpoint database: " << error;
        }
    }

    duckdb_close(&db);
    db = nullptr;
    initialized = false;
    ducklake_attached_ = false;
    CROW_LOG_DEBUG << "DatabaseManager: closed database";
}

DatabaseManager::~DatabaseManager() {
    // No SQL here, ever (#147).
    //
    // This destructor runs from the static shared_ptr's release inside exit(),
    // after the statics that SQL execution depends on - ours and DuckDB's -
    // have been destroyed. It used to DETACH and CHECKPOINT from here, and on
    // a failed startup that segfaulted 6 times in 12:
    //
    //   #0 flapi::registerActiveExecutor(...)                   <- ours
    //   #2 flapi::DatabaseManager::executeInitStatement("CHECKPOINT;")
    //   #3 flapi::DatabaseManager::~DatabaseManager()
    //
    //   #0 duckdb::Value::operator=(duckdb::Value const&)       <- DuckDB's
    //   #2 duckdb::GlobalUserSettings::TryGetSetting(...)
    //   #9 flapi::DatabaseManager::executeInitStatement("CHECKPOINT;")
    //
    // On a normal shutdown it had a luckier destruction order, and its log
    // lines never appeared in a real shutdown log - so whether it worked was
    // never observable either. The graceful work now happens in shutdown(),
    // which main() calls while the process is demonstrably alive: after the
    // handler pool is drained and the warmup thread joined, so nothing is
    // still reading or writing the catalog being detached.
    //
    // All that is left for here is the case where shutdown() never ran (an
    // early return), and closing - without SQL - is safe even then: measured
    // 0 crashes in 20 runs on exactly that path.
    if (db) {
        duckdb_close(&db);
    }
}

namespace {

// The local filesystem path a DuckLake metadata-path names, if it names one:
// a bare path, or one behind a `duckdb:` / `sqlite:` prefix. Postgres/MySQL DSNs
// and object-store URLs have no local parent directory to prepare.
std::optional<std::filesystem::path> localCatalogFile(const std::string& metadata_path) {
    std::string path = metadata_path;
    for (const char* prefix : {"duckdb:", "sqlite:"}) {
        if (path.rfind(prefix, 0) == 0) {
            path = path.substr(std::string(prefix).size());
            break;
        }
    }
    if (path.empty() || path.find("://") != std::string::npos) {
        return std::nullopt;
    }
    const auto colon = path.find(':');
    // `postgres:dbname=...`, `mysql:...`; a Windows drive letter (`C:`) is local.
    if (colon != std::string::npos && colon != 1) {
        return std::nullopt;
    }
    return std::filesystem::path(path);
}

bool isLocalDirectory(const std::string& data_path) {
    return !data_path.empty() && data_path.find("://") == std::string::npos;
}

} // namespace

std::optional<std::string> DatabaseManager::PrepareDuckLakeDirectories(const DuckLakeConfig& ducklake) {
    namespace fs = std::filesystem;
    std::vector<fs::path> needed;
    if (const auto file = localCatalogFile(ducklake.metadata_path)) {
        const auto parent = file->parent_path();
        if (!parent.empty()) {
            needed.push_back(parent);
        }
    }
    if (isLocalDirectory(ducklake.data_path)) {
        needed.push_back(fs::path(ducklake.data_path));
    }
    for (const auto& dir : needed) {
        std::error_code ec;
        if (fs::is_directory(dir, ec)) {
            continue;
        }
        if (ducklake.read_only) {
            return "directory " + RedactConnectionSecrets(dir.string()) +
                   " does not exist (ducklake.read-only: a replica does not create the catalog; "
                   "start the writer first or fix metadata-path/data-path)";
        }
        fs::create_directories(dir, ec);
        if (ec) {
            return "cannot create directory " + RedactConnectionSecrets(dir.string()) + ": " + ec.message();
        }
        CROW_LOG_INFO << "Created DuckLake directory " << RedactConnectionSecrets(dir.string());
    }
    return std::nullopt;
}

std::string DatabaseManager::DuckLakeAttachErrorMessage(const DuckLakeConfig& ducklake, const std::string& raw) {
    // The raw error quotes the whole ATTACH statement, metadata-path DSN included
    // - `password=...` and all - and went to the log and stderr in clear.
    std::string detail = RedactConnectionSecrets(raw);
    if (!ducklake.metadata_path.empty()) {
        const std::string redacted = RedactConnectionSecrets(ducklake.metadata_path);
        for (std::size_t pos = detail.find(ducklake.metadata_path); pos != std::string::npos;
             pos = detail.find(ducklake.metadata_path, pos + redacted.size())) {
            detail.replace(pos, ducklake.metadata_path.size(), redacted);
        }
    }
    if (IsCatalogLockError(raw)) {
        // A DuckDB-file catalog is single-process: a replica cannot open it while
        // the writer (or another replica) holds it. The raw IO error said neither.
        return "Failed to attach DuckLake catalog: another process holds the lock on the DuckDB-file catalog " +
               RedactConnectionSecrets(ducklake.metadata_path) +
               ". A DuckDB-file catalog serves one process at a time: stop the other flapi (the writer or a "
               "read-only replica), or use a Postgres/SQLite/MySQL DuckLake catalog to run a writer with "
               "read-only replicas. (" + detail + ")";
    }
    return "Failed to attach DuckLake catalog: " + detail;
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

        // Extensions first: the version query already needs core_functions, and offline
        // a missing one must fail with loadDefaultExtensions' message, not an autoload error.
        loadDefaultExtensions(config_manager);
        logDuckDBVersion();
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

            if (ducklake_config.read_only) {
                attach_stmt += ", READ_ONLY";
                CROW_LOG_INFO << "DuckLake catalog attached read-only";
            }

            attach_stmt += ");";

            // A missing parent directory used to surface as DuckDB's garbled
            // "...path +" IO error. A writer creates what it needs; a read-only
            // replica must not create an empty catalog, so it says what is missing.
            if (const auto problem = PrepareDuckLakeDirectories(ducklake_config)) {
                throw std::runtime_error("Failed to attach DuckLake catalog: " + *problem);
            }

            try {
                executeInitStatement(attach_stmt);
                ducklake_attached_ = true;
            } catch (const std::exception& e) {
                throw std::runtime_error(DuckLakeAttachErrorMessage(ducklake_config, e.what()));
            }
        }

        sql_processor = std::make_shared<SQLTemplateProcessor>(config_manager);

        cache_manager = std::make_unique<CacheManager>(shared_from_this());
        cache_manager->initializeReadiness(config_manager);

        // Last statement, deliberately: everything above can throw, and the
        // destructor must be able to tell a half-built manager from a working
        // one.
        initialized = true;
    }
}

QueryExecutor DatabaseManager::createQueryExecutor() 
{
    if (db == nullptr) {
        throw std::runtime_error("Database not initialized");
    }
    // session_default_order_: see createAndInitializeDuckDBConfig. A session
    // setting, so DuckLake's own connection keeps the ascending default it
    // depends on.
    return QueryExecutor(db, session_default_order_);
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
    // Offline: never download; an extension that is not installed is an error, not a fetch.
    const char* autoinstall = config_manager->getDuckDBConfig().offline_extensions ? "0" : "1";
    if (duckdb_set_config(config, "autoinstall_known_extensions", autoinstall) == DuckDBError) {
        duckdb_destroy_config(&config);
        throw std::runtime_error("Failed to set DuckDB configuration: autoinstall_known_extensions");
    }

    if (duckdb_set_config(config, "autoload_known_extensions", "1") == DuckDBError) {
        duckdb_destroy_config(&config);
        throw std::runtime_error("Failed to set DuckDB configuration: autoload_known_extensions");
    }

    // Apply settings from the configuration
    const auto& duckdb_settings = config_manager->getDuckDBConfig().settings;
    session_default_order_.reset();
    for (const auto& [key, value] : duckdb_settings) {
        // `default_order` is never set GLOBALLY while DuckLake is attached.
        // DuckLake runs its catalog queries on its own connection, which takes
        // the global default, and several of them rely on a bare ORDER BY
        // (`ORDER BY table_id, parent_column NULLS FIRST, column_order`). Under
        // a global DESC it read a table's columns in reverse, so every commit
        // into the catalog failed ("Failed to flush changes into DuckLake:
        // Could not convert string 'analytics' to INT64") - every cache build
        // and every audit event. Applied per flapi connection instead, so
        // endpoint and cache templates still see the configured order.
        if (key == "default_order" && config_manager->getDuckLakeConfig().enabled) {
            session_default_order_ = value;
            continue;
        }
        if (duckdb_set_config(config, key.c_str(), value.c_str()) == DuckDBError) {
            duckdb_destroy_config(&config);
            throw std::runtime_error("Failed to set DuckDB configuration: " + key);
        }
    }
}

void DatabaseManager::loadDefaultExtensions(std::shared_ptr<ConfigManager> config_manager) {
    auto extensions = config_manager->getDuckDBConfig().default_extensions;
    const bool offline = config_manager->getDuckDBConfig().offline_extensions;
    if (offline) {
        // Online, DuckDB autoinstalls these on first use; offline they must be checked here.
        for (const auto& implicit : ImplicitExtensions()) {
            if (std::find(extensions.begin(), extensions.end(), implicit) == extensions.end()) {
                extensions.insert(extensions.begin(), implicit);
            }
        }
    }

    // Install the extensions (try individually to handle failures gracefully).
    // Offline skips this: INSTALL is a download, and the point is that none happens.
    CROW_LOG_INFO << (offline ? "Offline extensions: skipping install of " : "Installing default ")
                  << extensions.size() << " extensions";
    for (const auto& extension : offline ? std::vector<std::string>{} : extensions) {
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
    std::vector<std::string> missing;
    for (const auto& extension : extensions) {
        std::string load_stmt = "LOAD " + extension + ";";
        try {
            executeInitStatement(load_stmt);
            CROW_LOG_DEBUG << "Successfully loaded extension: " << extension;
            loaded_count++;
        } catch (const std::exception& e) {
            CROW_LOG_WARNING << "Failed to load extension '" << extension << "': " << e.what() << ". Skipping.";
            missing.push_back(extension);
        }
    }
    if (offline && !missing.empty()) {
        std::string names;
        for (const auto& name : missing) {
            names += (names.empty() ? "" : ", ") + name;
        }
        throw std::runtime_error("Extension(s) not installed and FLAPI_EXTENSIONS_OFFLINE forbids downloading: " +
                                 names + ". Pre-install them with `flapi extensions install --config <flapi.yaml> "
                                 "--dir <dir>` and set FLAPI_EXTENSION_DIRECTORY=<dir>, or trim "
                                 "duckdb.default-extensions.");
    }
    CROW_LOG_INFO << "Successfully loaded " << loaded_count << " out of " << extensions.size() << " extensions";
}

void DatabaseManager::initializeConnections(std::shared_ptr<ConfigManager> config_manager) {
    const auto& connections = config_manager->getConnections();
    for (const auto& [connection_name, connection_config] : connections) {
        const auto& init = connection_config.getInit();
        if (!init.empty()) {
            CROW_LOG_DEBUG << "Executing init statement for connection: " << connection_name;
            // Named: with several connections, "IO Error: Extension ... not
            // found" at startup did not say whose init failed.
            try {
                executeInitStatement(init);
            } catch (const std::exception& e) {
                throw std::runtime_error("connection '" + connection_name + "' init: " + e.what());
            }
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

// SQLite permits a single writer, and DuckDB's sqlite_scanner surfaces that as
// "database is locked". It is TRANSIENT and retryable - which is the whole
// point: a 500 tells a client its request was wrong, when in fact it only
// arrived at a bad moment. DuckDB 1.5.5 exposes no sqlite_busy_timeout setting
// to push the wait down to the driver, so flAPI waits it out itself.
bool isLockContentionMessage(const std::string& message) {
    static constexpr std::string_view kNeedles[] = {
        "database is locked",
        "database table is locked",
        "SQLITE_BUSY",
    };
    for (const auto needle : kNeedles) {
        if (message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

namespace {

// Bounded retry with backoff. Deliberately short: the caller is a live HTTP
// request, so waiting longer trades one problem for another. If the lock
// outlives the budget the error propagates and the handler maps it to 503 with
// Retry-After - the honest answer is "try again", not "you broke something".
template <typename Fn>
auto retryOnLockContention(Fn&& fn, const char* what) -> decltype(fn()) {
    constexpr int kMaxAttempts = 5;
    auto delay = std::chrono::milliseconds(10);
    for (int attempt = 1;; ++attempt) {
        try {
            return fn();
        } catch (const std::exception& e) {
            if (attempt >= kMaxAttempts || !isLockContentionMessage(e.what())) {
                throw;
            }
            CROW_LOG_DEBUG << "lock contention on " << what << ", attempt "
                           << attempt << " of " << kMaxAttempts
                           << "; retrying in " << delay.count() << "ms";
            std::this_thread::sleep_for(delay);
            delay *= 2;
        }
    }
}

}  // namespace

DatabaseManager::AccessLocks DatabaseManager::lockSerialisedAccess(const EndpointConfig& endpoint) {
    AccessLocks locks;
    if (!config_manager) {
        return locks;
    }

    // Sorted, so every caller acquires the same connections in the same order.
    std::vector<std::string> names;
    const auto& connections = config_manager->getConnections();
    for (const auto& name : endpoint.connection) {
        const auto it = connections.find(name);
        if (it != connections.end() && it->second.serialisesAccess()) {
            names.push_back(name);
        }
    }
    if (names.empty()) {
        return locks;
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    for (const auto& name : names) {
        std::mutex* m = nullptr;
        {
            std::lock_guard<std::mutex> guard(access_mutexes_guard);
            auto& slot = access_mutexes[name];
            if (!slot) {
                slot = std::make_unique<std::mutex>();
            }
            m = slot.get();
        }
        locks.emplace_back(*m);
    }
    return locks;
}

DatabaseManager::AccessLocks DatabaseManager::lockSerialisedReadAccess(const EndpointConfig& endpoint) {
    // A cached endpoint with a template-file serves from DuckLake: its serving
    // template reads the cache table, and the source is touched only by the
    // populate template. Taking the source's lock here serialised every cached
    // read behind every other (16 clients: 93 rps, p50 168 ms; 283 rps, p50
    // 56 ms without it) to protect a connection the read never opens. Without
    // a template-file the serving template IS the source query, so it locks.
    if (isCacheEnabled(endpoint) && endpoint.cache.template_file) {
        return {};
    }
    return lockSerialisedAccess(endpoint);
}

QueryResult DatabaseManager::executeQuery(const EndpointConfig& endpoint, std::map<std::string, std::string>& params, bool with_pagination)
{
    // #116: a read concurrent with a write wedges a SQLite attachment for good,
    // so on those connections a read waits its turn like a write does.
    const auto access = lockSerialisedReadAccess(endpoint);

    // Reads are collateral damage in SQLite lock contention: a single writer
    // blocks them, and failing a GET because some unrelated POST held a lock is
    // the least defensible outcome of the lot.
    return retryOnLockContention([&]() -> QueryResult {
        return executeQueryOnce(endpoint, params, with_pagination);
    }, "query");
}

QueryResult DatabaseManager::executeQueryOnce(const EndpointConfig& endpoint, std::map<std::string, std::string>& params, bool with_pagination)
{
    cache_manager->addQueryCacheParamsIfNecessary(config_manager, endpoint, params);

    // W3.1 PR B: route typed scalar params through the prepared path.
    // The rewriter is conservative — endpoints without typed validators
    // produce an empty binding plan, so the call collapses to the
    // historic string-execute path with no behaviour change.
    //
    // We hold onto `params` *before* template rendering because
    // loadAndProcessTemplatePrepared mutates the map (it strips cache/
    // auth keys when assembling the Mustache context). The binder needs
    // the pre-strip map to look up the user-supplied param values.
    std::map<std::string, std::string> bind_values = params;
    auto prepared = sql_processor->loadAndProcessTemplatePrepared(endpoint, params);

    if (prepared.bindings.empty()) {
        return executeQuery(prepared.sql, params, with_pagination);
    }
    return executeQueryWithBindings(prepared.sql, prepared.bindings, bind_values, params, with_pagination);
}

QueryResult DatabaseManager::executeCacheQuery(const EndpointConfig& endpoint, const CacheConfig& cacheConfig, std::map<std::string, std::string>& params) 
{   
    std::string processedQuery = processCacheTemplate(endpoint, cacheConfig, params);
    return executeQuery(processedQuery, params, false);
}

QueryResult DatabaseManager::executeDuckLakeQuery(const std::string& query, const std::map<std::string, std::string>& params) {
    return executeQuery(query, params, false);
}

namespace {

std::string quotePopulateIdentifier(const std::string& name) {
    std::string out = "\"";
    for (const char c : name) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string quotePopulateLiteral(const std::string& value) {
    std::string out = "'";
    for (const char c : value) {
        out += c;
        if (c == '\'') {
            out += '\'';
        }
    }
    return out + "'";
}

std::string qualifiedTarget(const PopulateTarget& target) {
    return quotePopulateIdentifier(target.catalog) + "." + quotePopulateIdentifier(target.schema) + "." +
           quotePopulateIdentifier(target.table);
}

} // namespace

std::optional<std::size_t> DatabaseManager::FindSelectStatement(const std::string& script,
                                                                std::size_t* statement_count) {
    // DuckDB's own parser, not a regex: `WITH ... INSERT`, `CREATE TABLE ... AS
    // SELECT` and a SELECT inside a string literal or comment must all read
    // correctly. A script the standalone parser cannot read (an extension's
    // syntax) is not judged here; executing it reports whatever is wrong.
    try {
        duckdb::Parser parser;
        parser.ParseQuery(script);
        if (statement_count) {
            *statement_count = parser.statements.size();
        }
        for (std::size_t i = 0; i < parser.statements.size(); ++i) {
            if (parser.statements[i] && parser.statements[i]->type == duckdb::StatementType::SELECT_STATEMENT) {
                return i + 1;
            }
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::vector<std::string> DatabaseManager::SplitStatements(const std::string& script) {
    std::vector<std::string> out;
    std::string current;
    bool meaningful = false;  // anything but whitespace and comments so far
    const std::size_t n = script.size();
    const auto flush = [&]() {
        if (meaningful) {
            out.push_back(current);
        }
        current.clear();
        meaningful = false;
    };
    std::size_t i = 0;
    while (i < n) {
        const char c = script[i];
        if (c == '-' && i + 1 < n && script[i + 1] == '-') {
            const std::size_t end = script.find('\n', i);
            const std::size_t stop = end == std::string::npos ? n : end;
            current.append(script, i, stop - i);
            i = stop;
            continue;
        }
        if (c == '/' && i + 1 < n && script[i + 1] == '*') {
            const std::size_t end = script.find("*/", i + 2);
            const std::size_t stop = end == std::string::npos ? n : end + 2;
            current.append(script, i, stop - i);
            i = stop;
            continue;
        }
        if (c == '\'' || c == '"') {
            // '' and "" (the escaped quote) need no special case: the pair
            // closes and immediately re-opens the literal.
            const std::size_t end = script.find(c, i + 1);
            const std::size_t stop = end == std::string::npos ? n : end + 1;
            current.append(script, i, stop - i);
            meaningful = true;
            i = stop;
            continue;
        }
        if (c == '$') {
            // $$ ... $$ or $tag$ ... $tag$; `$1` is a parameter, not a quote.
            std::size_t j = i + 1;
            while (j < n && (std::isalnum(static_cast<unsigned char>(script[j])) || script[j] == '_')) {
                ++j;
            }
            const bool is_quote = j < n && script[j] == '$' &&
                                  (j == i + 1 || !std::isdigit(static_cast<unsigned char>(script[i + 1])));
            if (is_quote) {
                const std::string tag = script.substr(i, j - i + 1);
                const std::size_t end = script.find(tag, j + 1);
                const std::size_t stop = end == std::string::npos ? n : end + tag.size();
                current.append(script, i, stop - i);
                meaningful = true;
                i = stop;
                continue;
            }
        }
        if (c == ';') {
            flush();
            ++i;
            continue;
        }
        if (!std::isspace(static_cast<unsigned char>(c))) {
            meaningful = true;
        }
        current += c;
        ++i;
    }
    flush();
    return out;
}

namespace {

// PlainCreate: a definition that moves no data (table without AS, schema, view,
// macro, sequence). CreateAs: CREATE TABLE ... AS <query>. Create: anything
// else that creates (an index builds over the table's rows).
enum class StatementKind { Unknown, Select, Transaction, PlainCreate, CreateAs, Create, Other };

// What the standalone parser makes of one statement. Unknown when it cannot
// read it (an extension's syntax): then only the connection judges it.
StatementKind ClassifyStatement(const std::string& sql) {
    try {
        duckdb::Parser parser;
        parser.ParseQuery(sql);
        if (parser.statements.size() != 1 || !parser.statements[0]) {
            return StatementKind::Unknown;
        }
        const auto& statement = *parser.statements[0];
        switch (statement.type) {
            case duckdb::StatementType::SELECT_STATEMENT:
                return StatementKind::Select;
            case duckdb::StatementType::TRANSACTION_STATEMENT:
                return StatementKind::Transaction;
            case duckdb::StatementType::CREATE_STATEMENT: {
                const auto& info = statement.Cast<duckdb::CreateStatement>().info;
                if (!info || info->type == duckdb::CatalogType::INDEX_ENTRY) {
                    return StatementKind::Create;
                }
                if (info->type == duckdb::CatalogType::TABLE_ENTRY && info->Cast<duckdb::CreateTableInfo>().query) {
                    return StatementKind::CreateAs;
                }
                return StatementKind::PlainCreate;
            }
            default:
                return StatementKind::Other;
        }
    } catch (const std::exception&) {
        return StatementKind::Unknown;
    }
}

// Whether a DuckDB "... does not exist" message is about `table`.
bool NamesTable(const std::string& message, const std::string& table) {
    if (table.empty()) {
        return false;
    }
    for (const auto& form : {"name " + table + " ", "name " + table + "!", "\"" + table + "\"", "." + table + " ",
                             "." + table + "!"}) {
        if (message.find(form) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

DatabaseManager::ScriptValidation DatabaseManager::validateScript(const std::string& script,
                                                                  const PopulateTarget* populate,
                                                                  const PopulateTarget* cache_table) {
    ScriptValidation out;
    const auto statements = SplitStatements(script);
    out.statements = statements.size();
    if (statements.empty()) {
        out.errors.push_back({0, "parse", "the template renders no SQL statement"});
        return out;
    }

    auto executor = createQueryExecutor();
    duckdb_connection conn = executor.conn;

    // Everything below runs inside ONE transaction that is always rolled back,
    // so nothing it does - the plain CREATEs it replays - is ever committed. A
    // failed statement can leave the transaction unusable, so after one it is
    // restarted and the CREATEs so far replayed.
    std::vector<std::string> replay;
    const auto begin = [&]() {
        if (duckdb_query(conn, "BEGIN TRANSACTION", nullptr) == DuckDBError) {
            return false;
        }
        for (const auto& create : replay) {
            duckdb_query(conn, create.c_str(), nullptr);
        }
        return true;
    };
    const auto rollback = [&]() { duckdb_query(conn, "ROLLBACK", nullptr); };
    bool in_transaction = begin();

    bool creates_unrun = false;  // an earlier CREATE this check did not run
    for (std::size_t i = 0; i < statements.size(); ++i) {
        const std::size_t number = i + 1;
        const std::string& sql = statements[i];

        // The CONNECTION's parser, so syntax an extension adds is accepted.
        duckdb_extracted_statements extracted = nullptr;
        const idx_t count = duckdb_extract_statements(conn, sql.c_str(), &extracted);
        if (count == 0) {
            const char* raw = extracted ? duckdb_extract_statements_error(extracted) : nullptr;
            out.errors.push_back({number, "parse", raw ? raw : "statement could not be parsed"});
            duckdb_destroy_extracted(&extracted);
            continue;
        }

        const StatementKind kind = ClassifyStatement(sql);
        if (kind == StatementKind::Transaction) {
            // The script's own BEGIN/COMMIT: this check's transaction stands in.
            duckdb_destroy_extracted(&extracted);
            continue;
        }
        if (populate && kind == StatementKind::Select) {
            out.errors.push_back(
                {number, "bare_select", PopulateSelectError(*populate, number, statements.size())});
            duckdb_destroy_extracted(&extracted);
            continue;
        }

        // Preparing binds and plans - what EXPLAIN checks - and runs nothing.
        duckdb_prepared_statement prepared = nullptr;
        std::optional<std::string> failure;
        if (duckdb_prepare_extracted_statement(conn, extracted, 0, &prepared) == DuckDBError) {
            const char* raw = prepared ? duckdb_prepare_error(prepared) : nullptr;
            failure = raw ? raw : "statement could not be prepared";
        } else if (kind == StatementKind::PlainCreate && in_transaction) {
            // A definition without data, created in the doomed transaction so
            // the MERGE/INSERT after it binds against the real columns.
            duckdb_result result;
            if (duckdb_execute_prepared(prepared, &result) == DuckDBError) {
                const char* raw = duckdb_result_error(&result);
                failure = raw ? raw : "statement failed";
            } else {
                replay.push_back(sql);
            }
            duckdb_destroy_result(&result);
        }
        duckdb_destroy_prepare(&prepared);
        duckdb_destroy_extracted(&extracted);
        bool created = false;
        if (!failure && kind == StatementKind::CreateAs && in_transaction) {
            // `WITH NO DATA`: the table with the query's columns and no rows -
            // the query is planned, not run - so what follows binds against it.
            // Not every CTAS form accepts the suffix; one that does not just
            // leaves its table unchecked.
            const std::string no_data = sql + "\nWITH NO DATA";
            if (duckdb_query(conn, no_data.c_str(), nullptr) != DuckDBError) {
                replay.push_back(no_data);
                created = true;
            } else {
                rollback();
                in_transaction = begin();
            }
        }

        if (failure) {
            const std::string& message = *failure;
            const bool missing = message.find("does not exist") != std::string::npos;
            if (missing && cache_table && NamesTable(message, cache_table->table)) {
                out.warnings.push_back(
                    {number, "cache_not_built",
                     "cache table " + cache_table->catalog + "." + cache_table->schema + "." + cache_table->table +
                         " does not exist yet (cache not built); statement " + std::to_string(number) +
                         " was not checked against it"});
            } else if (missing && creates_unrun) {
                out.warnings.push_back({number, "not_checked",
                                        "statement " + std::to_string(number) +
                                            " was not checked: it uses a table an earlier statement creates (" +
                                            message + ")"});
            } else if (message.find("Could not determine type of parameters") != std::string::npos) {
                out.warnings.push_back(
                    {number, "not_checked", "statement " + std::to_string(number) + " was not fully checked: " + message});
            } else {
                out.errors.push_back({number, "bind", message});
            }
            if (in_transaction) {
                rollback();
                in_transaction = begin();
            }
        }
        if (kind == StatementKind::Create || kind == StatementKind::Unknown ||
            (kind == StatementKind::CreateAs && !created) || (kind == StatementKind::PlainCreate && failure)) {
            creates_unrun = true;
        }
    }
    if (in_transaction) {
        rollback();
    }
    return out;
}

std::optional<std::string> DatabaseManager::MaterialiseSelect(const std::string& script, const PopulateTarget& target) {
    try {
        duckdb::Parser parser;
        parser.ParseQuery(script);
        if (parser.statements.size() != 1 || !parser.statements[0] ||
            parser.statements[0]->type != duckdb::StatementType::SELECT_STATEMENT) {
            return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    // The text as rendered, not the parser's re-serialisation: what DuckDB runs
    // is then exactly what the template author wrote. A trailing `;` or
    // comment is harmless at the end of CREATE ... AS.
    return "CREATE OR REPLACE TABLE " + qualifiedTarget(target) + " AS\n" + script;
}

std::string DatabaseManager::PopulateSelectError(const PopulateTarget& target, std::size_t statement,
                                                 std::size_t statement_count) {
    // A SELECT before the last statement is usually a guard or a debugging
    // probe in a script that DOES write the table; "builds nothing" sent its
    // author looking for a missing write that was there all along.
    if (statement_count > 1 && statement < statement_count) {
        return "cache template statement " + std::to_string(statement) + " of " + std::to_string(statement_count) +
               " is a bare SELECT; its result is discarded. Wrap a check in CREATE OR REPLACE TEMP TABLE _chk AS "
               "SELECT ... (the template must write {{cache.catalog}}.{{cache.schema}}.{{cache.table}}, here " +
               target.catalog + "." + target.schema + "." + target.table + ")";
    }
    return "cache template must write {{cache.catalog}}.{{cache.schema}}.{{cache.table}} (here " +
           target.catalog + "." + target.schema + "." + target.table +
           "), e.g. CREATE OR REPLACE TABLE ... AS SELECT ...; statement " + std::to_string(statement) +
           " of the template is a bare SELECT, which builds nothing";
}

std::optional<int64_t> DatabaseManager::executeDuckLakeScript(const std::string& script,
                                                              const PopulateTarget* target,
                                                              bool rebuild) {
    // Refused BEFORE anything runs. A populate template that is (or ends in) a
    // bare SELECT used to "succeed": the result was materialised - all of it, on
    // a 10M-row source - and thrown away, nothing was built, the refresh was
    // recorded as a success, and the endpoint then answered 500 on a missing
    // table.
    if (target) {
        std::size_t statement_count = 0;
        if (const auto select_at = FindSelectStatement(script, &statement_count)) {
            throw std::runtime_error(PopulateSelectError(*target, *select_at, statement_count));
        }
    }

    auto executor = createQueryExecutor();

    duckdb_extracted_statements stmts = nullptr;
    const idx_t n_statements = duckdb_extract_statements(executor.conn, script.c_str(), &stmts);
    if (n_statements == 0) {
        const char* raw = stmts ? duckdb_extract_statements_error(stmts) : nullptr;
        const std::string error_message = raw ? raw : "no statements";
        duckdb_destroy_extracted(&stmts);
        throw std::runtime_error("Query execution failed: " + error_message);
    }

    // A refresh template used to run as one duckdb_query() in autocommit mode:
    // each statement committed on its own. A failure halfway (the MERGE after a
    // DELETE, or after the CREATE TABLE that bootstraps the first build) left
    // the earlier statements applied while the refresh reported failure - rows
    // deleted from a cache that "kept serving the last snapshot", or an empty
    // table that `warmup: if-empty` then took for a finished build.
    //
    // The probe uses its OWN extraction: preparing an extracted statement moves
    // it out of the set, so a second prepare of the same index gets nothing.
    bool own_transaction = false;
    {
        duckdb_extracted_statements probe_set = nullptr;
        const idx_t n_probe = duckdb_extract_statements(executor.conn, script.c_str(), &probe_set);
        for (idx_t i = 0; i < n_probe && !own_transaction; i++) {
            duckdb_prepared_statement probe = nullptr;
            // Preparing a later statement can fail before earlier ones ran (it
            // may name a table they create); that is not a transaction statement.
            if (duckdb_prepare_extracted_statement(executor.conn, probe_set, i, &probe) != DuckDBError &&
                duckdb_prepared_statement_type(probe) == DUCKDB_STATEMENT_TYPE_TRANSACTION) {
                own_transaction = true;
            }
            duckdb_destroy_prepare(&probe);
        }
        duckdb_destroy_extracted(&probe_set);
    }

    std::optional<int64_t> total;
    const auto run = [&]() {
        for (idx_t i = 0; i < n_statements; i++) {
            duckdb_prepared_statement stmt = nullptr;
            if (duckdb_prepare_extracted_statement(executor.conn, stmts, i, &stmt) == DuckDBError) {
                const char* raw = stmt ? duckdb_prepare_error(stmt) : nullptr;
                const std::string error_message = raw ? raw : "statement could not be prepared";
                duckdb_destroy_prepare(&stmt);
                throw std::runtime_error("Query execution failed: " + error_message);
            }
            try {
                executor.executePrepared(stmt, "cache refresh");
            } catch (...) {
                duckdb_destroy_prepare(&stmt);
                throw;
            }
            duckdb_destroy_prepare(&stmt);
            // DML (INSERT/UPDATE/DELETE/MERGE, and CREATE TABLE ... AS) answers
            // with one BIGINT column named Count.
            if (duckdb_column_count(&executor.result) == 1 &&
                std::string(duckdb_column_name(&executor.result, 0)) == "Count" &&
                duckdb_row_count(&executor.result) == 1) {
                total = total.value_or(0) + duckdb_value_int64(&executor.result, 0, 0);
            }
        }
    };

    // The script must leave its target existing. Asked before COMMIT, inside the
    // transaction, so a script that built nothing rolls back like any failure.
    const auto require_target = [&]() {
        if (!target) {
            return;
        }
        executor.execute("SELECT count(*) FROM duckdb_tables() WHERE database_name = " +
                             quotePopulateLiteral(target->catalog) + " AND schema_name = " +
                             quotePopulateLiteral(target->schema) + " AND table_name = " +
                             quotePopulateLiteral(target->table),
                         "cache refresh");
        if (duckdb_row_count(&executor.result) != 1 || duckdb_value_int64(&executor.result, 0, 0) == 0) {
            throw std::runtime_error("cache template ran but did not create " + target->catalog + "." +
                                     target->schema + "." + target->table +
                                     "; it must write {{cache.catalog}}.{{cache.schema}}.{{cache.table}}, "
                                     "e.g. CREATE OR REPLACE TABLE ... AS SELECT ...");
        }
    };

    try {
        if (!own_transaction) {
            executor.execute("BEGIN TRANSACTION", "cache refresh");
            if (rebuild && target) {
                // Same transaction as the template: readers keep the old
                // snapshot until COMMIT, and a failing rebuild restores it.
                executor.execute("DROP TABLE IF EXISTS " + qualifiedTarget(*target), "cache refresh");
            }
        } else if (rebuild && target) {
            CROW_LOG_WARNING << "Full rebuild of " << target->schema << "." << target->table
                             << ": the template manages its own transaction, so the table is not "
                                "dropped first; only the watermark is cleared.";
        }
        run();
        require_target();
        if (!own_transaction) {
            executor.execute("COMMIT", "cache refresh");
        }
    } catch (...) {
        if (!own_transaction) {
            try {
                executor.execute("ROLLBACK", "cache refresh");
            } catch (...) {
                // The connection is discarded right after; DuckDB rolls back an
                // open transaction on disconnect.
            }
        }
        duckdb_destroy_extracted(&stmts);
        throw;
    }
    duckdb_destroy_extracted(&stmts);
    return total;
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
    const auto access = lockSerialisedReadAccess(endpoint);
    cache_manager->addQueryCacheParamsIfNecessary(config_manager, endpoint, params);

    // W3.1 PR B: Arrow-streaming endpoint also takes the prepared path
    // when the endpoint has typed scalar params; falls back to direct
    // execute when the binding plan is empty (no behaviour change).
    std::map<std::string, std::string> bind_values = params;
    auto prepared = sql_processor->loadAndProcessTemplatePrepared(endpoint, params);

    auto executor = std::make_unique<QueryExecutor>(db, session_default_order_);
    if (prepared.bindings.empty()) {
        executor->execute(prepared.sql);
    } else {
        executor->executeWithBindings(prepared.sql, prepared.bindings, bind_values, "arrow streaming");
    }
    return executor;
}

QueryResult DatabaseManager::executeQueryWithBindings(
    const std::string& sql,
    const std::vector<PreparedBindingSpec>& bindings,
    const std::map<std::string, std::string>& bind_values,
    const std::map<std::string, std::string>& for_template_params,
    bool with_pagination)
{
    auto executor = createQueryExecutor();
    std::string paginatedQuery = sql;
    std::string countQuery = "SELECT COUNT(*) FROM (" + sql + ") AS subquery";

    int64_t limit = 0;
    int64_t offset = 0;
    bool hasPagination = false;

    if (with_pagination &&
        (for_template_params.find("limit") != for_template_params.end() ||
         for_template_params.find("offset") != for_template_params.end())) {
        hasPagination = true;
        if (for_template_params.find("limit") != for_template_params.end()) {
            limit = std::stoll(for_template_params.find("limit")->second);
        }
        if (for_template_params.find("offset") != for_template_params.end()) {
            offset = std::stoll(for_template_params.find("offset")->second);
        }
        paginatedQuery = "SELECT * FROM (" + paginatedQuery + ") AS subquery "
                         "LIMIT " + std::to_string(limit) +
                         " OFFSET " + std::to_string(offset);
    }

    // Run the main (possibly wrapped) query against the prepared path.
    executor.executeWithBindings(paginatedQuery, bindings, bind_values, "main query");

    QueryResult result;
    result.data = executor.toJson();
    result.columns = executor.columnNames();

    if (with_pagination) {
        auto countExecutor = createQueryExecutor();
        countExecutor.executeWithBindings(countQuery, bindings, bind_values, "count query");
        if (countExecutor.rowCount() > 0) {
            result.total_count = duckdb_value_int64(&countExecutor.result, 0, 0);
        }

        if (hasPagination && offset + limit < result.total_count) {
            std::stringstream next_ss;
            next_ss << "?offset=" << (offset + limit);
            for (const auto& [key, value] : for_template_params) {
                if (key != "offset") {
                    next_ss << "&" << key << "=" << value;
                }
            }
            result.next = next_ss.str();
        }
    }

    return result;
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
    result.columns = executor.columnNames();
    
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
    const auto access = lockSerialisedAccess(endpoint);
    auto executor = createQueryExecutor();
    return executeWrite(executor, endpoint, params);
}

WriteResult DatabaseManager::executeWrite(QueryExecutor& executor, const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    WriteResult result;

    // W3.1 PR B: route writes through the prepared path. The rewriter
    // produces SQL with `?` placeholders for typed scalar refs; we
    // distribute the binding plan across statements that
    // `splitSqlStatements` produces, so multi-statement INSERT;SELECT
    // and RETURNING workflows keep working — each statement is its own
    // prepared statement with the relevant slice of the binding plan.
    std::map<std::string, std::string> bind_values = params;
    auto prepared = sql_processor->loadAndProcessTemplatePrepared(endpoint, params);
    const std::string& processedQuery = prepared.sql;

    auto trim = [](const std::string& str) -> std::string {
        size_t first = str.find_first_not_of(" \t\n\r");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\n\r");
        return str.substr(first, (last - first + 1));
    };
    auto startsWith = [](const std::string& str, const std::string& prefix) -> bool {
        if (str.length() < prefix.length()) return false;
        std::string strLower = str;
        std::string prefixLower = prefix;
        std::transform(strLower.begin(), strLower.end(), strLower.begin(), ::tolower);
        std::transform(prefixLower.begin(), prefixLower.end(), prefixLower.begin(), ::tolower);
        return strLower.compare(0, prefixLower.length(), prefixLower) == 0;
    };

    std::vector<std::string> statements = splitSqlStatements(processedQuery);
    if (statements.empty()) {
        statements.push_back(trim(processedQuery));
    }

    // Slice the binding plan per statement based on `?` count.
    std::vector<std::vector<PreparedBindingSpec>> bindings_per_stmt;
    bindings_per_stmt.reserve(statements.size());
    std::size_t binding_cursor = 0;
    for (const auto& stmt : statements) {
        const std::size_t count = countSqlPlaceholders(stmt);
        std::vector<PreparedBindingSpec> slice;
        slice.reserve(count);
        for (std::size_t k = 0; k < count; ++k) {
            PreparedBindingSpec spec = prepared.bindings.at(binding_cursor + k);
            spec.position = k;   // re-base to the per-statement index
            slice.push_back(std::move(spec));
        }
        binding_cursor += count;
        bindings_per_stmt.push_back(std::move(slice));
    }
    // Defensive: the total ? count across statements must equal the
    // size of the binding plan. A mismatch would indicate a bug in the
    // rewriter (extra ? not in plan, or extra plan entries with no ?).
    if (binding_cursor != prepared.bindings.size()) {
        throw std::runtime_error(
            "Prepared-statement write path: binding plan/placeholder mismatch (" +
            std::to_string(binding_cursor) + " placeholders vs " +
            std::to_string(prepared.bindings.size()) + " bindings)");
    }

    auto runStatement = [&](size_t idx, const std::string& label) {
        if (bindings_per_stmt[idx].empty()) {
            executor.execute(statements[idx], label);
        } else {
            executor.executeWithBindings(statements[idx], bindings_per_stmt[idx], bind_values, label);
        }
    };

    bool hasReturningData = false;
    size_t statementsToExecute = statements.size();
    bool lastStatementIsSelect = false;

    if (statements.size() > 1 && endpoint.operation.returns_data) {
        std::string lastStatement = trim(statements.back());
        lastStatementIsSelect = startsWith(lastStatement, "SELECT");
        if (lastStatementIsSelect) {
            statementsToExecute = statements.size() - 1;
        }
    }

    for (size_t i = 0; i < statementsToExecute; ++i) {
        runStatement(i, "write operation statement " + std::to_string(i + 1));

        std::string currentStatement = trim(statements[i]);
        std::string upperStatement = currentStatement;
        std::transform(upperStatement.begin(), upperStatement.end(), upperStatement.begin(), ::toupper);
        bool hasReturningClause = upperStatement.find("RETURNING") != std::string::npos;

        // rows_affected is what the DML changed, summed over the statements.
        // It used to be the RESULT's row count - and a DML statement without
        // RETURNING answers with ONE row (a BIGINT column named Count), so an
        // UPDATE of 5 rows and a DELETE of a missing id both reported 1.
        // With RETURNING the result is the changed rows themselves.
        if (hasReturningClause) {
            result.rows_affected += static_cast<int64_t>(executor.rowCount());
        } else if (executor.columnCount() == 1 && executor.rowCount() == 1 &&
                   std::string(duckdb_column_name(&executor.result, 0)) == "Count") {
            result.rows_affected += duckdb_value_int64(&executor.result, 0, 0);
        }

        if (hasReturningClause && executor.columnCount() > 0 && executor.rowCount() > 0) {
            result.returned_data = executor.toJson();
            result.returned_columns = executor.columnNames();
            hasReturningData = true;
        }
    }

    if (endpoint.operation.returns_data && !hasReturningData && lastStatementIsSelect && statements.size() > 1) {
        runStatement(statements.size() - 1, "select returning data");
        if (executor.columnCount() > 0 && executor.rowCount() > 0) {
            result.returned_data = executor.toJson();
            result.returned_columns = executor.columnNames();
        }
    }

    return result;
}


WriteResult DatabaseManager::executeWriteInTransaction(const EndpointConfig& endpoint, std::map<std::string, std::string>& params) {
    const auto access = lockSerialisedAccess(endpoint);
    return retryOnLockContention([&]() -> WriteResult {
        WriteResult result;

        auto executor = createQueryExecutor();

        // Tracked so the rollback only runs when there is something to roll
        // back. BEGIN itself can fail - on a locked SQLite database it
        // routinely does - and rolling back then produced "cannot rollback - no
        // transaction is active", compounding every real error with a spurious
        // one and burying the actual cause in the log.
        bool in_transaction = false;

        try {
            executor.execute("BEGIN TRANSACTION", "begin transaction");
            in_transaction = true;

            // Same executor, so the same connection carries the transaction.
            result = executeWrite(executor, endpoint, params);

            executor.execute("COMMIT", "commit transaction");
            in_transaction = false;

        } catch (const std::exception& e) {
            if (in_transaction) {
                try {
                    executor.execute("ROLLBACK", "rollback transaction");
                } catch (const std::exception& rollback_error) {
                    // A failed COMMIT may already have aborted the transaction,
                    // so this is not necessarily a problem - log it without
                    // drowning out the original error below.
                    CROW_LOG_WARNING << "rollback after a failed write did not succeed: "
                                     << rollback_error.what();
                }
            }
            throw;
        }

        return result;
    }, "write transaction");
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
    // The secret document is bound, not interpolated. It comes from the secrets
    // manager rather than from a request, but it is still data: a single
    // apostrophe in any username or password terminated the literal and the
    // INSERT failed to parse, and a secret crafted by whoever can write to the
    // store executed SQL here - inside a DuckDB that reaches the filesystem and
    // every configured cloud backend.
    auto insert_stmt = "INSERT INTO " + secret_table + " VALUES (?)";

    try {
        executor.execute(create_table_stmt, "create table");

        duckdb_prepared_statement stmt = nullptr;
        if (duckdb_prepare(executor.conn, insert_stmt.c_str(), &stmt) == DuckDBError) {
            const char* err = duckdb_prepare_error(stmt);
            std::string detail = err ? err : "(no detail)";
            duckdb_destroy_prepare(&stmt);
            throw std::runtime_error("prepare failed: " + detail);
        }
        if (duckdb_bind_varchar_length(stmt, 1, secret_json.data(), secret_json.size()) == DuckDBError) {
            duckdb_destroy_prepare(&stmt);
            throw std::runtime_error("bind failed");
        }
        try {
            executor.executePrepared(stmt, "insert data");
        } catch (...) {
            duckdb_destroy_prepare(&stmt);
            throw;
        }
        duckdb_destroy_prepare(&stmt);
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to refresh JSON table '" + secret_table + "': " + e.what());
    }
}

std::optional<std::tuple<std::string, std::vector<std::string>>> DatabaseManager::findUserInSecretsTable(const std::string& secret_table, 
                                                                                                         const std::string& username) 
{
    // The username arrives base64-decoded from an `Authorization: Basic`
    // header and is therefore fully attacker-controlled on an UNauthenticated
    // request. It used to be concatenated into this query, which made the
    // header an injection point into the query that decides authentication -
    // and the query returns the stored password AND the role list, both of
    // which the caller then trusts. Controlling the result set therefore meant
    // choosing your own password hash and your own roles: auth bypass plus
    // privilege escalation, not just data disclosure.
    //
    // Bound as a parameter. `secret_table` stays interpolated because it is a
    // table identifier - it cannot be bound - but it comes from the config
    // file, not from the request.
    std::stringstream query;

    query << "SELECT y->>'username' AS username, y->>'password' AS password, CAST(json_extract_string(y, '$.roles[*]') AS varchar[]) AS roles "
          << "FROM (SELECT unnest(cast(j.auth as JSON[])) AS y FROM " << secret_table << ") AS x "
          << "WHERE y->>'username' = ? "
          << "LIMIT 1";

    auto executor = createQueryExecutor();

    duckdb_prepared_statement stmt = nullptr;
    if (duckdb_prepare(executor.conn, query.str().c_str(), &stmt) == DuckDBError) {
        const char* err = duckdb_prepare_error(stmt);
        std::string detail = err ? err : "(no detail)";
        duckdb_destroy_prepare(&stmt);
        throw std::runtime_error("Failed to prepare secrets lookup: " + detail);
    }
    if (duckdb_bind_varchar_length(stmt, 1, username.data(), username.size()) == DuckDBError) {
        duckdb_destroy_prepare(&stmt);
        throw std::runtime_error("Failed to bind username for secrets lookup");
    }
    try {
        executor.executePrepared(stmt, "secrets lookup");
    } catch (...) {
        duckdb_destroy_prepare(&stmt);
        throw;
    }
    duckdb_destroy_prepare(&stmt);

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
