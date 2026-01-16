# Path Resolution Refactoring Proposal

## Current Problem

The endpoint configuration parsing logic is duplicated across three methods in `ConfigManager`:
1. `loadEndpointConfig()` - Initial loading from disk
2. `reloadEndpointConfig()` - Reloading after external edits
3. `validateEndpointConfigFile()` - Validation before saving

Each method manually:
- Parses the YAML file with `ExtendedYamlParser`
- Gets the endpoint directory for relative path resolution
- Detects endpoint type (REST, MCP tool, resource, prompt)
- Resolves `template-source` relative to the YAML file
- Calls helper methods like `parseEndpointRequestFields`, `parseEndpointCache`, etc.

This leads to ~90 lines of duplicated code that must be kept in sync.

## Proposed Solution: `EndpointConfigParser` Class

### Design

```cpp
namespace flapi {

/**
 * @brief Parses endpoint configuration from YAML with proper path resolution
 * 
 * This class encapsulates the logic for parsing endpoint YAML files,
 * resolving relative paths, and creating EndpointConfig objects.
 */
class EndpointConfigParser {
public:
    /**
     * @brief Result of parsing an endpoint configuration
     */
    struct ParseResult {
        bool success;
        EndpointConfig config;
        std::string error_message;
        std::vector<std::string> warnings;
    };

    /**
     * @brief Construct a parser with references to ConfigManager state
     * 
     * @param yaml_parser Parser for extended YAML syntax
     * @param config_manager Parent ConfigManager for helper method access
     */
    EndpointConfigParser(
        ExtendedYamlParser& yaml_parser,
        ConfigManager* config_manager
    );

    /**
     * @brief Parse endpoint configuration from a YAML file
     * 
     * Resolves all relative paths (template-source, cache.template_file)
     * relative to the YAML file's directory.
     * 
     * @param yaml_file_path Path to the endpoint YAML file
     * @return ParseResult with the parsed configuration or error
     */
    ParseResult parseFromFile(const std::filesystem::path& yaml_file_path);

    /**
     * @brief Parse endpoint configuration from YAML content string
     * 
     * Note: Without file path context, relative paths cannot be resolved.
     * Use parseFromFile() when possible.
     * 
     * @param yaml_content YAML content as string
     * @return ParseResult with the parsed configuration or error
     */
    ParseResult parseFromString(const std::string& yaml_content);

private:
    ExtendedYamlParser& yaml_parser_;
    ConfigManager* config_manager_;  // For calling helper methods

    /**
     * @brief Parse endpoint configuration from YAML node with path resolution
     * 
     * @param yaml_node Parsed YAML node
     * @param endpoint_dir Directory of the YAML file (for relative path resolution)
     * @param result Output ParseResult to populate
     */
    void parseEndpointFromYaml(
        const YAML::Node& yaml_node,
        const std::filesystem::path& endpoint_dir,
        ParseResult& result
    );

    /**
     * @brief Detect the type of endpoint configuration
     */
    struct EndpointType {
        bool is_rest_endpoint;
        bool is_mcp_tool;
        bool is_mcp_resource;
        bool is_mcp_prompt;
        
        bool isValid() const {
            return is_rest_endpoint || is_mcp_tool || is_mcp_resource || is_mcp_prompt;
        }
    };
    
    EndpointType detectEndpointType(const YAML::Node& yaml_node) const;
};

} // namespace flapi
```

### Benefits

1. **Single Source of Truth** - Path resolution logic exists in one place
2. **Easier to Test** - Can test `EndpointConfigParser` independently
3. **Clearer API** - `parseFromFile()` vs `parseFromString()` makes path resolution explicit
4. **Better Error Handling** - Structured `ParseResult` instead of exceptions mixed with return values
5. **Reduced ConfigManager Size** - Move ~270 lines to dedicated parser class

### Migration Strategy

#### Phase 1: Create `EndpointConfigParser` class
- Extract common parsing logic
- Keep existing methods as thin wrappers

#### Phase 2: Update `ConfigManager` methods
```cpp
void ConfigManager::loadEndpointConfig(const std::string& config_file) {
    EndpointConfigParser parser(yaml_parser, this);
    auto result = parser.parseFromFile(config_file);
    
    if (!result.success) {
        CROW_LOG_ERROR << "Failed to load endpoint: " << result.error_message;
        return;
    }
    
    endpoints.push_back(result.config);
}

bool ConfigManager::reloadEndpointConfig(const std::string& slug_or_path) {
    // Find existing endpoint...
    
    EndpointConfigParser parser(yaml_parser, this);
    auto result = parser.parseFromFile(yaml_file);
    
    if (!result.success) {
        CROW_LOG_ERROR << "Failed to reload: " << result.error_message;
        return false;
    }
    
    // Log warnings
    for (const auto& warning : result.warnings) {
        CROW_LOG_WARNING << "  - " << warning;
    }
    
    *it = result.config;
    return true;
}

ValidationResult ConfigManager::validateEndpointConfigFile(const std::filesystem::path& file_path) const {
    // Use const_cast since parsing doesn't modify state
    auto* mutable_this = const_cast<ConfigManager*>(this);
    
    EndpointConfigParser parser(mutable_this->yaml_parser, mutable_this);
    auto parse_result = parser.parseFromFile(file_path);
    
    if (!parse_result.success) {
        return ValidationResult{false, {parse_result.error_message}, {}};
    }
    
    // Now validate the parsed config
    return validateEndpointConfig(parse_result.config);
}
```

#### Phase 3: Add comprehensive tests
- Test path resolution in isolation
- Test different endpoint types
- Test error cases
- Test relative vs absolute paths
- Test nested directories

### File Structure

```
src/
  include/
    endpoint_config_parser.hpp       # New header
  endpoint_config_parser.cpp          # New implementation

test/cpp/
  endpoint_config_parser_test.cpp     # New dedicated tests
  config_manager_path_resolution_test.cpp  # Can be simplified or merged
```

## Alternative: Path Resolution Strategy Pattern

For even more flexibility, we could use a strategy pattern:

```cpp
class IPathResolver {
public:
    virtual ~IPathResolver() = default;
    virtual std::filesystem::path resolve(
        const std::string& path,
        const std::filesystem::path& base_path
    ) const = 0;
};

class RelativePathResolver : public IPathResolver {
    // Resolves relative to YAML file directory
};

class AbsolutePathResolver : public IPathResolver {
    // Only accepts absolute paths
};

class TemplateRootPathResolver : public IPathResolver {
    // Resolves relative to template root (old behavior)
};
```

However, this might be over-engineering for our current needs. The simple `EndpointConfigParser` class is likely sufficient.

## Recommendation

**Proceed with the `EndpointConfigParser` extraction.**

Benefits far outweigh the refactoring effort:
- Fixes the duplication problem
- Makes the code more testable
- Reduces ConfigManager complexity
- Provides clearer separation of concerns

The refactoring can be done incrementally without breaking existing functionality.

