#pragma once

#include "config_manager.hpp"
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace flapi {

/**
 * Responsible for storing and retrieving endpoint configurations.
 * Provides efficient lookup by REST path/method and MCP name.
 * Does NOT interpret or validate configurations - that's handled elsewhere.
 */
class EndpointRepository {
public:
    EndpointRepository() = default;

    /**
     * Add an endpoint to the repository.
     * Replaces any existing endpoint with same path/method (REST) or name (MCP).
     *
     * @param endpoint Endpoint configuration to add
     */
    void addEndpoint(const EndpointConfig& endpoint);

    /**
     * Get an endpoint by REST path and HTTP method.
     * Returns empty optional if not found.
     *
     * @param url_path REST URL path (e.g., "/customers")
     * @param method HTTP method (e.g., "GET")
     * @return Endpoint configuration if found, empty optional otherwise
     */
    std::optional<EndpointConfig> getEndpointByRestPath(
        const std::string& url_path,
        const std::string& method) const;

    /**
     * Get an endpoint by MCP tool/resource/prompt name.
     * Returns empty optional if not found.
     *
     * @param name MCP entity name
     * @return Endpoint configuration if found, empty optional otherwise
     */
    std::optional<EndpointConfig> getEndpointByMCPName(const std::string& name) const;

    /**
     * Get all endpoints in the repository.
     *
     * @return Vector of all endpoint configurations
     */
    std::vector<EndpointConfig> getAllEndpoints() const;

    /**
     * Get all REST endpoints (filter out MCP-only endpoints).
     *
     * @return Vector of REST endpoint configurations
     */
    std::vector<EndpointConfig> getAllRestEndpoints() const;

    /**
     * Get all MCP endpoints (filter out REST-only endpoints).
     *
     * @return Vector of MCP endpoint configurations
     */
    std::vector<EndpointConfig> getAllMCPEndpoints() const;

    /**
     * Find endpoints matching a predicate function.
     *
     * @param predicate Function that returns true for matching endpoints
     * @return Vector of matching endpoint configurations
     */
    std::vector<EndpointConfig> findEndpoints(
        std::function<bool(const EndpointConfig&)> predicate) const;

    /**
     * Check if a REST endpoint exists.
     *
     * @param url_path REST URL path
     * @param method HTTP method
     * @return true if endpoint exists, false otherwise
     */
    bool hasRestEndpoint(const std::string& url_path, const std::string& method) const;

    /**
     * Check if an MCP endpoint exists.
     *
     * @param name MCP entity name
     * @return true if endpoint exists, false otherwise
     */
    bool hasMCPEndpoint(const std::string& name) const;

    /**
     * Remove a REST endpoint.
     *
     * @param url_path REST URL path
     * @param method HTTP method
     * @return true if endpoint was removed, false if it didn't exist
     */
    bool removeRestEndpoint(const std::string& url_path, const std::string& method);

    /**
     * Remove an MCP endpoint.
     *
     * @param name MCP entity name
     * @return true if endpoint was removed, false if it didn't exist
     */
    bool removeMCPEndpoint(const std::string& name);

    /**
     * Clear all endpoints from the repository.
     */
    void clear();

    /**
     * Get total number of endpoints in repository.
     *
     * @return Number of endpoints
     */
    size_t count() const;

    /**
     * Get number of REST endpoints.
     *
     * @return Number of REST endpoints
     */
    size_t countRestEndpoints() const;

    /**
     * Get number of MCP endpoints.
     *
     * @return Number of MCP endpoints
     */
    size_t countMCPEndpoints() const;

private:
    // REST endpoints: key format "METHOD:URL_PATH" (e.g., "GET:/customers")
    std::map<std::string, EndpointConfig> rest_endpoints_;

    // MCP endpoints: key is entity name (e.g., "customer_lookup")
    std::map<std::string, EndpointConfig> mcp_endpoints_;

    // Helper: Create REST endpoint key
    static std::string makeRestKey(const std::string& url_path, const std::string& method);

    // Helper: Determine if endpoint is REST or MCP
    static bool isRestEndpoint(const EndpointConfig& endpoint);

    // Helper: Extract MCP name from endpoint
    static std::string extractMCPName(const EndpointConfig& endpoint);
};

} // namespace flapi
