#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "config_service.hpp"
#include "config_manager.hpp"
#include <filesystem>
#include <fstream>

using namespace flapi;

namespace {

/**
 * Helper to create a test configuration with parameters
 */
std::pair<std::filesystem::path, std::filesystem::path> createTestConfigWithParameters() {
    auto temp_dir = std::filesystem::temp_directory_path() / "config_service_params_test";
    std::filesystem::create_directories(temp_dir);

    // Create main flapi.yaml
    auto config_path = temp_dir / "flapi.yaml";
    std::ofstream config_file(config_path);
    config_file << "project-name: test_params\n";
    config_file << "project-description: Test for parameters endpoint\n";
    config_file << "template:\n";
    config_file << "  path: " << temp_dir << "\n";
    config_file << "  environment-whitelist:\n";
    config_file << "    - TEST_VAR_1\n";
    config_file << "    - TEST_VAR_2\n";
    config_file << "    - UNSET_VAR\n";
    config_file.close();

    // Create endpoint YAML with request fields
    auto endpoint_path = temp_dir / "test_endpoint.yaml";
    std::ofstream endpoint_file(endpoint_path);
    endpoint_file << "url-path: /api/test\n";
    endpoint_file << "method: GET\n";
    endpoint_file << "template-source: test.sql\n";
    endpoint_file << "request:\n";
    endpoint_file << "  - field-name: user_id\n";
    endpoint_file << "    field-in: query\n";
    endpoint_file << "    description: User identifier\n";
    endpoint_file << "    required: true\n";
    endpoint_file << "    default: 123\n";
    endpoint_file << "    validators:\n";
    endpoint_file << "      - type: int\n";
    endpoint_file << "        min: 1\n";
    endpoint_file << "        max: 999999\n";
    endpoint_file << "  - field-name: status\n";
    endpoint_file << "    field-in: query\n";
    endpoint_file << "    description: Filter by status\n";
    endpoint_file << "    required: false\n";
    endpoint_file << "    default: active\n";
    endpoint_file << "    validators:\n";
    endpoint_file << "      - type: enum\n";
    endpoint_file << "        allowedValues: [active, inactive, pending]\n";
    endpoint_file << "  - field-name: email\n";
    endpoint_file << "    field-in: query\n";
    endpoint_file << "    description: Email filter\n";
    endpoint_file << "    required: false\n";
    endpoint_file << "    validators:\n";
    endpoint_file << "      - type: string\n";
    endpoint_file << "        regex: ^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}$\n";
    endpoint_file.close();

    // Create template file
    auto template_path = temp_dir / "test.sql";
    std::ofstream template_file(template_path);
    template_file << "SELECT * FROM users WHERE id = {{params.user_id}}";
    template_file.close();

    return {config_path, endpoint_path};
}

} // namespace

TEST_CASE("ConfigService: Get endpoint parameters with validators", "[config_service][parameters]") {
    auto [config_path, endpoint_path] = createTestConfigWithParameters();

    auto config_mgr = std::make_shared<ConfigManager>(config_path);
    config_mgr->loadConfig();
    config_mgr->loadEndpointConfig(endpoint_path);

    EndpointConfigHandler handler(config_mgr);
    
    crow::request req;
    auto response = handler.getEndpointParameters(req, "/api/test");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    
    // Check endpoint metadata
    REQUIRE(json["endpoint"].s() == "/api/test");
    REQUIRE(json["method"].s() == "GET");
    
    // Check parameters array
    REQUIRE(json["parameters"]);
    auto params = json["parameters"];
    REQUIRE(params.size() == 3);
    
    // Verify user_id parameter (int validator)
    auto user_id_param = params[0];
    REQUIRE(user_id_param["name"].s() == "user_id");
    REQUIRE(user_id_param["in"].s() == "query");
    REQUIRE(user_id_param["description"].s() == "User identifier");
    REQUIRE(user_id_param["required"].b() == true);
    REQUIRE(user_id_param["default"].s() == "123");
    
    // Check int validator
    auto validators = user_id_param["validators"];
    REQUIRE(validators.size() == 1);
    REQUIRE(validators[0]["type"].s() == "int");
    REQUIRE(validators[0]["min"].i() == 1);
    REQUIRE(validators[0]["max"].i() == 999999);
    
    // Verify status parameter (enum validator)
    auto status_param = params[1];
    REQUIRE(status_param["name"].s() == "status");
    REQUIRE(status_param["required"].b() == false);
    REQUIRE(status_param["default"].s() == "active");
    
    auto status_validators = status_param["validators"];
    REQUIRE(status_validators.size() == 1);
    REQUIRE(status_validators[0]["type"].s() == "enum");
    
    auto allowed_values = status_validators[0]["allowedValues"];
    REQUIRE(allowed_values.size() == 3);
    REQUIRE(allowed_values[0].s() == "active");
    REQUIRE(allowed_values[1].s() == "inactive");
    REQUIRE(allowed_values[2].s() == "pending");
    
    // Verify email parameter (string validator with regex)
    auto email_param = params[2];
    REQUIRE(email_param["name"].s() == "email");
    REQUIRE(email_param["required"].b() == false);
    
    auto email_validators = email_param["validators"];
    REQUIRE(email_validators.size() == 1);
    REQUIRE(email_validators[0]["type"].s() == "string");
    
    std::string regex_str = email_validators[0]["regex"].s();
    REQUIRE(regex_str.find("@") != std::string::npos);
}

TEST_CASE("ConfigService: Get parameters for non-existent endpoint", "[config_service][parameters]") {
    auto [config_path, endpoint_path] = createTestConfigWithParameters();

    auto config_mgr = std::make_shared<ConfigManager>(config_path);
    config_mgr->loadConfig();

    EndpointConfigHandler handler(config_mgr);
    
    crow::request req;
    auto response = handler.getEndpointParameters(req, "/nonexistent");
    
    REQUIRE(response.code == 404);
    REQUIRE(response.body == "Endpoint not found");
}

TEST_CASE("ConfigService: Get parameters for endpoint without request fields", "[config_service][parameters]") {
    auto temp_dir = std::filesystem::temp_directory_path() / "config_service_no_params_test";
    std::filesystem::create_directories(temp_dir);

    // Create main config
    auto config_path = temp_dir / "flapi.yaml";
    std::ofstream config_file(config_path);
    config_file << "project-name: test_no_params\n";
    config_file << "project-description: Test without parameters\n";
    config_file << "template:\n  path: " << temp_dir << "\n";
    config_file.close();

    // Create endpoint without request fields
    auto endpoint_path = temp_dir / "simple_endpoint.yaml";
    std::ofstream endpoint_file(endpoint_path);
    endpoint_file << "url-path: /api/simple\n";
    endpoint_file << "method: GET\n";
    endpoint_file << "template-source: simple.sql\n";
    endpoint_file.close();

    // Create template
    auto template_path = temp_dir / "simple.sql";
    std::ofstream template_file(template_path);
    template_file << "SELECT 1";
    template_file.close();

    auto config_mgr = std::make_shared<ConfigManager>(config_path);
    config_mgr->loadConfig();
    config_mgr->loadEndpointConfig(endpoint_path);

    EndpointConfigHandler handler(config_mgr);
    
    crow::request req;
    auto response = handler.getEndpointParameters(req, "/api/simple");
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["parameters"].size() == 0);
}

TEST_CASE("ConfigService: Get environment variables", "[config_service][environment]") {
    // Create fresh temp directory for this test
    auto temp_dir = std::filesystem::temp_directory_path() / "config_service_env_vars_test";
    std::filesystem::remove_all(temp_dir); // Clean up any previous run
    std::filesystem::create_directories(temp_dir);

    // Create main flapi.yaml
    auto config_path = temp_dir / "flapi.yaml";
    std::ofstream config_file(config_path);
    config_file << "project-name: test_env_vars\n";
    config_file << "project-description: Test for environment variables\n";
    config_file << "template:\n";
    config_file << "  path: " << temp_dir << "\n";
    config_file << "  environment-whitelist:\n";
    config_file << "    - TEST_VAR_1\n";
    config_file << "    - TEST_VAR_2\n";
    config_file << "    - UNSET_VAR\n";
    config_file.close();

    // Set test environment variables
    setenv("TEST_VAR_1", "value1", 1);
    setenv("TEST_VAR_2", "value2", 1);
    // UNSET_VAR is intentionally not set

    auto config_mgr = std::make_shared<ConfigManager>(config_path);
    config_mgr->loadConfig();

    ProjectConfigHandler handler(config_mgr);
    
    crow::request req;
    auto response = handler.getEnvironmentVariables(req);
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["variables"]);
    
    auto variables = json["variables"];
    REQUIRE(variables.size() == 3);
    
    // Check TEST_VAR_1 (should be available)
    bool found_var1 = false;
    bool found_var2 = false;
    bool found_unset = false;
    
    for (size_t i = 0; i < variables.size(); ++i) {
        auto var = variables[i];
        std::string name = var["name"].s();
        
        if (name == "TEST_VAR_1") {
            found_var1 = true;
            REQUIRE(var["value"].s() == "value1");
            REQUIRE(var["available"].b() == true);
        } else if (name == "TEST_VAR_2") {
            found_var2 = true;
            REQUIRE(var["value"].s() == "value2");
            REQUIRE(var["available"].b() == true);
        } else if (name == "UNSET_VAR") {
            found_unset = true;
            REQUIRE(var["value"].s() == "");
            REQUIRE(var["available"].b() == false);
        }
    }
    
    REQUIRE(found_var1);
    REQUIRE(found_var2);
    REQUIRE(found_unset);
    
    // Clean up
    unsetenv("TEST_VAR_1");
    unsetenv("TEST_VAR_2");
}

TEST_CASE("ConfigService: Get environment variables with empty whitelist", "[config_service][environment]") {
    auto temp_dir = std::filesystem::temp_directory_path() / "config_service_no_env_test";
    std::filesystem::create_directories(temp_dir);

    // Create config without environment whitelist
    auto config_path = temp_dir / "flapi.yaml";
    std::ofstream config_file(config_path);
    config_file << "project-name: test_no_env\n";
    config_file << "project-description: Test without env vars\n";
    config_file << "template:\n  path: " << temp_dir << "\n";
    config_file.close();

    auto config_mgr = std::make_shared<ConfigManager>(config_path);
    config_mgr->loadConfig();

    ProjectConfigHandler handler(config_mgr);
    
    crow::request req;
    auto response = handler.getEnvironmentVariables(req);
    
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["variables"].size() == 0);
}

TEST_CASE("ConfigService: Parameters endpoint handles MCP tools gracefully", "[config_service][parameters][mcp]") {
    auto temp_dir = std::filesystem::temp_directory_path() / "config_service_mcp_params_test";
    std::filesystem::create_directories(temp_dir);

    // Create main config
    auto config_path = temp_dir / "flapi.yaml";
    std::ofstream config_file(config_path);
    config_file << "project-name: test_mcp_params\n";
    config_file << "project-description: Test MCP tool params\n";
    config_file << "template:\n  path: " << temp_dir << "\n";
    config_file.close();

    // Create MCP tool endpoint
    auto endpoint_path = temp_dir / "mcp_tool.yaml";
    std::ofstream endpoint_file(endpoint_path);
    endpoint_file << "mcp-tool:\n";
    endpoint_file << "  name: test_tool\n";
    endpoint_file << "  description: Test tool\n";
    endpoint_file << "template-source: tool.sql\n";
    endpoint_file << "request:\n";
    endpoint_file << "  - field-name: input_data\n";
    endpoint_file << "    field-in: body\n";
    endpoint_file << "    required: true\n";
    endpoint_file.close();

    // Create template
    auto template_path = temp_dir / "tool.sql";
    std::ofstream template_file(template_path);
    template_file << "SELECT {{params.input_data}}";
    template_file.close();

    auto config_mgr = std::make_shared<ConfigManager>(config_path);
    config_mgr->loadConfig();
    config_mgr->loadEndpointConfig(endpoint_path);

    EndpointConfigHandler handler(config_mgr);
    
    crow::request req;
    // MCP tools don't have URL paths, so trying to get parameters by tool name
    // will match against the endpoint's getName() which works for MCP tools
    auto response = handler.getEndpointParameters(req, "test_tool");
    
    // Since the endpoint has request fields, it should return 200
    // (the handler uses getEndpointForPath which now uses matchesPath)
    REQUIRE(response.code == 200);
    
    auto json = crow::json::load(response.body);
    REQUIRE(json);
    REQUIRE(json["parameters"].size() == 1);
    REQUIRE(json["parameters"][0]["name"].s() == "input_data");
}

