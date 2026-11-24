#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include <duckdb.h>

namespace flapi {

class ConfigManager;

// A DuckDB extension and the repository it comes from (empty = the core repository).
struct ExtensionSpec {
    std::string name;
    std::string repository;
};

// The current process id, portable (POSIX getpid / Windows _getpid). Names temporary
// directories so concurrent processes never share one.
long CurrentProcessId();

// Extensions flAPI needs whatever the config says: every query needs core_functions.
// Builds that link it statically (DuckDB 2.0) skip it - see StaticallyLinkedExtensions.
const std::vector<std::string>& ImplicitExtensions();

// duckdb.default-extensions when the config does not set it. The loadable ones among them
// (httpfs, ducklake, fts, postgres, sqlite - json and parquet are built in) are what
// releases ship in extensions/ next to the binary; `flapi extensions install --defaults`.
const std::vector<std::string>& DefaultExtensions();

// The extensions linked into this binary (duckdb_extensions() install_mode STATICALLY_LINKED:
// core_functions, parquet, json and icu in the DuckDB 2.0 build). They are loaded when the
// database opens, so INSTALL/LOAD of one is a no-op - except that INSTALL still downloads
// the loadable copy (json 51 MB, core_functions 40 MB, parquet 32 MB) that nothing ever
// opens. Empty when the query fails: then nothing is skipped.
std::set<std::string> StaticallyLinkedExtensions(duckdb_connection connection);

// Parses `name` or `name@repository`. Both parts must be identifiers: they end up in SQL.
std::optional<ExtensionSpec> ParseExtensionSpec(const std::string& arg);

std::string BuildInstallStatement(const ExtensionSpec& spec);

// The `[FORCE] INSTALL <name> [FROM <repository>]` statements in a block of init SQL.
std::vector<ExtensionSpec> ExtractInstallSpecs(const std::string& init_sql);

// ImplicitExtensions(), duckdb.default-extensions, then every INSTALL in connections.*.init; first occurrence wins.
std::vector<ExtensionSpec> CollectExtensionSpecs(const ConfigManager& config_manager);

// Installs into `extension_directory` (empty = DuckDB's default). Returns the number of
// extensions that failed; every failure is written to `log`, so a build step reports all at once.
// An extension the shipped directory carries is copied from there (no download); with no
// target directory it is not installed at all - the server finds it there anyway.
int InstallExtensions(const std::vector<ExtensionSpec>& specs, const std::string& extension_directory,
                      std::ostream& log);

// The directory a bundle's extensions were extracted to (set once at startup by main).
// When set, it is the default extension_directory and implies offline extensions.
void SetBundledExtensionDirectory(std::optional<std::filesystem::path> dir);
std::optional<std::filesystem::path> BundledExtensionDirectory();

// ---- Extensions shipped with a release (#245) ----------------------------------------------
// Release archives, wheels and the Docker image carry the signed default extensions in an
// `extensions/` directory beside the executable, in DuckDB's own layout
// (extensions/<duckdb version>/<platform>/<name>.duckdb_extension).

// The outcome of looking for that directory: `root` (the `extensions/` directory, which is
// what DuckDB is given - it appends <version>/<platform> itself) when it carries extensions
// for `version`/`platform`; otherwise `ignored` says why an existing directory was not used
// (empty when there is none at all, which is not worth a log line).
struct ShippedExtensions {
    std::optional<std::filesystem::path> root;
    std::string ignored;
};
ShippedExtensions FindShippedExtensions(const std::filesystem::path& executable, const std::string& version,
                                        const std::string& platform);

// FindShippedExtensions for the running binary (symlinks resolved), computed and logged once.
std::optional<std::filesystem::path> ShippedExtensionDirectory();

// `SET GLOBAL extension_directories = [<user dir>, <shipped dir>]`, or nothing when there is
// no shipped directory. DuckDB installs into the FIRST directory and loads from the first
// that has the file, so the user directory stays the (writable) install target, the shipped
// one is only ever read, and a copy the user installed on purpose (FORCE INSTALL) wins.
// Callers run it only when no extension directory is configured: an explicit
// FLAPI_EXTENSION_DIRECTORY / duckdb.extension_directory, or a bundle's, is used alone.
std::optional<std::string> ShippedExtensionSearchStatement();

// The statement for an arbitrary list (exposed for tests): SQL string literals, so any path works.
std::string ExtensionDirectoriesStatement(const std::vector<std::string>& directories);

// LOAD `name`; when that fails and downloads are allowed, INSTALL then LOAD again. Loading first
// means an extension found in ANY search directory (the shipped one included) never causes a
// download: INSTALL only looks at the first directory. Returns the last error, empty on success.
std::string LoadOrInstallExtension(duckdb_connection connection, const std::string& name, bool offline);

// Whether `connection`'s database still has external access (duckdb-hardening, #253).
bool ExternalAccessEnabled(duckdb_connection connection);

// True when extensions must not be downloaded: FLAPI_EXTENSIONS_OFFLINE (1/true/yes/on, any case) when set,
// otherwise whether a bundle carries its extensions. The one spelling every caller shares.
bool ExtensionsOfflineRequested();

// Writes the bundle's kBundledExtensionsPrefix entries below `cache_root`, in a directory
// named by a hash of their contents (reused when it already exists). Returns that
// directory, or nothing when the bundle carries no extensions.
std::optional<std::filesystem::path> MaterializeBundledExtensions(
    const std::map<std::string, std::vector<std::uint8_t>>& entries, const std::filesystem::path& cache_root);

} // namespace flapi
