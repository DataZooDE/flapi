#pragma once

#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <regex>
#include <memory>
#include <crow/logging.h>

namespace flapi {

/**
 * @brief Extended YAML parser with include preprocessing functionality.
 *
 * This class provides enhanced YAML parsing capabilities including:
 * - File inclusion with `{{include:section from file.yaml}}` syntax
 * - Section-specific includes
 * - Conditional includes
 * - Environment variable substitution
 * - Circular dependency detection
 * - Path resolution (relative and absolute)
 */
class ExtendedYamlParser {
public:
    /**
     * @brief Configuration for include processing
     */
    struct IncludeConfig {
        bool allow_recursive_includes = true;
        bool allow_environment_variables = true;
        bool allow_conditional_includes = true;
        std::vector<std::string> include_paths;

        // Regex patterns (full match, case-insensitive) naming the variables
        // `{{env.NAME}}` may read. EMPTY MEANS NONE: an unset whitelist must never
        // mean "no protection". (It used to mean "allow everything" here while
        // meaning "allow nothing" for SQL templates - #157.)
        std::vector<std::string> environment_whitelist;

        // What to do with a `{{env.NAME}}` that is not allowed. false leaves it
        // in the text as a literal, which is right for callers that only sniff a
        // file's structure. true fails the parse, naming every such variable:
        // for a real configuration a literal `{{env.DB_PASSWORD}}` would silently
        // become the password.
        bool error_on_unlisted_environment_variable = false;

        /**
         * @brief Check if an environment variable is allowed
         */
        bool isEnvironmentVariableAllowed(const std::string& var_name) const;
    };

    /**
     * @brief Result of YAML parsing with metadata
     */
    struct ParseResult {
        YAML::Node node;
        std::vector<std::string> included_files;
        std::unordered_map<std::string, std::string> resolved_variables;
        bool success = false;
        std::string error_message;
    };

    /**
     * @brief Constructor
     */
    ExtendedYamlParser();

    /**
     * @brief Constructor with config
     */
    explicit ExtendedYamlParser(const IncludeConfig& config);

    /**
     * @brief Parse a YAML file with include preprocessing
     *
     * @param file_path Path to the YAML file
     * @param base_path Base path for resolving relative includes
     * @return ParseResult containing the parsed YAML and metadata
     */
    ParseResult parseFile(const std::filesystem::path& file_path,
                         const std::filesystem::path& base_path = "");

    /**
     * @brief Parse YAML content with include preprocessing
     *
     * @param content YAML content as string
     * @param base_path Base path for resolving relative includes
     * @return ParseResult containing the parsed YAML and metadata
     */
    ParseResult parseString(const std::string& content,
                           const std::filesystem::path& base_path = "");

    /**
     * @brief Set which environment variables `{{env.NAME}}` may read.
     *
     * Applies to every later parse, including endpoint files loaded through the
     * same parser. Lets the owner of the main configuration read the whitelist
     * (which lives in that file) BEFORE substituting anything into it.
     */
    void setEnvironmentPolicy(std::vector<std::string> whitelist, bool error_on_unlisted);

        /**
         * @brief Preprocess content to handle include directives before YAML parsing
         *
         * @param content YAML content as string
         * @param base_path Base path for resolving relative includes
         * @param included_files Set of already included files (for circular detection)
         * @return Processed content with includes replaced
         */
        std::string preprocessContent(const std::string& content,
                                     const std::filesystem::path& base_path,
                                     std::unordered_set<std::string>& included_files);

        /**
         * @brief Preprocess includes in a YAML node
         *
         * @param node YAML node to process
         * @param base_path Base path for resolving relative includes
         * @param included_files Set of already included files (for circular detection)
         * @return true if successful, false otherwise
         */
        bool preprocessIncludes(YAML::Node& node,
                               const std::filesystem::path& base_path,
                               std::unordered_set<std::string>& included_files);

    /**
     * @brief Resolve include directives
     *
     * @param include_directive Include directive like "section from file.yaml"
     * @param base_path Base path for resolving relative paths
     * @return Resolved include information
     */
    struct IncludeInfo {
        std::string section_name;
        std::filesystem::path file_path;
        bool is_section_include = false;
        bool is_conditional = false;
        std::string condition;

        IncludeInfo() = default;
        IncludeInfo(const std::string& section, const std::filesystem::path& path, bool section_include = false)
            : section_name(section), file_path(path), is_section_include(section_include) {}
    };

    std::optional<IncludeInfo> parseIncludeDirective(const std::string& directive) const;
    static bool resolveIncludePath(const std::filesystem::path& include_path,
                                  const std::filesystem::path& base_path,
                                  std::filesystem::path& resolved_path,
                                  const std::vector<std::string>& include_paths);

    /**
     * @brief Load and parse a YAML file
     */
    static YAML::Node loadYamlFile(const std::filesystem::path& file_path);

    /**
     * @brief Extract a section from a YAML node
     */
    static YAML::Node extractSection(const YAML::Node& node, const std::string& section_name);

    /**
     * @brief Merge YAML nodes
     */
    static YAML::Node mergeNodes(const YAML::Node& target, const YAML::Node& source);

    /**
     * @brief Substitute environment variables in a string
     */
    std::string substituteEnvironmentVariables(const std::string& input) const;

    /**
     * @brief Evaluate conditional expression
     */
    bool evaluateCondition(const std::string& condition) const;

    /**
     * @brief Get current configuration
     */
    const IncludeConfig& getConfig() const { return config_; }

    /**
     * @brief Update configuration
     */
    void setConfig(const IncludeConfig& config) { config_ = config; }

private:
    IncludeConfig config_;
    mutable std::unordered_map<std::string, std::string> resolved_variables_;

    /**
     * @brief Process scalar node for includes
     */
    bool processScalarNode(YAML::Node& node,
                          const std::filesystem::path& base_path,
                          std::unordered_set<std::string>& included_files);

    /**
     * @brief Process map node for includes
     */
    bool processMapNode(YAML::Node& node,
                       const std::filesystem::path& base_path,
                       std::unordered_set<std::string>& included_files);

    /**
     * @brief Process sequence node for includes
     */
    bool processSequenceNode(YAML::Node& node,
                            const std::filesystem::path& base_path,
                            std::unordered_set<std::string>& included_files);

    /**
     * @brief Check if a string contains include directives
     */
    bool containsIncludeDirective(const std::string& str) const;

    /**
     * @brief Extract and process include directives from a string
     */
    std::string processIncludeDirectives(const std::string& input,
                                        const std::filesystem::path& base_path,
                                        std::unordered_set<std::string>& included_files);
};

} // namespace flapi
