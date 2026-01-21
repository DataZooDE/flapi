#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <yaml-cpp/yaml.h>
#include "vfs_adapter.hpp"

namespace flapi {

/**
 * Responsible for loading and parsing YAML configuration files.
 * Handles file I/O, path resolution, and basic YAML parsing operations.
 * Does NOT interpret or validate the configuration - that's done elsewhere.
 *
 * Supports loading configurations from:
 * - Local filesystem (default)
 * - Remote storage via VFS (S3, GCS, Azure, HTTP/HTTPS) when appropriate provider is set
 */
class ConfigLoader {
public:
    /**
     * Create a ConfigLoader for a specific configuration file.
     * Uses LocalFileProvider by default for backward compatibility.
     *
     * @param config_file_path Path to the main flapi.yaml configuration file
     */
    explicit ConfigLoader(const std::filesystem::path& config_file_path);

    /**
     * Create a ConfigLoader with a custom file provider.
     * Use this constructor when loading configs from remote storage.
     *
     * @param config_file_path Path or URI to the main flapi.yaml configuration file
     * @param file_provider Custom file provider for file operations
     */
    ConfigLoader(const std::string& config_file_path,
                 std::shared_ptr<IFileProvider> file_provider);

    /**
     * Load and parse a YAML file from disk.
     *
     * @param file_path Absolute or relative path to YAML file
     * @return Parsed YAML::Node representing the file contents
     * @throws std::runtime_error if file cannot be loaded or parsed
     */
    YAML::Node loadYamlFile(const std::filesystem::path& file_path);

    /**
     * Get the base directory for resolving relative paths.
     * This is the directory containing the main configuration file.
     *
     * @return Base directory path
     */
    std::filesystem::path getBasePath() const;

    /**
     * Get the directory containing the main configuration file.
     *
     * @return Config directory path
     */
    std::filesystem::path getConfigDirectory() const;

    /**
     * Resolve a potentially relative path against the base path.
     * If path is absolute, returns it as-is.
     * If path is relative, resolves it relative to base path.
     *
     * @param relative_path Path to resolve (can be absolute or relative)
     * @return Resolved absolute path
     */
    std::filesystem::path resolvePath(const std::filesystem::path& relative_path) const;

    /**
     * Check if a file exists.
     *
     * @param file_path Path to check
     * @return true if file exists, false otherwise
     */
    bool fileExists(const std::filesystem::path& file_path) const;

    /**
     * Check if a directory exists.
     *
     * @param dir_path Path to check
     * @return true if directory exists, false otherwise
     */
    bool directoryExists(const std::filesystem::path& dir_path) const;

    /**
     * Get the main configuration file path.
     *
     * @return Path to the main flapi.yaml file
     */
    std::filesystem::path getConfigFilePath() const { return config_file_path_; }

    /**
     * Get the main configuration file path as string.
     * Useful for remote paths that aren't valid filesystem paths.
     *
     * @return Path string to the main configuration file
     */
    std::string getConfigFilePathString() const { return config_path_string_; }

    /**
     * Check if the configuration is loaded from a remote source.
     *
     * @return true if config path uses a remote scheme (s3://, https://, etc.)
     */
    bool isRemoteConfig() const;

    /**
     * Read raw file contents using the configured file provider.
     *
     * @param file_path Path to the file (can be relative or absolute)
     * @return File contents as string
     * @throws std::runtime_error if file cannot be read
     */
    std::string readFile(const std::string& file_path) const;

    /**
     * Get the file provider being used.
     *
     * @return Shared pointer to the file provider
     */
    std::shared_ptr<IFileProvider> getFileProvider() const { return _file_provider; }

private:
    std::filesystem::path config_file_path_;  // For local paths (backward compat)
    std::string config_path_string_;          // Original path string (supports remote)
    std::filesystem::path base_path_;         // Base path for relative resolution
    std::string base_path_string_;            // Base path as string (supports remote)
    std::shared_ptr<IFileProvider> _file_provider;
    bool _is_remote;
};

} // namespace flapi
