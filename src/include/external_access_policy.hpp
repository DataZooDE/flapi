#pragma once

#include <optional>
#include <string>

namespace flapi {

// duckdb-hardening (#253): whether DuckDB's external access has been switched off.
//
// Set once, by DatabaseManager::applyExternalAccessPolicy at the end of startup, and
// read by the code that has to behave differently afterwards: template rendering
// (refuses INSTALL/LOAD and secret DDL), extension loading (cannot load anything new)
// and query errors (explains an autoload refusal).
bool ExternalAccessRestricted();
void SetExternalAccessRestricted(bool restricted);

// The statement flAPI refuses in a template while external access is restricted, as
// "INSTALL", "LOAD", "CREATE SECRET", ... - or nullopt when there is none.
//
// INSTALL and LOAD: DuckDB lets INSTALL reach the official repositories even with
// external access off, and an extension, once loaded, is native code with no file
// policy at all. Secret DDL: a temporary secret is instance-wide, so one template's
// `CREATE SECRET (TYPE s3, ENDPOINT ..., SCOPE 's3://allowed-bucket/')` redirects every
// endpoint's reads of the allowed bucket - and flAPI's own remote config reads - to a
// host of the author's choosing.
std::optional<std::string> RefusedTemplateStatement(const std::string& sql);

// Throws std::runtime_error naming the statement when `sql` holds one that
// RefusedTemplateStatement refuses and external access is restricted. A no-op otherwise.
void RefuseRestrictedTemplateStatements(const std::string& sql);

// The error for an extension that is not loaded and can no longer be: names the
// extension and `duckdb-hardening`.
std::string ExtensionUnavailableUnderHardening(const std::string& extension);

// A DuckDB error with, when external access is restricted and the error is DuckDB
// refusing to (auto)load an extension, a sentence saying why and what to do.
std::string ExplainExternalAccessError(const std::string& duckdb_error);

}  // namespace flapi
