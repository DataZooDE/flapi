#include "config_serializer.hpp"
#include <crow/logging.h>
#include <fstream>
#include <sstream>
#include <yaml-cpp/yaml.h>

namespace flapi {

std::string ConfigSerializer::serializeEndpointConfigToYaml(const EndpointConfig& config) const {
    YAML::Emitter out;
    out << YAML::BeginMap;

    // Serialize REST endpoint
    serializeRestEndpoint(config, out);

    // Serialize MCP tool
    serializeMCPTool(config, out);

    // Serialize MCP resource
    serializeMCPResource(config, out);

    // Serialize MCP prompt
    serializeMCPPrompt(config, out);

    // Serialize template source
    out << YAML::Key << "template-source" << YAML::Value << config.templateSource;

    // Serialize connections
    if (!config.connection.empty()) {
        out << YAML::Key << "connection" << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (const auto& conn : config.connection) {
            out << conn;
        }
        out << YAML::EndSeq;
    }

    // Serialize request fields
    serializeRequestFields(config, out);

    // Serialize cache config
    serializeCacheConfig(config, out);

    // Serialize auth config
    serializeAuthConfig(config, out);

    // Serialize rate limit config
    serializeRateLimitConfig(config, out);

    out << YAML::EndMap;

    return out.c_str();
}

void ConfigSerializer::serializeRestEndpoint(const EndpointConfig& config, YAML::Emitter& out) const {
    if (config.isRESTEndpoint()) {
        out << YAML::Key << "url-path" << YAML::Value << config.urlPath;
        out << YAML::Key << "method" << YAML::Value << config.method;
    }
}

void ConfigSerializer::serializeMCPTool(const EndpointConfig& config, YAML::Emitter& out) const {
    if (config.isMCPTool()) {
        out << YAML::Key << "mcp-tool" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << config.mcp_tool->name;
        if (!config.mcp_tool->description.empty()) {
            out << YAML::Key << "description" << YAML::Value << config.mcp_tool->description;
        }
        if (!config.mcp_tool->result_mime_type.empty() && config.mcp_tool->result_mime_type != "application/json") {
            out << YAML::Key << "result-mime-type" << YAML::Value << config.mcp_tool->result_mime_type;
        }
        out << YAML::EndMap;
    }
}

void ConfigSerializer::serializeMCPResource(const EndpointConfig& config, YAML::Emitter& out) const {
    if (config.isMCPResource()) {
        out << YAML::Key << "mcp-resource" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << config.mcp_resource->name;
        if (!config.mcp_resource->description.empty()) {
            out << YAML::Key << "description" << YAML::Value << config.mcp_resource->description;
        }
        if (!config.mcp_resource->mime_type.empty() && config.mcp_resource->mime_type != "application/json") {
            out << YAML::Key << "mime-type" << YAML::Value << config.mcp_resource->mime_type;
        }
        out << YAML::EndMap;
    }
}

void ConfigSerializer::serializeMCPPrompt(const EndpointConfig& config, YAML::Emitter& out) const {
    if (config.isMCPPrompt()) {
        out << YAML::Key << "mcp-prompt" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "name" << YAML::Value << config.mcp_prompt->name;
        if (!config.mcp_prompt->description.empty()) {
            out << YAML::Key << "description" << YAML::Value << config.mcp_prompt->description;
        }
        if (!config.mcp_prompt->template_content.empty()) {
            out << YAML::Key << "template-content" << YAML::Value << config.mcp_prompt->template_content;
        }
        if (!config.mcp_prompt->arguments.empty()) {
            out << YAML::Key << "arguments" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (const auto& arg : config.mcp_prompt->arguments) {
                out << arg;
            }
            out << YAML::EndSeq;
        }
        out << YAML::EndMap;
    }
}

void ConfigSerializer::serializeRequestFields(const EndpointConfig& config, YAML::Emitter& out) const {
    if (!config.request_fields.empty()) {
        out << YAML::Key << "request" << YAML::Value << YAML::BeginSeq;
        for (const auto& field : config.request_fields) {
            out << YAML::BeginMap;
            out << YAML::Key << "field-name" << YAML::Value << field.fieldName;
            out << YAML::Key << "field-in" << YAML::Value << field.fieldIn;
            if (!field.description.empty()) {
                out << YAML::Key << "description" << YAML::Value << field.description;
            }
            if (field.required) {
                out << YAML::Key << "required" << YAML::Value << true;
            }
            if (!field.defaultValue.empty()) {
                out << YAML::Key << "default-value" << YAML::Value << field.defaultValue;
            }
            if (!field.validators.empty()) {
                out << YAML::Key << "validators" << YAML::Value << YAML::BeginSeq;
                for (const auto& validator : field.validators) {
                    out << YAML::BeginMap;
                    out << YAML::Key << "type" << YAML::Value << validator.type;
                    if (!validator.regex.empty()) {
                        out << YAML::Key << "regex" << YAML::Value << validator.regex;
                    }
                    if (validator.min > 0) {
                        out << YAML::Key << "min" << YAML::Value << validator.min;
                    }
                    if (validator.max > 0) {
                        out << YAML::Key << "max" << YAML::Value << validator.max;
                    }
                    out << YAML::EndMap;
                }
                out << YAML::EndSeq;
            }
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
    }
}

void ConfigSerializer::serializeCacheConfig(const EndpointConfig& config, YAML::Emitter& out) const {
    if (config.cache.enabled) {
        out << YAML::Key << "cache" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "enabled" << YAML::Value << true;
        if (!config.cache.table.empty()) {
            out << YAML::Key << "table" << YAML::Value << config.cache.table;
        }
        if (!config.cache.schema.empty() && config.cache.schema != "cache") {
            out << YAML::Key << "schema" << YAML::Value << config.cache.schema;
        }
        if (config.cache.schedule.has_value()) {
            out << YAML::Key << "schedule" << YAML::Value << config.cache.schedule.value();
        }
        out << YAML::EndMap;
    }
}

void ConfigSerializer::serializeAuthConfig(const EndpointConfig& config, YAML::Emitter& out) const {
    if (config.auth.enabled) {
        out << YAML::Key << "auth" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "enabled" << YAML::Value << true;
        if (!config.auth.type.empty()) {
            out << YAML::Key << "type" << YAML::Value << config.auth.type;
        }
        out << YAML::EndMap;
    }
}

void ConfigSerializer::serializeRateLimitConfig(const EndpointConfig& config, YAML::Emitter& out) const {
    if (config.rate_limit.enabled) {
        out << YAML::Key << "rate-limit" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "enabled" << YAML::Value << true;
        if (config.rate_limit.max > 0) {
            out << YAML::Key << "max" << YAML::Value << config.rate_limit.max;
        }
        if (config.rate_limit.interval > 0) {
            out << YAML::Key << "interval" << YAML::Value << config.rate_limit.interval;
        }
        out << YAML::EndMap;
    }
}

EndpointConfig ConfigSerializer::deserializeEndpointConfigFromYaml(const std::string& yaml_content) const {
    try {
        YAML::Node node = YAML::Load(yaml_content);

        EndpointConfig config;

        // Deserialize REST endpoint
        deserializeRestEndpoint(node, config);

        // Deserialize MCP tool
        deserializeMCPTool(node, config);

        // Deserialize MCP resource
        deserializeMCPResource(node, config);

        // Deserialize MCP prompt
        deserializeMCPPrompt(node, config);

        // Deserialize template source
        if (node["template-source"]) {
            config.templateSource = node["template-source"].as<std::string>();
        }

        // Deserialize connections
        if (node["connection"]) {
            if (node["connection"].IsSequence()) {
                for (const auto& conn : node["connection"]) {
                    config.connection.push_back(conn.as<std::string>());
                }
            } else {
                config.connection.push_back(node["connection"].as<std::string>());
            }
        }

        // Deserialize request fields
        deserializeRequestFields(node, config);

        // Deserialize cache config
        deserializeCacheConfig(node, config);

        // Deserialize auth config
        deserializeAuthConfig(node, config);

        // Deserialize rate limit config
        deserializeRateLimitConfig(node, config);

        return config;

    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("YAML parsing error: ") + e.what());
    }
}

void ConfigSerializer::deserializeRestEndpoint(const YAML::Node& node, EndpointConfig& config) const {
    if (node["url-path"]) {
        config.urlPath = node["url-path"].as<std::string>();
        config.method = node["method"] ? node["method"].as<std::string>() : "GET";
    }
}

void ConfigSerializer::deserializeMCPTool(const YAML::Node& node, EndpointConfig& config) const {
    if (node["mcp-tool"]) {
        EndpointConfig::MCPToolInfo tool;
        tool.name = node["mcp-tool"]["name"].as<std::string>();
        tool.description = node["mcp-tool"]["description"] ? node["mcp-tool"]["description"].as<std::string>() : "";
        tool.result_mime_type = node["mcp-tool"]["result-mime-type"] ? node["mcp-tool"]["result-mime-type"].as<std::string>() : "application/json";
        config.mcp_tool = tool;
    }
}

void ConfigSerializer::deserializeMCPResource(const YAML::Node& node, EndpointConfig& config) const {
    if (node["mcp-resource"]) {
        EndpointConfig::MCPResourceInfo resource;
        resource.name = node["mcp-resource"]["name"].as<std::string>();
        resource.description = node["mcp-resource"]["description"] ? node["mcp-resource"]["description"].as<std::string>() : "";
        resource.mime_type = node["mcp-resource"]["mime-type"] ? node["mcp-resource"]["mime-type"].as<std::string>() : "application/json";
        config.mcp_resource = resource;
    }
}

void ConfigSerializer::deserializeMCPPrompt(const YAML::Node& node, EndpointConfig& config) const {
    if (node["mcp-prompt"]) {
        EndpointConfig::MCPPromptInfo prompt;
        prompt.name = node["mcp-prompt"]["name"].as<std::string>();
        prompt.description = node["mcp-prompt"]["description"] ? node["mcp-prompt"]["description"].as<std::string>() : "";
        prompt.template_content = node["mcp-prompt"]["template-content"] ? node["mcp-prompt"]["template-content"].as<std::string>() : "";
        if (node["mcp-prompt"]["arguments"]) {
            for (const auto& arg : node["mcp-prompt"]["arguments"]) {
                prompt.arguments.push_back(arg.as<std::string>());
            }
        }
        config.mcp_prompt = prompt;
    }
}

void ConfigSerializer::deserializeRequestFields(const YAML::Node& node, EndpointConfig& config) const {
    if (node["request"]) {
        for (const auto& field_node : node["request"]) {
            RequestFieldConfig field;
            field.fieldName = field_node["field-name"].as<std::string>();
            field.fieldIn = field_node["field-in"].as<std::string>();
            field.description = field_node["description"] ? field_node["description"].as<std::string>() : "";
            field.required = field_node["required"] ? field_node["required"].as<bool>() : false;
            field.defaultValue = field_node["default-value"] ? field_node["default-value"].as<std::string>() : "";

            if (field_node["validators"]) {
                for (const auto& validator_node : field_node["validators"]) {
                    ValidatorConfig validator;
                    validator.type = validator_node["type"].as<std::string>();
                    validator.regex = validator_node["regex"] ? validator_node["regex"].as<std::string>() : "";
                    validator.min = validator_node["min"] ? validator_node["min"].as<int>() : 0;
                    validator.max = validator_node["max"] ? validator_node["max"].as<int>() : 0;
                    field.validators.push_back(validator);
                }
            }

            config.request_fields.push_back(field);
        }
    }
}

void ConfigSerializer::deserializeCacheConfig(const YAML::Node& node, EndpointConfig& config) const {
    if (node["cache"]) {
        config.cache.enabled = node["cache"]["enabled"] ? node["cache"]["enabled"].as<bool>() : false;
        if (node["cache"]["table"]) {
            config.cache.table = node["cache"]["table"].as<std::string>();
        }
        if (node["cache"]["schema"]) {
            config.cache.schema = node["cache"]["schema"].as<std::string>();
        }
        if (node["cache"]["schedule"]) {
            config.cache.schedule = node["cache"]["schedule"].as<std::string>();
        }
    }
}

void ConfigSerializer::deserializeAuthConfig(const YAML::Node& node, EndpointConfig& config) const {
    if (node["auth"]) {
        config.auth.enabled = node["auth"]["enabled"] ? node["auth"]["enabled"].as<bool>() : false;
        if (node["auth"]["type"]) {
            config.auth.type = node["auth"]["type"].as<std::string>();
        }
    }
}

void ConfigSerializer::deserializeRateLimitConfig(const YAML::Node& node, EndpointConfig& config) const {
    if (node["rate-limit"]) {
        config.rate_limit.enabled = node["rate-limit"]["enabled"] ? node["rate-limit"]["enabled"].as<bool>() : false;
        if (node["rate-limit"]["max"]) {
            config.rate_limit.max = node["rate-limit"]["max"].as<int>();
        }
        if (node["rate-limit"]["interval"]) {
            config.rate_limit.interval = node["rate-limit"]["interval"].as<int>();
        }
    }
}

void ConfigSerializer::persistEndpointConfigToFile(
    const EndpointConfig& config,
    const std::filesystem::path& file_path) const {

    try {
        std::string yaml_content = serializeEndpointConfigToYaml(config);

        // Ensure parent directory exists
        if (file_path.has_parent_path()) {
            std::filesystem::create_directories(file_path.parent_path());
        }

        // Write to file
        std::ofstream file(file_path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for writing: " + file_path.string());
        }

        file << yaml_content;
        if (!file.good()) {
            throw std::runtime_error("Failed to write to file: " + file_path.string());
        }

        CROW_LOG_INFO << "Persisted endpoint configuration to: " << file_path.string();

    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to persist configuration: ") + e.what());
    }
}

std::string ConfigSerializer::loadEndpointConfigYamlFromFile(const std::filesystem::path& file_path) const {
    try {
        if (!std::filesystem::exists(file_path)) {
            throw std::runtime_error("File does not exist: " + file_path.string());
        }

        if (!std::filesystem::is_regular_file(file_path)) {
            throw std::runtime_error("Path is not a regular file: " + file_path.string());
        }

        std::ifstream file(file_path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for reading: " + file_path.string());
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        return buffer.str();

    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Failed to load configuration: ") + e.what());
    }
}

} // namespace flapi
