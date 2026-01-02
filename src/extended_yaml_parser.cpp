#include "include/extended_yaml_parser.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <algorithm>
#include <cstdlib> // for getenv

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#endif

namespace flapi {

// IncludeConfig implementation
bool ExtendedYamlParser::IncludeConfig::isEnvironmentVariableAllowed(const std::string& var_name) const {
    if (environment_whitelist.empty()) {
        return true; // Allow all if no whitelist
    }

    for (const auto& pattern : environment_whitelist) {
        std::regex regex_pattern(pattern, std::regex_constants::icase);
        if (std::regex_match(var_name, regex_pattern)) {
            return true;
        }
    }
    return false;
}

// ExtendedYamlParser implementation
ExtendedYamlParser::ExtendedYamlParser() : config_() {
    CROW_LOG_DEBUG << "ExtendedYamlParser default constructor called, environment variables allowed: " << config_.allow_environment_variables;
}

ExtendedYamlParser::ExtendedYamlParser(const IncludeConfig& config) : config_(config) {
    CROW_LOG_DEBUG << "ExtendedYamlParser constructor with config called, environment variables allowed: " << config_.allow_environment_variables;
}

ExtendedYamlParser::ParseResult ExtendedYamlParser::parseFile(const std::filesystem::path& file_path,
                                                             const std::filesystem::path& base_path) {
    ParseResult result;

    try {
        // Determine base path
        std::filesystem::path actual_base_path = base_path;
        if (actual_base_path.empty()) {
            actual_base_path = file_path.parent_path();
        }

        // Load file content
        std::ifstream file(file_path);
        if (!file.is_open()) {
            result.success = false;
            result.error_message = "Could not open file: " + file_path.string();
            return result;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();

        // Track included files for circular dependency detection
        std::unordered_set<std::string> included_files;
        included_files.insert(std::filesystem::absolute(file_path).string());

        // Preprocess includes before parsing YAML
        std::string processed_content = preprocessContent(content, actual_base_path, included_files);

        // If no includes were processed, remove the main file from included_files
        if (included_files.size() == 1) {
            included_files.clear();
        }

        // Parse the processed YAML
        result.node = YAML::Load(processed_content);

        result.included_files.insert(result.included_files.end(), included_files.begin(), included_files.end());
        result.resolved_variables = resolved_variables_;
        result.success = true;

    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Parse error: ") + e.what();
    }

    return result;
}

ExtendedYamlParser::ParseResult ExtendedYamlParser::parseString(const std::string& content,
                                                               const std::filesystem::path& base_path) {
    ParseResult result;

    try {
        CROW_LOG_DEBUG << "parseString called with content length: " << content.length();

        // Preprocess includes before parsing YAML
        std::unordered_set<std::string> included_files;
        std::string processed_content = preprocessContent(content, base_path, included_files);

        // Parse the processed YAML
        result.node = YAML::Load(processed_content);

        // If no includes were processed, don't add any files to result
        if (included_files.empty()) {
            result.included_files.clear();
        } else {
            result.included_files.insert(result.included_files.end(), included_files.begin(), included_files.end());
        }
        result.resolved_variables = resolved_variables_;
        result.success = true;

    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Parse error: ") + e.what();
        CROW_LOG_DEBUG << "Parse error: " << e.what();
    }

    return result;
}

std::string ExtendedYamlParser::preprocessContent(const std::string& content,
                                                 const std::filesystem::path& base_path,
                                                 std::unordered_set<std::string>& included_files) {
    std::string result = content;
    std::regex include_regex(R"(\{\{include(?::([^}]+))?\s+from\s+((?:[^{}]|\{\{[^}]*\}\})*?)(?:\s+if\s+([^}]+))?\}\})");

    CROW_LOG_DEBUG << "preprocessContent called with content length: " << content.length();

        // Always perform environment variable substitution, even if no includes
        if (config_.allow_environment_variables) {
            CROW_LOG_DEBUG << "Environment variable substitution enabled";
            std::string before_substitution = result;
            result = substituteEnvironmentVariables(result);
            //CROW_LOG_DEBUG << "Environment substitution: '" << before_substitution << "' -> '" << result << "'";
        }

    // Find include directives after environment variable substitution
    auto matches_begin = std::sregex_iterator(result.begin(), result.end(), include_regex);
    auto matches_end = std::sregex_iterator();


    // Filter out include directives that are inside YAML comments
    std::vector<std::smatch> valid_matches;
    for (auto it = matches_begin; it != matches_end; ++it) {
        const std::smatch& match = *it;
        size_t match_pos = match.position();
        
        // Find the start of the line containing this match
        size_t line_start = result.rfind('\n', match_pos);
        if (line_start == std::string::npos) {
            line_start = 0;
        } else {
            line_start++; // Skip the newline character
        }
        
        // Check if this line starts with # (YAML comment)
        bool is_in_comment = false;
        std::string line_content;
        for (size_t i = line_start; i < match_pos; i++) {
            char c = result[i];
            line_content += c;
            if (c == ' ' || c == '\t') {
                // Skip leading whitespace
                continue;
            } else if (c == '#') {
                // This line is a comment, skip this include directive
                is_in_comment = true;
                break;
            } else {
                // This line is not a comment, include directive is valid
                break;
            }
        }
        
        if (!is_in_comment) {
            valid_matches.push_back(match);
        } else {
            CROW_LOG_DEBUG << "Skipping include directive in YAML comment at position " << match_pos;
        }
    }

    // If no valid include directives found, return the result after environment substitution
    if (valid_matches.empty()) {
        CROW_LOG_DEBUG << "No valid include directives found, returning after environment substitution";
        return result;
    }

    CROW_LOG_DEBUG << "Found " << valid_matches.size() << " valid include directives (excluding comments)";

    // Process matches in reverse order to maintain positions
    std::vector<std::pair<size_t, size_t>> match_positions;
    for (const auto& match : valid_matches) {
        match_positions.push_back({match.position(), match.length()});
    }

    std::reverse(match_positions.begin(), match_positions.end());

    for (const auto& [pos, length] : match_positions) {
        std::smatch match;
        if (std::regex_search(result.cbegin() + pos, result.cbegin() + pos + length, match, include_regex)) {
            std::string section = match[1].str();
            std::string file_path_str = match[2].str();
            std::string condition = match[3].str();

            // Substitute environment variables in the file path
            if (config_.allow_environment_variables) {
                file_path_str = substituteEnvironmentVariables(file_path_str);
            }

            // Reconstruct the directive with the substituted file path
            std::string substituted_directive;
            if (!section.empty()) {
                substituted_directive = "{{include:" + section + " from " + file_path_str;
            } else {
                substituted_directive = "{{include from " + file_path_str;
            }
            if (!condition.empty()) {
                substituted_directive += " if " + condition;
            }
            substituted_directive += "}}";

            // Check conditional include
            if (!condition.empty()) {
                if (!config_.allow_conditional_includes) {
                    // Conditional includes are disabled, generate error
                    CROW_LOG_DEBUG << "Conditional includes are disabled, failing parse";
                    throw std::runtime_error("Invalid include directive: conditional includes are disabled. Use IncludeConfig::allow_conditional_includes = true to enable them.");
                }

                CROW_LOG_DEBUG << "Evaluating conditional include: condition='" << condition << "'";
                if (!evaluateCondition(condition)) {
                    // Replace with empty string for false conditions
                    result.replace(pos, length, "");
                    CROW_LOG_DEBUG << "Conditional include evaluated to false, replacing with empty string";
                    continue;
                } else {
                    CROW_LOG_DEBUG << "Conditional include evaluated to true, processing include";
                }
            }

            // Parse include directive with substituted file path
            auto include_info_opt = parseIncludeDirective(substituted_directive);
            if (!include_info_opt) {
                continue;
            }

            auto& include_info = *include_info_opt;

            // Resolve include path
            std::filesystem::path resolved_path;
            if (!resolveIncludePath(include_info.file_path, base_path, resolved_path, config_.include_paths)) {
                CROW_LOG_DEBUG << "Could not resolve include path: " << include_info.file_path;
                throw std::runtime_error("Could not resolve include path: " + include_info.file_path.string());
            }

            // Check for circular dependency - only prevent if this file is currently being processed
            std::string abs_path = std::filesystem::absolute(resolved_path).string();
            
            // Only prevent circular dependency if this file is currently in the inclusion chain
            // Allow multiple includes from the same file within one parsing session
            bool is_circular = included_files.count(abs_path) > 0;

            // Load the included file
            YAML::Node included_node;
            try {
                included_node = loadYamlFile(resolved_path);
                // Don't add to included_files for multiple includes from same file
                if (!is_circular) {
                    included_files.insert(abs_path);
                }
            } catch (const std::exception& e) {
                CROW_LOG_DEBUG << "Failed to load include file: " << e.what();
                throw std::runtime_error("Could not resolve include path: " + resolved_path.string());
            }

            // Extract section if specified
            if (include_info.is_section_include) {
                included_node = extractSection(included_node, include_info.section_name);
            }

            // Convert to string and replace
            std::string replacement;
            
            if (include_info.is_section_include) {
                // For section includes, we need to preserve the section name
                // Create a temporary node with the section name as the key
                YAML::Node temp_node;
                temp_node[include_info.section_name] = included_node;
                replacement = YAML::Dump(temp_node);
                
                // Remove the YAML document markers (---, ...)
                std::string temp_replacement = replacement;
                // Remove leading "---\n" if present
                if (temp_replacement.substr(0, 4) == "---\n") {
                    temp_replacement = temp_replacement.substr(4);
                }
                // Remove trailing "\n..." if present  
                size_t dots_pos = temp_replacement.rfind("\n...");
                if (dots_pos != std::string::npos) {
                    temp_replacement = temp_replacement.substr(0, dots_pos);
                }
                replacement = temp_replacement;
            } else {
                replacement = YAML::Dump(included_node);
            }
            result.replace(pos, length, replacement);
        }
    }

    return result;
}

bool ExtendedYamlParser::preprocessIncludes(YAML::Node& node,
                                           const std::filesystem::path& base_path,
                                           std::unordered_set<std::string>& included_files) {
    if (!node.IsDefined()) {
        return true;
    }

    switch (node.Type()) {
        case YAML::NodeType::Scalar:
            return processScalarNode(node, base_path, included_files);

        case YAML::NodeType::Map:
            return processMapNode(node, base_path, included_files);

        case YAML::NodeType::Sequence:
            return processSequenceNode(node, base_path, included_files);

        default:
            return true; // No processing needed for other types
    }
}

bool ExtendedYamlParser::processScalarNode(YAML::Node& node,
                                          const std::filesystem::path& base_path,
                                          std::unordered_set<std::string>& included_files) {
    if (!node.IsScalar()) {
        return true;
    }

    std::string value = node.Scalar();
    if (!containsIncludeDirective(value)) {
        return true;
    }

    std::string processed = processIncludeDirectives(value, base_path, included_files);
    node = processed;
    return true;
}

bool ExtendedYamlParser::processMapNode(YAML::Node& node,
                                       const std::filesystem::path& base_path,
                                       std::unordered_set<std::string>& included_files) {
    if (!node.IsMap()) {
        return true;
    }

    // First pass: collect keys that need processing
    std::vector<std::string> keys_to_process;
    for (const auto& item : node) {
        std::string key = item.first.Scalar();
        if (key.find("{{") != std::string::npos) {
            keys_to_process.push_back(key);
        }
    }

    // Process keys that contain include directives
    for (const auto& old_key : keys_to_process) {
        YAML::Node key_node = YAML::Node(old_key);
        if (processScalarNode(key_node, base_path, included_files)) {
            std::string new_key = key_node.Scalar();

            // Move the value to new key and remove old key
            if (node[new_key]) {
                // Key already exists, merge the nodes
                mergeNodes(node[new_key], node[old_key]);
            } else {
                node[new_key] = node[old_key];
            }
            node.remove(old_key);
        }
    }

    // Process values
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!preprocessIncludes(it->second, base_path, included_files)) {
            return false;
        }
    }

    return true;
}

bool ExtendedYamlParser::processSequenceNode(YAML::Node& node,
                                            const std::filesystem::path& base_path,
                                            std::unordered_set<std::string>& included_files) {
    if (!node.IsSequence()) {
        return true;
    }

    for (auto it = node.begin(); it != node.end(); ++it) {
        YAML::Node item = *it;  // Create a copy to pass by reference
        if (!preprocessIncludes(item, base_path, included_files)) {
            return false;
        }
    }

    return true;
}

std::string ExtendedYamlParser::processIncludeDirectives(const std::string& input,
                                                        const std::filesystem::path& base_path,
                                                        std::unordered_set<std::string>& included_files) {
    std::string result = input;
    std::regex include_regex(R"(\{\{include(?::([^}]+))?\s+from\s+((?:[^{}]|\{\{[^}]*\}\})*?)(?:\s+if\s+([^}]+))?\}\})");

    auto matches_begin = std::sregex_iterator(input.begin(), input.end(), include_regex);
    auto matches_end = std::sregex_iterator();

    CROW_LOG_DEBUG << "Processing include directives in input of length: " << input.length();
    CROW_LOG_DEBUG << "Found " << std::distance(matches_begin, matches_end) << " matches";

    // Process matches in reverse order to maintain positions
    std::vector<std::pair<size_t, size_t>> match_positions;
    for (auto it = matches_begin; it != matches_end; ++it) {
        match_positions.push_back({it->position(), it->length()});
    }

    std::reverse(match_positions.begin(), match_positions.end());

    for (const auto& [pos, length] : match_positions) {
        std::smatch match;
        if (std::regex_search(input.begin() + pos, input.begin() + pos + length, match, include_regex)) {
            std::string section = match[1].str();
            std::string file_path_str = match[2].str();
            std::string condition = match[3].str();

            // Check conditional include
            if (!condition.empty() && config_.allow_conditional_includes) {
                if (!evaluateCondition(condition)) {
                    // Replace with empty string for false conditions
                    result.replace(pos, length, "");
                    continue;
                }
            }

            // Substitute environment variables in file path if they exist
            if (config_.allow_environment_variables && file_path_str.find("{{env.") != std::string::npos) {
                CROW_LOG_DEBUG << "Substituting environment variables in file path: " << file_path_str;
                file_path_str = substituteEnvironmentVariables(file_path_str);
                CROW_LOG_DEBUG << "File path after substitution: " << file_path_str;
            }

            // Reconstruct the full directive with the substituted file path
            std::string full_directive = "{{include" + (section.empty() ? "" : ":" + section) + " from " + file_path_str + (condition.empty() ? "" : " if " + condition) + "}}";

            // Parse include directive
            auto include_info_opt = parseIncludeDirective(full_directive);
            if (!include_info_opt) {
                throw std::runtime_error("Invalid include directive: " + file_path_str);
            }

            auto& include_info = *include_info_opt;

            // Resolve include path
            std::filesystem::path resolved_path;
            if (!resolveIncludePath(include_info.file_path, base_path, resolved_path, config_.include_paths)) {
                CROW_LOG_DEBUG << "Could not resolve include path: " << include_info.file_path;
                throw std::runtime_error("Could not resolve include path: " + include_info.file_path.string());
            }

            // Check for circular dependency
            std::string abs_path = std::filesystem::absolute(resolved_path).string();
            if (included_files.count(abs_path)) {
                throw std::runtime_error("Circular dependency detected including file: " + abs_path);
            }

            // Load the included file
            YAML::Node included_node;
            try {
                included_node = loadYamlFile(resolved_path);
                included_files.insert(abs_path);
            } catch (const std::exception& e) {
                throw std::runtime_error("Failed to load included file '" + resolved_path.string() + "': " + e.what());
            }

            // Extract section if specified
            if (include_info.is_section_include) {
                included_node = extractSection(included_node, include_info.section_name);
            }

            // Convert to string and replace
            std::string replacement;
            
            if (include_info.is_section_include) {
                // For section includes, we need to preserve the section name
                // Create a temporary node with the section name as the key
                YAML::Node temp_node;
                temp_node[include_info.section_name] = included_node;
                replacement = YAML::Dump(temp_node);
                
                // Remove the leading "---" and trailing "..." that YAML::Dump adds
                replacement = replacement.substr(replacement.find('\n') + 1);
                replacement = replacement.substr(0, replacement.rfind('\n'));
                // Remove extra indentation
                size_t indent_pos = replacement.find(include_info.section_name + ":");
                if (indent_pos != std::string::npos) {
                    replacement = replacement.substr(indent_pos);
                }
            } else {
                replacement = YAML::Dump(included_node);
            }
            result.replace(pos, length, replacement);
        }
    }

    // Substitute environment variables
    if (config_.allow_environment_variables) {
        CROW_LOG_DEBUG << "Environment variable substitution enabled, calling substituteEnvironmentVariables";
        result = substituteEnvironmentVariables(result);
    } else {
        CROW_LOG_DEBUG << "Environment variable substitution disabled";
    }

    return result;
}

bool ExtendedYamlParser::containsIncludeDirective(const std::string& str) const {
    return str.find("{{include") != std::string::npos && str.find("}}") != std::string::npos;
}

std::optional<ExtendedYamlParser::IncludeInfo> ExtendedYamlParser::parseIncludeDirective(const std::string& directive) const {
    // Parse "{{include:section from file.yaml}}" or "{{include from file.yaml}}"
    // Note: The file path should not contain ' if ' or '}}' to avoid confusion with conditional includes
    // Complex regex that handles nested braces in file paths (like {{env.VAR}})
    std::regex directive_regex(R"(\{\{include(?::([^}]+))?\s+from\s+((?:[^{}]|\{\{[^}]*\}\})*?)(?:\s+if\s+([^}]+))?\}\})");

    std::smatch match;
    if (!std::regex_search(directive, match, directive_regex)) {
        CROW_LOG_DEBUG << "Failed to parse include directive: " << directive;
        return std::nullopt;
    }

    // File path may contain environment variables that have already been substituted
    std::string section = match[1].str();
    std::string file_path = match[2].str();
    std::string condition = match[3].str();

    CROW_LOG_DEBUG << "Regex match results: directive='" << directive << "'";
    CROW_LOG_DEBUG << "  match[0]='" << match[0].str() << "'";
    CROW_LOG_DEBUG << "  match[1]='" << match[1].str() << "'";
    CROW_LOG_DEBUG << "  match[2]='" << match[2].str() << "'";
    CROW_LOG_DEBUG << "  match[3]='" << match[3].str() << "'";
    CROW_LOG_DEBUG << "  match.size()=" << match.size();

    bool is_section_include = !section.empty();
    bool is_conditional = !condition.empty();

    IncludeInfo info(section, std::filesystem::path(file_path), is_section_include);
    info.is_conditional = is_conditional;
    info.condition = condition;

    return info;
}

bool ExtendedYamlParser::resolveIncludePath(const std::filesystem::path& include_path,
                                           const std::filesystem::path& base_path,
                                           std::filesystem::path& resolved_path,
                                           const std::vector<std::string>& include_paths) {
    // First try relative to base path
    resolved_path = base_path / include_path;
    if (std::filesystem::exists(resolved_path)) {
        return true;
    }

    // Try absolute path
    if (include_path.is_absolute() && std::filesystem::exists(include_path)) {
        resolved_path = include_path;
        return true;
    }

    // Try include paths
    for (const auto& include_base : include_paths) {
        resolved_path = std::filesystem::path(include_base) / include_path;
        if (std::filesystem::exists(resolved_path)) {
            return true;
        }
    }

    return false;
}

YAML::Node ExtendedYamlParser::loadYamlFile(const std::filesystem::path& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + file_path.string());
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return YAML::Load(buffer.str());
}

YAML::Node ExtendedYamlParser::extractSection(const YAML::Node& node, const std::string& section_name) {
    if (node[section_name]) {
        return node[section_name];
    }

    throw std::runtime_error("Section '" + section_name + "' not found in YAML file");
}

YAML::Node ExtendedYamlParser::mergeNodes(const YAML::Node& target, const YAML::Node& source) {
    if (!target.IsMap() || !source.IsMap()) {
        return source; // If not maps, just return source
    }

    YAML::Node result = YAML::Clone(target);

    for (const auto& item : source) {
        std::string key = item.first.Scalar();

        if (result[key] && result[key].IsMap() && item.second.IsMap()) {
            // Recursively merge nested maps
            result[key] = mergeNodes(result[key], item.second);
        } else {
            // Replace or add
            result[key] = item.second;
        }
    }

    return result;
}

std::string ExtendedYamlParser::substituteEnvironmentVariables(const std::string& input) const {
    // If environment variables are disabled, return input unchanged
    if (!config_.allow_environment_variables) {
        CROW_LOG_DEBUG << "Environment variable substitution disabled, returning input unchanged";
        return input;
    }

    std::string result = input;
    std::regex env_regex(R"(\{\{env\.([A-Za-z_][A-Za-z0-9_]*)\}\})");

    auto matches_begin = std::sregex_iterator(result.begin(), result.end(), env_regex);
    auto matches_end = std::sregex_iterator();

    int match_count = std::distance(matches_begin, matches_end);
    CROW_LOG_DEBUG << "Environment variable substitution: found " << match_count << " matches in input";

    if (match_count == 0) {
        return result;
    }

    // ✅ FIX STEP 1: Collect all match information FIRST (no string modification)
    struct EnvVarMatch {
        size_t position;
        size_t length;
        std::string var_name;
        std::string replacement;
    };

    std::vector<EnvVarMatch> matches;
    matches.reserve(match_count);

    // Recreate iterator (previous one was consumed by std::distance)
    matches_begin = std::sregex_iterator(result.begin(), result.end(), env_regex);

    for (auto it = matches_begin; it != matches_end; ++it) {
        std::string var_name = it->str(1);
        CROW_LOG_DEBUG << "Processing environment variable: " << var_name;

        if (!config_.isEnvironmentVariableAllowed(var_name)) {
            CROW_LOG_DEBUG << "Environment variable not allowed: " << var_name;
            continue; // Skip disallowed variables
        }

        const char* env_value = std::getenv(var_name.c_str());
        std::string replacement = env_value ? env_value : "";
        CROW_LOG_DEBUG << "Environment variable " << var_name << " = '" << replacement << "'";

        // Store for logging (preserve existing behavior)
        resolved_variables_[var_name] = replacement;

        // Collect match info (no string modification yet)
        matches.push_back({
            static_cast<size_t>(it->position()),
            static_cast<size_t>(it->length()),
            var_name,
            replacement
        });
    }

    // ✅ FIX STEP 2: Process replacements in REVERSE order
    // (This preserves positions of earlier matches)
    std::reverse(matches.begin(), matches.end());

    // ✅ FIX STEP 3: Perform replacements using collected positions
    for (const auto& match : matches) {
        result.replace(match.position, match.length, match.replacement);
        CROW_LOG_DEBUG << "Replaced environment variable in result";
    }

    return result;
}

bool ExtendedYamlParser::evaluateCondition(const std::string& condition) const {
    // Simple condition evaluation
    // Support: "true", "false", "env.VAR_NAME", "!env.VAR_NAME"

    if (condition == "true") return true;
    if (condition == "false") return false;

    if (condition.find("env.") == 0) {
        std::string var_name = condition.substr(4);
        const char* env_value = std::getenv(var_name.c_str());
        return env_value != nullptr && std::string(env_value) != "";
    }

    if (condition.find("!env.") == 0) {
        std::string var_name = condition.substr(5);
        const char* env_value = std::getenv(var_name.c_str());
        return env_value == nullptr || std::string(env_value) == "";
    }

    // For more complex conditions, could extend this
    return false;
}

} // namespace flapi
