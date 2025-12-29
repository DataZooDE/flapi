#include <catch2/catch_test_macros.hpp>
#include "config_manager.hpp"
#include "config_service.hpp"
#include <filesystem>
#include <fstream>
#include <crow.h>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;

namespace {

/**
 * Test fixture for template lookup tests
 */
class TemplateLookupTestFixture {
public:
    TemplateLookupTestFixture() {
        // Use both time and thread_id for uniqueness (for parallel test execution)
        auto time_str = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
        test_dir = fs::temp_directory_path() / ("flapi_template_lookup_test_" + time_str + "_" + std::to_string(thread_id));
        fs::create_directories(test_dir);
        sqls_dir = test_dir / "sqls";
        fs::create_directories(sqls_dir);
    }

    ~TemplateLookupTestFixture() {
        if (fs::exists(test_dir)) {
            fs::remove_all(test_dir);
        }
    }

    std::string createMainConfig() {
        auto config_path = test_dir / "flapi.yaml";
        std::ofstream config(config_path);
        config << "project-name: template-lookup-test\n";
        config << "project-description: Test project for template lookup\n";
        config << "server-name: flapi-test-server\n";
        config << "http-port: 8080\n";
        config << "connections:\n";
        config << "  test_db:\n";
        config << "    type: duckdb\n";
        config << "    path: ':memory:'\n";
        config << "template:\n";
        config << "  path: " << sqls_dir.string() << "\n";
        config << "  environment_whitelist:\n";
        config << "    - 'TEST_.*'\n";
        config.close();
        return config_path.string();
    }

    std::string createSqlTemplate(const std::string& name) {
        auto sql_path = sqls_dir / (name + ".sql");
        std::ofstream sql(sql_path);
        sql << "SELECT * FROM test_table WHERE id = {{params.id}};\n";
        sql.close();
        return sql_path.string();
    }

    std::string createYamlEndpoint(const std::string& name, const std::string& url_path, const std::string& template_file) {
        auto yaml_path = sqls_dir / (name + ".yaml");
        std::ofstream yaml(yaml_path);
        yaml << "url-path: " << url_path << "\n";
        yaml << "method: GET\n";
        yaml << "template-source: " << template_file << "\n";
        yaml << "connection:\n";
        yaml << "  - test_db\n";
        yaml << "request:\n";
        yaml << "  - field-name: id\n";
        yaml << "    field-in: query\n";
        yaml << "    required: true\n";
        yaml.close();
        return yaml_path.string();
    }

    std::string createMcpToolEndpoint(const std::string& name, const std::string& tool_name, const std::string& template_file) {
        auto yaml_path = sqls_dir / (name + ".yaml");
        std::ofstream yaml(yaml_path);
        yaml << "mcp-tool:\n";
        yaml << "  name: " << tool_name << "\n";
        yaml << "  description: Test MCP tool\n";
        yaml << "template-source: " << template_file << "\n";
        yaml << "connection:\n";
        yaml << "  - test_db\n";
        yaml.close();
        return yaml_path.string();
    }

    fs::path test_dir;
    fs::path sqls_dir;
};

} // anonymous namespace

TEST_CASE("ConfigService: Find endpoints by template - single endpoint", "[config_service][template_lookup]") {
    TemplateLookupTestFixture fixture;
    
    // Create SQL template
    std::string template_path = fixture.createSqlTemplate("customers");
    
    // Create endpoint that uses this template
    fixture.createYamlEndpoint("customers-rest", "/customers/", template_path);
    
    // Create main config
    std::string config_file = fixture.createMainConfig();
    
    // Load configuration
    auto config_manager = std::make_shared<flapi::ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    // Create config service
    flapi::ConfigService service(config_manager, true, "test_token");
    
    // Create test request
    crow::request req;
    req.add_header("Authorization", "Bearer test_token");
    
    // Create handler
    flapi::EndpointConfigHandler handler(config_manager);
    
    // Call the endpoint
    auto response = handler.findEndpointsByTemplate(req, template_path);
    
    // Verify response
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json.has("count"));
    REQUIRE(json["count"].i() == 1);
    REQUIRE(json.has("endpoints"));
    REQUIRE(json["endpoints"].size() == 1);
    
    auto endpoint = json["endpoints"][0];
    REQUIRE(endpoint.has("url_path"));
    REQUIRE(endpoint["url_path"].s() == "/customers/");
    REQUIRE(endpoint.has("method"));
    REQUIRE(endpoint["method"].s() == "GET");
    REQUIRE(endpoint.has("type"));
    REQUIRE(endpoint["type"].s() == "REST");
    REQUIRE(endpoint.has("template_source"));
}

TEST_CASE("ConfigService: Find endpoints by template - multiple endpoints", "[config_service][template_lookup]") {
    TemplateLookupTestFixture fixture;
    
    // Create shared SQL template
    std::string template_path = fixture.createSqlTemplate("shared_query");
    
    // Create multiple endpoints that use the same template
    fixture.createYamlEndpoint("endpoint1", "/api/v1/data/", template_path);
    fixture.createYamlEndpoint("endpoint2", "/api/v2/data/", template_path);
    fixture.createYamlEndpoint("endpoint3", "/internal/data/", template_path);
    
    // Create another endpoint with different template
    std::string other_template = fixture.createSqlTemplate("other_query");
    fixture.createYamlEndpoint("other", "/other/", other_template);
    
    // Create main config
    std::string config_file = fixture.createMainConfig();
    
    // Load configuration
    auto config_manager = std::make_shared<flapi::ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    // Create handler
    flapi::EndpointConfigHandler handler(config_manager);
    
    // Create test request
    crow::request req;
    
    // Call the endpoint
    auto response = handler.findEndpointsByTemplate(req, template_path);
    
    // Verify response
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json.has("count"));
    REQUIRE(json["count"].i() == 3);
    REQUIRE(json.has("endpoints"));
    REQUIRE(json["endpoints"].size() == 3);
    
    // Verify all endpoints are present
    std::vector<std::string> found_paths;
    for (size_t i = 0; i < json["endpoints"].size(); ++i) {
        auto endpoint = json["endpoints"][i];
        REQUIRE(endpoint.has("url_path"));
        found_paths.push_back(endpoint["url_path"].s());
    }
    
    REQUIRE(std::find(found_paths.begin(), found_paths.end(), "/api/v1/data/") != found_paths.end());
    REQUIRE(std::find(found_paths.begin(), found_paths.end(), "/api/v2/data/") != found_paths.end());
    REQUIRE(std::find(found_paths.begin(), found_paths.end(), "/internal/data/") != found_paths.end());
}

TEST_CASE("ConfigService: Find endpoints by template - no matches", "[config_service][template_lookup]") {
    TemplateLookupTestFixture fixture;
    
    // Create SQL templates
    std::string template1 = fixture.createSqlTemplate("query1");
    std::string template2 = fixture.createSqlTemplate("query2");
    
    // Create endpoint that uses template1
    fixture.createYamlEndpoint("endpoint1", "/api/data/", template1);
    
    // Create main config
    std::string config_file = fixture.createMainConfig();
    
    // Load configuration
    auto config_manager = std::make_shared<flapi::ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    // Create handler
    flapi::EndpointConfigHandler handler(config_manager);
    
    // Create test request
    crow::request req;
    
    // Call the endpoint with template2 (which has no associated endpoints)
    auto response = handler.findEndpointsByTemplate(req, template2);
    
    // Verify response
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json.has("count"));
    REQUIRE(json["count"].i() == 0);
    REQUIRE(json.has("endpoints"));
    REQUIRE(json["endpoints"].size() == 0);
}

TEST_CASE("ConfigService: Find endpoints by template - MCP tool", "[config_service][template_lookup]") {
    TemplateLookupTestFixture fixture;
    
    // Create SQL template
    std::string template_path = fixture.createSqlTemplate("mcp_query");
    
    // Create MCP tool endpoint
    fixture.createMcpToolEndpoint("mcp-tool", "test_tool", template_path);
    
    // Create main config
    std::string config_file = fixture.createMainConfig();
    
    // Load configuration
    auto config_manager = std::make_shared<flapi::ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    // Create handler
    flapi::EndpointConfigHandler handler(config_manager);
    
    // Create test request
    crow::request req;
    
    // Call the endpoint
    auto response = handler.findEndpointsByTemplate(req, template_path);
    
    // Verify response
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json.has("count"));
    REQUIRE(json["count"].i() == 1);
    REQUIRE(json.has("endpoints"));
    REQUIRE(json["endpoints"].size() == 1);
    
    auto endpoint = json["endpoints"][0];
    REQUIRE(endpoint.has("type"));
    REQUIRE(endpoint["type"].s() == "MCP_Tool");
    REQUIRE(endpoint.has("mcp_name"));
    REQUIRE(endpoint["mcp_name"].s() == "test_tool");
    REQUIRE(endpoint.has("template_source"));
}

TEST_CASE("ConfigService: Find endpoints by template - mixed REST and MCP", "[config_service][template_lookup]") {
    TemplateLookupTestFixture fixture;
    
    // Create shared SQL template
    std::string template_path = fixture.createSqlTemplate("shared_query");
    
    // Create REST endpoint
    fixture.createYamlEndpoint("rest-endpoint", "/api/data/", template_path);
    
    // Create MCP tool endpoint
    fixture.createMcpToolEndpoint("mcp-endpoint", "data_tool", template_path);
    
    // Create main config
    std::string config_file = fixture.createMainConfig();
    
    // Load configuration
    auto config_manager = std::make_shared<flapi::ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    // Create handler
    flapi::EndpointConfigHandler handler(config_manager);
    
    // Create test request
    crow::request req;
    
    // Call the endpoint
    auto response = handler.findEndpointsByTemplate(req, template_path);
    
    // Verify response
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json.has("count"));
    REQUIRE(json["count"].i() == 2);
    REQUIRE(json.has("endpoints"));
    REQUIRE(json["endpoints"].size() == 2);
    
    // Verify both endpoint types are present
    bool has_rest = false;
    bool has_mcp = false;
    
    for (size_t i = 0; i < json["endpoints"].size(); ++i) {
        auto endpoint = json["endpoints"][i];
        REQUIRE(endpoint.has("type"));
        
        std::string type = endpoint["type"].s();
        if (type == "REST") {
            has_rest = true;
            REQUIRE(endpoint.has("url_path"));
            REQUIRE(endpoint["url_path"].s() == "/api/data/");
        } else if (type == "MCP_Tool") {
            has_mcp = true;
            REQUIRE(endpoint.has("mcp_name"));
            REQUIRE(endpoint["mcp_name"].s() == "data_tool");
        }
    }
    
    REQUIRE(has_rest);
    REQUIRE(has_mcp);
}

TEST_CASE("ConfigService: Find endpoints by template - path normalization", "[config_service][template_lookup]") {
    TemplateLookupTestFixture fixture;
    
    // Create SQL template
    std::string template_path = fixture.createSqlTemplate("test_query");
    
    // Create endpoint
    fixture.createYamlEndpoint("test-endpoint", "/test/", template_path);
    
    // Create main config
    std::string config_file = fixture.createMainConfig();
    
    // Load configuration
    auto config_manager = std::make_shared<flapi::ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    // Create handler
    flapi::EndpointConfigHandler handler(config_manager);
    
    // Create test request
    crow::request req;
    
    // Test with different path variations (should all match due to normalization)
    std::vector<std::string> path_variations = {
        template_path,
        fs::path(template_path).lexically_normal().string(),
        (fs::path(template_path).parent_path() / "." / fs::path(template_path).filename()).string()
    };
    
    for (const auto& path : path_variations) {
        auto response = handler.findEndpointsByTemplate(req, path);
        
        REQUIRE(response.code == 200);
        
        auto json = crow::json::load(response.body);
        REQUIRE(json);
        REQUIRE(json.has("count"));
        REQUIRE(json["count"].i() == 1);
    }
}

TEST_CASE("ConfigService: Find endpoints by template - nonexistent template", "[config_service][template_lookup]") {
    TemplateLookupTestFixture fixture;
    
    // Create SQL template and endpoint
    std::string template_path = fixture.createSqlTemplate("real_query");
    fixture.createYamlEndpoint("real-endpoint", "/real/", template_path);
    
    // Create main config
    std::string config_file = fixture.createMainConfig();
    
    // Load configuration
    auto config_manager = std::make_shared<flapi::ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    // Create handler
    flapi::EndpointConfigHandler handler(config_manager);
    
    // Create test request
    crow::request req;
    
    // Query for a template that doesn't exist
    std::string fake_template = (fixture.sqls_dir / "nonexistent.sql").string();
    auto response = handler.findEndpointsByTemplate(req, fake_template);
    
    // Should return empty results, not error
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json.has("count"));
    REQUIRE(json["count"].i() == 0);
    REQUIRE(json.has("endpoints"));
    REQUIRE(json["endpoints"].size() == 0);
}

