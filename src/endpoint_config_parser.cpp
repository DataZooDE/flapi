#include "endpoint_config_parser.hpp"
#include <crow/logging.h>

namespace flapi {

EndpointConfigParser::EndpointConfigParser(
    ExtendedYamlParser& yaml_parser,
    ConfigManager* config_manager
) : yaml_parser_(yaml_parser), config_manager_(config_manager) {}

EndpointConfigParser::ParseResult EndpointConfigParser::parseFromFile(
    const std::filesystem::path& yaml_file_path
) {
    ParseResult result;
    
    try {
        // Parse the YAML file with extended syntax support
        auto parse_result = yaml_parser_.parseFile(yaml_file_path.string());
        if (!parse_result.success) {
            result.success = false;
            result.error_message = "Failed to parse YAML: " + parse_result.error_message;
            return result;
        }
        
        YAML::Node yaml_node = parse_result.node;
        std::filesystem::path endpoint_dir = yaml_file_path.parent_path();
        
        // Parse the endpoint configuration with path resolution
        parseEndpointFromYaml(yaml_node, endpoint_dir, result);
        
        // Store the config file path for reloading
        if (result.success) {
            result.config.config_file_path = std::filesystem::absolute(yaml_file_path).string();
        }
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Exception during parsing: ") + e.what();
    }
    
    return result;
}

EndpointConfigParser::ParseResult EndpointConfigParser::parseFromString(
    const std::string& yaml_content
) {
    ParseResult result;
    
    try {
        // Parse YAML from string (no path resolution possible)
        YAML::Node yaml_node = YAML::Load(yaml_content);
        
        // Use empty path for endpoint_dir (relative paths won't resolve correctly)
        std::filesystem::path endpoint_dir;
        
        parseEndpointFromYaml(yaml_node, endpoint_dir, result);
        
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Exception during parsing: ") + e.what();
    }
    
    return result;
}

EndpointConfigParser::EndpointType EndpointConfigParser::detectEndpointType(
    const YAML::Node& yaml_node
) const {
    EndpointType type;
    type.is_rest_endpoint = yaml_node["url-path"].IsDefined();
    type.is_mcp_tool = yaml_node["mcp-tool"].IsDefined();
    type.is_mcp_resource = yaml_node["mcp-resource"].IsDefined();
    type.is_mcp_prompt = yaml_node["mcp-prompt"].IsDefined();
    return type;
}

void EndpointConfigParser::parseEndpointFromYaml(
    const YAML::Node& yaml_node,
    const std::filesystem::path& endpoint_dir,
    ParseResult& result
) {
    // Detect endpoint type
    EndpointType type = detectEndpointType(yaml_node);
    
    if (!type.isValid()) {
        result.success = false;
        result.error_message = "Not a valid endpoint configuration (missing url-path or mcp-* fields)";
        return;
    }
    
    EndpointConfig& config = result.config;
    
    // Parse type-specific fields with error handling
    try {
        if (type.is_rest_endpoint) {
            parseRestEndpointFields(yaml_node, config);
        }
        
        if (type.is_mcp_tool) {
            parseMcpToolFields(yaml_node, config);
        }
        
        if (type.is_mcp_resource) {
            parseMcpResourceFields(yaml_node, config);
        }
        
        if (type.is_mcp_prompt) {
            parseMcpPromptFields(yaml_node, config, result);
            // Check if error occurred (parseMcpPromptFields sets error_message on failure)
            if (!result.error_message.empty()) {
                result.success = false;
                return; // Error occurred in prompt parsing
            }
        }
        
        // Parse template source (except for MCP prompts which have embedded templates)
        if (!type.is_mcp_prompt) {
            parseTemplateSource(yaml_node, endpoint_dir, config, result);
        }
        
        // Parse common fields using ConfigManager helper methods
        parseCommonFields(yaml_node, endpoint_dir, config);
        
        result.success = true;
        
    } catch (const std::exception& e) {
        result.success = false;
        std::string what_msg = e.what();
        if (what_msg.empty()) {
            what_msg = "(empty exception message)";
        }
        result.error_message = std::string("Parse error: ") + what_msg;
    }
}

void EndpointConfigParser::parseRestEndpointFields(
    const YAML::Node& yaml_node,
    EndpointConfig& config
) {
    config.urlPath = config_manager_->safeGet<std::string>(yaml_node, "url-path", "url-path");
    config.method = config_manager_->safeGet<std::string>(yaml_node, "method", "method", "GET");
    config.with_pagination = config_manager_->safeGet<bool>(yaml_node, "with-pagination", "with-pagination", true);
    config.request_fields_validation = config_manager_->safeGet<bool>(yaml_node, "request-fields-validation", "request-fields-validation", false);
}

void EndpointConfigParser::parseMcpToolFields(
    const YAML::Node& yaml_node,
    EndpointConfig& config
) {
    auto mcp_tool_node = yaml_node["mcp-tool"];
    EndpointConfig::MCPToolInfo tool_info;
    tool_info.name = config_manager_->safeGet<std::string>(mcp_tool_node, "name", "mcp-tool.name");
    tool_info.description = config_manager_->safeGet<std::string>(mcp_tool_node, "description", "mcp-tool.description");
    tool_info.result_mime_type = config_manager_->safeGet<std::string>(mcp_tool_node, "result_mime_type", "mcp-tool.result_mime_type", "application/json");
    config.mcp_tool = tool_info;
}

void EndpointConfigParser::parseMcpResourceFields(
    const YAML::Node& yaml_node,
    EndpointConfig& config
) {
    auto mcp_resource_node = yaml_node["mcp-resource"];
    EndpointConfig::MCPResourceInfo resource_info;
    resource_info.name = config_manager_->safeGet<std::string>(mcp_resource_node, "name", "mcp-resource.name");
    resource_info.description = config_manager_->safeGet<std::string>(mcp_resource_node, "description", "mcp-resource.description");
    resource_info.mime_type = config_manager_->safeGet<std::string>(mcp_resource_node, "mime_type", "mcp-resource.mime_type", "application/json");
    config.mcp_resource = resource_info;
}

void EndpointConfigParser::parseMcpPromptFields(
    const YAML::Node& yaml_node,
    EndpointConfig& config,
    ParseResult& result
) {
    try {
        auto mcp_prompt_node = yaml_node["mcp-prompt"];
        if (!mcp_prompt_node) {
            result.success = false;
            result.error_message = "MCP prompt node is missing or invalid";
            return;
        }
        
        EndpointConfig::MCPPromptInfo prompt_info;
        
        // Parse required fields
        try {
            prompt_info.name = config_manager_->safeGet<std::string>(mcp_prompt_node, "name", "mcp-prompt.name");
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = std::string("Failed to parse mcp-prompt.name: ") + e.what();
            return;
        }
        
        try {
            prompt_info.description = config_manager_->safeGet<std::string>(mcp_prompt_node, "description", "mcp-prompt.description");
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = std::string("Failed to parse mcp-prompt.description: ") + e.what();
            return;
        }
        
        // Parse template field
        if (mcp_prompt_node["template"]) {
            try {
                prompt_info.template_content = mcp_prompt_node["template"].as<std::string>();
            } catch (const std::exception& e) {
                result.success = false;
                result.error_message = std::string("Failed to parse mcp-prompt.template: ") + e.what();
                return;
            }
        } else {
            result.success = false;
            result.error_message = "MCP prompt must have a 'template' field";
            return;
        }
        
        // Parse optional arguments field
        if (mcp_prompt_node["arguments"]) {
            try {
                for (const auto& arg : mcp_prompt_node["arguments"]) {
                    prompt_info.arguments.push_back(arg.as<std::string>());
                }
            } catch (const std::exception& e) {
                result.success = false;
                result.error_message = std::string("Failed to parse mcp-prompt.arguments: ") + e.what();
                return;
            }
        }
        
        config.mcp_prompt = prompt_info;
    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = std::string("Failed to parse MCP prompt fields: ") + e.what();
    }
}

void EndpointConfigParser::parseTemplateSource(
    const YAML::Node& yaml_node,
    const std::filesystem::path& endpoint_dir,
    EndpointConfig& config,
    ParseResult& result
) {
    // Get template source (will throw ConfigurationError if missing)
    std::string template_source = config_manager_->safeGet<std::string>(yaml_node, "template-source", "template-source");
    
    // Resolve relative paths to absolute, relative to the YAML file's directory
    if (!endpoint_dir.empty()) {
        std::filesystem::path template_path(template_source);
        if (!template_path.is_absolute()) {
            config.templateSource = (endpoint_dir / template_source).string();
        } else {
            config.templateSource = template_source;
        }
    } else {
        // No directory context (parsed from string), use as-is
        config.templateSource = template_source;
    }
}

void EndpointConfigParser::parseCommonFields(
    const YAML::Node& yaml_node,
    const std::filesystem::path& endpoint_dir,
    EndpointConfig& config
) {
    // Use ConfigManager helper methods for complex field parsing
    try {
        config_manager_->parseEndpointRequestFields(yaml_node, config);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("parseEndpointRequestFields failed: ") + e.what());
    }
    
    try {
        config_manager_->parseEndpointConnection(yaml_node, config);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("parseEndpointConnection failed: ") + e.what());
    }
    
    try {
        config_manager_->parseEndpointRateLimit(yaml_node, config);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("parseEndpointRateLimit failed: ") + e.what());
    }
    
    try {
        config_manager_->parseEndpointAuth(yaml_node, config);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("parseEndpointAuth failed: ") + e.what());
    }
    
    try {
        config_manager_->parseEndpointCache(yaml_node, endpoint_dir, config);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("parseEndpointCache failed: ") + e.what());
    }
    
    try {
        config_manager_->parseEndpointHeartbeat(yaml_node, config);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("parseEndpointHeartbeat failed: ") + e.what());
    }
}

} // namespace flapi

