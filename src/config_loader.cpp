#include "config_loader.hpp"
#include <crow/logging.h>
#include <fstream>

namespace flapi {

ConfigLoader::ConfigLoader(const std::filesystem::path& config_file_path)
    : config_file_path_(config_file_path),
      base_path_(config_file_path_.parent_path()) {

    // Normalize paths
    config_file_path_ = std::filesystem::absolute(config_file_path_);
    base_path_ = std::filesystem::absolute(base_path_);

    CROW_LOG_DEBUG << "ConfigLoader initialized with config file: " << config_file_path_.string();
    CROW_LOG_DEBUG << "Base path for relative path resolution: " << base_path_.string();
}

YAML::Node ConfigLoader::loadYamlFile(const std::filesystem::path& file_path) {
    try {
        std::filesystem::path full_path = resolvePath(file_path);

        if (!std::filesystem::exists(full_path)) {
            throw std::runtime_error("Configuration file not found: " + full_path.string());
        }

        CROW_LOG_DEBUG << "Loading YAML file: " << full_path.string();

        YAML::Node node = YAML::LoadFile(full_path.string());
        return node;
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to parse YAML file '" + file_path.string() + "': " + e.what());
    } catch (const std::exception& e) {
        throw std::runtime_error("Error loading YAML file '" + file_path.string() + "': " + e.what());
    }
}

std::filesystem::path ConfigLoader::getBasePath() const {
    return base_path_;
}

std::filesystem::path ConfigLoader::getConfigDirectory() const {
    return base_path_;
}

std::filesystem::path ConfigLoader::resolvePath(const std::filesystem::path& relative_path) const {
    if (relative_path.empty()) {
        return base_path_;
    }

    // If path is already absolute, return it as-is
    if (relative_path.is_absolute()) {
        return relative_path;
    }

    // Otherwise, resolve relative to base path
    std::filesystem::path resolved = base_path_ / relative_path;

    // Normalize the path (resolve .. and .)
    try {
        resolved = std::filesystem::canonical(resolved);
    } catch (const std::filesystem::filesystem_error&) {
        // If canonical fails (e.g., path doesn't exist yet), just normalize manually
        resolved = std::filesystem::absolute(resolved);
    }

    return resolved;
}

bool ConfigLoader::fileExists(const std::filesystem::path& file_path) const {
    return std::filesystem::exists(file_path) && std::filesystem::is_regular_file(file_path);
}

bool ConfigLoader::directoryExists(const std::filesystem::path& dir_path) const {
    return std::filesystem::exists(dir_path) && std::filesystem::is_directory(dir_path);
}

} // namespace flapi
