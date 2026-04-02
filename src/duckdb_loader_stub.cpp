#include "duckdb/main/extension_helper.hpp"

// Dummy loader — provides empty implementations so the project links without
// pulling in any DuckDB extensions. Built from source to avoid depending on
// the system-installed version, which may be compiled with GCC LTO.

namespace duckdb {
void ExtensionHelper::LoadAllExtensions(DuckDB &db) {
	// nop
}
} // namespace duckdb
