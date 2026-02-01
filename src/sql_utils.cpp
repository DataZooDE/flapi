#include "sql_utils.hpp"
#include <algorithm>
#include <cctype>

namespace flapi {

std::string trimSqlString(const std::string& str) {
    const auto start = std::find_if_not(str.begin(), str.end(),
        [](unsigned char ch) { return std::isspace(ch); });
    const auto end = std::find_if_not(str.rbegin(), str.rend(),
        [](unsigned char ch) { return std::isspace(ch); }).base();

    if (start >= end) {
        return "";
    }
    return std::string(start, end);
}

std::vector<std::string> splitSqlStatements(const std::string& query) {
    std::vector<std::string> statements;
    std::string current;

    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    bool inDollarQuote = false;
    std::string dollarTag;

    for (size_t i = 0; i < query.size(); ++i) {
        char c = query[i];

        // Handle dollar-quoting ($tag$...$tag$ or $$...$$)
        // Only check for dollar quotes when not inside other quotes
        if (!inSingleQuote && !inDoubleQuote && c == '$') {
            // Look for the end of the tag (next $)
            size_t tagEnd = query.find('$', i + 1);
            if (tagEnd != std::string::npos) {
                std::string potentialTag = query.substr(i, tagEnd - i + 1);

                // Validate tag: must be $identifier$ where identifier is alphanumeric or empty
                bool validTag = true;
                for (size_t j = 1; j < potentialTag.size() - 1; ++j) {
                    char tc = potentialTag[j];
                    if (!std::isalnum(static_cast<unsigned char>(tc)) && tc != '_') {
                        validTag = false;
                        break;
                    }
                }

                if (validTag) {
                    if (inDollarQuote && potentialTag == dollarTag) {
                        // Closing dollar quote
                        inDollarQuote = false;
                        current += potentialTag;
                        i = tagEnd;
                        continue;
                    } else if (!inDollarQuote) {
                        // Opening dollar quote
                        inDollarQuote = true;
                        dollarTag = potentialTag;
                        current += potentialTag;
                        i = tagEnd;
                        continue;
                    }
                }
            }
            // If we get here, it's just a regular $ character
            current += c;
            continue;
        }

        // Handle single quotes (with '' escape)
        // Only process if not in double quotes or dollar quotes
        if (!inDoubleQuote && !inDollarQuote && c == '\'') {
            if (inSingleQuote) {
                // Check for escaped quote ''
                if (i + 1 < query.size() && query[i + 1] == '\'') {
                    // Escaped quote - add both and skip next
                    current += "''";
                    ++i;
                    continue;
                } else {
                    // Closing quote
                    inSingleQuote = false;
                }
            } else {
                // Opening quote
                inSingleQuote = true;
            }
            current += c;
            continue;
        }

        // Handle double quotes
        // Only process if not in single quotes or dollar quotes
        if (!inSingleQuote && !inDollarQuote && c == '"') {
            if (inDoubleQuote) {
                // Check for escaped quote ""
                if (i + 1 < query.size() && query[i + 1] == '"') {
                    current += "\"\"";
                    ++i;
                    continue;
                } else {
                    inDoubleQuote = false;
                }
            } else {
                inDoubleQuote = true;
            }
            current += c;
            continue;
        }

        // Split on semicolon only if not in any quoted context
        if (c == ';' && !inSingleQuote && !inDoubleQuote && !inDollarQuote) {
            std::string trimmed = trimSqlString(current);
            if (!trimmed.empty()) {
                statements.push_back(trimmed);
            }
            current.clear();
        } else {
            current += c;
        }
    }

    // Don't forget the last statement (may not have trailing semicolon)
    std::string trimmed = trimSqlString(current);
    if (!trimmed.empty()) {
        statements.push_back(trimmed);
    }

    return statements;
}

} // namespace flapi
