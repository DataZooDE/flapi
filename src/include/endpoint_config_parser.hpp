#pragma once

#include "config_manager.hpp"
#include "extended_yaml_parser.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <optional>

namespace flapi {

// Forward declaration
class ConfigManager;

/**
 * @brief Parses endpoint configuration from YAML with proper path resolution
 * 
 * This class encapsulates the logic for parsing endpoint YAML files,
 * resolving relative paths (template-source, cache.template_file) relative
 * to the YAML file's directory, and creating EndpointConfig objects.
 * 
 * By centralizing this logic, we ensure consistency across:
 * - Initial configuration loading
 * - Configuration reloading after external edits
 * - Configuration validation before saving
 */
class EndpointConfigParser {
public:
    /**
     * @brief Result of parsing an endpoint configuration
     */
    struct ParseResult {
        bool success = false;
        EndpointConfig config;
        std::string error_message;
        std::vector<std::string> warnings;
    };

    /**
     * @brief Construct a parser with references to required dependencies
     * 
     * @param yaml_parser Parser for extended YAML syntax (handles {{include}} directives)
     * @param config_manager Parent ConfigManager for accessing helper methods and state
     */
    EndpointConfigParser(
        ExtendedYamlParser& yaml_parser,
        ConfigManager* config_manager
    );

    /**
     * @brief Parse endpoint configuration from a YAML file
     * 
     * Resolves all relative paths (template-source, cache.template_file)
     * relative to the YAML file's directory.
     * 
     * @param yaml_file_path Path to the endpoint YAML file
     * @return ParseResult with the parsed configuration or error details
     */
    ParseResult parseFromFile(const std::filesystem::path& yaml_file_path);

    /**
     * @brief Parse endpoint configuration from YAML content string
     * 
     * Note: Without file path context, relative paths cannot be resolved properly.
     * This method is primarily for backward compatibility. Use parseFromFile() when possible.
     * 
     * @param yaml_content YAML content as string
     * @return ParseResult with the parsed configuration or error details
     */
    ParseResult parseFromString(const std::string& yaml_content);

private:
    ExtendedYamlParser& yaml_parser_;
    ConfigManager* config_manager_;

    /**
     * @brief Endpoint type detection result
     */
    struct EndpointType {
        bool is_rest_endpoint = false;
        bool is_mcp_tool = false;
        bool is_mcp_resource = false;
        bool is_mcp_prompt = false;
        
        bool isValid() const {
            return is_rest_endpoint || is_mcp_tool || is_mcp_resource || is_mcp_prompt;
        }
    };

    /**
     * @brief Detect the type of endpoint from YAML node
     * 
     * @param yaml_node Parsed YAML configuration
     * @return EndpointType with flags set for detected types
     */
    EndpointType detectEndpointType(const YAML::Node& yaml_node) const;

    /**
     * @brief Parse endpoint configuration from YAML node with path resolution
     * 
     * This is the core parsing logic that:
     * - Detects endpoint type (REST, MCP tool/resource/prompt)
     * - Parses type-specific fields
     * - Resolves template-source and cache.template_file relative to endpoint_dir
     * - Calls ConfigManager helper methods for complex fields
     * 
     * @param yaml_node Parsed YAML configuration node
     * @param endpoint_dir Directory of the YAML file (for relative path resolution)
     * @param result Output ParseResult to populate with config or errors
     */
    void parseEndpointFromYaml(
        const YAML::Node& yaml_node,
        const std::filesystem::path& endpoint_dir,
        ParseResult& result
    );

    /**
     * @brief Parse REST endpoint specific fields
     */
    void parseRestEndpointFields(
        const YAML::Node& yaml_node,
        EndpointConfig& config
    );

    /**
     * @brief Parse MCP tool specific fields
     */
    void parseMcpToolFields(
        const YAML::Node& yaml_node,
        EndpointConfig& config
    );

    /**
     * @brief Parse MCP resource specific fields
     */
    void parseMcpResourceFields(
        const YAML::Node& yaml_node,
        EndpointConfig& config
    );

    /**
     * @brief Parse MCP prompt specific fields
     */
    void parseMcpPromptFields(
        const YAML::Node& yaml_node,
        EndpointConfig& config,
        ParseResult& result
    );

    /**
     * @brief Parse template source with path resolution
     */
    void parseTemplateSource(
        const YAML::Node& yaml_node,
        const std::filesystem::path& endpoint_dir,
        EndpointConfig& config,
        ParseResult& result
    );

    /**
     * @brief Parse common endpoint fields using ConfigManager helper methods
     */
    void parseCommonFields(
        const YAML::Node& yaml_node,
        const std::filesystem::path& endpoint_dir,
        EndpointConfig& config
    );
};

} // namespace flapi

