#pragma once

#include <string>
#include <vector>

namespace flapi {

/**
 * Splits SQL query into statements by semicolons, respecting quoted strings.
 *
 * This function is critical for security - it must correctly identify
 * statement boundaries without being fooled by semicolons inside strings.
 *
 * Handles:
 * - Single quotes with '' escape: 'text with '';' semicolon'
 * - Double quotes: "identifier; with semicolon"
 * - Dollar-quoting: $tag$content; here$tag$ (PostgreSQL/DuckDB style)
 *
 * Security considerations:
 * - Unclosed quotes treat the rest of the string as quoted (fail-safe)
 * - Backslash is NOT treated as escape character (SQL standard uses '')
 * - Empty statements are filtered out
 *
 * @param query The SQL query string potentially containing multiple statements
 * @return Vector of individual SQL statements, trimmed of whitespace
 */
std::vector<std::string> splitSqlStatements(const std::string& query);

/**
 * Trims whitespace from both ends of a string.
 * @param str The string to trim
 * @return Trimmed string
 */
std::string trimSqlString(const std::string& str);

} // namespace flapi
