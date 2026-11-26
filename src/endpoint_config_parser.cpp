#include "endpoint_config_parser.hpp"
#include "tracing_config.hpp"
#include "vfs_adapter.hpp"
#include <crow/logging.h>
#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "mcp_description_scanner.hpp"
#include "mustache_syntax_check.hpp"

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
        if (PathSchemeUtils::IsRemotePath(yaml_file_path.string())) {
            // Directory of the YAML's URL, carrying its ?query (a SAS token / signature that
            // every sibling file needs) - parent_path() of the whole URL would drop it.
            const std::string full = yaml_file_path.string();
            const std::string no_query = PathSchemeUtils::StripQuery(full);
            const auto slash = no_query.find_last_of('/');
            endpoint_dir = no_query.substr(0, slash == std::string::npos ? 0 : slash) + full.substr(no_query.size());
        }
        
        // Parse the endpoint configuration with path resolution
        parseEndpointFromYaml(yaml_node, endpoint_dir, result);
        
        // Store the config file path for reloading
        if (result.success) {
            // A remote path is not a filesystem path: absolute() would put the cwd in front of it.
            result.config.config_file_path = PathSchemeUtils::IsRemotePath(yaml_file_path.string())
                                                 ? yaml_file_path.string()
                                                 : std::filesystem::absolute(yaml_file_path).string();
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

        // A template crow cannot render safely is a load/reload error, never a
        // request-time crash or hang. Only with a directory: a string parse has
        // no base to resolve a relative template-source against.
        if (!endpoint_dir.empty()) {
            if (!type.is_mcp_prompt) {
                if (auto error = CheckMustacheTemplateFile(config.templateSource)) {
                    throw std::runtime_error(*error);
                }
                // auth.* outside a plain '...' literal is never safe: a load
                // error, so startup, reload and --validate-config refuse it.
                if (auto error = CheckAuthPlacementInTemplateFile(config.templateSource)) {
                    throw std::runtime_error(*error);
                }
            }
            if (config.cache.template_file) {
                if (auto error = CheckMustacheTemplateFile(*config.cache.template_file)) {
                    throw std::runtime_error(*error);
                }
                if (auto error = CheckAuthPlacementInTemplateFile(*config.cache.template_file)) {
                    throw std::runtime_error(*error);
                }
            }
        }

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
    // Canonical (trimmed, upper-case, empty -> GET) from here on, and only the
    // five methods routing knows. `method: get` used to load as "get": the
    // duplicate check compared it case-sensitively against a protected `GET`
    // on the same route and let it in, while routing upper-cases both and
    // could pick it - an open copy answering the protected URL.
    // `method:` with no value is YAML null: the default, like `method: ''`.
    const std::string raw_method = (yaml_node["method"] && yaml_node["method"].IsNull())
        ? std::string("GET")
        : config_manager_->safeGet<std::string>(yaml_node, "method", "method", "GET");
    if (auto problem = ConfigManager::HttpMethodProblem(raw_method, config.urlPath); !problem.empty()) {
        throw std::runtime_error(problem);
    }
    config.method = ConfigManager::CanonicalHttpMethod(raw_method);
    // A url-path the router cannot match literally (`/customers/{cid}`) used
    // to load and then crash every request that reached it in the route scan.
    if (auto problem = RouteTranslator::urlPathProblem(config.urlPath); !problem.empty()) {
        throw std::runtime_error(problem);
    }
    config.allow_shadowing = config_manager_->safeGet<bool>(yaml_node, "allow-shadowing", "allow-shadowing", false);
    config.description = config_manager_->safeGet<std::string>(yaml_node, "description", "description", "");
    config.with_pagination = config_manager_->safeGet<bool>(yaml_node, "with-pagination", "with-pagination", true);
    config.total_count = config_manager_->safeGet<bool>(yaml_node, "total-count", "total-count", true);
    config.request_fields_validation = config_manager_->safeGet<bool>(yaml_node, "request-fields-validation", "request-fields-validation", false);
    
    // Auto-detect write operation based on HTTP method (for backward compatibility)
    // This can be overridden by explicit operation.type in YAML
    std::string method_upper = config.method;
    std::transform(method_upper.begin(), method_upper.end(), method_upper.begin(), ::toupper);
    // DELETE too: REST already runs it through the write path, but an MCP tool
    // over a DELETE endpoint ran as a query, so the row was deleted while the
    // tool reported an error and the endpoint's cache was not refreshed.
    if (method_upper == "POST" || method_upper == "PUT" || method_upper == "PATCH" || method_upper == "DELETE") {
        config.operation.type = OperationConfig::Write;
    }
}

void EndpointConfigParser::parseMcpToolFields(
    const YAML::Node& yaml_node,
    EndpointConfig& config
) {
    auto mcp_tool_node = yaml_node["mcp-tool"];
    EndpointConfig::MCPToolInfo tool_info;
    tool_info.name = config_manager_->safeGet<std::string>(mcp_tool_node, "name", "mcp-tool.name");
    tool_info.description = config_manager_->safeGet<std::string>(mcp_tool_node, "description", "mcp-tool.description");
    tool_info.result_mime_type = config_manager_->safeGet<std::string>(mcp_tool_node, "result-mime-type", "mcp-tool.result-mime-type", "application/json");

    // W2.3: scan the description for control characters, prompt-injection
    // phrases, and oversize content. Always warn; reject only when the
    // operator has opted into strict mode.
    {
        MCPDescriptionScanner scanner;
        const auto issues = scanner.scan(tool_info.description);
        const bool strict = config_manager_->getMCPConfig().strict_descriptions;
        for (const auto& issue : issues) {
            CROW_LOG_WARNING << "MCP tool '" << tool_info.name << "' description: ["
                             << issue.code << "] " << issue.message;
        }
        if (strict && !issues.empty()) {
            throw std::runtime_error(
                "MCP tool '" + tool_info.name + "' has a description that failed strict "
                "validation (mcp.strict-descriptions is true). First issue: [" +
                issues.front().code + "] " + issues.front().message);
        }
    }

    // Per-tool RBAC (W2.1). Absent key → std::nullopt (deny-by-default under
    // MCP auth, transparent under demo mode). Present-but-empty list is the
    // strict "no role passes this gate" form.
    if (mcp_tool_node["allowed-roles"].IsDefined()) {
        std::vector<std::string> roles;
        for (const auto& role_node : mcp_tool_node["allowed-roles"]) {
            roles.push_back(role_node.as<std::string>());
        }
        tool_info.allowed_roles = std::move(roles);
    }

    // W2.4: parse optional response-shaping block. All fields are independent
    // and default-inert, so leaving the block out preserves existing behaviour.
    if (auto response_node = mcp_tool_node["response"]; response_node.IsDefined()) {
        if (auto mr = response_node["max-rows"]; mr.IsDefined()) {
            tool_info.response.max_rows = mr.as<std::size_t>();
        }
        if (auto rc = response_node["redact-columns"]; rc.IsDefined()) {
            for (const auto& column : rc) {
                tool_info.response.redact_columns.push_back(column.as<std::string>());
            }
        }
        if (auto sample = response_node["sample"]; sample.IsDefined()) {
            tool_info.response.sample = sample.as<bool>();
        }
    }

    // Per-endpoint tracing capture override. Declared alongside the response
    // block because the redaction it governs reuses response.redact-columns.
    //
    // std::optional, so "unset" stays distinguishable from "set to the global
    // default" - a global `capture: off` must still win over an endpoint that
    // asks for payload.
    if (auto tracing_node = mcp_tool_node["tracing"]; tracing_node.IsDefined()) {
        if (auto capture = tracing_node["capture"]; capture.IsDefined()) {
            tool_info.response.tracing_capture =
                parseCaptureTier(capture.as<std::string>(), CaptureTier::Metadata);
        }
    }

    // W2.5: per-tool rate limit. Absent block → enabled=false (no gate).
    if (auto rl = mcp_tool_node["rate-limit"]; rl.IsDefined()) {
        tool_info.rate_limit.enabled = config_manager_->safeGet<bool>(rl, "enabled", "mcp-tool.rate-limit.enabled", true);
        tool_info.rate_limit.max = config_manager_->safeGet<int>(rl, "max", "mcp-tool.rate-limit.max", 100);
        tool_info.rate_limit.interval = config_manager_->safeGet<int>(rl, "interval", "mcp-tool.rate-limit.interval", 60);
    }

    // MCP 2026-07-28 Tasks: async / async-after-ms. async-after-ms is a plain
    // integer number of milliseconds (`async-after-ms: 5000`); a duration
    // string such as "5s" is NOT accepted and fails the load as a type error.
    tool_info.async = config_manager_->safeGet<bool>(mcp_tool_node, "async", "mcp-tool.async", false);
    tool_info.async_after_ms = config_manager_->safeGet<int>(mcp_tool_node, "async-after-ms", "mcp-tool.async-after-ms", 0);

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
    resource_info.mime_type = config_manager_->safeGet<std::string>(mcp_resource_node, "mime-type", "mcp-resource.mime-type", "application/json");
    resource_info.uri_template = config_manager_->safeGet<std::string>(mcp_resource_node, "uri-template", "mcp-resource.uri-template", "");

    // Per-resource RBAC (mirrors mcp-tool.allowed-roles). Absent → nullopt
    // (deny-by-default under mcp.auth.enabled, transparent under demo mode).
    if (mcp_resource_node["allowed-roles"].IsDefined()) {
        std::vector<std::string> roles;
        for (const auto& role_node : mcp_resource_node["allowed-roles"]) {
            roles.push_back(role_node.as<std::string>());
        }
        resource_info.allowed_roles = std::move(roles);
    }

    // Response shaping, the same keys as mcp-tool.response. Optional so that
    // "unset" can fall back to the file's mcp-tool.response at read time.
    if (auto response_node = mcp_resource_node["response"]; response_node.IsDefined()) {
        if (auto mr = response_node["max-rows"]; mr.IsDefined()) {
            resource_info.response.max_rows = mr.as<std::size_t>();
        }
        if (auto rc = response_node["redact-columns"]; rc.IsDefined()) {
            std::vector<std::string> columns;
            for (const auto& column : rc) {
                columns.push_back(column.as<std::string>());
            }
            resource_info.response.redact_columns = std::move(columns);
        }
    }

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
        
        // Per-prompt RBAC (mirrors mcp-tool.allowed-roles). Absent → nullopt
        // (deny-by-default under mcp.auth.enabled, transparent under demo mode).
        if (mcp_prompt_node["allowed-roles"].IsDefined()) {
            std::vector<std::string> roles;
            for (const auto& role_node : mcp_prompt_node["allowed-roles"]) {
                roles.push_back(role_node.as<std::string>());
            }
            prompt_info.allowed_roles = std::move(roles);
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
        if (PathSchemeUtils::IsRemotePath(template_source)) {
            config.templateSource = template_source;
        } else if (PathSchemeUtils::IsRemotePath(endpoint_dir.string())) {
            config.templateSource = PathSchemeUtils::JoinUrl(endpoint_dir.string(), template_source);
        } else if (!template_path.is_absolute()) {
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
    
    // Parse operation configuration (can override method-based defaults)
    parseOperationConfig(yaml_node, config);

    // Checked here, after parseOperationConfig: only now is it known whether this
    // is a write endpoint (an explicit operation.type overrides the method).
    checkCacheTemplateFile(config);
}

void EndpointConfigParser::checkCacheTemplateFile(const EndpointConfig& config) {
    const auto& cache = config.cache;
    if (!cache.enabled || cache.table.empty() || cache.template_file) {
        return;
    }
    const std::string name = !config.urlPath.empty() ? config.urlPath : config.getName();
    if (config.operation.type == OperationConfig::Write) {
        // #276: without template-file the populate SQL is the endpoint's own
        // template - here the INSERT/UPDATE/DELETE - and every refresh (warm-up,
        // schedule, refresh-on-write) ran the write again.
        throw std::runtime_error(
            "endpoint " + name + ": a write endpoint's cache needs template-file: without one the populate "
            "SQL would be the endpoint's own write template, run again on every refresh. Declare "
            "cache.template-file with SQL that rebuilds " + cache.table +
            " (e.g. CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS SELECT ...)");
    }
    CROW_LOG_WARNING << "cache on " << name
                     << " has no template-file: the endpoint's own template is the populate SQL; a single "
                        "SELECT is materialised into the cache table with a full refresh, whatever "
                        "primary-key/cursor imply. Declare template-file for an incremental refresh";
}

void EndpointConfigParser::parseOperationConfig(
    const YAML::Node& yaml_node,
    EndpointConfig& config
) {
    if (!yaml_node["operation"]) {
        // No operation config specified, keep defaults (set by method or default to Read)
        return;
    }
    
    auto operation_node = yaml_node["operation"];
    
    // Parse operation type if specified
    if (operation_node["type"]) {
        std::string type_str = operation_node["type"].as<std::string>();
        std::transform(type_str.begin(), type_str.end(), type_str.begin(), ::tolower);
        if (type_str == "write") {
            config.operation.type = OperationConfig::Write;
        } else if (type_str == "read") {
            config.operation.type = OperationConfig::Read;
        }
        // Otherwise keep existing value (from method-based detection)
    }
    
    // Parse returns_data
    if (operation_node["returns-data"]) {
        config.operation.returns_data = operation_node["returns-data"].as<bool>();
    } else if (operation_node["returnsData"]) {
        config.operation.returns_data = operation_node["returnsData"].as<bool>();
    }
    
    // Parse transaction
    if (operation_node["transaction"]) {
        config.operation.transaction = operation_node["transaction"].as<bool>();
    }
    
    // Parse validate_before_write
    if (operation_node["validate-before-write"]) {
        config.operation.validate_before_write = operation_node["validate-before-write"].as<bool>();
    }
}

} // namespace flapi

