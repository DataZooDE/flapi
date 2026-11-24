#include <stdexcept>
#include <filesystem>
#include <cstdio>
#include <sstream>
#include <set>
#include <algorithm>
#include <cctype>
#include "crow/json.h"
#include <yaml-cpp/yaml.h>

#include "duckdb.hpp"
#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/statement/attach_statement.hpp"
#include "duckdb/parser/statement/detach_statement.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/detach_info.hpp"

#include "sql_template_processor.hpp"
#include "sql_utils.hpp"
#include "database_manager.hpp"
#include "redaction.hpp"
#include "database_health.hpp"
#include "cache_refresh_command.hpp"
#include "extension_installer.hpp"
#include "external_access_policy.hpp"
#include "vfs_adapter.hpp"
#include <regex>
#include <chrono>
#include <thread>
#include "duckdb_raii.hpp"
#include "duckdb_http_log_sink.hpp"
#include "usable_cpus.hpp"

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
            const std::string detach = "DETACH " + sql::QuoteIdentifier(ducklake_config.alias) + ";";
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

    QueryExecutor::InvalidatePooledConnections();
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
    if (ducklake.read_only) {
        // A replica attaches a catalog the WRITER creates. Started first - the
        // usual cause on a Postgres/SQLite/MySQL catalog, whose database exists
        // but holds no DuckLake metadata yet - it failed with DuckDB's text alone.
        return "Failed to attach DuckLake catalog read-only: a read-only replica cannot create the catalog; if "
               "the writer has not run yet, start the writer first (it creates the catalog on its first start), "
               "then this replica. (" + detail + ")";
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

        // Opt-in DuckDB HTTP request log (#252), before the extension loads and
        // init statements below so their downloads and reads are logged too. (A
        // connection's logger follows the global setting: DuckDB 2.0.1 recreates
        // it at every query start and end, RefreshLoggerInternal.) A template that
        // runs `CALL disable_logging()` turns it off until restart. Off, DuckDB
        // logging is never touched.
        const bool http_request_log = config_manager->getLogHttpRequests();
        if (http_request_log) {
            EnableDuckDBHttpRequestLog(db);
        }

        // No extension directory configured (duckdb.extension_directory, FLAPI_EXTENSION_DIRECTORY
        // or a bundle's): search the user directory, then the one shipped next to the binary.
        bool directory_configured = false;
        for (const auto& [key, value] : config_manager->getDuckDBConfig().settings) {
            std::string lowered = key;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            directory_configured = directory_configured || lowered == "extension_directory" ||
                                   lowered == "extension_directories";
        }
        if (!directory_configured) {
            if (const auto statement = ShippedExtensionSearchStatement()) {
                duckdb_connection conn = nullptr;
                duckdb_result result;
                if (duckdb_connect(db, &conn) != DuckDBSuccess ||
                    duckdb_query(conn, statement->c_str(), &result) != DuckDBSuccess) {
                    CROW_LOG_WARNING << "Could not add the shipped extension directory: "
                                     << (conn ? duckdb_result_error(&result) : "no connection");
                }
                if (conn) {
                    duckdb_destroy_result(&result);
                    duckdb_disconnect(&conn);
                }
            }
        }

        // Extensions first: the version query already needs core_functions, and offline
        // a missing one must fail with loadDefaultExtensions' message, not an autoload error.
        loadDefaultExtensions(config_manager);
        applyTimeZone();
        logDuckDBVersion();
        initializeConnections(config_manager);
        if (http_request_log) {
            // An init statement may have turned DuckDB's own redaction off.
            EnableDuckDBHttpRequestLog(db);
        }

        this->config_manager = config_manager;

        const auto& ducklake_config = config_manager->getDuckLakeConfig();
        if (ducklake_config.enabled) {
            CROW_LOG_INFO << "Attaching DuckLake catalog at alias '" << ducklake_config.alias << "'";
            std::string attach_stmt = "ATTACH 'ducklake:" + sql::EscapeSqlLiteral(ducklake_config.metadata_path) + "' AS " +
                                      sql::QuoteIdentifier(ducklake_config.alias) + " (DATA_PATH '" +
                                      sql::EscapeSqlLiteral(ducklake_config.data_path) + "'";

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
                count_cache_ = std::make_unique<CountCacheState>();
                count_cache_->alias = ducklake_config.alias;
                // DuckLake keeps its metadata under this fixed catalog name,
                // whatever the backend; every committed change adds a snapshot.
                count_cache_->generation_sql =
                    "SELECT max(snapshot_id) FROM " + sql::QuoteIdentifier("__ducklake_metadata_" + ducklake_config.alias) +
                    ".ducklake_snapshot";
                // Only once it worked: this used to be logged before the ATTACH,
                // so a replica that could not attach said "attached" first.
                if (ducklake_config.read_only) {
                    CROW_LOG_INFO << "DuckLake catalog attached read-only";
                }
            } catch (const std::exception& e) {
                throw std::runtime_error(DuckLakeAttachErrorMessage(ducklake_config, e.what()));
            }
        }

        sql_processor = std::make_shared<SQLTemplateProcessor>(config_manager);

        cache_manager = std::make_unique<CacheManager>(shared_from_this());
        cache_manager->initializeReadiness(config_manager);
        // The readiness registry follows the endpoint table: a reload that
        // changed `cache.table` used to leave the old table's entry (and its
        // `failed` state) in /health until restart, and the new table had none,
        // so its endpoint answered 500 instead of 503 cache_not_built.
        config_manager->addEndpointsChangedListener(
            [weak_cache = std::weak_ptr<CacheManager>(cache_manager),
             weak_config = std::weak_ptr<ConfigManager>(config_manager)]() {
                auto cache = weak_cache.lock();
                auto config = weak_config.lock();
                if (cache && config) {
                    cache->syncReadinessWithEndpoints(config);
                }
            });

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
    return QueryExecutor(db, session_default_order_, session_time_zone_fallback_);
}

void DatabaseManager::applyTimeZone() {
    // DuckDB takes the host's zone (TZ, /etc/localtime) as the session zone once
    // ICU loads, which moves every TIMESTAMPTZ -> TIMESTAMP cast and
    // date_trunc by the host's offset - and ICU loads lazily, on the first
    // query that needs it, so the shift appears minutes after startup. The
    // zone is pinned instead: UTC unless the config says otherwise (#212).
    const std::string statement = "SET GLOBAL TimeZone = '" + sql::EscapeSqlLiteral(effective_time_zone_) + "'";
    std::string error;
    duckdb_connection conn;
    bool ok = duckdb_connect(db, &conn) == DuckDBSuccess;
    if (!ok) {
        error = "could not open a connection";
    } else {
        duckdb_result result;
        ok = duckdb_query(conn, statement.c_str(), &result) == DuckDBSuccess;
        if (!ok) {
            const char* message = duckdb_result_error(&result);
            error = message ? message : "unknown error";
        }
        duckdb_destroy_result(&result);
        duckdb_disconnect(&conn);
    }
    if (ok) {
        CROW_LOG_INFO << "DuckDB TimeZone: " << effective_time_zone_;
        return;
    }
    // ICU is not available (offline without it) or the zone name is unknown.
    // Each connection tries its own SET; if that cannot work either, DuckDB's
    // own default stays and this warning says so.
    session_time_zone_fallback_ = effective_time_zone_;
    CROW_LOG_WARNING << "Could not set the DuckDB TimeZone to '" << effective_time_zone_ << "' globally: " << error
                     << ". Each connection will try its own SET; the host zone may apply otherwise.";
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

namespace {

// A local path as DuckDB will compare it: absolute and normalised. URLs as written
// minus their query string - a signed URL's signature is not a prefix of anything,
// and must not reach the log either.
std::string AllowListEntry(const std::string& path, bool directory) {
    std::string entry;
    if (PathSchemeUtils::IsRemotePath(path) || path.find("://") != std::string::npos) {
        entry = PathSchemeUtils::StripQuery(path);
    } else {
        entry = std::filesystem::absolute(std::filesystem::path(path)).lexically_normal().string();
    }
    if (directory && (entry.empty() || entry.back() != '/')) {
        entry += '/';
    }
    return entry;
}

// A DuckLake data path is a directory; a metadata path that names a backend
// (`postgres:...`, `sqlite:...`) is not a file of ours.
bool IsPlainPath(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    if (path.find("://") != std::string::npos) {
        return true;
    }
    const auto colon = path.find(':');
    // `C:\...` is a drive letter, not a scheme.
    return colon == std::string::npos || colon == 1;
}

bool NamesAzure(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text.find("az://") != std::string::npos || text.find("azure://") != std::string::npos ||
           text.find("abfss://") != std::string::npos || text.find("abfs://") != std::string::npos;
}

// The extensions an init statement LOADs or INSTALLs.
void CollectLoadedExtensions(const std::string& sql, std::vector<std::string>& out) {
    static const std::regex kLoad(R"((?:^|[;\s(])(?:force\s+)?(?:install|load)\s+['"]?([A-Za-z0-9_]+))",
                                  std::regex::icase);
    for (std::sregex_iterator it(sql.begin(), sql.end(), kLoad), end; it != end; ++it) {
        std::string name = (*it)[1].str();
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
    }
}

std::string SqlStringList(const std::vector<std::string>& items) {
    std::string list = "[";
    for (const auto& item : items) {
        list += (list.size() > 1 ? ", " : "") + std::string("'") + sql::EscapeSqlLiteral(item) + "'";
    }
    return list + "]";
}

}  // namespace

DatabaseManager::ExternalAccessAllowList DatabaseManager::ComputeExternalAccessAllowList(const ConfigManager& config,
                                                                                         bool bundle_present) {
    ExternalAccessAllowList allow;
    const auto add_directory = [&](const std::string& path) {
        if (path.empty()) {
            return;
        }
        const auto entry = AllowListEntry(path, true);
        if (std::find(allow.directories.begin(), allow.directories.end(), entry) == allow.directories.end()) {
            allow.directories.push_back(entry);
        }
    };

    if (bundle_present) {
        // Config and templates are served from the archive by flAPI itself, never
        // through DuckDB; data files the bundle carries are read as embed://.
        allow.directories.push_back("embed://");
    } else {
        // The config's own directory - for a remote config, its URL prefix: data next
        // to flapi.yaml in the bucket is what such a deployment reads.
        add_directory(config.getBasePath().empty() ? std::string(".") : config.getBasePath());
        add_directory(config.getTemplatePath());
    }

    const auto& ducklake = config.getDuckLakeConfig();
    if (ducklake.enabled && IsPlainPath(ducklake.data_path)) {
        add_directory(ducklake.data_path);
    }
    // The DuckLake metadata file and the main database are added by DuckDB itself
    // when external access goes off (every attached database path, with its WAL).

    const auto& hardening = config.getDuckDBHardeningConfig();
    for (const auto& directory : hardening.allowed_directories) {
        add_directory(directory);
    }
    for (const auto& path : hardening.allowed_paths) {
        const auto entry = AllowListEntry(path, false);
        if (std::find(allow.paths.begin(), allow.paths.end(), entry) == allow.paths.end()) {
            allow.paths.push_back(entry);
        }
    }
    return allow;
}

std::vector<std::string> DatabaseManager::ComputeExternalAccessPreload(const ConfigManager& config) {
    std::vector<std::string> preload = {"httpfs"};
    if (config.getDuckLakeConfig().enabled) {
        preload.push_back("ducklake");
    }
    bool azure = NamesAzure(config.getTemplatePath()) || NamesAzure(config.getBasePath()) ||
                 NamesAzure(config.getDuckLakeConfig().data_path);
    for (const auto& [name, connection] : config.getConnections()) {
        azure = azure || NamesAzure(connection.getInit());
        CollectLoadedExtensions(connection.getInit(), preload);
    }
    if (azure && std::find(preload.begin(), preload.end(), "azure") == preload.end()) {
        preload.push_back("azure");
    }
    if (const auto endpoints = config.getEndpoints()) {
        for (const auto& endpoint : *endpoints) {
            if (endpoint.auth.from_aws_secretmanager) {
                CollectLoadedExtensions(endpoint.auth.from_aws_secretmanager->init, preload);
            }
        }
    }
    return preload;
}

void DatabaseManager::applyExternalAccessPolicy() {
    if (!config_manager || db == nullptr) {
        throw std::runtime_error("external access policy: the database is not initialised");
    }
    const auto& hardening = config_manager->getDuckDBHardeningConfig();
    const bool remote = config_manager->isRemoteConfig();
    bool restricted = false;
    std::string reason;
    switch (hardening.external_access) {
        case ExternalAccessMode::Full:
            reason = "duckdb-hardening.external-access: full";
            break;
        case ExternalAccessMode::Restricted:
            restricted = true;
            reason = "duckdb-hardening.external-access: restricted";
            break;
        case ExternalAccessMode::Auto:
            restricted = remote;
            reason = remote ? "auto: the configuration is remote" : "auto: the configuration is local";
            break;
    }
    if (!restricted) {
        SetExternalAccessRestricted(false);
        CROW_LOG_INFO << "external access: full (" << reason << ")";
        return;
    }

    duckdb_connection conn = nullptr;
    if (duckdb_connect(db, &conn) != DuckDBSuccess) {
        throw std::runtime_error("external access policy: could not open a DuckDB connection");
    }
    const auto run = [&](const std::string& statement) {
        duckdb_result result;
        const bool ok = duckdb_query(conn, statement.c_str(), &result) == DuckDBSuccess;
        std::string error;
        if (!ok) {
            const char* message = duckdb_result_error(&result);
            error = message ? message : "unknown error";
        }
        duckdb_destroy_result(&result);
        if (!ok) {
            throw std::runtime_error("external access policy: `" + statement + "` failed: " + error);
        }
    };

    try {
        // 1. Everything flAPI will need, loaded while loading is still possible:
        // httpfs reads every s3/gcs/http(s) path and the remote config itself; azure
        // when something names az://; ducklake when the cache is on (its ATTACH loaded
        // it already); whatever connection inits and the AWS Secrets Manager inits load
        // (the latter run when the APIServer starts, after this).
        const auto preload = ComputeExternalAccessPreload(*config_manager);
        const bool offline = config_manager->getDuckDBConfig().offline_extensions;
        for (const auto& extension : preload) {
            const std::string error = LoadOrInstallExtension(conn, extension, offline);
            if (!error.empty()) {
                CROW_LOG_WARNING << "external access policy: could not preload extension '" << extension
                                 << "': " << error << ". It cannot be loaded later either.";
            }
        }
        // DuckDB still INSTALLs from the official repositories with external access off;
        // an autoload would download an extension it then may not load.
        run("SET GLOBAL autoinstall_known_extensions = false");

        // 2. Where templates may read and write.
        const auto allow = ComputeExternalAccessAllowList(*config_manager, FileProviderFactory::GetBundleContents() != nullptr);
        run("SET GLOBAL allowed_directories = " + SqlStringList(allow.directories));
        run("SET GLOBAL allowed_paths = " + SqlStringList(allow.paths));

        // 3. Off. DuckDB adds every attached database (the main one, the DuckLake
        // catalog) and the temp directory to the allowed paths as it does.
        run("SET GLOBAL enable_external_access = false");

        // 4. Locked: a template cannot undo any of the above, nor point secret_directory,
        // http_proxy or temp_directory somewhere else. allowed_configs is exactly what
        // flAPI itself SETs per connection after startup: the session TimeZone fallback
        // and default_order (QueryExecutor), and the profiling pair (tracing).
        run("SET GLOBAL allowed_configs = ['TimeZone', 'default_order', 'enable_profiling', 'tracked_metrics']");
        run("SET GLOBAL lock_configuration = true");

        duckdb_disconnect(&conn);
        SetExternalAccessRestricted(true);

        std::string allowed;
        for (const auto& entry : allow.directories) {
            allowed += (allowed.empty() ? "" : ", ") + entry;
        }
        for (const auto& entry : allow.paths) {
            allowed += (allowed.empty() ? "" : ", ") + entry;
        }
        CROW_LOG_INFO << RedactUrlsInText("external access: restricted (" + reason + "; allowed: " +
                                          (allowed.empty() ? std::string("nothing") : allowed) + ")");
    } catch (...) {
        if (conn != nullptr) {
            duckdb_disconnect(&conn);
        }
        throw;
    }
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

namespace {
template <class Map>
bool duckdb_settings_has(const Map& settings, const std::string& name) {
    for (const auto& [key, value] : settings) {
        std::string lowered = key;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered == name) {
            return true;
        }
    }
    return false;
}
}  // namespace

namespace {
bool NamesRemoteStorage(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* scheme : {"s3://", "s3a://", "s3n://", "gs://", "gcs://", "az://", "azure://", "abfs://",
                               "abfss://", "http://", "https://", "r2://", "hf://"}) {
        if (text.find(scheme) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int64_t DefaultAsyncThreads(const ConfigManager& config) {
    bool remote = NamesRemoteStorage(config.getTemplatePath()) || NamesRemoteStorage(config.getBasePath()) ||
                  NamesRemoteStorage(config.getDuckLakeConfig().metadata_path) ||
                  NamesRemoteStorage(config.getDuckLakeConfig().data_path);
    for (const auto& [name, connection] : config.getConnections()) {
        remote = remote || NamesRemoteStorage(connection.getInit());
        for (const auto& [key, value] : connection.properties) {
            remote = remote || NamesRemoteStorage(value);
        }
    }
    if (!remote) {
        return 0;
    }
    int64_t threads = static_cast<int64_t>(UsableCpus());
    for (const auto& [key, value] : config.getDuckDBConfig().settings) {
        std::string lowered = key;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered == "threads" || lowered == "worker_threads") {
            try {
                threads = std::stoll(value);
            } catch (const std::exception&) {
            }
        }
    }
    return std::max<int64_t>(2, 2 * threads);
}
}  // namespace

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

    // DuckDB 2.0's async I/O pool (4 threads per core, independent of `threads`) is the main cost
    // of 2.0 under flAPI's many short concurrent queries on LOCAL data (-25% throughput, ~3x
    // memory) but makes uncached remote reads 2-3x faster than 1.5. So: no pool when nothing is
    // remote, 2x the worker threads when a connection, template.path, the config or the DuckLake
    // paths name object storage or http(s). `duckdb: async_threads: N` always wins.
    if (!duckdb_settings_has(config_manager->getDuckDBConfig().settings, "async_threads")) {
        const std::string async = std::to_string(DefaultAsyncThreads(*config_manager));
        if (duckdb_set_config(config, "async_threads", async.c_str()) == DuckDBError) {
            CROW_LOG_WARNING << "Could not set DuckDB async_threads=" << async << "; DuckDB's default applies";
        } else {
            CROW_LOG_INFO << "DuckDB async_threads=" << async
                          << (async == "0" ? " (no remote storage configured)" : " (remote storage configured)");
        }
    }

    // DuckDB sizes its worker pool from every host CPU; in a container with a CPU limit, or
    // pinned by taskset/cpusets, that is threads waking for CPUs it cannot get.
    if (!duckdb_settings_has(config_manager->getDuckDBConfig().settings, "threads") &&
        !duckdb_settings_has(config_manager->getDuckDBConfig().settings, "worker_threads")) {
        const unsigned usable = UsableCpus();
        if (usable < std::thread::hardware_concurrency()) {
            duckdb_set_config(config, "threads", std::to_string(usable).c_str());
        }
    }

    // Apply settings from the configuration
    const auto& duckdb_settings = config_manager->getDuckDBConfig().settings;
    session_default_order_.reset();
    effective_time_zone_ = "UTC";
    session_time_zone_fallback_.reset();
    for (const auto& [key, value] : duckdb_settings) {
        // The zone is applied after the engine is open (applyTimeZone): the
        // setting belongs to the ICU extension, which is not loaded yet, so
        // duckdb_set_config would refuse it. Matched case-insensitively - DuckDB
        // itself does - so `timezone:` and `TimeZone:` both count as the user's.
        std::string lowered_key = key;
        std::transform(lowered_key.begin(), lowered_key.end(), lowered_key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered_key == "timezone") {
            effective_time_zone_ = value;
            continue;
        }
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

    // Linked into the binary (core_functions, parquet, json, icu): already loaded when the
    // database opened, so LOAD is a no-op - but INSTALL still downloaded the loadable copy
    // (84 MB for json and parquet, ~1.6 s on a first start) that nothing ever opens.
    {
        std::set<std::string> built_in;
        duckdb_connection conn = nullptr;
        if (duckdb_connect(db, &conn) == DuckDBSuccess) {
            built_in = StaticallyLinkedExtensions(conn);
            duckdb_disconnect(&conn);
        }
        extensions.erase(std::remove_if(extensions.begin(), extensions.end(),
                                        [&](const std::string& extension) {
                                            if (built_in.count(extension) == 0) {
                                                return false;
                                            }
                                            CROW_LOG_DEBUG << "Extension '" << extension
                                                           << "' is built into flapi; not installing or loading it";
                                            return true;
                                        }),
                         extensions.end());
    }

    // Load each extension, installing (downloading) only the ones no search directory has:
    // INSTALL looks at the first directory alone, so INSTALL-then-LOAD fetched an extension
    // the shipped directory already carries. Offline never installs. One at a time, so one
    // failure does not stop the others.
    CROW_LOG_INFO << (offline ? "Offline extensions: loading " : "Loading ") << extensions.size()
                  << " default extensions" << (offline ? ", downloading none" : "");
    int loaded_count = 0;
    std::vector<std::string> missing;
    duckdb_connection conn = nullptr;
    if (duckdb_connect(db, &conn) != DuckDBSuccess) {
        throw std::runtime_error("Failed to connect to DuckDB to load extensions");
    }
    for (const auto& extension : extensions) {
        const std::string error = LoadOrInstallExtension(conn, extension, offline);
        if (error.empty()) {
            CROW_LOG_DEBUG << "Successfully loaded extension: " << extension;
            loaded_count++;
        } else {
            CROW_LOG_WARNING << "Failed to load extension '" << extension << "': " << error << ". Skipping.";
            missing.push_back(extension);
        }
    }
    duckdb_disconnect(&conn);
    if (offline && !missing.empty()) {
        std::string names;
        for (const auto& name : missing) {
            names += (names.empty() ? "" : ", ") + name;
        }
        throw std::runtime_error("Extension(s) not installed and FLAPI_EXTENSIONS_OFFLINE forbids downloading: " +
                                 names + ". Pre-install them with `flapi extensions install --config <flapi.yaml> "
                                 "--dir <dir>` and set FLAPI_EXTENSION_DIRECTORY=<dir>, use a release that "
                                 "ships them in extensions/ next to the binary, or trim "
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
    // An init script routinely carries credentials: a connection's
    // `CREATE SECRET (... SECRET '...')`, an ATTACH with a password in its
    // connection string. Neither the script nor DuckDB's error about it (which
    // quotes the statement) is logged or put into an exception as it is: only
    // the statement's position and the redacted error.
    auto executor = createQueryExecutor();

    duckdb_extracted_statements stmts = nullptr;
    idx_t n_statements = duckdb_extract_statements(executor.conn, init_statement.c_str(), &stmts);
    if (n_statements == 0) {
        const char* error_message = duckdb_extract_statements_error(stmts);
        const std::string detail = redactSqlErrorMessage(error_message ? error_message : "(no detail)", init_statement);
        duckdb_destroy_extracted(&stmts);
        throw std::runtime_error("Failed to parse init statements: " + detail);
    }

    for (idx_t i = 0; i < n_statements; i++) {
        const std::string position = "init statement " + std::to_string(i + 1) + " of " + std::to_string(n_statements);
        duckdb_prepared_statement prepared_stmt = nullptr;
        if (duckdb_prepare_extracted_statement(executor.conn, stmts, i, &prepared_stmt) == DuckDBError) {
            const char* error_message = duckdb_prepare_error(prepared_stmt);
            const std::string detail = redactSqlErrorMessage(error_message ? error_message : "(no detail)", init_statement);
            duckdb_destroy_prepare(&prepared_stmt);
            duckdb_destroy_extracted(&stmts);
            throw std::runtime_error("Failed to prepare " + position + ": " + detail);
        }
        try {
            executor.executePrepared(prepared_stmt);
        } catch (const std::exception& e) {
            duckdb_destroy_prepare(&prepared_stmt);
            duckdb_destroy_extracted(&stmts);
            throw std::runtime_error("Failed to execute " + position + ": " + redactSqlErrorMessage(e.what(), init_statement));
        }
        duckdb_destroy_prepare(&prepared_stmt);
    }

    CROW_LOG_DEBUG << n_statements << " init statement(s) executed successfully";
    duckdb_destroy_extracted(&stmts);
}

void DatabaseManager::createSchemaIfNecessary(const std::string& schema) {
    auto executor = createQueryExecutor();
    executor.execute("CREATE SCHEMA IF NOT EXISTS " + sql::QuoteIdentifier(schema));
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

std::shared_ptr<void> DatabaseManager::holdSerialisedAccess(const EndpointConfig& endpoint) {
    auto locks = std::make_shared<AccessLocks>(lockSerialisedAccess(endpoint));
    if (locks->empty()) {
        return nullptr;
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

QueryResult DatabaseManager::executeQuery(const EndpointConfig& endpoint, std::map<std::string, std::string>& params,
                                          bool with_pagination, TotalCount total, const ResultSink* sink)
{
    // #116: a read concurrent with a write wedges a SQLite attachment for good,
    // so on those connections a read waits its turn like a write does.
    const auto access = lockSerialisedReadAccess(endpoint);

    // Reads are collateral damage in SQLite lock contention: a single writer
    // blocks them, and failing a GET because some unrelated POST held a lock is
    // the least defensible outcome of the lot.
    return retryOnLockContention([&]() -> QueryResult {
        return executeQueryOnce(endpoint, params, with_pagination, total, sink);
    }, "query");
}


namespace {
// Steady-clock milliseconds, for the count cache's retry backoff (#242).
std::int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

QueryResult DatabaseManager::executeQueryOnce(const EndpointConfig& endpoint, std::map<std::string, std::string>& params,
                                              bool with_pagination, TotalCount total, const ResultSink* sink)
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

    // #242: a DuckLake-cached endpoint may reuse its total_count (see RunPaginated).
    CountCacheUse count_use;
    const CountCacheUse* count_cache = nullptr;
    if (count_cache_ && with_pagination && total == TotalCount::Exact && endpoint.cache.enabled &&
        !endpoint.cache.table.empty() && count_cache_->power.usable(SteadyNowMs())) {
        count_use.state = count_cache_.get();
        // Length-prefixed parts: no value can be read as part of another.
        for (const auto& spec : prepared.bindings) {
            const auto it = bind_values.find(spec.field_name);
            AppendKeyPart(count_use.bound, spec.field_name);
            if (it == bind_values.end()) {
                count_use.bound += '-';   // absent: binds NULL or ''
            } else {
                count_use.bound += '=';
                AppendKeyPart(count_use.bound, it->second);
            }
            if (count_use.bound.size() > kMaxCountKeyBytes) {
                count_use.too_long = true;
                break;
            }
        }
        if (!count_use.too_long) {
            count_cache = &count_use;
        }
    }

    // Endpoint reads reuse this thread's connection and its prepared statements.
    QueryExecutor::ReuseConnectionScope reuse;
    if (prepared.bindings.empty()) {
        return executeQueryText(prepared.sql, params, with_pagination, total, sink, count_cache);
    }
    return executeQueryWithBindings(prepared.sql, prepared.bindings, bind_values, params, with_pagination, total,
                                    sink, count_cache);
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
        auto parser = duckdb::Parser::GetBuiltinParser();
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
        auto parser = duckdb::Parser::GetBuiltinParser();
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

// An ATTACH or DETACH as validateScript sees it.
struct AttachDetach {
    bool attach = false;   // else DETACH
    std::string alias;
    bool read_only = false;
};

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::optional<AttachDetach> ParseAttachDetach(const std::string& sql) {
    try {
        auto parser = duckdb::Parser::GetBuiltinParser();
        parser.ParseQuery(sql);
        if (parser.statements.size() != 1 || !parser.statements[0]) {
            return std::nullopt;
        }
        auto& statement = *parser.statements[0];
        if (statement.type == duckdb::StatementType::DETACH_STATEMENT) {
            const auto& info = statement.Cast<duckdb::DetachStatement>().info;
            if (!info) {
                return std::nullopt;
            }
            return AttachDetach{false, LowerAscii(info->name.GetIdentifierName()), false};
        }
        if (statement.type != duckdb::StatementType::ATTACH_STATEMENT) {
            return std::nullopt;
        }
        const auto& info = statement.Cast<duckdb::AttachStatement>().info;
        if (!info) {
            return std::nullopt;
        }
        AttachDetach out;
        out.attach = true;
        out.alias = info->name.GetIdentifierName();
        if (out.alias.empty()) {
            // ATTACH 'dir/file.duckdb' with no AS: DuckDB names it after the file.
            out.alias = std::filesystem::path(info->path).stem().string();
        }
        out.alias = LowerAscii(out.alias);
        const auto flag = [](const duckdb::unique_ptr<duckdb::ParsedExpression>& expr) {
            if (!expr) {
                return true;   // `(READ_ONLY)` with no value
            }
            const auto text = LowerAscii(expr->ToString());
            return text != "false" && text != "0" && text != "'false'";
        };
        const auto apply = [&](const std::string& key, bool value, const std::string& text) {
            const auto name = LowerAscii(key);
            if (name == "read_only" || name == "readonly") {
                out.read_only = value;
            } else if (name == "read_write" || name == "readwrite") {
                out.read_only = !value;
            } else if (name == "access_mode") {
                out.read_only = LowerAscii(text).find("read_only") != std::string::npos;
            }
        };
        for (const auto& [key, expr] : info->parsed_options) {
            apply(key, flag(expr), expr ? expr->ToString() : "");
        }
        // DuckDB 2.0's parser puts constant options - a bare `(READ_ONLY)` among them - into the
        // bound `options` map instead of parsed_options.
        const auto value_flag = [](const duckdb::Value& v) {
            const auto text = LowerAscii(v.ToString());
            return text != "false" && text != "0";
        };
        for (const auto& [key, value] : info->options) {
            apply(key, value_flag(value), value.ToString());
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Whether the file a DuckDB database was attached from has since been replaced
// (an atomic rename over it) or deleted: the process still holds the OLD file
// open, so the alias reads data the path no longer has. Linux only: the open
// file's /proc/self/fd link reads "<path> (deleted)" once it is unlinked.
bool AttachedFileReplaced(const std::string& attached_path) {
#ifdef __linux__
    if (attached_path.empty() || attached_path == ":memory:" || attached_path.find("://") != std::string::npos) {
        return false;
    }
    std::error_code ec;
    const std::string absolute = std::filesystem::absolute(attached_path, ec).lexically_normal().string();
    if (ec) {
        return false;
    }
    const std::string deleted = absolute + " (deleted)";
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", ec)) {
        std::error_code link_ec;
        const auto target = std::filesystem::read_symlink(entry.path(), link_ec);
        if (!link_ec && target.string() == deleted) {
            return true;
        }
    }
#else
    (void)attached_path;
#endif
    return false;
}

// Whether a DuckDB error message names `alias` (a catalog/schema qualifier).
bool NamesAlias(const std::string& message, const std::string& alias) {
    const auto lower = LowerAscii(message);
    for (const auto& form : {"name " + alias + " ", "name " + alias + "!", "\"" + alias + "\"", alias + "."}) {
        if (lower.find(form) != std::string::npos) {
            return true;
        }
    }
    return false;
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

    // A populate template that reads its source through
    //     DETACH DATABASE IF EXISTS src; ATTACH '<file>' AS src (READ_ONLY);
    // could not be checked: neither statement ran, so every later statement
    // failed with "schema src does not exist". A READ_ONLY ATTACH is executed
    // now (a writable one never is - validation must not be able to write), a
    // DETACH only of what this validation attached, and everything it attached
    // is detached again at the end. Databases attached before it (the cache
    // catalog, a connection's own ATTACH) are never touched. The caller holds
    // the catalog lock (CacheManager::lockCatalogWrites), so this cannot race a
    // refresh's ATTACH of the same name.
    std::set<std::string> attached_before;
    // Aliases the server attached before this check whose FILE has been replaced
    // since (the writer publishes by atomic rename): statements that read them
    // are checked against the old file.
    std::set<std::string> stale_aliases;
    if (populate) {
        duckdb_result databases;
        if (duckdb_query(conn, "SELECT database_name, path FROM duckdb_databases()", &databases) == DuckDBSuccess) {
            for (idx_t row = 0; row < duckdb_row_count(&databases); ++row) {
                char* name = duckdb_value_varchar(&databases, 0, row);
                char* path = duckdb_value_varchar(&databases, 1, row);
                if (name) {
                    attached_before.insert(LowerAscii(name));
                    if (path && AttachedFileReplaced(path)) {
                        stale_aliases.insert(LowerAscii(name));
                    }
                }
                if (name) {
                    duckdb_free(name);
                }
                if (path) {
                    duckdb_free(path);
                }
            }
        }
        duckdb_destroy_result(&databases);
    }
    std::set<std::string> attached_here;      // attached by this validation, still attached
    std::set<std::string> ever_attached_here; // detached again at the end
    std::set<std::string> not_attached;       // writable ATTACHes this check skipped

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

        if (populate) {
            if (const auto ad = ParseAttachDetach(sql)) {
                duckdb_destroy_extracted(&extracted);
                if (!ad->attach) {
                    // Only what this validation attached; DETACH of anything
                    // else would detach a database the server is using.
                    if (attached_here.erase(ad->alias) > 0) {
                        duckdb_query(conn, sql.c_str(), nullptr);
                        replay.push_back(sql);
                    }
                    continue;
                }
                if (!ad->read_only) {
                    not_attached.insert(ad->alias);
                    out.warnings.push_back(
                        {number, "not_checked",
                         "statement " + std::to_string(number) + " attaches " + ad->alias +
                             " without READ_ONLY; validation attaches only READ_ONLY databases, so statements "
                             "reading " + ad->alias + " were not checked (add (READ_ONLY) to the ATTACH)"});
                    continue;
                }
                if (attached_before.count(ad->alias) > 0 && attached_here.count(ad->alias) == 0) {
                    if (stale_aliases.count(ad->alias) > 0) {
                        out.warnings.push_back(
                            {number, "stale",
                             "statement " + std::to_string(number) + ": " + ad->alias +
                                 " is attached on the server from a file that has since been replaced (renamed "
                                 "over or deleted); validation checked against the OLD file. This template "
                                 "re-attaches on refresh, which reads the new one, so a table or column only "
                                 "the new file has is reported here, not as an error"});
                        continue;
                    }
                    out.warnings.push_back(
                        {number, "not_checked",
                         "statement " + std::to_string(number) + ": " + ad->alias +
                             " is already attached on the server; validation checked against that database"});
                    continue;
                }
                duckdb_result result;
                if (duckdb_query(conn, sql.c_str(), &result) == DuckDBError) {
                    const char* raw = duckdb_result_error(&result);
                    out.errors.push_back({number, "attach", raw ? raw : "ATTACH failed"});
                    not_attached.insert(ad->alias);
                } else {
                    attached_here.insert(ad->alias);
                    ever_attached_here.insert(ad->alias);
                    // Replayed like a CREATE, should a restarted transaction
                    // have lost it; a replay that finds it attached is a no-op.
                    replay.push_back(sql);
                }
                duckdb_destroy_result(&result);
                continue;
            }
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
            duckdb_result no_data_result;
            if (duckdb_query(conn, no_data.c_str(), &no_data_result) != DuckDBError) {
                replay.push_back(no_data);
                created = true;
            } else {
                // DuckDB 2.0 prepares a CTAS without binding its query, so this is where a
                // missing column or table shows up: report it. A parser error only means
                // this CTAS form does not take the suffix.
                const char* raw = duckdb_result_error(&no_data_result);
                const std::string message = raw ? raw : "";
                if (message.find("Binder Error") != std::string::npos ||
                    message.find("Catalog Error") != std::string::npos) {
                    failure = message;
                }
                rollback();
                in_transaction = begin();
            }
            duckdb_destroy_result(&no_data_result);
        }

        if (failure) {
            const std::string& message = *failure;
            const bool missing = message.find("does not exist") != std::string::npos;
            const auto unattached = std::find_if(not_attached.begin(), not_attached.end(),
                                                 [&](const std::string& alias) { return NamesAlias(message, alias); });
            if (unattached != not_attached.end()) {
                out.warnings.push_back({number, "not_checked",
                                        "statement " + std::to_string(number) + " was not checked: it reads " +
                                            *unattached + ", which validation did not attach"});
            } else if (missing && cache_table && NamesTable(message, cache_table->table)) {
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
            } else if (const auto stale = std::find_if(stale_aliases.begin(), stale_aliases.end(),
                                                       [&](const std::string& alias) {
                                                           return NamesAlias(sql, alias);
                                                       });
                       stale != stale_aliases.end()) {
                // Bound against the file the server attached BEFORE the writer's
                // rename: not evidence the template is wrong.
                out.warnings.push_back(
                    {number, "stale",
                     "statement " + std::to_string(number) + " does not bind against " + *stale +
                         ", which the server attached from a file that has since been replaced; the next "
                         "refresh re-attaches it and may succeed (" + message + ")"});
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
    // Never leave anything attached.
    for (const auto& alias : ever_attached_here) {
        if (attached_before.count(alias) > 0) {
            continue;
        }
        const std::string detach = "DETACH DATABASE IF EXISTS " + quotePopulateIdentifier(alias);
        duckdb_query(conn, detach.c_str(), nullptr);
    }
    return out;
}

std::optional<std::string> DatabaseManager::MaterialiseSelect(const std::string& script, const PopulateTarget& target) {
    try {
        auto parser = duckdb::Parser::GetBuiltinParser();
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
        DatabaseHealth::NoteDatabaseError(error_message);
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
                DatabaseHealth::NoteDatabaseError(error_message);
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

std::unique_ptr<QueryExecutor> DatabaseManager::executeQueryRaw(const EndpointConfig& endpoint, std::map<std::string, std::string>& params,
                                                                std::optional<int64_t> explicit_limit,
                                                                std::optional<int64_t> explicit_offset) {
    const auto access = lockSerialisedReadAccess(endpoint);
    cache_manager->addQueryCacheParamsIfNecessary(config_manager, endpoint, params);

    // W3.1 PR B: Arrow-streaming endpoint also takes the prepared path
    // when the endpoint has typed scalar params; falls back to direct
    // execute when the binding plan is empty (no behaviour change).
    std::map<std::string, std::string> bind_values = params;
    auto prepared = sql_processor->loadAndProcessTemplatePrepared(endpoint, params);
    if (endpoint.with_pagination && (explicit_limit || explicit_offset)) {
        std::string paged = "SELECT * FROM (" + prepared.sql + ") AS subquery";
        if (explicit_limit) {
            paged += " LIMIT " + std::to_string(*explicit_limit);
        }
        if (explicit_offset) {
            paged += " OFFSET " + std::to_string(*explicit_offset);
        }
        prepared.sql = paged;
    }

    auto executor = std::make_unique<QueryExecutor>(db, session_default_order_);
    if (prepared.bindings.empty()) {
        executor->execute(prepared.sql);
    } else {
        executor->executeWithBindings(prepared.sql, prepared.bindings, bind_values, "arrow streaming");
    }
    return executor;
}

namespace {
// Pagination parameters as the request handler set them (limit/offset are reserved for it).
struct PageWindow {
    bool paged = false;
    int64_t limit = 0;
    int64_t offset = 0;
};

PageWindow ReadPageWindow(bool with_pagination, const std::map<std::string, std::string>& params) {
    PageWindow window;
    if (!with_pagination) {
        return window;
    }
    const auto limit = params.find("limit");
    const auto offset = params.find("offset");
    if (limit == params.end() && offset == params.end()) {
        return window;
    }
    window.paged = true;
    if (limit != params.end()) {
        window.limit = std::stoll(limit->second);
    }
    if (offset != params.end()) {
        window.offset = std::stoll(offset->second);
    }
    return window;
}

std::string NextLink(const PageWindow& window, const std::map<std::string, std::string>& params) {
    std::stringstream next_ss;
    next_ss << "?offset=" << (window.offset + window.limit);
    for (const auto& [key, value] : params) {
        if (key != "offset") {
            next_ss << "&" << key << "=" << value;
        }
    }
    return next_ss.str();
}
}  // namespace

namespace {

// The functions a count-cached query may call (#242), as DuckDB itself reports
// them: every overload a scalar, aggregate or window function, none VOLATILE
// (random, uuid, nextval, ...), and not one of those that are ONLY consistent
// within a query - now(), current_date, getvariable, current_setting and the
// like, whose answer changes between requests. Mixed names (`+`, date_part:
// their TIMESTAMPTZ overloads read the TimeZone setting, which is fixed for the
// server) pass - except those CountableQuery already refused by name
// (UnstableFunctionName: age reads the transaction start in a CONSISTENT
// overload). A macro or an unknown name fails.
bool FunctionsAreStable(QueryExecutor& executor, const std::vector<std::string>& functions) {
    std::set<std::string> names(functions.begin(), functions.end());
    if (names.empty()) {
        return true;
    }
    std::string in;
    for (const auto& name : names) {
        in += (in.empty() ? "" : ", ") + std::string("'") + sql::EscapeSqlLiteral(name) + "'";
    }
    executor.execute(
        "SELECT lower(function_name) AS name, bool_and(function_type IN ('scalar', 'aggregate', 'window') "
        "AND stability IS NOT NULL AND stability <> 'VOLATILE') AND NOT bool_and(stability = "
        "'CONSISTENT_WITHIN_QUERY') AS ok FROM duckdb_functions() WHERE lower(function_name) IN (" + in +
            ") GROUP BY 1",
        "count cache check");
    const idx_t rows = executor.rowCount();
    std::set<std::string> stable;
    for (idx_t row = 0; row < rows; ++row) {
        if (!duckdb_value_boolean(&executor.result, 1, row)) {
            return false;
        }
        char* name = duckdb_value_varchar(&executor.result, 0, row);
        if (name != nullptr) {
            stable.insert(name);
            duckdb_free(name);
        }
    }
    return stable.size() == names.size();
}

// Whether every three-part name is a base TABLE of its catalog (#242). The
// caller memoises the answer per rendered SQL and generation, so this runs
// (and the SQL is re-parsed for its names) only when the catalog changed.
bool TablesAreDuckLakeTables(QueryExecutor& executor, const std::vector<TableName>& tables) {
    if (tables.empty()) {
        return true;
    }
    std::string where;
    std::set<std::string> distinct;
    for (const auto& t : tables) {
        distinct.insert(t.catalog + '\x1f' + t.schema + '\x1f' + t.name);
        where += (where.empty() ? "" : " OR ") + std::string("(lower(database_name) = '") +
                 sql::EscapeSqlLiteral(t.catalog) + "' AND lower(schema_name) = '" + sql::EscapeSqlLiteral(t.schema) +
                 "' AND lower(table_name) = '" + sql::EscapeSqlLiteral(t.name) + "')";
    }
    // duckdb_tables() lists base tables only: a VIEW (whose body may read
    // anything) or a name that does not resolve is not counted.
    executor.execute("SELECT count(DISTINCT (lower(database_name), lower(schema_name), lower(table_name))) "
                     "FROM duckdb_tables() WHERE " + where,
                     "count cache tables");
    return executor.rowCount() > 0 &&
           duckdb_value_int64(&executor.result, 0, 0) == static_cast<std::int64_t>(distinct.size());
}

// Whether `sql`'s count may be cached at `generation`: the parse and function
// rule once per rendered SQL, the catalog check once per generation.
bool CountCacheTablesOk(QueryExecutor& executor, const std::string& sql, CountCacheState& state,
                        std::int64_t generation) {
    if (const auto known = state.cache.tablesVerdict(sql, generation)) {
        return *known;
    }
    std::vector<std::string> functions;
    std::vector<TableName> tables;
    bool ok = CountableQuery(sql, state.alias, functions, tables);
    try {
        ok = ok && TablesAreDuckLakeTables(executor, tables);
    } catch (const std::exception&) {
        return false;   // not stored: judged again next time
    }
    state.cache.storeTablesVerdict(sql, generation, ok);
    return ok;
}

// Whether `sql`'s count may be cached, judged once per rendered SQL.
CountCache::Verdict CountCacheEligible(QueryExecutor& executor, const std::string& sql, CountCacheState& state) {
    if (const auto known = state.cache.verdict(sql)) {
        return *known;
    }
    std::vector<std::string> functions;
    bool eligible = CountableQuery(sql, state.alias, functions);
    if (eligible) {
        try {
            eligible = FunctionsAreStable(executor, functions);
        } catch (const std::exception&) {
            eligible = false;
        }
    }
    return state.cache.storeVerdict(sql, eligible);
}

// The DuckLake catalog's newest snapshot id, read BEFORE the count runs: a
// change committed after this read can only make a stored count look older
// than it is, never newer.
std::optional<std::int64_t> CountGeneration(QueryExecutor& executor, CountCacheState& state) {
    try {
        executor.execute(state.generation_sql, "count cache generation");
        if (state.power.succeeded()) {
            CROW_LOG_INFO << "total_count caching is back on: the DuckLake snapshot id can be read again";
        }
        if (executor.rowCount() > 0 && !duckdb_value_is_null(&executor.result, 0, 0)) {
            return duckdb_value_int64(&executor.result, 0, 0);
        }
    } catch (const std::exception& e) {
        // Off for a while, then tried again: a metadata database that was
        // briefly unreachable must not disable the cache for the process.
        if (state.power.failed(SteadyNowMs())) {
            CROW_LOG_WARNING << "total_count caching is off for " << CountCacheSwitch::kRetryAfterMs / 1000
                             << " s: the DuckLake snapshot id cannot be read (" << e.what() << ")";
        }
    }
    return std::nullopt;
}

}  // namespace

// Runs one paginated (or plain) read and fills data, total_count and next.
//
// The COUNT(*) query costs a second full execution, and on DuckDB 2.0 a second parse and
// plan; it is only run when the page cannot tell the total itself:
//   * no limit/offset applied: the result IS everything, total = rows;
//   * a short page (fewer rows than the limit) that is not empty past the end: total = offset + rows;
//   * TotalCount::Skip (MCP, `total-count: false`): never; one row past the page tells `next`;
//   * a DuckLake-cached endpoint (`count_cache`): reused while the catalog's snapshot id is
//     unchanged (#242, CountCache).
template <class Run>
static QueryResult RunPaginated(QueryExecutor& executor, const std::string& sql,
                                const std::map<std::string, std::string>& params, bool with_pagination,
                                DatabaseManager::TotalCount total, const ResultSink* sink,
                                const CountCacheUse* count_cache, Run&& run) {
    const PageWindow window = ReadPageWindow(with_pagination, params);
    const bool skip = total == DatabaseManager::TotalCount::Skip;
    std::string page_sql = sql;
    if (window.paged) {
        const int64_t fetch = skip ? window.limit + 1 : window.limit;
        page_sql = "SELECT * FROM (" + sql + ") AS subquery LIMIT " + std::to_string(fetch) +
                   " OFFSET " + std::to_string(window.offset);
    }

    run(page_sql, "main query");
    const auto rows = static_cast<int64_t>(executor.rowCount());

    QueryResult result;
    result.columns = executor.columnNames();
    const auto take = [&](idx_t max_rows) {
        if (sink && sink->reset) {
            sink->reset();
        }
        if (sink) {
            result.streamed = true;
            result.streamed_rows = QueryResult::streamResult(executor.result, max_rows, *sink);
        } else {
            result.data = executor.toJson(max_rows);
        }
    };
    if (!with_pagination) {
        take(static_cast<idx_t>(-1));
        return result;
    }
    if (!window.paged) {
        take(static_cast<idx_t>(-1));
        result.total_count = rows;
        return result;
    }

    const int64_t shown = std::min(rows, window.limit);
    take(static_cast<idx_t>(std::max<int64_t>(shown, 0)));
    const bool short_page = rows < window.limit && (rows > 0 || window.offset == 0);
    if (skip) {
        if (rows > window.limit) {
            result.next = NextLink(window, params);
        } else if (short_page) {
            result.total_count = window.offset + rows;
        }
        return result;
    }
    if (short_page) {
        result.total_count = window.offset + rows;
    } else {
        // #242: on a DuckLake-cached endpoint the count cannot change until the
        // catalog does; reuse it while the catalog's snapshot id is unchanged.
        std::optional<std::int64_t> generation;
        std::string key;
        std::optional<std::int64_t> known;
        if (count_cache != nullptr && count_cache->state->power.usable(SteadyNowMs())) {
            const auto verdict = CountCacheEligible(executor, sql, *count_cache->state);
            if (verdict.eligible) {
                generation = CountGeneration(executor, *count_cache->state);
            }
            if (generation && !CountCacheTablesOk(executor, sql, *count_cache->state, *generation)) {
                generation.reset();
            }
            if (generation) {
                AppendKeyPart(key, std::to_string(verdict.id));
                key.append(count_cache->bound);
                known = count_cache->state->cache.find(key, *generation);
            }
        }
        if (known) {
            result.total_count = *known;
        } else {
            // Same connection: the page result is already converted.
            run("SELECT COUNT(*) FROM (" + sql + ") AS subquery", "count query");
            if (executor.rowCount() > 0) {
                result.total_count = duckdb_value_int64(&executor.result, 0, 0);
                if (generation) {
                    count_cache->state->cache.store(key, *generation, result.total_count);
                }
            }
        }
    }
    if (window.offset + window.limit < result.total_count) {
        result.next = NextLink(window, params);
    }
    return result;
}

QueryResult DatabaseManager::executeQueryWithBindings(
    const std::string& sql,
    const std::vector<PreparedBindingSpec>& bindings,
    const std::map<std::string, std::string>& bind_values,
    const std::map<std::string, std::string>& for_template_params,
    bool with_pagination, TotalCount total, const ResultSink* sink, const CountCacheUse* count_cache)
{
    auto executor = createQueryExecutor();
    return RunPaginated(executor, sql, for_template_params, with_pagination, total, sink, count_cache,
                        [&](const std::string& query, const char* what) {
                            executor.executeWithBindings(query, bindings, bind_values, what);
                        });
}

QueryResult DatabaseManager::executeQuery(const std::string& query,
                                          const std::map<std::string, std::string>& params,
                                          bool with_pagination, TotalCount total, const ResultSink* sink) {
    return executeQueryText(query, params, with_pagination, total, sink, nullptr);
}

QueryResult DatabaseManager::executeQueryText(const std::string& query,
                                              const std::map<std::string, std::string>& params,
                                              bool with_pagination, TotalCount total, const ResultSink* sink,
                                              const CountCacheUse* count_cache) {
    auto executor = createQueryExecutor();
    return RunPaginated(executor, query, params, with_pagination, total, sink, count_cache,
                        [&](const std::string& sql, const char* what) { executor.execute(sql, what); });
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

bool DatabaseManager::dropTableIfExists(const std::string& name) {
    // `name` is `table` or `schema.table` (the main schema by default), as an
    // older flAPI interpolated it.
    std::string schema = "main";
    std::string table = name;
    if (const auto dot = name.rfind('.'); dot != std::string::npos) {
        schema = name.substr(0, dot);
        table = name.substr(dot + 1);
    }
    const auto found = executeQuery(
        "SELECT table_name FROM information_schema.tables WHERE table_schema = '" + sql::EscapeSqlLiteral(schema) +
            "' AND table_name = '" + sql::EscapeSqlLiteral(table) + "'",
        {}, false);
    if (found.data.t() == crow::json::type::Null || found.data.size() == 0) {
        return false;
    }
    executeQuery("DROP TABLE IF EXISTS " + sql::QuoteIdentifier(schema) + "." + sql::QuoteIdentifier(table), {}, false);
    return true;
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
