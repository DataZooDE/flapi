#include "external_access_policy.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <cctype>
#include <stdexcept>

#include "duckdb.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/create_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/parser/parsed_data/load_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/statement/load_statement.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/parser/statement/set_statement.hpp"
#include "duckdb/parser/parsed_data/alter_info.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/setting_info.hpp"

namespace flapi {

namespace {

std::atomic<bool> g_external_access_restricted{false};

std::string Lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string LoadVerb(duckdb::LoadType type) {
    switch (type) {
        case duckdb::LoadType::INSTALL:
            return "INSTALL";
        case duckdb::LoadType::FORCE_INSTALL:
            return "FORCE INSTALL";
        case duckdb::LoadType::CREATE_REPOSITORY:
            return "CREATE REPOSITORY";
        case duckdb::LoadType::DROP_REPOSITORY:
            return "DROP REPOSITORY";
        default:
            return "LOAD";
    }
}

// Catalog objects a template may create, drop or alter. A SECRET is not one of
// them: secrets are instance-wide and redirect every read of their scope.
bool AllowedCatalogType(duckdb::CatalogType type) {
    switch (type) {
        case duckdb::CatalogType::TABLE_ENTRY:
        case duckdb::CatalogType::VIEW_ENTRY:
        case duckdb::CatalogType::MACRO_ENTRY:
        case duckdb::CatalogType::TABLE_MACRO_ENTRY:
        case duckdb::CatalogType::SCHEMA_ENTRY:
        case duckdb::CatalogType::SEQUENCE_ENTRY:
        case duckdb::CatalogType::INDEX_ENTRY:
        case duckdb::CatalogType::TYPE_ENTRY:
            return true;
        default:
            return false;
    }
}

// A SET/RESET that touches only this connection. GLOBAL is refused outright (the
// configuration lock checks a setting's name, not the scope: `SET GLOBAL TimeZone`
// from one template would move every endpoint). A bare SET takes the setting's own
// default scope, which for default_order and others is global, so it must resolve
// to the session - otherwise the template has to say SET SESSION.
std::optional<std::string> RefusedSet(const duckdb::SetStatement& set) {
    const std::string verb = set.set_type == duckdb::SetType::RESET ? "RESET" : "SET";
    const std::string name = set.name.GetIdentifierName();
    switch (set.scope) {
        case duckdb::SetScope::VARIABLE:
        case duckdb::SetScope::SESSION:
        case duckdb::SetScope::LOCAL:
            return std::nullopt;
        case duckdb::SetScope::GLOBAL:
            return verb + " GLOBAL " + name;
        case duckdb::SetScope::AUTOMATIC:
            break;
    }
    const auto lowered = Lowered(name);
    if (lowered == "schema" || lowered == "search_path") {
        return std::nullopt;
    }
    // The same resolution as DuckDB's PhysicalSet::GetSettingScope.
    const auto option = duckdb::DBConfig::GetOptionByName(set.name);
    if (option) {
        bool session = false;
        if (option->set_local != nullptr) {
            session = true;
        } else if (option->set_global == nullptr) {
            session = option->scope == duckdb::SettingScopeTarget::LOCAL_ONLY ||
                      option->scope == duckdb::SettingScopeTarget::LOCAL_DEFAULT;
        }
        if (session) {
            return std::nullopt;
        }
    }
    return verb + " " + name + " without SESSION";
}

// The statement of `statement` (or one it wraps) outside the allowlist, or nullopt.
std::optional<std::string> RefusedStatement(const duckdb::SQLStatement& statement) {
    using duckdb::StatementType;
    switch (statement.type) {
        case StatementType::SELECT_STATEMENT:
        case StatementType::INSERT_STATEMENT:
        case StatementType::UPDATE_STATEMENT:
        case StatementType::DELETE_STATEMENT:
        case StatementType::MERGE_INTO_STATEMENT:
        case StatementType::COPY_STATEMENT:  // file access is DuckDB's allowed_directories
        case StatementType::TRANSACTION_STATEMENT:
        case StatementType::VACUUM_STATEMENT:
        case StatementType::ANALYZE_STATEMENT:
        case StatementType::CALL_STATEMENT:  // table functions only; CHECKPOINT parses to CALL
        case StatementType::EXECUTE_STATEMENT:  // runs a PREPARE that was itself checked
        case StatementType::CREATE_FUNC_STATEMENT:
            return std::nullopt;
        case StatementType::CREATE_STATEMENT: {
            const auto& create = statement.Cast<duckdb::CreateStatement>();
            if (create.info && AllowedCatalogType(create.info->type)) {
                return std::nullopt;
            }
            if (create.info && create.info->type == duckdb::CatalogType::SECRET_ENTRY) {
                return std::string("CREATE SECRET");
            }
            return std::string("CREATE of this object type");
        }
        case StatementType::DROP_STATEMENT: {
            const auto& drop = statement.Cast<duckdb::DropStatement>();
            if (drop.info && (AllowedCatalogType(drop.info->type) ||
                              drop.info->type == duckdb::CatalogType::PREPARED_STATEMENT)) {
                return std::nullopt;
            }
            if (drop.info && drop.info->type == duckdb::CatalogType::SECRET_ENTRY) {
                return std::string("DROP SECRET");
            }
            return std::string("DROP of this object type");
        }
        case StatementType::ALTER_STATEMENT: {
            const auto& alter = statement.Cast<duckdb::AlterStatement>();
            if (alter.info && AllowedCatalogType(alter.info->GetCatalogType())) {
                return std::nullopt;
            }
            return std::string("ALTER of this object type");
        }
        case StatementType::SET_STATEMENT:
        case StatementType::VARIABLE_SET_STATEMENT:
            return RefusedSet(statement.Cast<duckdb::SetStatement>());
        case StatementType::EXPLAIN_STATEMENT: {
            const auto& explain = statement.Cast<duckdb::ExplainStatement>();
            if (!explain.stmt) {
                return std::string("EXPLAIN");
            }
            if (auto inner = RefusedStatement(*explain.stmt)) {
                return "EXPLAIN " + *inner;
            }
            return std::nullopt;
        }
        case StatementType::PREPARE_STATEMENT: {
            const auto& prepare = statement.Cast<duckdb::PrepareStatement>();
            if (!prepare.statement) {
                return std::string("PREPARE");
            }
            if (auto inner = RefusedStatement(*prepare.statement)) {
                return "PREPARE " + *inner;
            }
            return std::nullopt;
        }
        case StatementType::MULTI_STATEMENT: {
            for (const auto& inner : statement.Cast<duckdb::MultiStatement>().statements) {
                if (!inner) {
                    continue;
                }
                if (auto refused = RefusedStatement(*inner)) {
                    return refused;
                }
            }
            return std::nullopt;
        }
        case StatementType::LOAD_STATEMENT:
            return LoadVerb(statement.Cast<duckdb::LoadStatement>().info->load_type);
        case StatementType::PRAGMA_STATEMENT: {
            // IMPORT DATABASE is PRAGMA import_database: it runs <dir>/schema.sql verbatim.
            const auto& pragma = statement.Cast<duckdb::PragmaStatement>();
            return "PRAGMA " + (pragma.info ? pragma.info->name.GetIdentifierName() : std::string());
        }
        case StatementType::ATTACH_STATEMENT:
            return std::string("ATTACH");
        case StatementType::DETACH_STATEMENT:
            return std::string("DETACH");
        case StatementType::EXPORT_STATEMENT:
            return std::string("EXPORT DATABASE");
        case StatementType::COPY_DATABASE_STATEMENT:
            return std::string("COPY FROM DATABASE");
        case StatementType::UPDATE_EXTENSIONS_STATEMENT:
            return std::string("UPDATE EXTENSIONS");
        default:
            return std::string("this statement type");
    }
}

// Rendered SQL that passed, so a template whose text does not change per request
// (every bound parameter is a $n) is parsed once. Bounded; cleared when full.
std::mutex g_approved_mutex;
std::unordered_set<std::string> g_approved;
constexpr std::size_t kMaxApproved = 4096;
constexpr std::size_t kMaxApprovedBytes = 64 * 1024;

}  // namespace

bool ExternalAccessRestricted() {
    return g_external_access_restricted.load(std::memory_order_acquire);
}

void SetExternalAccessRestricted(bool restricted) {
    g_external_access_restricted.store(restricted, std::memory_order_release);
}

std::optional<std::string> RefusedTemplateStatement(const std::string& sql) {
    auto parser = duckdb::Parser::GetBuiltinParser();
    try {
        parser.ParseQuery(sql);
    } catch (const std::exception&) {
        // DuckDB cannot run what it cannot parse either - except syntax a loaded
        // extension added, which this parser does not know. Fail closed.
        return std::string("SQL flAPI cannot parse to check");
    }
    for (const auto& statement : parser.statements) {
        if (!statement) {
            continue;
        }
        if (auto refused = RefusedStatement(*statement)) {
            return refused;
        }
    }
    return std::nullopt;
}

void RefuseRestrictedTemplateStatements(const std::string& sql) {
    if (!ExternalAccessRestricted()) {
        return;
    }
    const bool cacheable = sql.size() <= kMaxApprovedBytes;
    if (cacheable) {
        std::lock_guard<std::mutex> lock(g_approved_mutex);
        if (g_approved.count(sql) != 0) {
            return;
        }
    }
    if (const auto what = RefusedTemplateStatement(sql)) {
        throw std::runtime_error(
            *what + " is not allowed in a template: external access is restricted "
                    "(duckdb-hardening.external-access). Templates may query, write tables, COPY within the "
                    "allowed directories and SET SESSION; extensions load and secrets are created at startup "
                    "(duckdb.default-extensions, a connection's `init`).");
    }
    if (cacheable) {
        std::lock_guard<std::mutex> lock(g_approved_mutex);
        if (g_approved.size() >= kMaxApproved) {
            g_approved.clear();
        }
        g_approved.insert(sql);
    }
}

std::string ExtensionUnavailableUnderHardening(const std::string& extension) {
    return "extension '" + extension + "' is not loaded, and cannot be loaded now: external access is "
           "restricted (duckdb-hardening.external-access), which allows extensions to load at startup only. "
           "Load it at startup: LOAD it in a connection's `init`, or list it in duckdb.default-extensions - which "
           "REPLACES the defaults (httpfs, ducklake, fts, json, postgres, sqlite, parquet), so list those too - "
           "or set duckdb-hardening.external-access: full.";
}

std::string ExplainExternalAccessError(const std::string& duckdb_error) {
    if (!ExternalAccessRestricted()) {
        return duckdb_error;
    }
    const bool extension_refused =
        duckdb_error.find("Loading external extensions is disabled") != std::string::npos ||
        duckdb_error.find("automatically install") != std::string::npos ||
        duckdb_error.find("Autoload") != std::string::npos || duckdb_error.find("autoload") != std::string::npos;
    if (!extension_refused) {
        return duckdb_error;
    }
    return duckdb_error + " (flAPI: external access is restricted by duckdb-hardening, so an extension that was "
                          "not loaded at startup cannot be loaded now; LOAD it in a connection's `init`, or list it "
                          "in duckdb.default-extensions together with the defaults it replaces: httpfs, ducklake, "
                          "fts, json, postgres, sqlite, parquet)";
}

}  // namespace flapi
