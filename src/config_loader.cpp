#include "config_loader.hpp"
#include <crow/logging.h>

namespace flapi {

ConfigLoader::ConfigLoader(const std::filesystem::path& config_file_path)
    : config_file_path_(config_file_path),
      config_path_string_(config_file_path.string()),
      base_path_(),  // Will be set in constructor body after making config path absolute
      base_path_string_(),
      _file_provider(std::make_shared<LocalFileProvider>()),
      _is_remote(false) {

    // First make config file path absolute, THEN derive base path from it
    // This handles the case where config_file_path has no directory component
    // (e.g., "flapi.yaml" instead of "./flapi.yaml")
    config_file_path_ = std::filesystem::absolute(config_file_path_);
    base_path_ = config_file_path_.parent_path();
    config_path_string_ = config_file_path_.string();
    base_path_string_ = base_path_.string();

    CROW_LOG_DEBUG << "ConfigLoader initialized with config file: " << config_file_path_.string();
    CROW_LOG_DEBUG << "Base path for relative path resolution: " << base_path_.string();
}

ConfigLoader::ConfigLoader(const std::string& config_file_path,
                           std::shared_ptr<IFileProvider> file_provider)
    : config_path_string_(config_file_path),
      _file_provider(std::move(file_provider)),
      _is_remote(PathSchemeUtils::IsRemotePath(config_file_path)) {

    if (_is_remote) {
        // For remote paths, extract base path from the URI
        // e.g., s3://bucket/path/to/flapi.yaml -> s3://bucket/path/to/
        auto last_slash = config_path_string_.rfind('/');
        if (last_slash != std::string::npos) {
            base_path_string_ = config_path_string_.substr(0, last_slash + 1);
        } else {
            base_path_string_ = config_path_string_;
        }
        // For remote configs, filesystem paths are not meaningful
        // but we keep them for API compatibility
        config_file_path_ = std::filesystem::path(config_path_string_);
        base_path_ = std::filesystem::path(base_path_string_);

        CROW_LOG_DEBUG << "ConfigLoader initialized with remote config: " << config_path_string_;
        CROW_LOG_DEBUG << "Remote base path: " << base_path_string_;
    } else {
        // Local path - normalize using filesystem
        std::string actual_path = PathSchemeUtils::StripFileScheme(config_file_path);
        config_file_path_ = std::filesystem::absolute(actual_path);
        base_path_ = config_file_path_.parent_path();
        config_path_string_ = config_file_path_.string();
        base_path_string_ = base_path_.string();

        CROW_LOG_DEBUG << "ConfigLoader initialized with config file: " << config_file_path_.string();
        CROW_LOG_DEBUG << "Base path for relative path resolution: " << base_path_.string();
    }
}

YAML::Node ConfigLoader::loadYamlFile(const std::filesystem::path& file_path) {
    try {
        std::filesystem::path full_path = resolvePath(file_path);
        std::string path_str = full_path.string();

        // Use file provider for existence check
        if (!_file_provider->FileExists(path_str)) {
            throw std::runtime_error("Configuration file not found: " + path_str);
        }

        CROW_LOG_DEBUG << "Loading YAML file: " << path_str;

        // Read file content using file provider and parse YAML
        std::string content = _file_provider->ReadFile(path_str);
        YAML::Node node = YAML::Load(content);
        return node;
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to parse YAML file '" + file_path.string() + "': " + e.what());
    } catch (const FileOperationError& e) {
        throw std::runtime_error("Error loading YAML file '" + file_path.string() + "': " + e.what());
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

    std::string path_str = relative_path.string();

    // If path is a remote URI, return it as-is
    if (PathSchemeUtils::IsRemotePath(path_str)) {
        return relative_path;
    }

    // If path is already absolute, return it as-is
    if (relative_path.is_absolute()) {
        return relative_path;
    }

    // For remote base paths, concatenate strings
    if (_is_remote) {
        std::string resolved = base_path_string_;
        if (!resolved.empty() && resolved.back() != '/') {
            resolved += '/';
        }
        resolved += path_str;
        return std::filesystem::path(resolved);
    }

    // Otherwise, resolve relative to base path (local filesystem)
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
    return _file_provider->FileExists(file_path.string());
}

bool ConfigLoader::directoryExists(const std::filesystem::path& dir_path) const {
    // For remote paths, we can't easily check if a directory exists
    // Most cloud storage systems don't have true directories
    if (_is_remote || PathSchemeUtils::IsRemotePath(dir_path.string())) {
        // For remote storage, assume directory exists if we can list files in it
        // This is a best-effort check
        return true;
    }
    return std::filesystem::exists(dir_path) && std::filesystem::is_directory(dir_path);
}

bool ConfigLoader::isRemoteConfig() const {
    return _is_remote;
}

std::string ConfigLoader::readFile(const std::string& file_path) const {
    // Resolve the path first
    std::filesystem::path resolved = resolvePath(std::filesystem::path(file_path));
    return _file_provider->ReadFile(resolved.string());
}

} // namespace flapi
