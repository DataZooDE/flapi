#pragma once

#include "config_manager.hpp"
#include <filesystem>
#include <string>
#include <optional>

namespace flapi {

/**
 * Responsible for serializing and deserializing endpoint configurations.
 * Converts between EndpointConfig objects and YAML/JSON representations.
 * Handles both in-memory conversion and file I/O operations.
 *
 * Does NOT validate configurations - that's handled by ConfigValidator.
 * Does NOT interpret configurations - just converts between formats.
 */
class ConfigSerializer {
public:
    ConfigSerializer() = default;

    /**
     * Serialize an endpoint configuration to YAML string.
     * Includes all endpoint fields: REST, MCP tool/resource/prompt, cache, auth, etc.
     *
     * @param config Endpoint configuration to serialize
     * @return YAML string representation of the configuration
     * @throws std::runtime_error on serialization failure
     */
    std::string serializeEndpointConfigToYaml(const EndpointConfig& config) const;

    /**
     * Deserialize an endpoint configuration from YAML string.
     * Parses YAML content and creates EndpointConfig object.
     *
     * @param yaml_content YAML string containing endpoint configuration
     * @return Deserialized endpoint configuration
     * @throws std::runtime_error on parse failure or invalid content
     */
    EndpointConfig deserializeEndpointConfigFromYaml(const std::string& yaml_content) const;

    /**
     * Persist an endpoint configuration to file.
     * Serializes configuration to YAML and writes to disk.
     * Creates parent directories if needed.
     *
     * @param config Endpoint configuration to persist
     * @param file_path Path where configuration should be written
     * @throws std::runtime_error on file write failure
     */
    void persistEndpointConfigToFile(
        const EndpointConfig& config,
        const std::filesystem::path& file_path) const;

    /**
     * Load an endpoint configuration from file as YAML string.
     * Reads file contents without parsing.
     *
     * @param file_path Path to YAML configuration file
     * @return Raw YAML content from file
     * @throws std::runtime_error on file read failure
     */
    std::string loadEndpointConfigYamlFromFile(const std::filesystem::path& file_path) const;

private:
    // Helper: Serialize REST endpoint section
    void serializeRestEndpoint(const EndpointConfig& config, YAML::Emitter& out) const;

    // Helper: Serialize MCP tool section
    void serializeMCPTool(const EndpointConfig& config, YAML::Emitter& out) const;

    // Helper: Serialize MCP resource section
    void serializeMCPResource(const EndpointConfig& config, YAML::Emitter& out) const;

    // Helper: Serialize MCP prompt section
    void serializeMCPPrompt(const EndpointConfig& config, YAML::Emitter& out) const;

    // Helper: Serialize request fields
    void serializeRequestFields(const EndpointConfig& config, YAML::Emitter& out) const;

    // Helper: Serialize cache configuration
    void serializeCacheConfig(const EndpointConfig& config, YAML::Emitter& out) const;

    // Helper: Serialize auth configuration
    void serializeAuthConfig(const EndpointConfig& config, YAML::Emitter& out) const;

    // Helper: Serialize rate limit configuration
    void serializeRateLimitConfig(const EndpointConfig& config, YAML::Emitter& out) const;

    // Helper: Deserialize REST endpoint section
    void deserializeRestEndpoint(const YAML::Node& node, EndpointConfig& config) const;

    // Helper: Deserialize MCP tool section
    void deserializeMCPTool(const YAML::Node& node, EndpointConfig& config) const;

    // Helper: Deserialize MCP resource section
    void deserializeMCPResource(const YAML::Node& node, EndpointConfig& config) const;

    // Helper: Deserialize MCP prompt section
    void deserializeMCPPrompt(const YAML::Node& node, EndpointConfig& config) const;

    // Helper: Deserialize request fields
    void deserializeRequestFields(const YAML::Node& node, EndpointConfig& config) const;

    // Helper: Deserialize cache configuration
    void deserializeCacheConfig(const YAML::Node& node, EndpointConfig& config) const;

    // Helper: Deserialize auth configuration
    void deserializeAuthConfig(const YAML::Node& node, EndpointConfig& config) const;

    // Helper: Deserialize rate limit configuration
    void deserializeRateLimitConfig(const YAML::Node& node, EndpointConfig& config) const;
};

} // namespace flapi
