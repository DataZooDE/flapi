#include "endpoint_repository.hpp"
#include <crow/logging.h>
#include <algorithm>

namespace flapi {

std::string EndpointRepository::makeRestKey(
    const std::string& url_path,
    const std::string& method) {
    return method + ":" + url_path;
}

bool EndpointRepository::isRestEndpoint(const EndpointConfig& endpoint) {
    return !endpoint.urlPath.empty();
}

std::string EndpointRepository::extractMCPName(const EndpointConfig& endpoint) {
    if (endpoint.mcp_tool.has_value()) {
        return endpoint.mcp_tool->name;
    }
    if (endpoint.mcp_resource.has_value()) {
        return endpoint.mcp_resource->name;
    }
    if (endpoint.mcp_prompt.has_value()) {
        return endpoint.mcp_prompt->name;
    }
    return "";
}

void EndpointRepository::addEndpoint(const EndpointConfig& endpoint) {
    if (isRestEndpoint(endpoint)) {
        std::string key = makeRestKey(endpoint.urlPath, endpoint.method);
        rest_endpoints_[key] = endpoint;
        CROW_LOG_DEBUG << "Added REST endpoint: " << endpoint.method << " " << endpoint.urlPath;
    }

    // MCP endpoints can be added in addition to REST endpoints
    std::string mcp_name = extractMCPName(endpoint);
    if (!mcp_name.empty()) {
        mcp_endpoints_[mcp_name] = endpoint;
        CROW_LOG_DEBUG << "Added MCP endpoint: " << mcp_name;
    }
}

std::optional<EndpointConfig> EndpointRepository::getEndpointByRestPath(
    const std::string& url_path,
    const std::string& method) const {

    std::string key = makeRestKey(url_path, method);
    auto it = rest_endpoints_.find(key);

    if (it != rest_endpoints_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::optional<EndpointConfig> EndpointRepository::getEndpointByMCPName(
    const std::string& name) const {

    auto it = mcp_endpoints_.find(name);

    if (it != mcp_endpoints_.end()) {
        return it->second;
    }

    return std::nullopt;
}

std::vector<EndpointConfig> EndpointRepository::getAllEndpoints() const {
    std::vector<EndpointConfig> result;

    // Add all REST endpoints
    for (const auto& pair : rest_endpoints_) {
        result.push_back(pair.second);
    }

    // Add all MCP endpoints (avoid duplicates if endpoint is both REST and MCP)
    for (const auto& pair : mcp_endpoints_) {
        // Check if this MCP endpoint is already included as a REST endpoint
        bool already_included = false;
        for (const auto& rest_ep : result) {
            if (rest_ep.isSameEndpoint(pair.second)) {
                already_included = true;
                break;
            }
        }

        if (!already_included) {
            result.push_back(pair.second);
        }
    }

    return result;
}

std::vector<EndpointConfig> EndpointRepository::getAllRestEndpoints() const {
    std::vector<EndpointConfig> result;

    for (const auto& pair : rest_endpoints_) {
        result.push_back(pair.second);
    }

    return result;
}

std::vector<EndpointConfig> EndpointRepository::getAllMCPEndpoints() const {
    std::vector<EndpointConfig> result;

    for (const auto& pair : mcp_endpoints_) {
        result.push_back(pair.second);
    }

    return result;
}

std::vector<EndpointConfig> EndpointRepository::findEndpoints(
    std::function<bool(const EndpointConfig&)> predicate) const {

    std::vector<EndpointConfig> result;

    for (const auto& endpoint : getAllEndpoints()) {
        if (predicate(endpoint)) {
            result.push_back(endpoint);
        }
    }

    return result;
}

bool EndpointRepository::hasRestEndpoint(
    const std::string& url_path,
    const std::string& method) const {

    std::string key = makeRestKey(url_path, method);
    return rest_endpoints_.find(key) != rest_endpoints_.end();
}

bool EndpointRepository::hasMCPEndpoint(const std::string& name) const {
    return mcp_endpoints_.find(name) != mcp_endpoints_.end();
}

bool EndpointRepository::removeRestEndpoint(
    const std::string& url_path,
    const std::string& method) {

    std::string key = makeRestKey(url_path, method);
    size_t removed = rest_endpoints_.erase(key);

    if (removed > 0) {
        CROW_LOG_DEBUG << "Removed REST endpoint: " << method << " " << url_path;
    }

    return removed > 0;
}

bool EndpointRepository::removeMCPEndpoint(const std::string& name) {
    size_t removed = mcp_endpoints_.erase(name);

    if (removed > 0) {
        CROW_LOG_DEBUG << "Removed MCP endpoint: " << name;
    }

    return removed > 0;
}

void EndpointRepository::clear() {
    rest_endpoints_.clear();
    mcp_endpoints_.clear();
    CROW_LOG_DEBUG << "Cleared all endpoints from repository";
}

size_t EndpointRepository::count() const {
    size_t rest_count = rest_endpoints_.size();
    size_t mcp_count = mcp_endpoints_.size();

    // Subtract duplicates (endpoints that are both REST and MCP)
    size_t duplicates = 0;
    for (const auto& rest_pair : rest_endpoints_) {
        for (const auto& mcp_pair : mcp_endpoints_) {
            if (rest_pair.second.isSameEndpoint(mcp_pair.second)) {
                duplicates++;
            }
        }
    }

    return rest_count + mcp_count - duplicates;
}

size_t EndpointRepository::countRestEndpoints() const {
    return rest_endpoints_.size();
}

size_t EndpointRepository::countMCPEndpoints() const {
    return mcp_endpoints_.size();
}

} // namespace flapi
