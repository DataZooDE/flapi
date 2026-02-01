#include "config_validator.hpp"
#include "endpoint_config_parser.hpp"
#include <crow/logging.h>
#include <fstream>
#include <sstream>

namespace flapi {

std::vector<std::string> ConfigValidator::ValidationResult::getAllMessages() const {
    std::vector<std::string> all_messages;
    all_messages.insert(all_messages.end(), errors.begin(), errors.end());
    all_messages.insert(all_messages.end(), warnings.begin(), warnings.end());
    return all_messages;
}

std::string ConfigValidator::ValidationResult::getErrorSummary() const {
    if (errors.empty()) {
        return "";
    }

    std::stringstream ss;
    ss << "Errors (" << errors.size() << "):\n";
    for (size_t i = 0; i < errors.size(); ++i) {
        ss << "  " << (i + 1) << ". " << errors[i] << "\n";
    }
    return ss.str();
}

std::string ConfigValidator::ValidationResult::getWarningSummary() const {
    if (warnings.empty()) {
        return "";
    }

    std::stringstream ss;
    ss << "Warnings (" << warnings.size() << "):\n";
    for (size_t i = 0; i < warnings.size(); ++i) {
        ss << "  " << (i + 1) << ". " << warnings[i] << "\n";
    }
    return ss.str();
}

ConfigValidator::ConfigValidator(
    const std::unordered_map<std::string, ConnectionConfig>& connections,
    const std::string& template_path)
    : connections_(connections), template_path_(template_path) {
}

void ConfigValidator::setContext(
    const std::unordered_map<std::string, ConnectionConfig>& connections,
    const std::string& template_path) {
    connections_ = connections;
    template_path_ = template_path;
}

void ConfigValidator::setConfigParser(EndpointConfigParser* parser) {
    config_parser_ = parser;
}

std::filesystem::path ConfigValidator::resolvePath(const std::string& file_path) const {
    std::filesystem::path path(file_path);

    // Handle empty path - return template path as-is
    if (path.empty()) {
        return std::filesystem::path(template_path_);
    }

    // Absolute paths are returned as-is
    if (path.is_absolute()) {
        return path;
    }

    // Relative paths are resolved against template_path_
    std::filesystem::path resolved = std::filesystem::path(template_path_) / path;

    // Handle case where template_path_ is empty - resolved might still be relative
    if (resolved.empty()) {
        resolved = path;  // Use original path if nothing to resolve against
    }

    // Try to canonicalize (resolve .. and .)
    try {
        resolved = std::filesystem::canonical(resolved);
    } catch (const std::filesystem::filesystem_error&) {
        // If canonical fails, just make it absolute
        // Guard against empty path which would crash absolute()
        if (!resolved.empty()) {
            resolved = std::filesystem::absolute(resolved);
        }
    }

    return resolved;
}

void ConfigValidator::validateEndpointStructure(
    const EndpointConfig& config,
    ValidationResult& result) const {

    // Use endpoint's self-validation for type-specific checks
    auto self_errors = config.validateSelf();
    if (!self_errors.empty()) {
        result.valid = false;
        result.errors.insert(result.errors.end(), self_errors.begin(), self_errors.end());
    }
}

void ConfigValidator::validateTemplateSource(
    const EndpointConfig& config,
    ValidationResult& result) const {

    // Validate template source
    if (config.templateSource.empty()) {
        result.valid = false;
        result.errors.emplace_back("template-source cannot be empty");
        return;
    }

    // Check if template file exists
    // Note: config.templateSource is already resolved relative to YAML file directory during parsing
    std::filesystem::path template_path = resolvePath(config.templateSource);

    if (!std::filesystem::exists(template_path)) {
        result.warnings.emplace_back("Template file does not exist: " + template_path.string());
        CROW_LOG_WARNING << "Template file not found: " << template_path.string();
    }
}

void ConfigValidator::validateCacheTemplate(
    const EndpointConfig& config,
    ValidationResult& result) const {

    // Validate cache template if specified
    if (config.cache.enabled && config.cache.template_file) {
        // Note: config.cache.template_file is already resolved during parsing
        std::filesystem::path cache_template_path = resolvePath(config.cache.template_file.value());

        if (!std::filesystem::exists(cache_template_path)) {
            result.warnings.emplace_back("Cache template file does not exist: " + cache_template_path.string());
            CROW_LOG_WARNING << "Cache template file not found: " << cache_template_path.string();
        }
    }
}

void ConfigValidator::validateConnections(
    const EndpointConfig& config,
    ValidationResult& result) const {

    // Validate connections
    if (config.connection.empty()) {
        result.warnings.emplace_back("No database connection specified");
    } else {
        for (const auto& conn_name : config.connection) {
            if (connections_.find(conn_name) == connections_.end()) {
                result.valid = false;
                result.errors.emplace_back("Connection '" + conn_name + "' not found in configuration");
            }
        }
    }
}

ConfigValidator::ValidationResult ConfigValidator::validateEndpointConfig(
    const EndpointConfig& config) const {

    ValidationResult result;

    // Validate endpoint structure (type-specific checks)
    validateEndpointStructure(config, result);

    // Validate template source
    validateTemplateSource(config, result);

    // Validate connections
    validateConnections(config, result);

    // Validate cache template if specified
    validateCacheTemplate(config, result);

    CROW_LOG_DEBUG << "Endpoint validation: " << (result.valid ? "VALID" : "INVALID");

    return result;
}

ConfigValidator::ValidationResult ConfigValidator::validateEndpointConfigFromYaml(
    const std::string& yaml_content) {

    ValidationResult result;

    try {
        if (!config_parser_) {
            result.valid = false;
            result.errors.emplace_back("ConfigParser not initialized for validation");
            return result;
        }

        // Parse the YAML content
        auto parse_result = config_parser_->parseFromString(yaml_content);

        if (!parse_result.success) {
            result.valid = false;
            result.errors.emplace_back(parse_result.error_message);
            return result;
        }

        // Validate the parsed configuration
        return validateEndpointConfig(parse_result.config);

    } catch (const YAML::Exception& e) {
        result.valid = false;
        result.errors.emplace_back(std::string("YAML parsing error: ") + e.what());
        CROW_LOG_ERROR << "YAML parsing failed: " << e.what();
        return result;
    } catch (const std::exception& e) {
        result.valid = false;
        result.errors.emplace_back(std::string("Configuration error: ") + e.what());
        CROW_LOG_ERROR << "Configuration error: " << e.what();
        return result;
    }
}

ConfigValidator::ValidationResult ConfigValidator::validateEndpointConfigFile(
    const std::filesystem::path& file_path) {

    ValidationResult result;

    // Check if file exists
    if (!std::filesystem::exists(file_path)) {
        result.valid = false;
        result.errors.emplace_back("File does not exist: " + file_path.string());
        return result;
    }

    // Check if it's actually a file (not a directory)
    if (!std::filesystem::is_regular_file(file_path)) {
        result.valid = false;
        result.errors.emplace_back("Path is not a regular file: " + file_path.string());
        return result;
    }

    try {
        if (!config_parser_) {
            result.valid = false;
            result.errors.emplace_back("ConfigParser not initialized for validation");
            return result;
        }

        // Parse the file
        auto parse_result = config_parser_->parseFromFile(file_path);

        if (!parse_result.success) {
            result.valid = false;
            result.errors.emplace_back(parse_result.error_message);
            return result;
        }

        // Now validate the parsed configuration
        return validateEndpointConfig(parse_result.config);

    } catch (const std::exception& e) {
        result.valid = false;
        result.errors.emplace_back(std::string("Validation error: ") + e.what());
        CROW_LOG_ERROR << "File validation failed: " << e.what();
        return result;
    }
}

} // namespace flapi
