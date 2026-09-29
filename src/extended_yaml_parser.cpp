#include "include/extended_yaml_parser.hpp"
#include "include/vfs_adapter.hpp"
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

namespace {

// Read a file through FileProviderFactory so bundled-mode startup
// (`flapi pack` then run from a clean cwd) can resolve `flapi.yaml`
// and any `{{include from ...}}` references against the in-memory
// archive instead of std::ifstream. When no bundle is active the
// factory returns a LocalFileProvider, so the unbundled path is
// behaviourally unchanged.
std::string ReadConfigFile(const std::filesystem::path& file_path) {
    const std::string path_str = file_path.string();
    auto provider = FileProviderFactory::CreateProvider(path_str);
    if (!provider->FileExists(path_str)) {
        throw std::runtime_error("Could not open file: " + path_str);
    }
    return provider->ReadFile(path_str);
}

// Provider-aware existence check used by include resolution. When a bundle
// is active, std::filesystem::exists would always say "no" for entries
// that live only in the in-memory archive.
bool ConfigFileExists(const std::filesystem::path& file_path) {
    const std::string path_str = file_path.string();
    auto provider = FileProviderFactory::CreateProvider(path_str);
    return provider->FileExists(path_str);
}

}  // namespace

// IncludeConfig implementation
bool ExtendedYamlParser::IncludeConfig::isEnvironmentVariableAllowed(const std::string& var_name) const {
    if (environment_whitelist.empty()) {
        return false; // Empty means NONE - see the field's comment (#157)
    }

    for (const auto& pattern : environment_whitelist) {
        // Case-SENSITIVE, like the SQL-template matcher. Environment variable names
        // are case-sensitive on Linux and macOS, so `aws_region` is a different
        // variable from `AWS_REGION`: an icase match let a pattern for one authorise
        // reading the other (found by the security review of #157).
        std::regex regex_pattern(pattern);
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

YAML::Node ExtendedYamlParser::loadWithoutResolving(const std::filesystem::path& file_path) {
    std::string content = ReadConfigFile(file_path);

    // Each `{{env.NAME}}` becomes an inert scalar. Done FIRST, so an include
    // directive that carries one in its path (`{{include from {{env.DIR}}/x}}`)
    // has no nested braces left when the directives are dropped next.
    static const std::regex env_ref(R"(\{\{env\.([A-Za-z_][A-Za-z0-9_]*)\}\})");
    content = std::regex_replace(content, env_ref, "ENVREF_$1");

    // Include directives are not YAML until they are expanded, and expanding
    // them needs the very variables being decided about.
    static const std::regex include_directive(R"(\{\{include[^}]*\}\})");
    content = std::regex_replace(content, include_directive, "");

    return YAML::Load(content);
}

void ExtendedYamlParser::setEnvironmentPolicy(std::vector<std::string> whitelist,
                                              bool error_on_unlisted) {
    config_.environment_whitelist = std::move(whitelist);
    config_.error_on_unlisted_environment_variable = error_on_unlisted;
}

ExtendedYamlParser::ExtendedYamlParser(const IncludeConfig& config) : config_(config) {
    CROW_LOG_DEBUG << "ExtendedYamlParser constructor with config called, environment variables allowed: " << config_.allow_environment_variables;
}

ExtendedYamlParser::ParseResult ExtendedYamlParser::parseFile(const std::filesystem::path& file_path,
                                                             const std::filesystem::path& base_path) {
    ParseResult result;
    resolved_variables_.clear();

    try {
        // Determine base path
        std::filesystem::path actual_base_path = base_path;
        if (actual_base_path.empty()) {
            actual_base_path = file_path.parent_path();
        }

        std::string content;
        try {
            content = ReadConfigFile(file_path);
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = e.what();
            return result;
        }

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
    resolved_variables_.clear();

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
    if (ConfigFileExists(resolved_path)) {
        return true;
    }

    // Try absolute path
    if (include_path.is_absolute() && ConfigFileExists(include_path)) {
        resolved_path = include_path;
        return true;
    }

    // Try include paths
    for (const auto& include_base : include_paths) {
        resolved_path = std::filesystem::path(include_base) / include_path;
        if (ConfigFileExists(resolved_path)) {
            return true;
        }
    }

    return false;
}

YAML::Node ExtendedYamlParser::loadYamlFile(const std::filesystem::path& file_path) {
    return YAML::Load(ReadConfigFile(file_path));
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

namespace {

// Which lines of a YAML text are full-line COMMENTS.
//
// "The first non-blank character is '#'" is not enough: inside a block scalar
// (`key: |`, `key: >-`, `- |`) a line starting with '#' is CONTENT - a Markdown
// heading in an MCP prompt, say - and a variable on it must be substituted or
// refused like any other. Treating it as a comment left a literal {{env.X}} in
// the text with no error, which is the silent failure the whitelist enforcement
// exists to prevent.
//
// A block scalar runs from the line after its header to the last line that is
// blank or indented deeper than the header. Where that is ambiguous this errs
// toward "content": the only cost is enforcing a variable in something that was
// a comment, which fails loudly and is easy to fix.
class CommentMask {
public:
    explicit CommentMask(const std::string& text) {
        static const std::regex block_header(R"((?:^|[:\-])\s*[|>][+\-0-9]*\s*(?:#.*)?$)");
        bool in_block = false;
        size_t block_indent = 0;
        size_t pos = 0;
        for (;;) {
            const size_t eol = text.find('\n', pos);
            const size_t end = (eol == std::string::npos) ? text.size() : eol;
            std::string line = text.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            starts_.push_back(pos);

            const size_t first = line.find_first_not_of(" \t");
            const bool blank = (first == std::string::npos);
            const size_t indent = blank ? line.size() : first;

            bool comment = false;
            bool handled = false;
            if (in_block) {
                if (blank || indent > block_indent) {
                    handled = true;               // block scalar content
                } else {
                    in_block = false;             // the block ended; this line is ordinary
                }
            }
            if (!handled && !blank) {
                if (line[indent] == '#') {
                    comment = true;
                } else if (line.find_first_of("|>") != std::string::npos &&
                           std::regex_search(line.substr(indent), block_header)) {
                    in_block = true;
                    block_indent = indent;
                }
            }
            comment_.push_back(comment);

            if (eol == std::string::npos) {
                break;
            }
            pos = eol + 1;
        }
    }

    // True when `pos` lies on a full-line comment.
    bool inComment(size_t pos) const {
        const auto it = std::upper_bound(starts_.begin(), starts_.end(), pos);
        const size_t line = static_cast<size_t>(it - starts_.begin()) - 1;
        return comment_[line];
    }

private:
    std::vector<size_t> starts_;
    std::vector<bool> comment_;
};

}  // namespace

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

    // Every variable that is referenced but not whitelisted, in first-seen
    // order, so ONE error names all of them.
    std::vector<std::string> unlisted;

    // Built once per input, not once per match.
    const CommentMask comments(result);

    // Recreate iterator (previous one was consumed by std::distance)
    matches_begin = std::sregex_iterator(result.begin(), result.end(), env_regex);

    for (auto it = matches_begin; it != matches_end; ++it) {
        std::string var_name = it->str(1);
        CROW_LOG_DEBUG << "Processing environment variable: " << var_name;

        // A full-line YAML comment cannot reference anything. Operators write
        // "# password comes from {{env.DB_PASSWORD}}" as documentation; with the
        // whitelist enforced that must not stop startup, and it must not pull a
        // secret into text nothing will read. (A trailing comment, `key: v # {{env.X}}`,
        // is not skipped: telling it from a `#` inside a quoted value takes a
        // real tokenizer, and failing loudly is the safe way to be wrong.)
        if (comments.inComment(static_cast<size_t>(it->position()))) {
            CROW_LOG_DEBUG << "Skipping environment variable in a comment: " << var_name;
            continue;
        }

        if (!config_.isEnvironmentVariableAllowed(var_name)) {
            CROW_LOG_DEBUG << "Environment variable not allowed: " << var_name;
            if (std::find(unlisted.begin(), unlisted.end(), var_name) == unlisted.end()) {
                unlisted.push_back(var_name);
            }
            continue; // Left as a literal unless the caller asked for an error
        }

        const char* env_value = std::getenv(var_name.c_str());
        std::string replacement = env_value ? env_value : "";
        // The NAME only. This line used to print the value, so every secret a
        // configuration pulled from the environment was written to the debug
        // log.
        CROW_LOG_DEBUG << "Environment variable " << var_name << " substituted";

        // Store for logging (preserve existing behavior)
        // Names only: the VALUE of a secret must not outlive the substitution (#166).
        resolved_variables_[var_name] = "<redacted>";

        // Collect match info (no string modification yet)
        matches.push_back({
            static_cast<size_t>(it->position()),
            static_cast<size_t>(it->length()),
            var_name,
            replacement
        });
    }

    // A `{{env.NAME}}` left as a literal is not harmless in a configuration:
    // `password: '{{env.DB_PASSWORD}}'` would quietly become the password. For a
    // real configuration, refuse - naming what to change - instead of guessing.
    if (config_.error_on_unlisted_environment_variable && !unlisted.empty()) {
        std::string names;
        for (const auto& name : unlisted) {
            names += (names.empty() ? "" : ", ") + name;
        }
        throw std::runtime_error(
            std::string("environment variable") + (unlisted.size() > 1 ? "s " : " ") + names +
            " referenced as {{env.NAME}} but not whitelisted. Add a pattern matching "
            + (unlisted.size() > 1 ? "each" : "it") +
            " to `template.environment-whitelist` in the main configuration file; "
            "an empty or missing whitelist allows no variables.");
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

    // `env.NAME` reads the environment, so it obeys the same whitelist as
    // `{{env.NAME}}` (#157). It only ever revealed whether a variable was set and
    // non-empty, never its value - but it was the one place a configuration
    // could still probe any variable it liked. A variable that is not allowed is
    // an error where the whitelist is enforced (silently treating it as "unset"
    // would quietly skip an include the operator asked for), and reads as unset
    // where it is not.
    auto read = [this, &condition](const std::string& var_name) -> const char* {
        if (config_.isEnvironmentVariableAllowed(var_name)) {
            return std::getenv(var_name.c_str());
        }
        if (config_.error_on_unlisted_environment_variable) {
            throw std::runtime_error(
                "environment variable " + var_name + " is read by the include condition '" +
                condition + "' but is not whitelisted. Add a pattern matching it to "
                "`template.environment-whitelist` in the main configuration file; "
                "an empty or missing whitelist allows no variables.");
        }
        return nullptr;
    };

    if (condition.find("env.") == 0) {
        const char* env_value = read(condition.substr(4));
        return env_value != nullptr && std::string(env_value) != "";
    }

    if (condition.find("!env.") == 0) {
        const char* env_value = read(condition.substr(5));
        return env_value == nullptr || std::string(env_value) == "";
    }

    // For more complex conditions, could extend this
    return false;
}

} // namespace flapi
