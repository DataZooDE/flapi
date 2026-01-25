#include "vfs_health_checker.hpp"
#include <crow/logging.h>

namespace flapi {

std::string VFSHealthChecker::getSchemeType(const std::string& path) {
    if (PathSchemeUtils::IsS3Path(path)) {
        return "s3";
    }
    if (PathSchemeUtils::IsGCSPath(path)) {
        return "gs";
    }
    if (PathSchemeUtils::IsAzurePath(path)) {
        return "az";
    }
    if (PathSchemeUtils::IsHttpPath(path)) {
        // Distinguish between http and https
        if (path.find("https://") == 0 || path.find("HTTPS://") == 0) {
            return "https";
        }
        return "http";
    }
    return "local";
}

StorageBackendStatus VFSHealthChecker::checkPath(const std::string& name,
                                                   const std::string& path) {
    StorageBackendStatus status;
    status.name = name;
    status.path = path;
    status.scheme = getSchemeType(path);
    status.accessible = false;
    status.latency_ms = 0;
    status.error = "";

    if (path.empty()) {
        status.error = "Path is empty";
        return status;
    }

    auto start = std::chrono::steady_clock::now();

    try {
        // Create appropriate file provider
        auto provider = FileProviderFactory::CreateProvider(path);

        // Check if path exists/is accessible
        // For directories, we try to list files; for files, we check existence
        bool exists = false;

        if (PathSchemeUtils::IsRemotePath(path)) {
            // For remote paths, try to access via FileExists
            // Note: For directories, this may not work on all backends
            // We attempt a simple existence check
            exists = provider->FileExists(path);
            if (!exists) {
                // For remote directories, try listing with a simple pattern
                try {
                    auto files = provider->ListFiles(path, "*");
                    exists = true; // If no exception, directory is accessible
                } catch (const FileOperationError&) {
                    // Directory listing failed - not accessible
                    exists = false;
                }
            }
        } else {
            // For local paths
            exists = provider->FileExists(path);
            if (!exists) {
                // Check if it's a directory
                try {
                    auto files = provider->ListFiles(path, "*");
                    exists = true;
                } catch (const FileOperationError&) {
                    exists = false;
                }
            }
        }

        auto end = std::chrono::steady_clock::now();
        status.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

        status.accessible = exists;
        if (!exists) {
            status.error = "Path not found or not accessible";
        }

    } catch (const FileOperationError& e) {
        auto end = std::chrono::steady_clock::now();
        status.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        status.accessible = false;
        status.error = e.what();
    } catch (const std::exception& e) {
        auto end = std::chrono::steady_clock::now();
        status.latency_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        status.accessible = false;
        status.error = std::string("Unexpected error: ") + e.what();
    }

    return status;
}

StorageHealthStatus VFSHealthChecker::checkHealth(const std::string& config_path,
                                                    const std::string& templates_path) {
    StorageHealthStatus health;
    health.healthy = true;
    health.total_latency_ms = 0;

    auto overall_start = std::chrono::steady_clock::now();

    // Check config path
    if (!config_path.empty()) {
        auto config_status = checkPath("config", config_path);
        health.backends.push_back(config_status);
        if (!config_status.accessible) {
            health.healthy = false;
        }
    }

    // Check templates path
    if (!templates_path.empty()) {
        auto templates_status = checkPath("templates", templates_path);
        health.backends.push_back(templates_status);
        if (!templates_status.accessible) {
            health.healthy = false;
        }
    }

    auto overall_end = std::chrono::steady_clock::now();
    health.total_latency_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(overall_end - overall_start).count());

    return health;
}

bool VFSHealthChecker::verifyStartupHealth(const std::string& config_path,
                                            const std::string& templates_path) {
    CROW_LOG_DEBUG << "Verifying storage health on startup...";

    auto health = checkHealth(config_path, templates_path);

    for (const auto& backend : health.backends) {
        if (backend.accessible) {
            CROW_LOG_INFO << "Storage backend '" << backend.name << "' is healthy"
                          << " (path: " << backend.path << ", scheme: " << backend.scheme
                          << ", latency: " << backend.latency_ms << "ms)";
        } else {
            CROW_LOG_WARNING << "Storage backend '" << backend.name << "' is NOT accessible"
                             << " (path: " << backend.path << ", scheme: " << backend.scheme
                             << ", error: " << backend.error << ")";
        }
    }

    if (health.healthy) {
        CROW_LOG_INFO << "All storage backends healthy (total check time: "
                      << health.total_latency_ms << "ms)";
    } else {
        CROW_LOG_WARNING << "Some storage backends are not accessible. "
                         << "The server will start but may have issues loading configurations.";
    }

    return health.healthy;
}

} // namespace flapi
