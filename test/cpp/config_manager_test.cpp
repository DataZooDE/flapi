#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "config_manager.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>

using namespace flapi;

// Helper function to create a temporary directory
std::string createTempDir() {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "flapi_test_templates";
    std::filesystem::create_directories(temp_dir);
    return temp_dir.string();
}

// Helper function to create a temporary YAML file
std::string createTempYamlFile(const std::string& content, const std::string& filename = "temp_config.yaml") {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path temp_file = temp_dir / filename;
    std::ofstream file(temp_file);
    file << content;
    file.close();
    return temp_file.string();
}

// Helper function to check if a string ends with another string
bool ends_with(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

TEST_CASE("ConfigManager basic functionality", "[config_manager]") {
    
    std::string temp_template_dir = createTempDir();

    SECTION("Load valid configuration") {
        std::string yaml_content = R"(
project_name: TestProject
project_description: Test Description
template:
  path: )" + temp_template_dir + R"(
connections:
  default:
    init: "SELECT 1;"
    properties:
      db_file: ./data/test.db
    log-queries: true
    log-parameters: false
    allow: "*"
enforce-https:
  enabled: true
  ssl-cert-file: ./ssl/cert.pem
  ssl-key-file: ./ssl/key.pem
duckdb:
  db_path: ":memory:"
  max_memory: "2GB"
  threads: "4"
)";

        std::string config_file = createTempYamlFile(yaml_content);
        ConfigManager mgr{std::filesystem::path(config_file)};
        mgr.loadConfig();

        REQUIRE(mgr.getProjectName() == "TestProject");
        REQUIRE(mgr.getProjectDescription() == "Test Description");
        REQUIRE(mgr.isHttpsEnforced() == true);
        REQUIRE(mgr.isAuthEnabled() == false);

        const auto& connections = mgr.getConnections();
        REQUIRE(connections.size() == 1);
        REQUIRE(connections.at("default").init == "SELECT 1;");
        REQUIRE(connections.at("default").properties.at("db_file") == "./data/test.db");
        REQUIRE(connections.at("default").log_queries == true);
        REQUIRE(connections.at("default").log_parameters == false);

        const auto& duckdb_config = mgr.getDuckDBConfig();
        REQUIRE(duckdb_config.db_path == ":memory:");
        REQUIRE(duckdb_config.settings.at("max_memory") == "2GB");
        REQUIRE(duckdb_config.settings.at("threads") == "4");
    }

    SECTION("Load invalid configuration") {
        std::string yaml_content = R"(
invalid_key: value
)";

        std::string config_file = createTempYamlFile(yaml_content);
        ConfigManager mgr{std::filesystem::path(config_file)};
        REQUIRE_THROWS(mgr.loadConfig());
    }

    std::filesystem::remove_all(temp_template_dir);
}

TEST_CASE("ConfigManager endpoint configuration", "[config_manager]") {
    ConfigManager mgr{std::filesystem::path("path/to/config.yaml")};
    std::string temp_template_dir = createTempDir();

    SECTION("Load valid endpoint configuration") {
        std::string yaml_content = R"(
project_name: TestProject
project_description: Test Description
template:
  path: )" + temp_template_dir + R"(
connections:
  default:
    init: "SELECT 1;"
    properties:
      db_file: ./data/test.db
)";

        std::string config_file = createTempYamlFile(yaml_content);
        ConfigManager mgr{std::filesystem::path(config_file)};
        mgr.loadConfig();

        std::string endpoint_yaml = R"(
url-path: /test
template-source: test.sql
request:
  - field-name: id
    field-in: query
    description: User ID
    required: true
    validators:
      - type: int
        min: 1
        max: 100
connection:
  - default
rate-limit:
  enabled: true
  max: 10
  interval: 60
auth:
  enabled: true
  type: basic
  users:
    - username: testuser
      password: testpass
      roles:
        - user
cache:
  cache-table-name: test_cache
  cache-source: test_source
  refresh-time: 1h
  refresh-endpoint: true

)";

        std::string endpoint_file = createTempYamlFile(endpoint_yaml, "endpoint_config.yaml");
        mgr.loadEndpointConfig(endpoint_file);

        const auto& endpoints = mgr.getEndpoints();
        REQUIRE(endpoints.size() == 1);
        const auto& endpoint = endpoints[0];

        REQUIRE(endpoint.urlPath == "/test");
        REQUIRE(endpoint.templateSource == "/tmp/test.sql");
        REQUIRE(endpoint.connection == std::vector<std::string>{"default"});

        REQUIRE(endpoint.requestFields.size() == 1);
        const auto& field = endpoint.requestFields[0];
        REQUIRE(field.fieldName == "id");
        REQUIRE(field.fieldIn == "query");
        REQUIRE(field.description == "User ID");
        REQUIRE(field.required == true);

        REQUIRE(field.validators.size() == 1);
        const auto& validator = field.validators[0];
        REQUIRE(validator.type == "int");
        REQUIRE(validator.min == 1);
        REQUIRE(validator.max == 100);

        REQUIRE(endpoint.rate_limit.enabled == true);
        REQUIRE(endpoint.rate_limit.max == 10);
        REQUIRE(endpoint.rate_limit.interval == 60);

        REQUIRE(endpoint.auth.enabled == true);
        REQUIRE(endpoint.auth.type == "basic");
        REQUIRE(endpoint.auth.users.size() == 1);
        REQUIRE(endpoint.auth.users[0].username == "testuser");
        REQUIRE(endpoint.auth.users[0].password == "testpass");
        REQUIRE(endpoint.auth.users[0].roles == std::vector<std::string>{"user"});

        REQUIRE(endpoint.cache.cacheTableName == "test_cache");
        REQUIRE(endpoint.cache.cacheSource == "/tmp/test_source");
        REQUIRE(endpoint.cache.refreshTime == "1h");
        REQUIRE(endpoint.cache.refreshEndpoint == true);
    }

    std::filesystem::remove_all(temp_template_dir);
}

TEST_CASE("ConfigManager getEndpointForPath", "[config_manager]") {
    std::string temp_template_dir = createTempDir();

    std::string yaml_content = R"(
project_name: TestProject
project_description: Test Description
template:
  path: )" + temp_template_dir + R"(
connections:
  default:
    init: "SELECT 1;"
    properties:
      db_file: ./data/test.db
)";

    std::string config_file = createTempYamlFile(yaml_content);
    ConfigManager mgr{std::filesystem::path(config_file)};
    mgr.loadConfig();

    std::string endpoint_yaml = R"(
url-path: /users/:id
method: GET
template-source: user.sql
request:
  - field-name: id
    field-in: path
    description: User ID
    required: true
    validators:
      - type: int
        min: 1
connection:
  - default
)";

    std::string endpoint_file = createTempYamlFile(endpoint_yaml, "endpoint_config.yaml");
    
    // Load the endpoint configuration
    mgr.loadEndpointConfig(endpoint_file);

    SECTION("Match existing endpoint") {
        const auto* found_endpoint = mgr.getEndpointForPath("/users/42");
        REQUIRE(found_endpoint != nullptr);
        REQUIRE(found_endpoint->urlPath == "/users/:id");
    }

    SECTION("No match for non-existent endpoint") {
        const auto* found_endpoint = mgr.getEndpointForPath("/non-existent");
        REQUIRE(found_endpoint == nullptr);
    }

    std::filesystem::remove_all(temp_template_dir);
}

TEST_CASE("ConfigManager template configuration", "[config_manager]") {
    ConfigManager mgr{std::filesystem::path("path/to/config.yaml")};
    std::string temp_template_dir = createTempDir();

    std::string yaml_content = R"(
project_name: TestProject
project_description: Test Description
template:
  path: )" + temp_template_dir + R"(
  environment-whitelist:
    - "ALLOWED_.*"
    - "SAFE_VAR"
connections:
  default:
    init: "SELECT 1;"
    properties:
      db_file: ./data/test.db
)";

    std::string config_file = createTempYamlFile(yaml_content);
    mgr = ConfigManager{std::filesystem::path(config_file)};
    mgr.loadConfig();

    const auto& template_config = mgr.getTemplateConfig();

    SECTION("Template path") {
        REQUIRE(ends_with(template_config.path, "flapi_test_templates"));
    }

    SECTION("Environment variable whitelist") {
        REQUIRE(template_config.isEnvironmentVariableAllowed("ALLOWED_VAR"));
        REQUIRE(template_config.isEnvironmentVariableAllowed("SAFE_VAR"));
        REQUIRE_FALSE(template_config.isEnvironmentVariableAllowed("UNSAFE_VAR"));
    }

    std::filesystem::remove_all(temp_template_dir);
}

TEST_CASE("ConfigManager getPropertiesForTemplates", "[config_manager]") {
    
    std::string temp_template_dir = createTempDir();

    std::string yaml_content = R"(
project_name: TestProject
project_description: Test Description
template:
  path: )" + temp_template_dir + R"(
connections:
  default:
    init: "SELECT 1;"
    properties:
      db_file: ./data/test.db
      relative_path: ./relative/path
      absolute_path: /absolute/path
)";

    std::string config_file = createTempYamlFile(yaml_content);
    ConfigManager mgr{std::filesystem::path(config_file)};
    mgr.loadConfig();

    SECTION("Get properties for existing connection") {
        auto props = mgr.getPropertiesForTemplates("default");
        REQUIRE(props.size() == 3);
        REQUIRE(ends_with(props["db_file"], "/data/test.db"));
        REQUIRE(ends_with(props["relative_path"], "/relative/path"));
        REQUIRE(props["absolute_path"] == "/absolute/path");
    }

    SECTION("Get properties for non-existent connection") {
        auto props = mgr.getPropertiesForTemplates("non_existent");
        REQUIRE(props.empty());
    }

    std::filesystem::remove_all(temp_template_dir);
}