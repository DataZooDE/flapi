#pragma once

#include "config_manager.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace flapi {

/**
 * Responsible for validating endpoint and connection configurations.
 * Validates configuration against constraints:
 * - Required fields are present and non-empty
 * - Referenced files exist
 * - Referenced connections are defined
 * - Configuration structure is valid
 *
 * Does NOT interpret or execute configurations - that's handled elsewhere.
 * Does NOT modify configuration data - validation is read-only.
 */
class ConfigValidator {
public:
    /**
     * Result of a validation operation.
     * Contains status, error messages, and warning messages.
     */
    struct ValidationResult {
        bool valid = true;                          // True if validation passed (no errors)
        std::vector<std::string> errors;            // Error messages (must be fixed)
        std::vector<std::string> warnings;          // Warning messages (should be reviewed)

        // Helper method to get all messages (errors + warnings)
        std::vector<std::string> getAllMessages() const;

        // Helper method to get error summary
        std::string getErrorSummary() const;

        // Helper method to get warning summary
        std::string getWarningSummary() const;
    };

    /**
     * Create a default ConfigValidator.
     * Connections and template path can be set later via setContext().
     */
    ConfigValidator() = default;

    /**
     * Create a ConfigValidator with reference to connections and template config.
     *
     * @param connections Map of available connections (for validation)
     * @param template_path Path to template directory for resolving relative paths
     */
    ConfigValidator(
        const std::unordered_map<std::string, ConnectionConfig>& connections,
        const std::string& template_path);

    /**
     * Set the validation context (connections and template path).
     * Must be called before validation if using default constructor.
     */
    void setContext(
        const std::unordered_map<std::string, ConnectionConfig>& connections,
        const std::string& template_path);

    /**
     * Validate an endpoint configuration structure.
     * Checks:
     * - Required fields are non-empty
     * - Referenced connections exist
     * - Template files exist
     * - URL paths are well-formed (for REST endpoints)
     * - MCP fields are valid (for MCP endpoints)
     *
     * @param config Endpoint configuration to validate
     * @return Validation result with any errors and warnings
     */
    ValidationResult validateEndpointConfig(const EndpointConfig& config) const;

    /**
     * Validate an endpoint configuration from YAML string.
     * First parses the YAML, then validates the resulting configuration.
     *
     * @param yaml_content YAML string containing endpoint configuration
     * @return Validation result with any errors and warnings
     */
    ValidationResult validateEndpointConfigFromYaml(const std::string& yaml_content);

    /**
     * Validate an endpoint configuration from file.
     * First checks file exists and reads it, then parses and validates.
     *
     * @param file_path Path to YAML configuration file
     * @return Validation result with any errors and warnings
     */
    ValidationResult validateEndpointConfigFile(const std::filesystem::path& file_path);

    /**
     * Set reference to EndpointConfigParser for parsing operations.
     * Required for proper YAML parsing during validation.
     *
     * @param parser Pointer to EndpointConfigParser
     */
    void setConfigParser(class EndpointConfigParser* parser);

private:
    // Configuration data for validation (copied to avoid lifetime issues)
    std::unordered_map<std::string, ConnectionConfig> connections_;
    std::string template_path_;

    // Parser for deserializing YAML during validation
    class EndpointConfigParser* config_parser_ = nullptr;

    // Helper: Validate endpoint structure (type-specific)
    void validateEndpointStructure(const EndpointConfig& config, ValidationResult& result) const;

    // Helper: Validate template source file
    void validateTemplateSource(const EndpointConfig& config, ValidationResult& result) const;

    // Helper: Validate cache template file
    void validateCacheTemplate(const EndpointConfig& config, ValidationResult& result) const;

    // Helper: Validate all referenced connections exist
    void validateConnections(const EndpointConfig& config, ValidationResult& result) const;

    // Helper: Resolve file path (handles relative paths)
    std::filesystem::path resolvePath(const std::string& file_path) const;
};

} // namespace flapi
