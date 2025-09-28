#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#include "api_server.hpp"
#include "config_service.hpp"
#include "database_manager.hpp"
#include "request_handler.hpp"

namespace flapi {
namespace test {

class TestFixture {
protected:
    std::shared_ptr<ConfigManager> config_manager;
    std::shared_ptr<DatabaseManager> db_manager;
    std::shared_ptr<RequestHandler> request_handler;
    std::shared_ptr<ConfigService> config_service;
    std::filesystem::path temp_dir;
    std::filesystem::path config_path;
    std::filesystem::path templates_dir;

    void SetUp() {
        // Create temporary directory structure
        temp_dir = std::filesystem::temp_directory_path() / "flapi_config_test";
        templates_dir = temp_dir / "templates";
        config_path = temp_dir / "config.yaml";
        
        std::filesystem::create_directories(templates_dir);

        // Create a basic config file with corrected YAML
        std::ofstream config_file(config_path);
        config_file << R"(
project_name: TestProject
project_description: Test Description
server_name: test_server

template:
  path: )" << templates_dir.string() << R"(
  environment-whitelist:
    - '^FLAPI_.*'

connections:
  default:
    init: "CREATE TABLE IF NOT EXISTS test_table (id INTEGER, value TEXT);"

duckdb:
  db_path: ":memory:"
)";
        config_file.close();

        // Initialize components
        config_manager = std::make_shared<ConfigManager>(config_path);
        config_manager->loadConfig();
        
        db_manager = DatabaseManager::getInstance();
        db_manager->initializeDBManagerFromConfig(config_manager);
        
        request_handler = std::make_shared<RequestHandler>(db_manager, config_manager);
        config_service = std::make_shared<ConfigService>(config_manager);
    }

    void TearDown() {
        std::filesystem::remove_all(temp_dir);
    }

    // Helper method to create a mock request
    crow::request createMockRequest(const std::string& method = "GET", 
                                  const std::string& body = "",
                                  const std::string& url = "/") {
        crow::request req;
        if (method == "GET") req.method = crow::HTTPMethod::Get;
        else if (method == "POST") req.method = crow::HTTPMethod::Post;
        else if (method == "PUT") req.method = crow::HTTPMethod::Put;
        else if (method == "DELETE") req.method = crow::HTTPMethod::Delete;
        req.body = body;
        req.url = url;
        return req;
    }

public:
    TestFixture() { SetUp(); }
    ~TestFixture() { TearDown(); }
};

crow::json::rvalue w2r(const crow::json::wvalue& wval) {
    return crow::json::load(wval.dump());
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Get project configuration", "[config_service]") {
    auto req = createMockRequest("GET");
    auto response = config_service->getProjectConfig(req);
    
    // Add debug information
    if (response.code != 200) {
        CROW_LOG_ERROR << "Failed to get project config. Response code: " << response.code;
        CROW_LOG_ERROR << "Response body: " << response.body;
    }
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["name"] == "TestProject");
    REQUIRE(json["description"] == "Test Description");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: List endpoints", "[config_service]") {
    // Add a test endpoint
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = (temp_dir / "test.sql").string();
    endpoint.method = "GET";
    
    // Initialize all boolean fields to prevent uninitialized values
    endpoint.auth.enabled = false;
    endpoint.auth.type = "";
    endpoint.cache.enabled = false;
    endpoint.cache.cacheSource = "";
    endpoint.cache.cacheTableName = "";
    endpoint.cache.refreshTime = "";
    endpoint.rate_limit.enabled = false;
    endpoint.rate_limit.max = 0;
    endpoint.rate_limit.interval = 0;
    endpoint.heartbeat.enabled = false;
    endpoint.request_fields_validation = false;
    endpoint.with_pagination = false;
    
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("GET");
    auto response = config_service->listEndpoints(req);
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["/test"]["urlPath"].s() == "/test");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Create endpoint", "[config_service]") {
    crow::json::wvalue endpoint_json;
    endpoint_json["url-path"] = "/new-endpoint";
    endpoint_json["template-source"] = "new_template.sql";
    
    auto req = createMockRequest("POST", endpoint_json.dump());
    auto response = config_service->createEndpoint(req);
    
    REQUIRE(response.code == 201);
    
    // Verify endpoint was created
    const auto* endpoint = config_manager->getEndpointForPath("/new-endpoint");
    REQUIRE(endpoint != nullptr);
    REQUIRE(endpoint->urlPath == "/new-endpoint");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Create endpoint accepts snake_case", "[config_service]") {
    crow::json::wvalue endpoint_json;
    endpoint_json["url_path"] = "/snake-endpoint";
    endpoint_json["template_source"] = "snake_template.sql";

    auto req = createMockRequest("POST", endpoint_json.dump());
    auto response = config_service->createEndpoint(req);

    REQUIRE(response.code == 201);
    const auto* endpoint = config_manager->getEndpointForPath("/snake-endpoint");
    REQUIRE(endpoint != nullptr);
    REQUIRE(endpoint->templateSource == "snake_template.sql");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Create endpoint accepts camelCase", "[config_service]") {
    crow::json::wvalue endpoint_json;
    endpoint_json["urlPath"] = "/camel-endpoint";
    endpoint_json["templateSource"] = "camel_template.sql";

    auto req = createMockRequest("POST", endpoint_json.dump());
    auto response = config_service->createEndpoint(req);

    REQUIRE(response.code == 201);
    const auto* endpoint = config_manager->getEndpointForPath("/camel-endpoint");
    REQUIRE(endpoint != nullptr);
    REQUIRE(endpoint->templateSource == "camel_template.sql");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Get endpoint configuration", "[config_service]") {
    // Add a test endpoint
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = (temp_dir / "test.sql").string();
    endpoint.method = "GET";
    
    // Initialize all boolean fields to prevent uninitialized values
    endpoint.auth.enabled = false;
    endpoint.auth.type = "";
    endpoint.cache.enabled = false;
    endpoint.cache.cacheSource = "";
    endpoint.cache.cacheTableName = "";
    endpoint.cache.refreshTime = "";
    endpoint.rate_limit.enabled = false;
    endpoint.rate_limit.max = 0;
    endpoint.rate_limit.interval = 0;
    endpoint.heartbeat.enabled = false;
    endpoint.request_fields_validation = false;
    endpoint.with_pagination = false;
    
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("GET");
    auto response = config_service->getEndpointConfig(req, "/test");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["url-path"].s() == "/test");
    REQUIRE(json["template-source"].s() == (temp_dir / "test.sql").string());
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Get non-existent endpoint", "[config_service]") {
    auto req = createMockRequest("GET");
    auto response = config_service->getEndpointConfig(req, "/non-existent");
    
    REQUIRE(response.code == 404);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Invalid JSON handling", "[config_service]") {
    auto req = createMockRequest("POST", "invalid json");
    auto response = config_service->createEndpoint(req);
    
    REQUIRE(response.code == 400);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: JSON conversion", "[config_service]") {
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    endpoint.method = "GET";
    
    // Initialize all boolean fields to prevent uninitialized values
    endpoint.auth.enabled = false;
    endpoint.auth.type = "";
    endpoint.cache.enabled = false;
    endpoint.cache.cacheSource = "";
    endpoint.cache.cacheTableName = "";
    endpoint.cache.refreshTime = "";
    endpoint.rate_limit.enabled = false;
    endpoint.rate_limit.max = 0;
    endpoint.rate_limit.interval = 0;
    endpoint.heartbeat.enabled = false;
    endpoint.request_fields_validation = false;
    endpoint.with_pagination = false;
    
    RequestFieldConfig field;
    field.fieldName = "id";
    field.fieldIn = "query";
    field.description = "Test ID";
    field.required = true;
    endpoint.request_fields.push_back(field);

    auto json = w2r(config_service->endpointConfigToJson(endpoint));
    
    REQUIRE(json["url-path"] == "/test");
    REQUIRE(json["template-source"] == "test.sql");
    REQUIRE(json["request"][0]["field-name"] == "id");
    REQUIRE(json["request"][0]["field-in"] == "query");
    REQUIRE(json["request"][0]["description"] == "Test ID");
    REQUIRE(json["request"][0]["required"].b() == true);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Route registration", "[config_service]") {
    FlapiApp app;
    REQUIRE_NOTHROW(config_service->registerRoutes(app));
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Update endpoint configuration", "[config_service]") {
    // First create an endpoint
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    config_manager->addEndpoint(endpoint);

    // Prepare update JSON
    crow::json::wvalue update_json;
    update_json["url-path"] = "/test";
    update_json["template-source"] = (temp_dir / "updated_test.sql").string();
    
    auto req = createMockRequest("PUT", update_json.dump());
    auto response = config_service->updateEndpointConfig(req, "/test");
    
    REQUIRE(response.code == 200);
    
    // Verify endpoint was updated
    const auto* updated = config_manager->getEndpointForPath("/test");
    REQUIRE(updated != nullptr);
    REQUIRE(updated->templateSource == (temp_dir / "updated_test.sql").string());
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Delete endpoint", "[config_service]") {
    // First create an endpoint
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("DELETE");
    auto response = config_service->deleteEndpoint(req, "/test");
    
    REQUIRE(response.code == 200);
    
    // Verify endpoint was deleted
    const auto* deleted = config_manager->getEndpointForPath("/test");
    REQUIRE(deleted == nullptr);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Update non-existent endpoint", "[config_service]") {
    crow::json::wvalue update_json;
    update_json["url-path"] = "/non-existent";
    update_json["template-source"] = "test.sql";
    
    auto req = createMockRequest("PUT", update_json.dump());
    auto response = config_service->updateEndpointConfig(req, "/non-existent");
    
    REQUIRE(response.code == 404);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Delete non-existent endpoint", "[config_service]") {
    auto req = createMockRequest("DELETE");
    auto response = config_service->deleteEndpoint(req, "/non-existent");
    
    REQUIRE(response.code == 404);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Get endpoint template", "[config_service]") {
    // Create a test template file
    std::filesystem::path template_path = temp_dir / "test_template.sql";
    std::string template_content = "SELECT * FROM {{table}} WHERE id = {{id}}";
    {
        std::ofstream file(template_path);
        file << template_content;
    }

    // Create an endpoint using this template
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = template_path.string();
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("GET");
    auto response = config_service->getEndpointTemplate(req, "/test");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["template"].s() == template_content);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Update endpoint template", "[config_service]") {
    // Create a test template file
    std::filesystem::path template_path = temp_dir / "test_template.sql";
    {
        std::ofstream file(template_path);
        file << "initial content";
    }

    // Create an endpoint using this template
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = template_path.string();
    config_manager->addEndpoint(endpoint);

    // Update the template
    std::string new_content = "SELECT * FROM {{table}} WHERE id = {{id}}";
    crow::json::wvalue update_json;
    update_json["template"] = new_content;
    
    auto req = createMockRequest("PUT", update_json.dump());
    auto response = config_service->updateEndpointTemplate(req, "/test");
    
    REQUIRE(response.code == 200);
    
    // Verify the file was updated
    std::ifstream file(template_path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    REQUIRE(buffer.str() == new_content);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Get template for non-existent endpoint", "[config_service]") {
    auto req = createMockRequest("GET");
    auto response = config_service->getEndpointTemplate(req, "/non-existent");
    
    REQUIRE(response.code == 404);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Update template with invalid JSON", "[config_service]") {
    // Create endpoint first
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = (temp_dir / "test.sql").string();
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("PUT", "{\"wrong_field\": \"value\"}");
    auto response = config_service->updateEndpointTemplate(req, "/test");
    
    REQUIRE(response.code == 400);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Expand template", "[config_service]") {
    // Create a test template file
    std::filesystem::path template_path = temp_dir / "test_template.sql";
    std::string template_content = "SELECT * FROM {{params.table}} WHERE id = {{params.id}}";
    {
        std::ofstream file(template_path);
        file << template_content;
    }

    // Create an endpoint using this template
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = template_path.string();
    endpoint.connection.push_back("default");
    config_manager->addEndpoint(endpoint);

    // Create parameters for expansion
    crow::json::wvalue params_json;
    params_json["parameters"]["table"] = "users";
    params_json["parameters"]["id"] = "123";
    
    auto req = createMockRequest("POST", params_json.dump());
    auto response = config_service->expandTemplate(req, "/test");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["expanded"].s() == "SELECT * FROM users WHERE id = 123");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Test template", "[config_service]") {
    // First ensure the test table exists, and necessary data is inserted
    db_manager->executeInitStatement("CREATE TABLE IF NOT EXISTS test_table (id INTEGER, value TEXT)");
    db_manager->executeInitStatement("INSERT INTO test_table VALUES (1, 'test')");

    // Create a test template file with a valid SQL query
    std::filesystem::path template_path = temp_dir / "test_template.sql";
    std::string template_content = "SELECT value FROM test_table WHERE id = {{params.id}}";
    {
        std::ofstream file(template_path);
        file << template_content;
    }

    // Create an endpoint using this template
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = template_path.string();
    endpoint.connection.push_back("default");
    config_manager->addEndpoint(endpoint);

    // Create parameters for testing
    crow::json::wvalue params_json;
    params_json["parameters"]["id"] = "1";
    
    auto req = createMockRequest("POST", params_json.dump());
    auto response = config_service->testTemplate(req, "/test");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    std::cout << "JSON: ************** " << std::endl << json << std::endl;
    REQUIRE(json);
    REQUIRE(json["success"].b() == true);
    REQUIRE(json["columns"].size() == 1);
    REQUIRE(json["columns"][0].s() == "value");
    REQUIRE(json["rows"].size() == 1);
    REQUIRE(json["rows"][0]["value"].s() == "test");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Expand template with invalid parameters", "[config_service]") {
    // Create a test template file
    std::filesystem::path template_path = temp_dir / "test_template.sql";
    std::string template_content = "SELECT * FROM {{table}}";
    {
        std::ofstream file(template_path);
        file << template_content;
    }

    // Create an endpoint using this template
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = template_path.string();
    config_manager->addEndpoint(endpoint);

    // Create request without parameters
    auto req = createMockRequest("POST", "{}");
    auto response = config_service->expandTemplate(req, "/test");
    
    REQUIRE(response.code == 400);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Test template with invalid SQL", "[config_service]") {
    // Create a test template file with invalid SQL
    std::filesystem::path template_path = temp_dir / "test_template.sql";
    std::string template_content = "SELECT * FROM {{table}} INVALID SQL";
    {
        std::ofstream file(template_path);
        file << template_content;
    }

    // Create an endpoint using this template
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = template_path.string();
    endpoint.connection.push_back("default");
    config_manager->addEndpoint(endpoint);

    // Create parameters for testing
    crow::json::wvalue params;
    params["parameters"]["table"] = "users";
    
    auto req = createMockRequest("POST", params.dump());
    auto response = config_service->testTemplate(req, "/test");
    
    REQUIRE(response.code == 400);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Get cache config when disabled", "[config_service]") {
    // Create an endpoint without cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("GET");
    auto response = config_service->getCacheConfig(req, "/test");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["enabled"].b() == false);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Get cache config when enabled", "[config_service]") {
    // Create an endpoint with cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    
    endpoint.cache.enabled = true;
    endpoint.cache.refreshTime = "1h";
    endpoint.cache.cacheSource = "cache.sql";
    endpoint.cache.cacheTableName = "test_cache";
    
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("GET");
    auto response = config_service->getCacheConfig(req, "/test");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["enabled"].b() == true);
    REQUIRE(json["refresh-time"].s() == "1h");
    REQUIRE(json["cache-source"].s() == "cache.sql");
    REQUIRE(json["cache-schema"].s() == "flapi");
    REQUIRE(json["cache-table"].s() == "test_cache");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Update cache config to enable", "[config_service]") {
    // Create an endpoint without cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    config_manager->addEndpoint(endpoint);

    // Create update request
    crow::json::wvalue update_json;
    update_json["enabled"] = true;
    update_json["refresh-time"] = "1h";
    update_json["cache-source"] = "cache.sql";
    update_json["cache-schema"] = "cache";
    update_json["cache-table"] = "test_cache";
    
    auto req = createMockRequest("PUT", update_json.dump());
    auto response = config_service->updateCacheConfig(req, "/test");
    
    REQUIRE(response.code == 200);
    
    // Verify cache was enabled
    const auto* updated = config_manager->getEndpointForPath("/test");
    REQUIRE(updated != nullptr);
    REQUIRE(updated->cache.enabled == true);
    REQUIRE(updated->cache.refreshTime == "1h");
    REQUIRE(updated->cache.cacheSource == "cache.sql");
    REQUIRE(updated->cache.cacheTableName == "test_cache");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Update cache config to disable", "[config_service]") {
    // Create an endpoint with cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    endpoint.cache.enabled = true;
    endpoint.cache.refreshTime = "1h";
    endpoint.cache.cacheSource = "cache.sql";
    endpoint.cache.cacheTableName = "test_cache";
    config_manager->addEndpoint(endpoint);

    // Create update request
    crow::json::wvalue update_json;
    update_json["enabled"] = false;
    
    auto req = createMockRequest("PUT", update_json.dump());
    auto response = config_service->updateCacheConfig(req, "/test");
    
    REQUIRE(response.code == 200);
    
    // Verify cache was disabled
    const auto* updated = config_manager->getEndpointForPath("/test");
    REQUIRE(updated != nullptr);
    REQUIRE(updated->cache.enabled == false);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Update cache config with invalid refresh time", "[config_service]") {
    // Create an endpoint
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    config_manager->addEndpoint(endpoint);

    // Create update request with invalid refresh time
    crow::json::wvalue update_json;
    update_json["enabled"] = true;
    update_json["refresh-time"] = "invalid";
    update_json["cache-source"] = "cache.sql";
    update_json["cache-schema"] = "cache";
    update_json["cache-table"] = "test_cache";
    
    auto req = createMockRequest("PUT", update_json.dump());
    auto response = config_service->updateCacheConfig(req, "/test");
    
    REQUIRE(response.code == 400);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Get cache template when cache disabled", "[config_service]") {
    // Create an endpoint without cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("GET");
    auto response = config_service->getCacheTemplate(req, "/test");
    
    REQUIRE(response.code == 400);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Get cache template when enabled", "[config_service]") {
    // Create a cache template file
    std::filesystem::path cache_template_path = temp_dir / "cache_template.sql";
    std::string template_content = "SELECT * FROM source_table";
    {
        std::ofstream file(cache_template_path);
        file << template_content;
    }

    // Create an endpoint with cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    endpoint.cache.enabled = true;
    endpoint.cache.cacheSource = cache_template_path.string();
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("GET");
    auto response = config_service->getCacheTemplate(req, "/test");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["template"].s() == template_content);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Update cache template when cache disabled", "[config_service]") {
    // Create an endpoint without cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    config_manager->addEndpoint(endpoint);

    crow::json::wvalue update_json;
    update_json["template"] = "SELECT * FROM source_table";
    
    auto req = createMockRequest("PUT", update_json.dump());
    auto response = config_service->updateCacheTemplate(req, "/test");
    
    REQUIRE(response.code == 400);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Update cache template when enabled", "[config_service]") {
    // Create a cache template file
    std::filesystem::path cache_template_path = temp_dir / "cache_template.sql";
    {
        std::ofstream file(cache_template_path);
        file << "initial content";
    }

    // Create an endpoint with cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    endpoint.cache.enabled = true;
    endpoint.cache.cacheSource = cache_template_path.string();
    config_manager->addEndpoint(endpoint);

    // Update the template
    std::string new_content = "SELECT * FROM source_table";
    crow::json::wvalue update_json;
    update_json["template"] = new_content;
    
    auto req = createMockRequest("PUT", update_json.dump());
    auto response = config_service->updateCacheTemplate(req, "/test");
    
    REQUIRE(response.code == 200);
    
    // Verify the file was updated
    std::ifstream file(cache_template_path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    REQUIRE(buffer.str() == new_content);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Refresh cache when cache disabled", "[config_service]") {
    // Create an endpoint without cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("POST");
    auto response = config_service->refreshCache(req, "/test");
    
    REQUIRE(response.code == 400);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Refresh cache with invalid template", "[config_service]") {
    // Create a cache template file with invalid SQL
    std::filesystem::path cache_template_path = temp_dir / "cache_template.sql";
    std::string template_content = "INVALID SQL";
    {
        std::ofstream file(cache_template_path);
        file << template_content;
    }

    // Create an endpoint with cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    endpoint.cache.enabled = true;
    endpoint.cache.refreshTime = "1h";
    endpoint.cache.cacheSource = cache_template_path.string();
    endpoint.cache.cacheTableName = "test_cache";
    endpoint.connection.push_back("default");
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("POST");
    auto response = config_service->refreshCache(req, "/test");
    
    REQUIRE(response.code == 400);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Get schema", "[config_service]") {
    // First create some test tables in the my_schema schema
    db_manager->executeInitStatement("CREATE SCHEMA IF NOT EXISTS my_schema;");
    db_manager->executeInitStatement("SET search_path = 'my_schema';");
    db_manager->executeInitStatement("DROP TABLE IF EXISTS test_table1;");
    db_manager->executeInitStatement("DROP TABLE IF EXISTS test_table2;");
    db_manager->executeInitStatement("DROP VIEW IF EXISTS test_view1;");
    db_manager->executeInitStatement("CREATE TABLE test_table1 (id INTEGER, name TEXT);");
    db_manager->executeInitStatement("CREATE TABLE test_table2 (value DOUBLE, timestamp TIMESTAMP);");
    db_manager->executeInitStatement("CREATE VIEW test_view1 AS SELECT * FROM test_table1;");

    auto req = createMockRequest("GET");
    auto response = config_service->getSchema(req);
    
    SKIP(); // TODO: Fix this test

    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);

    // JSON structure is:
    // {
    //   "my_schema": {
    //     "tables": null
    //   },
    //   "main": {
    //     "tables": {
    //       "test_table2": {
    //         "columns": {
    //           "value": {
    //             "nullable": true,
    //             "type": "DOUBLE"
    //           },
    //           "timestamp": {
    //             "nullable": true,
    //             "type": "TIMESTAMP"
    //           }
    //         },
    //         "is_view": false
    //       },
    //       "test_view1": {
    //         "columns": {
    //           "id": {
    //             "nullable": true,
    //             "type": "INTEGER"
    //           },
    //           "name": {
    //             "nullable": true,
    //             "type": "VARCHAR"
    //           }
    //         },
    //         "is_view": true
    //       },
    //       "test_table1": {
    //         "columns": {
    //           "name": {
    //             "nullable": true,
    //             "type": "VARCHAR"
    //           },
    //           "id": {
    //             "nullable": true,
    //             "type": "INTEGER"
    //           }
    //         },
    //         "is_view": false
    //       },
    //       "test_table": {
    //         "columns": {
    //           "value": {
    //             "nullable": true,
    //             "type": "VARCHAR"
    //           },
    //           "id": {
    //             "nullable": true,
    //             "type": "INTEGER"
    //           }
    //         },
    //         "is_view": false
    //       }
    //     }
    //   }
    // }
    

    REQUIRE(json);
    REQUIRE(json.has("my_schema"));

    const auto& my_schema = json["my_schema"];
    REQUIRE(my_schema.has("tables"));
    REQUIRE(my_schema["tables"].t() == crow::json::type::Null);

    const auto & main_schema = json["main"];
    REQUIRE(main_schema.has("tables"));
    REQUIRE(main_schema["tables"].t() != crow::json::type::Null);

    const auto& test_table2 = main_schema["tables"]["test_table2"];
    REQUIRE(test_table2.has("columns"));
    REQUIRE(test_table2["columns"].has("value"));
    REQUIRE(test_table2["columns"]["value"].has("nullable"));
    REQUIRE(test_table2["columns"]["value"]["nullable"].b() == true);
    REQUIRE(test_table2["columns"]["value"].has("type"));
    REQUIRE(test_table2["columns"]["value"]["type"].s() == "DOUBLE");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Refresh schema", "[config_service]") {
    auto req = createMockRequest("POST");
    auto response = config_service->refreshSchema(req);
    
    REQUIRE(response.code == 200);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Refresh schema with new tables", "[config_service]") {
    // First get current schema
    db_manager->executeInitStatement("CREATE SCHEMA IF NOT EXISTS my_schema;");
    db_manager->executeInitStatement("SET search_path = 'my_schema';");
    
    auto initial_req = createMockRequest("GET");
    auto initial_response = config_service->getSchema(initial_req);
    auto initial_json = crow::json::load(initial_response.body);

    // Create a new table
    db_manager->executeInitStatement("DROP TABLE IF EXISTS my_schema.refresh_test_table;");
    db_manager->executeInitStatement("CREATE TABLE my_schema.refresh_test_table (id INTEGER);");

    // Refresh schema
    auto refresh_req = createMockRequest("POST");
    auto refresh_response = config_service->refreshSchema(refresh_req);
    REQUIRE(refresh_response.code == 200);

    // Get updated schema
    auto final_req = createMockRequest("GET");
    auto final_response = config_service->getSchema(final_req);
    auto final_json = crow::json::load(final_response.body);

    std::cout << "#### Initial JSON: " << initial_json << std::endl;
    std::cout << "#### Final JSON: " << final_json << std::endl;

    SKIP(); // TODO: Fix this test

    // Verify the new table exists in my_schema
    REQUIRE(final_json.has("my_schema"));
    REQUIRE(final_json["my_schema"].has("tables"));
    REQUIRE(final_json["my_schema"]["tables"].has("refresh_test_table"));
    
    // Verify the table structure
    const auto& table = final_json["my_schema"]["tables"]["refresh_test_table"];
    REQUIRE(table.has("columns"));
    REQUIRE(table["columns"].has("id"));
    REQUIRE(table["columns"]["id"]["type"].s() == "INTEGER");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Error handling - Invalid JSON", "[config_service][errors]") {
    // Test with invalid JSON in various endpoints
    auto req = createMockRequest("PUT", "invalid json");
    
    auto response1 = config_service->updateEndpointConfig(req, "/test");
    REQUIRE(response1.code == 400);
    REQUIRE(response1.body.find("Invalid JSON") != std::string::npos);
    
    auto response2 = config_service->updateEndpointTemplate(req, "/test");
    REQUIRE(response2.code == 400);
    REQUIRE(response2.body.find("Invalid JSON") != std::string::npos);
    
    auto response3 = config_service->updateCacheConfig(req, "/test");
    REQUIRE(response3.code == 400);
    REQUIRE(response3.body.find("Invalid JSON") != std::string::npos);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Error handling - File operations", "[config_service][errors]") {
    // Create an endpoint with non-existent template file
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "/nonexistent/path/template.sql";
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("GET");
    auto response = config_service->getEndpointTemplate(req, "/test");
    
    REQUIRE(response.code == 500);
    REQUIRE(response.body.find("Could not open template file") != std::string::npos);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Cache management - File sync", "[config_service][cache]") {
    // Create temporary files for testing
    auto cache_dir = temp_dir / "cache";
    std::filesystem::create_directories(cache_dir);
    auto cache_template = cache_dir / "cache.sql";
    
    // Create endpoint with cache
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = (temp_dir / "template.sql").string();
    endpoint.cache.enabled = true;
    endpoint.cache.cacheSource = cache_template.string();
    endpoint.cache.refreshTime = "1h";
    endpoint.cache.cacheTableName = "test_cache";
    config_manager->addEndpoint(endpoint);

    // Write initial content
    {
        std::ofstream file(cache_template);
        file << "SELECT 1";
    }

    // Update cache template
    crow::json::wvalue update;
    update["template"] = "SELECT 2";
    auto req = createMockRequest("PUT", update.dump());
    auto response = config_service->updateCacheTemplate(req, "/test");
    
    REQUIRE(response.code == 200);

    // Verify file was updated
    std::ifstream file(cache_template);
    std::string content;
    std::getline(file, content);
    REQUIRE(content == "SELECT 2");
}

TEST_CASE_METHOD(TestFixture, "ConfigService: File sync - Template updates", "[config_service][files]") {
    // Create a template file
    auto template_path = temp_dir / "test.sql";
    {
        std::ofstream file(template_path);
        file << "SELECT * FROM test";
    }

    // Create endpoint
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = template_path.string();
    config_manager->addEndpoint(endpoint);

    // Update template
    crow::json::wvalue update;
    update["template"] = "SELECT id FROM test";
    auto req = createMockRequest("PUT", update.dump());
    auto response = config_service->updateEndpointTemplate(req, "/test");
    
    REQUIRE(response.code == 200);

    // Verify file content
    std::ifstream file(template_path);
    std::string content;
    std::getline(file, content);
    REQUIRE(content == "SELECT id FROM test");

    // Verify file permissions
    auto perms = std::filesystem::status(template_path).permissions();
    REQUIRE((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none);
    REQUIRE((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Update endpoint template resolves relative path", "[config_service]") {
    // Create a template file in the templates directory
    std::filesystem::path template_path = templates_dir / "relative_template.sql";
    {
        std::ofstream file(template_path);
        file << "initial content";
    }

    // Create an endpoint using a relative template path
    EndpointConfig endpoint;
    endpoint.urlPath = "/relative";
    endpoint.templateSource = "relative_template.sql";
    config_manager->addEndpoint(endpoint);

    // Update the template
    const std::string new_content = "SELECT 1";
    crow::json::wvalue update_json;
    update_json["template"] = new_content;

    auto req = createMockRequest("PUT", update_json.dump());
    auto response = config_service->updateEndpointTemplate(req, "/relative");

    REQUIRE(response.code == 200);

    std::ifstream file(template_path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    REQUIRE(buffer.str() == new_content);
}

TEST_CASE_METHOD(TestFixture, "ConfigService: Expand template without connection", "[config_service]") {
    // Create a test template file
    std::filesystem::path template_path = temp_dir / "test_template.sql";
    {
        std::ofstream file(template_path);
        file << "SELECT * FROM {{params.table}}";
    }

    // Create an endpoint without connection configuration
    EndpointConfig endpoint;
    endpoint.urlPath = "/test-no-conn";
    endpoint.templateSource = template_path.string();
    config_manager->addEndpoint(endpoint);

    crow::json::wvalue params_json;
    params_json["parameters"]["table"] = "users";

    auto req = createMockRequest("POST", params_json.dump());
    auto response = config_service->expandTemplate(req, "/test-no-conn");

    REQUIRE(response.code == 200);
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["expanded"].s() == "SELECT * FROM users");
}

} // namespace test
} // namespace flapi 