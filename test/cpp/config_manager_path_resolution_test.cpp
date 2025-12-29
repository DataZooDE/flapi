#include <catch2/catch_test_macros.hpp>
#include "config_manager.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <ctime>

namespace fs = std::filesystem;

namespace flapi {

class PathResolutionTestFixture {
public:
    fs::path test_dir;
    fs::path config_file;
    fs::path endpoint_dir;
    fs::path template_file;
    fs::path cache_template_file;

    PathResolutionTestFixture() {
        // Create a temporary test directory structure
        test_dir = fs::temp_directory_path() / ("flapi_path_test_" + std::to_string(std::time(nullptr)));
        fs::create_directories(test_dir);
        
        endpoint_dir = test_dir / "endpoints" / "test_endpoint";
        fs::create_directories(endpoint_dir);
        
        // Create main config file
        config_file = test_dir / "flapi.yaml";
        createMainConfig();
        
        // Create SQL template files
        template_file = endpoint_dir / "query.sql";
        cache_template_file = endpoint_dir / "cache_query.sql";
        createTemplateFiles();
    }

    ~PathResolutionTestFixture() {
        // Clean up test directory
        if (fs::exists(test_dir)) {
            fs::remove_all(test_dir);
        }
    }

    void createMainConfig() {
        std::ofstream config(config_file);
        config << "project-name: test_project\n";
        config << "project-description: Test project for path resolution\n";
        config << "http-port: 8080\n";
        config << "template:\n";
        config << "  path: " << (test_dir / "endpoints").string() << "\n";
        config << "connections:\n";
        config << "  test_db:\n";
        config << "    init: \"SELECT 1\"\n";
        config << "    properties:\n";
        config << "      database: \":memory:\"\n";
        config.close();
    }

    void createTemplateFiles() {
        std::ofstream template_f(template_file);
        template_f << "SELECT * FROM test_table WHERE id = {{params.id}}";
        template_f.close();
        
        std::ofstream cache_template_f(cache_template_file);
        cache_template_f << "SELECT * FROM cache_table";
        cache_template_f.close();
    }

    void createEndpointYaml(const std::string& template_source, 
                           const std::string& cache_template_source = "") {
        fs::path endpoint_yaml = endpoint_dir / "endpoint.yaml";
        std::ofstream yaml(endpoint_yaml);
        yaml << "url-path: /test\n";
        yaml << "template-source: " << template_source << "\n";
        yaml << "connection:\n";
        yaml << "  - test_db\n";
        
        if (!cache_template_source.empty()) {
            yaml << "cache:\n";
            yaml << "  enabled: true\n";
            yaml << "  table: test_cache\n";
            yaml << "  schema: public\n";
            yaml << "  template-file: " << cache_template_source << "\n";
        }
        
        yaml.close();
    }
};

TEST_CASE("Path Resolution: Initial load with relative paths", "[config_manager][path_resolution]") {
    PathResolutionTestFixture fixture;
    
    // Create endpoint config with relative paths
    fixture.createEndpointYaml("query.sql", "cache_query.sql");
    
    // Load configuration
    ConfigManager manager(fixture.config_file);
    manager.loadConfig();
    
    // Get the loaded endpoint
    auto endpoints = manager.getEndpoints();
    REQUIRE(endpoints.size() == 1);
    
    const auto& endpoint = endpoints[0];
    
    // Verify template source path is resolved to absolute
    INFO("Template source: " << endpoint.templateSource);
    REQUIRE(endpoint.templateSource == fixture.template_file.string());
    REQUIRE(fs::path(endpoint.templateSource).is_absolute());
    REQUIRE(fs::exists(endpoint.templateSource));
    
    // Verify cache template path is resolved to absolute
    REQUIRE(endpoint.cache.template_file.has_value());
    INFO("Cache template: " << endpoint.cache.template_file.value());
    REQUIRE(endpoint.cache.template_file.value() == fixture.cache_template_file.string());
    REQUIRE(fs::path(endpoint.cache.template_file.value()).is_absolute());
    REQUIRE(fs::exists(endpoint.cache.template_file.value()));
}

TEST_CASE("Path Resolution: Initial load with absolute paths", "[config_manager][path_resolution]") {
    PathResolutionTestFixture fixture;
    
    // Create endpoint config with absolute paths
    fixture.createEndpointYaml(fixture.template_file.string(), fixture.cache_template_file.string());
    
    // Load configuration
    ConfigManager manager(fixture.config_file);
    manager.loadConfig();
    
    // Get the loaded endpoint
    auto endpoints = manager.getEndpoints();
    REQUIRE(endpoints.size() == 1);
    
    const auto& endpoint = endpoints[0];
    
    // Verify template source path remains absolute
    REQUIRE(endpoint.templateSource == fixture.template_file.string());
    REQUIRE(fs::path(endpoint.templateSource).is_absolute());
    
    // Verify cache template path remains absolute
    REQUIRE(endpoint.cache.template_file.has_value());
    REQUIRE(endpoint.cache.template_file.value() == fixture.cache_template_file.string());
    REQUIRE(fs::path(endpoint.cache.template_file.value()).is_absolute());
}

TEST_CASE("Path Resolution: Reload with relative paths", "[config_manager][path_resolution]") {
    PathResolutionTestFixture fixture;
    
    // Initial load with relative paths
    fixture.createEndpointYaml("query.sql", "cache_query.sql");
    
    ConfigManager manager(fixture.config_file);
    manager.loadConfig();
    
    auto endpoints_before = manager.getEndpoints();
    REQUIRE(endpoints_before.size() == 1);
    
    std::string template_before = endpoints_before[0].templateSource;
    std::string cache_template_before = endpoints_before[0].cache.template_file.value();
    
    // Modify the endpoint YAML (simulate external edit)
    fs::path endpoint_yaml = fixture.endpoint_dir / "endpoint.yaml";
    std::ofstream yaml(endpoint_yaml);
    yaml << "url-path: /test\n";
    yaml << "method: POST\n";  // Change something
    yaml << "template-source: query.sql\n";  // Still relative
    yaml << "connection:\n";
    yaml << "  - test_db\n";
    yaml << "cache:\n";
    yaml << "  enabled: true\n";
    yaml << "  table: test_cache\n";
    yaml << "  schema: public\n";
    yaml << "  template-file: cache_query.sql\n";  // Still relative
    yaml.close();
    
    // Reload the endpoint
    bool reload_success = manager.reloadEndpointConfig("/test");
    REQUIRE(reload_success);
    
    // Get the reloaded endpoint
    auto endpoints_after = manager.getEndpoints();
    REQUIRE(endpoints_after.size() == 1);
    
    const auto& endpoint = endpoints_after[0];
    
    // Verify paths are still resolved correctly after reload
    INFO("Template after reload: " << endpoint.templateSource);
    INFO("Template before reload: " << template_before);
    REQUIRE(endpoint.templateSource == fixture.template_file.string());
    REQUIRE(endpoint.templateSource == template_before);  // Should be same as before
    REQUIRE(fs::path(endpoint.templateSource).is_absolute());
    REQUIRE(fs::exists(endpoint.templateSource));
    
    REQUIRE(endpoint.cache.template_file.has_value());
    INFO("Cache template after reload: " << endpoint.cache.template_file.value());
    INFO("Cache template before reload: " << cache_template_before);
    REQUIRE(endpoint.cache.template_file.value() == fixture.cache_template_file.string());
    REQUIRE(endpoint.cache.template_file.value() == cache_template_before);  // Should be same as before
    REQUIRE(fs::path(endpoint.cache.template_file.value()).is_absolute());
    REQUIRE(fs::exists(endpoint.cache.template_file.value()));
    
    // Verify the method was actually updated
    REQUIRE(endpoint.method == "POST");
}

TEST_CASE("Path Resolution: Validation with relative paths", "[config_manager][path_resolution]") {
    PathResolutionTestFixture fixture;
    fixture.createEndpointYaml("query.sql", "cache_query.sql");
    
    ConfigManager manager(fixture.config_file);
    manager.loadConfig();
    
    // Validate the endpoint YAML file
    fs::path endpoint_yaml = fixture.endpoint_dir / "endpoint.yaml";
    auto validation = manager.validateEndpointConfigFile(endpoint_yaml);
    
    // Validation should pass
    INFO("Validation errors: " << validation.errors.size());
    for (const auto& error : validation.errors) {
        INFO("Error: " << error);
    }
    REQUIRE(validation.valid);
    REQUIRE(validation.errors.empty());
    
    // Check that there are no warnings about template files not existing
    bool has_template_warning = false;
    bool has_cache_template_warning = false;
    
    for (const auto& warning : validation.warnings) {
        INFO("Warning: " << warning);
        if (warning.find("Template file does not exist") != std::string::npos) {
            has_template_warning = true;
        }
        if (warning.find("Cache template file does not exist") != std::string::npos) {
            has_cache_template_warning = true;
        }
    }
    
    REQUIRE_FALSE(has_template_warning);
    REQUIRE_FALSE(has_cache_template_warning);
}

TEST_CASE("Path Resolution: Consistency between load, reload, and validation", "[config_manager][path_resolution]") {
    PathResolutionTestFixture fixture;
    fixture.createEndpointYaml("query.sql", "cache_query.sql");
    
    ConfigManager manager(fixture.config_file);
    
    // 1. Initial load
    manager.loadConfig();
    auto endpoints_load = manager.getEndpoints();
    REQUIRE(endpoints_load.size() == 1);
    std::string template_after_load = endpoints_load[0].templateSource;
    std::string cache_template_after_load = endpoints_load[0].cache.template_file.value();
    
    // 2. Validation
    fs::path endpoint_yaml = fixture.endpoint_dir / "endpoint.yaml";
    auto validation = manager.validateEndpointConfigFile(endpoint_yaml);
    INFO("Validation result: " << (validation.valid ? "valid" : "invalid"));
    for (const auto& error : validation.errors) {
        INFO("Validation error: " << error);
    }
    REQUIRE(validation.valid);
    
    // 3. Reload
    bool reload_success = manager.reloadEndpointConfig("/test");
    REQUIRE(reload_success);
    auto endpoints_reload = manager.getEndpoints();
    REQUIRE(endpoints_reload.size() == 1);
    std::string template_after_reload = endpoints_reload[0].templateSource;
    std::string cache_template_after_reload = endpoints_reload[0].cache.template_file.value();
    
    // All three operations should result in the same resolved paths
    INFO("Template after load: " << template_after_load);
    INFO("Template after reload: " << template_after_reload);
    REQUIRE(template_after_load == template_after_reload);
    
    INFO("Cache template after load: " << cache_template_after_load);
    INFO("Cache template after reload: " << cache_template_after_reload);
    REQUIRE(cache_template_after_load == cache_template_after_reload);
    
    // Paths should be absolute
    REQUIRE(fs::path(template_after_load).is_absolute());
    REQUIRE(fs::path(cache_template_after_load).is_absolute());
    
    // Files should exist
    REQUIRE(fs::exists(template_after_load));
    REQUIRE(fs::exists(cache_template_after_load));
}

TEST_CASE("Path Resolution: Nested directory structure", "[config_manager][path_resolution]") {
    PathResolutionTestFixture fixture;
    
    // Create a nested directory structure
    fs::path nested_dir = fixture.endpoint_dir / "subdir";
    fs::create_directories(nested_dir);
    
    fs::path nested_template = nested_dir / "nested_query.sql";
    std::ofstream nested_f(nested_template);
    nested_f << "SELECT * FROM nested_table";
    nested_f.close();
    
    // Create endpoint config with relative path to nested file
    fixture.createEndpointYaml("subdir/nested_query.sql");
    
    ConfigManager manager(fixture.config_file);
    manager.loadConfig();
    
    auto endpoints = manager.getEndpoints();
    REQUIRE(endpoints.size() == 1);
    
    // Verify nested path is resolved correctly
    INFO("Template source: " << endpoints[0].templateSource);
    INFO("Expected: " << nested_template.string());
    REQUIRE(endpoints[0].templateSource == nested_template.string());
    REQUIRE(fs::exists(endpoints[0].templateSource));
}

TEST_CASE("Path Resolution: Missing file detection", "[config_manager][path_resolution]") {
    PathResolutionTestFixture fixture;
    
    // Create endpoint config with non-existent file
    fixture.createEndpointYaml("nonexistent.sql");
    
    ConfigManager manager(fixture.config_file);
    manager.loadConfig();
    
    auto endpoints = manager.getEndpoints();
    REQUIRE(endpoints.size() == 1);
    
    // Path should still be resolved (to absolute), even if file doesn't exist
    std::string resolved_path = endpoints[0].templateSource;
    INFO("Resolved path: " << resolved_path);
    REQUIRE(fs::path(resolved_path).is_absolute());
    REQUIRE_FALSE(fs::exists(resolved_path));
    
    // Validation should warn about missing file
    fs::path endpoint_yaml = fixture.endpoint_dir / "endpoint.yaml";
    auto validation = manager.validateEndpointConfigFile(endpoint_yaml);
    
    // Should have a warning about template file not existing
    bool has_warning = false;
    for (const auto& warning : validation.warnings) {
        INFO("Validation warning: " << warning);
        if (warning.find("Template file does not exist") != std::string::npos) {
            has_warning = true;
            // Warning should mention the resolved absolute path
            REQUIRE(warning.find(resolved_path) != std::string::npos);
        }
    }
    REQUIRE(has_warning);
}

} // namespace flapi
