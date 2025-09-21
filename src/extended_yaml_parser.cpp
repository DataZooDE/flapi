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
ExtendedYamlParser::ExtendedYamlParser() : config_() {}

ExtendedYamlParser::ExtendedYamlParser(const IncludeConfig& config) : config_(config) {}

ExtendedYamlParser::ParseResult ExtendedYamlParser::parseFile(const std::filesystem::path& file_path,
                                                             const std::filesystem::path& base_path) {
    ParseResult result;

    try {
        // Determine base path
        std::filesystem::path actual_base_path = base_path;
        if (actual_base_path.empty()) {
            actual_base_path = file_path.parent_path();
        }

        // Load initial YAML
        result.node = loadYamlFile(file_path);

        // Track included files for circular dependency detection
        std::unordered_set<std::string> included_files;
        included_files.insert(std::filesystem::absolute(file_path).string());

        // Preprocess includes
        if (!preprocessIncludes(result.node, actual_base_path, included_files)) {
            result.success = false;
            result.error_message = "Failed to preprocess includes";
            return result;
        }

        result.included_files.insert(result.included_files.end(), included_files.begin(), included_files.end());
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
        // Parse initial YAML
        result.node = YAML::Load(content);

        // Track included files for circular dependency detection
        std::unordered_set<std::string> included_files;

        // Preprocess includes
        if (!preprocessIncludes(result.node, base_path, included_files)) {
            result.success = false;
            result.error_message = "Failed to preprocess includes";
            return result;
        }

        result.included_files.insert(result.included_files.end(), included_files.begin(), included_files.end());
        result.success = true;

    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Parse error: ") + e.what();
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
        std::cout << "DEBUG: Node is not scalar, type: " << node.Type() << std::endl;
        return true;
    }

    std::string value = node.Scalar();
    std::cout << "DEBUG: Processing scalar node: '" << value << "'" << std::endl;
    if (!containsIncludeDirective(value)) {
        std::cout << "DEBUG: No include directive found in: " << value << std::endl;
        return true;
    }

    std::cout << "DEBUG: Found include directive, processing..." << std::endl;
    std::string processed = processIncludeDirectives(value, base_path, included_files);
    std::cout << "DEBUG: Processed result: '" << processed << "'" << std::endl;
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
    std::regex include_regex(R"(\{\{include(?::([^}]+))?\s+from\s+([^}]+)(?:\s+if\s+([^}]+))?\}\})");

    auto matches_begin = std::sregex_iterator(input.begin(), input.end(), include_regex);
    auto matches_end = std::sregex_iterator();

    std::cout << "DEBUG: Processing include directives in: " << input << std::endl;
    std::cout << "DEBUG: Found " << std::distance(matches_begin, matches_end) << " matches" << std::endl;

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

            // Parse include directive
            auto include_info_opt = parseIncludeDirective(file_path_str);
            if (!include_info_opt) {
                throw std::runtime_error("Invalid include directive: " + file_path_str);
            }

            auto& include_info = *include_info_opt;

            // Resolve include path
            std::filesystem::path resolved_path;
            if (!resolveIncludePath(include_info.file_path, base_path, resolved_path, config_.include_paths)) {
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
            std::string replacement = YAML::Dump(included_node);
            result.replace(pos, length, replacement);
        }
    }

    // Substitute environment variables
    if (config_.allow_environment_variables) {
        result = substituteEnvironmentVariables(result);
    }

    return result;
}

bool ExtendedYamlParser::containsIncludeDirective(const std::string& str) const {
    bool has_include = str.find("{{include") != std::string::npos;
    bool has_close = str.find("}}") != std::string::npos;
    bool result = has_include && has_close;
    std::cout << "DEBUG: containsIncludeDirective('" << str << "') -> include: " << has_include << ", close: " << has_close << ", result: " << result << std::endl;
    return result;
}

std::optional<ExtendedYamlParser::IncludeInfo> ExtendedYamlParser::parseIncludeDirective(const std::string& directive) {
    // Parse "{{include:section from file.yaml}}" or "{{include from file.yaml}}"
    std::regex directive_regex(R"(\{\{include(?::([^}]+))?\s+from\s+([^}]+)(?:\s+if\s+([^}]+))?\}\})");

    std::smatch match;
    if (!std::regex_search(directive, match, directive_regex)) {
        return std::nullopt;
    }

    std::string section = match[1].str();
    std::string file_path = match[2].str();
    std::string condition = match[3].str();

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
    std::string result = input;
    std::regex env_regex(R"(\{\{env\.([A-Za-z_][A-Za-z0-9_]*)\}\})");

    auto matches_begin = std::sregex_iterator(result.begin(), result.end(), env_regex);
    auto matches_end = std::sregex_iterator();

    for (auto it = matches_begin; it != matches_end; ++it) {
        std::string var_name = it->str(1);

        if (!config_.isEnvironmentVariableAllowed(var_name)) {
            continue; // Skip disallowed variables
        }

        const char* env_value = std::getenv(var_name.c_str());
        std::string replacement = env_value ? env_value : "";

        resolved_variables_[var_name] = replacement;
        result = std::regex_replace(result, std::regex("\\{\\{env\\." + var_name + "\\}\\}"), replacement);
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
