#include "external_access_policy.hpp"

#include <algorithm>
#include <atomic>
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

namespace flapi {

namespace {

std::atomic<bool> g_external_access_restricted{false};

std::string Lowered(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// `word` as a whole token somewhere in `lowered`.
bool HasWord(const std::string& lowered, const std::string& word) {
    for (auto at = lowered.find(word); at != std::string::npos; at = lowered.find(word, at + 1)) {
        const bool starts = at == 0 || !IsWordChar(lowered[at - 1]);
        const auto end = at + word.size();
        const bool ends = end >= lowered.size() || !IsWordChar(lowered[end]);
        if (starts && ends) {
            return true;
        }
    }
    return false;
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

}  // namespace

bool ExternalAccessRestricted() {
    return g_external_access_restricted.load(std::memory_order_acquire);
}

void SetExternalAccessRestricted(bool restricted) {
    g_external_access_restricted.store(restricted, std::memory_order_release);
}

std::optional<std::string> RefusedTemplateStatement(const std::string& sql) {
    // Every request renders a template: parse only when one of the keywords is there.
    const std::string lowered = Lowered(sql);
    const bool load = HasWord(lowered, "load") || HasWord(lowered, "install") || HasWord(lowered, "repository");
    const bool secret = HasWord(lowered, "secret");
    const bool update = HasWord(lowered, "update") && HasWord(lowered, "extensions");
    if (!load && !secret && !update) {
        return std::nullopt;
    }
    auto parser = duckdb::Parser::GetBuiltinParser();
    try {
        parser.ParseQuery(sql);
    } catch (const std::exception&) {
        // DuckDB cannot run what it cannot parse either - except syntax a loaded
        // extension added, which this parser does not know. Fail closed on the
        // keywords rather than let an unchecked statement through.
        if (load) {
            return std::string("INSTALL/LOAD");
        }
        if (secret) {
            return std::string("CREATE/DROP SECRET");
        }
        return std::string("UPDATE EXTENSIONS");
    }
    for (const auto& statement : parser.statements) {
        if (!statement) {
            continue;
        }
        switch (statement->type) {
            case duckdb::StatementType::LOAD_STATEMENT:
                return LoadVerb(statement->Cast<duckdb::LoadStatement>().info->load_type);
            case duckdb::StatementType::UPDATE_EXTENSIONS_STATEMENT:
                return std::string("UPDATE EXTENSIONS");
            case duckdb::StatementType::CREATE_STATEMENT: {
                const auto& create = statement->Cast<duckdb::CreateStatement>();
                if (create.info && create.info->type == duckdb::CatalogType::SECRET_ENTRY) {
                    return std::string("CREATE SECRET");
                }
                break;
            }
            case duckdb::StatementType::DROP_STATEMENT: {
                const auto& drop = statement->Cast<duckdb::DropStatement>();
                if (drop.info && drop.info->type == duckdb::CatalogType::SECRET_ENTRY) {
                    return std::string("DROP SECRET");
                }
                break;
            }
            default:
                break;
        }
    }
    return std::nullopt;
}

void RefuseRestrictedTemplateStatements(const std::string& sql) {
    if (!ExternalAccessRestricted()) {
        return;
    }
    if (const auto verb = RefusedTemplateStatement(sql)) {
        throw std::runtime_error(*verb + " is refused in a template: external access is restricted "
                                 "(duckdb-hardening.external-access). Extensions are loaded and secrets "
                                 "created at startup only - add the extension to duckdb.default-extensions "
                                 "and the secret to a connection's `init`.");
    }
}

std::string ExtensionUnavailableUnderHardening(const std::string& extension) {
    return "extension '" + extension + "' is not loaded, and cannot be loaded now: external access is "
           "restricted (duckdb-hardening.external-access), which allows extensions to load at startup only. "
           "Add it to duckdb.default-extensions, or set duckdb-hardening.external-access: full.";
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
                          "not loaded at startup cannot be loaded now; add it to duckdb.default-extensions)";
}

}  // namespace flapi
