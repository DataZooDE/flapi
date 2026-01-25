#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include "vfs_adapter.hpp"

namespace flapi {

/**
 * Status of a single storage backend.
 */
struct StorageBackendStatus {
    std::string name;           // "config", "templates"
    std::string path;           // Actual path or URI
    bool accessible;            // Whether the path is accessible
    int latency_ms;             // Time to check accessibility
    std::string error;          // Error message if not accessible
    std::string scheme;         // "local", "s3", "gs", "az", "https"
};

/**
 * Overall storage health status.
 */
struct StorageHealthStatus {
    bool healthy;                               // Overall health status
    std::vector<StorageBackendStatus> backends; // Individual backend statuses
    int total_latency_ms;                       // Total time to check all backends
};

/**
 * VFS Health Checker component.
 * Provides health check functionality for storage backends (local and remote).
 *
 * Usage:
 *   VFSHealthChecker checker;
 *   auto status = checker.checkHealth(config_path, templates_path);
 *   if (!status.healthy) {
 *       // Handle unhealthy storage
 *   }
 */
class VFSHealthChecker {
public:
    VFSHealthChecker() = default;
    ~VFSHealthChecker() = default;

    /**
     * Check health of all storage backends.
     *
     * @param config_path Path to configuration file (local or remote)
     * @param templates_path Path to templates directory (local or remote)
     * @return StorageHealthStatus with overall and per-backend status
     */
    StorageHealthStatus checkHealth(const std::string& config_path,
                                     const std::string& templates_path);

    /**
     * Check health of a single path.
     *
     * @param name Human-readable name for this backend (e.g., "config")
     * @param path Path to check (local or remote URI)
     * @return StorageBackendStatus for this path
     */
    StorageBackendStatus checkPath(const std::string& name, const std::string& path);

    /**
     * Get scheme type string from path.
     *
     * @param path Path to analyze
     * @return "local", "s3", "gs", "az", "https", or "http"
     */
    static std::string getSchemeType(const std::string& path);

    /**
     * Verify startup storage accessibility.
     * Logs warnings if any storage backends are unreachable.
     *
     * @param config_path Path to configuration file
     * @param templates_path Path to templates directory
     * @return true if all backends are accessible, false otherwise
     */
    bool verifyStartupHealth(const std::string& config_path,
                              const std::string& templates_path);
};

} // namespace flapi
