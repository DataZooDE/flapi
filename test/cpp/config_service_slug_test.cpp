#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "config_service.hpp"
#include "config_manager.hpp"
#include "path_utils.hpp"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>

using namespace flapi;
namespace fs = std::filesystem;

namespace {

/**
 * Test fixture for slug/path conversion tests
 */
class SlugTestFixture {
public:
    SlugTestFixture() {
        // Create unique temp directory using high-resolution timestamp and thread ID
        auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
        temp_dir_ = fs::temp_directory_path() / 
                    ("flapi_slug_test_" + std::to_string(timestamp) + "_" + std::to_string(thread_id));
        fs::create_directories(temp_dir_);
    }
    
    ~SlugTestFixture() {
        if (fs::exists(temp_dir_)) {
            fs::remove_all(temp_dir_);
        }
    }
    
    std::string createMainConfig() {
        auto config_path = temp_dir_ / "flapi.yaml";
        std::ofstream config_file(config_path);
        config_file << "project_name: slug-test\n";
        config_file << "project_description: Test slug conversion\n";
        config_file << "template:\n";
        config_file << "  path: " << temp_dir_.string() << "\n";
        config_file << "connections:\n";
        config_file << "  default:\n";
        config_file << "    init: ':memory:'\n";
        config_file.close();
        return config_path.string();
    }
    
    std::string createSqlTemplate(const std::string& name) {
        auto template_path = temp_dir_ / (name + ".sql");
        std::ofstream template_file(template_path);
        template_file << "SELECT * FROM " << name << " WHERE id = {{params.id}}";
        template_file.close();
        return template_path.string();
    }
    
    std::string createRestEndpoint(const std::string& yaml_name, const std::string& url_path, const std::string& template_path) {
        auto endpoint_path = temp_dir_ / (yaml_name + ".yaml");
        std::ofstream endpoint_file(endpoint_path);
        endpoint_file << "url-path: " << url_path << "\n";
        endpoint_file << "method: GET\n";
        endpoint_file << "template-source: " << template_path << "\n";
        endpoint_file << "connection:\n";
        endpoint_file << "  - default\n";
        endpoint_file.close();
        return endpoint_path.string();
    }
    
    std::string createMcpToolEndpoint(const std::string& yaml_name, const std::string& tool_name, const std::string& template_path) {
        auto endpoint_path = temp_dir_ / (yaml_name + ".yaml");
        std::ofstream endpoint_file(endpoint_path);
        endpoint_file << "mcp-tool:\n";
        endpoint_file << "  name: " << tool_name << "\n";
        endpoint_file << "  description: Test MCP tool\n";
        endpoint_file << "template-source: " << template_path << "\n";
        endpoint_file << "connection:\n";
        endpoint_file << "  - default\n";
        endpoint_file.close();
        return endpoint_path.string();
    }
    
    std::string createMcpResourceEndpoint(const std::string& yaml_name, const std::string& resource_name, const std::string& template_path) {
        auto endpoint_path = temp_dir_ / (yaml_name + ".yaml");
        std::ofstream endpoint_file(endpoint_path);
        endpoint_file << "mcp-resource:\n";
        endpoint_file << "  name: " << resource_name << "\n";
        endpoint_file << "  description: Test MCP resource\n";
        endpoint_file << "template-source: " << template_path << "\n";
        endpoint_file << "connection:\n";
        endpoint_file << "  - default\n";
        endpoint_file.close();
        return endpoint_path.string();
    }
    
    fs::path temp_dir_;
};

} // namespace

// ============================================================================
// PathUtils Tests
// ============================================================================

TEST_CASE("PathUtils: Path to slug conversion - simple paths", "[slug][path_utils]") {
    REQUIRE(PathUtils::pathToSlug("/customers/") == "customers-slash");
    REQUIRE(PathUtils::pathToSlug("/api/v1/data/") == "api-v1-data-slash");
    REQUIRE(PathUtils::pathToSlug("/sap/functions") == "sap-functions");
    REQUIRE(PathUtils::pathToSlug("/") == "empty");  // Root path becomes "empty"
}

TEST_CASE("PathUtils: Path to slug conversion - edge cases", "[slug][path_utils]") {
    REQUIRE(PathUtils::pathToSlug("") == "empty");  // Empty string becomes "empty"
    REQUIRE(PathUtils::pathToSlug("/single") == "single");
    REQUIRE(PathUtils::pathToSlug("/multiple/nested/path/") == "multiple-nested-path-slash");
}

TEST_CASE("PathUtils: Slug to path conversion - round trip", "[slug][path_utils]") {
    std::vector<std::string> test_paths = {
        "/customers/",
        "/api/v1/data/",
        "/sap/functions",
        "/single",
        "/multiple/nested/path/"
    };
    
    for (const auto& original_path : test_paths) {
        std::string slug = PathUtils::pathToSlug(original_path);
        std::string reconstructed_path = PathUtils::slugToPath(slug);
        REQUIRE(reconstructed_path == original_path);
    }
    
    // Special case: "/" and "" both become "empty" and reconstruct to ""
    REQUIRE(PathUtils::pathToSlug("/") == "empty");
    REQUIRE(PathUtils::slugToPath("empty") == "");
}

// ============================================================================
// EndpointConfig::getSlug() Tests
// ============================================================================

TEST_CASE("EndpointConfig: getSlug() for REST endpoints", "[slug][endpoint_config]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("test");
    fixture.createRestEndpoint("test-endpoint", "/customers/", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    const auto& endpoints = config_manager->getEndpoints();
    REQUIRE(endpoints.size() == 1);
    
    const auto& endpoint = endpoints[0];
    REQUIRE(endpoint.urlPath == "/customers/");
    REQUIRE(endpoint.getSlug() == "customers-slash");
}

TEST_CASE("EndpointConfig: getSlug() for MCP tool", "[slug][endpoint_config]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("test");
    fixture.createMcpToolEndpoint("mcp-tool", "customer_lookup", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    const auto& endpoints = config_manager->getEndpoints();
    REQUIRE(endpoints.size() == 1);
    
    const auto& endpoint = endpoints[0];
    REQUIRE(endpoint.mcp_tool.has_value());
    REQUIRE(endpoint.mcp_tool->name == "customer_lookup");
    REQUIRE(endpoint.getSlug() == "customer_lookup");  // MCP names used as-is
}

TEST_CASE("EndpointConfig: getSlug() for MCP resource", "[slug][endpoint_config]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("test");
    fixture.createMcpResourceEndpoint("mcp-resource", "data_resource", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    const auto& endpoints = config_manager->getEndpoints();
    REQUIRE(endpoints.size() == 1);
    
    const auto& endpoint = endpoints[0];
    REQUIRE(endpoint.mcp_resource.has_value());
    REQUIRE(endpoint.mcp_resource->name == "data_resource");
    REQUIRE(endpoint.getSlug() == "data_resource");  // MCP names used as-is
}

TEST_CASE("EndpointConfig: getSlug() consistency - multiple REST endpoints", "[slug][endpoint_config]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("test");
    fixture.createRestEndpoint("endpoint1", "/api/v1/customers/", template_path);
    fixture.createRestEndpoint("endpoint2", "/api/v2/products", template_path);
    fixture.createRestEndpoint("endpoint3", "/root", template_path);  // Use "/root" instead of "/"
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    const auto& endpoints = config_manager->getEndpoints();
    REQUIRE(endpoints.size() == 3);
    
    // Verify each endpoint has correct slug
    std::map<std::string, std::string> expected_slugs = {
        {"/api/v1/customers/", "api-v1-customers-slash"},
        {"/api/v2/products", "api-v2-products"},
        {"/root", "root"}
    };
    
    for (const auto& endpoint : endpoints) {
        auto it = expected_slugs.find(endpoint.urlPath);
        REQUIRE(it != expected_slugs.end());
        REQUIRE(endpoint.getSlug() == it->second);
    }
}

// ============================================================================
// Slug-Based API Lookup Tests
// ============================================================================

TEST_CASE("ConfigService: Slug-based endpoint lookup - REST", "[slug][config_service]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("customers");
    fixture.createRestEndpoint("customers-rest", "/customers/", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    EndpointConfigHandler handler(config_manager);
    
    crow::request req;
    auto response = handler.getEndpointConfigBySlug(req, "customers-slash");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json.has("url-path"));
    REQUIRE(json["url-path"].s() == "/customers/");
}

TEST_CASE("ConfigService: Slug-based endpoint lookup - MCP tool", "[slug][config_service]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("customers");
    fixture.createMcpToolEndpoint("customers-mcp", "customer_lookup", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    EndpointConfigHandler handler(config_manager);
    
    crow::request req;
    auto response = handler.getEndpointConfigBySlug(req, "customer_lookup");  // Use MCP name as-is
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json.has("mcp-tool"));
    REQUIRE(json["mcp-tool"]["name"].s() == "customer_lookup");
}

TEST_CASE("ConfigService: Slug-based endpoint lookup - not found", "[slug][config_service]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("customers");
    fixture.createRestEndpoint("customers-rest", "/customers/", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    EndpointConfigHandler handler(config_manager);
    
    crow::request req;
    auto response = handler.getEndpointConfigBySlug(req, "nonexistent-slug");
    
    REQUIRE(response.code == 404);
}

TEST_CASE("ConfigService: Slug-based vs path-based lookup consistency", "[slug][config_service]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("test");
    fixture.createRestEndpoint("test-endpoint", "/api/v1/test/", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    EndpointConfigHandler handler(config_manager);
    crow::request req;
    
    // Get via slug
    auto slug_response = handler.getEndpointConfigBySlug(req, "api-v1-test-slash");
    REQUIRE(slug_response.code == 200);
    auto slug_json = crow::json::load(slug_response.body);
    
    // Get via legacy path
    auto path_response = handler.getEndpointConfig(req, "/api/v1/test/");
    REQUIRE(path_response.code == 200);
    auto path_json = crow::json::load(path_response.body);
    
    // Both should return the same endpoint
    REQUIRE(slug_json["url-path"].s() == path_json["url-path"].s());
    REQUIRE(slug_json["method"].s() == path_json["method"].s());
}

// ============================================================================
// Mixed REST and MCP Slug Uniqueness Tests
// ============================================================================

TEST_CASE("ConfigService: Slug uniqueness - REST and MCP can coexist", "[slug][config_service]") {
    SlugTestFixture fixture;
    
    std::string template_path1 = fixture.createSqlTemplate("customers_rest");
    std::string template_path2 = fixture.createSqlTemplate("customers_mcp");
    
    // Create REST endpoint with slug "customers-slash"
    fixture.createRestEndpoint("customers-rest", "/customers/", template_path1);
    
    // Create MCP tool with name "customer_lookup" (different slug)
    fixture.createMcpToolEndpoint("customers-mcp", "customer_lookup", template_path2);
    
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    EndpointConfigHandler handler(config_manager);
    crow::request req;
    
    // Both endpoints should be accessible via their respective slugs
    auto rest_response = handler.getEndpointConfigBySlug(req, "customers-slash");
    REQUIRE(rest_response.code == 200);
    auto rest_json = crow::json::load(rest_response.body);
    REQUIRE(rest_json["url-path"].s() == "/customers/");
    
    auto mcp_response = handler.getEndpointConfigBySlug(req, "customer_lookup");
    REQUIRE(mcp_response.code == 200);
    auto mcp_json = crow::json::load(mcp_response.body);
    REQUIRE(mcp_json["mcp-tool"]["name"].s() == "customer_lookup");
}

TEST_CASE("ConfigService: Complex path slugging", "[slug][config_service]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("complex");
    
    // Create endpoints with various complex paths
    fixture.createRestEndpoint("endpoint1", "/api/v1/customers/orders/", template_path);
    fixture.createRestEndpoint("endpoint2", "/sap/erp/functions/materialize", template_path);
    
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    EndpointConfigHandler handler(config_manager);
    crow::request req;
    
    // Test complex slugs
    auto response1 = handler.getEndpointConfigBySlug(req, "api-v1-customers-orders-slash");
    REQUIRE(response1.code == 200);
    auto json1 = crow::json::load(response1.body);
    REQUIRE(json1["url-path"].s() == "/api/v1/customers/orders/");
    
    auto response2 = handler.getEndpointConfigBySlug(req, "sap-erp-functions-materialize");
    REQUIRE(response2.code == 200);
    auto json2 = crow::json::load(response2.body);
    REQUIRE(json2["url-path"].s() == "/sap/erp/functions/materialize");
}

// ============================================================================
// Template Handler Slug-Based Tests
// ============================================================================

TEST_CASE("TemplateHandler: Slug-based template expand - REST endpoint", "[slug][template_handler]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("test");
    fixture.createRestEndpoint("test-endpoint", "/test/", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    TemplateHandler handler(config_manager);
    
    crow::request req;
    req.body = R"({"parameters": {"id": {"value": "123"}}})";
    
    auto response = handler.expandTemplateBySlug(req, "test-slash");
    
    REQUIRE(response.code == 200);
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json.has("expanded"));
}

TEST_CASE("TemplateHandler: Slug-based template expand - MCP tool", "[slug][template_handler]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("test");
    fixture.createMcpToolEndpoint("test-mcp", "test_tool", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    TemplateHandler handler(config_manager);
    
    crow::request req;
    req.body = R"({"parameters": {"id": {"value": "456"}}})";
    
    auto response = handler.expandTemplateBySlug(req, "test_tool");  // Use MCP name as-is
    
    REQUIRE(response.code == 200);
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json.has("expanded"));
}

TEST_CASE("TemplateHandler: Slug-based template test - not found", "[slug][template_handler]") {
    SlugTestFixture fixture;
    
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    TemplateHandler handler(config_manager);
    
    crow::request req;
    req.body = R"({"parameters": {}})";
    
    auto response = handler.expandTemplateBySlug(req, "nonexistent");
    
    REQUIRE(response.code == 404);
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST_CASE("Integration: End-to-end slug workflow - REST", "[slug][integration]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("customers");
    fixture.createRestEndpoint("customers-rest", "/api/customers/", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    // 1. Find slug from endpoint
    const auto& endpoints = config_manager->getEndpoints();
    REQUIRE(endpoints.size() == 1);
    std::string slug = endpoints[0].getSlug();
    REQUIRE(slug == "api-customers-slash");
    
    // 2. Use slug to get config
    EndpointConfigHandler endpoint_handler(config_manager);
    crow::request req;
    auto config_response = endpoint_handler.getEndpointConfigBySlug(req, slug);
    REQUIRE(config_response.code == 200);
    
    // 3. Use slug to expand template
    TemplateHandler template_handler(config_manager);
    req.body = R"({"parameters": {"id": {"value": "100"}}})";
    auto expand_response = template_handler.expandTemplateBySlug(req, slug);
    REQUIRE(expand_response.code == 200);
}

TEST_CASE("Integration: End-to-end slug workflow - MCP", "[slug][integration]") {
    SlugTestFixture fixture;
    
    std::string template_path = fixture.createSqlTemplate("data");
    fixture.createMcpToolEndpoint("data-mcp", "data_fetcher", template_path);
    std::string config_file = fixture.createMainConfig();
    
    auto config_manager = std::make_shared<ConfigManager>(fs::path(config_file));
    config_manager->loadConfig();
    
    // 1. Find slug from endpoint
    const auto& endpoints = config_manager->getEndpoints();
    REQUIRE(endpoints.size() == 1);
    std::string slug = endpoints[0].getSlug();
    REQUIRE(slug == "data_fetcher");  // MCP name used as-is
    
    // 2. Use slug to get config
    EndpointConfigHandler endpoint_handler(config_manager);
    crow::request req;
    auto config_response = endpoint_handler.getEndpointConfigBySlug(req, slug);
    REQUIRE(config_response.code == 200);
    auto json = crow::json::load(config_response.body);
    REQUIRE(json["mcp-tool"]["name"].s() == "data_fetcher");
    
    // 3. Use slug to expand template
    TemplateHandler template_handler(config_manager);
    req.body = R"({"parameters": {"id": {"value": "200"}}})";
    auto expand_response = template_handler.expandTemplateBySlug(req, slug);
    REQUIRE(expand_response.code == 200);
}

