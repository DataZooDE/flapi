#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

#include "config_service.hpp"
#include "api_server.hpp"
#include "request_handler.hpp"
#include "database_manager.hpp"

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

        // Create a basic config file
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
    init: "SELECT 1;"
    properties:
      db_file: ./data/test.db

duckdb:
  db_path: ":memory:"
  access_mode: READ_WRITE
  threads: 4
  max_memory: "2GB"
  default_order: DESC

heartbeat:
  enabled: false
  worker-interval: 10

enforce-https:
  enabled: false
)";
        config_file.close();

        // Initialize components
        config_manager = std::make_shared<ConfigManager>(config_path);
        try {
            config_manager->loadConfig();
            CROW_LOG_DEBUG << "Config loaded successfully in test setup";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "Failed to load config in test setup: " << e.what();
            throw;
        }
        
        db_manager = DatabaseManager::getInstance();
        db_manager->initializeDBManagerFromConfig(config_manager);
        
        request_handler = std::make_shared<RequestHandler>(db_manager, config_manager);
        config_service = std::make_shared<ConfigService>(config_manager, request_handler);
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
    endpoint.templateSource = "test.sql";
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

TEST_CASE_METHOD(TestFixture, "ConfigService: Get endpoint configuration", "[config_service]") {
    // Add a test endpoint
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.templateSource = "test.sql";
    config_manager->addEndpoint(endpoint);

    auto req = createMockRequest("GET");
    auto response = config_service->getEndpointConfig(req, "/test");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["url-path"].s() == "/test");
    REQUIRE(json["template-source"].s() == "test.sql");
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
    
    RequestFieldConfig field;
    field.fieldName = "id";
    field.fieldIn = "query";
    field.description = "Test ID";
    field.required = true;
    endpoint.requestFields.push_back(field);

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

} // namespace test
} // namespace flapi 