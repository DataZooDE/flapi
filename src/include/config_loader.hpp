#pragma once

#include <filesystem>
#include <string>
#include <yaml-cpp/yaml.h>

namespace flapi {

/**
 * Responsible for loading and parsing YAML configuration files.
 * Handles file I/O, path resolution, and basic YAML parsing operations.
 * Does NOT interpret or validate the configuration - that's done elsewhere.
 */
class ConfigLoader {
public:
    /**
     * Create a ConfigLoader for a specific configuration file.
     *
     * @param config_file_path Path to the main flapi.yaml configuration file
     */
    explicit ConfigLoader(const std::filesystem::path& config_file_path);

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

private:
    std::filesystem::path config_file_path_;
    std::filesystem::path base_path_;
};

} // namespace flapi
