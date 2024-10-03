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
    ConfigManager config_manager;
    std::string temp_template_dir = createTempDir();

    SECTION("Load valid configuration") {
        std::string yaml_content = R"(
name: TestProject
description: Test Description
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
duckdb:
  db_path: ":memory:"
  max_memory: "2GB"
  threads: "4"
)";

        std::string config_file = createTempYamlFile(yaml_content);
        REQUIRE_NOTHROW(config_manager.loadConfig(config_file));

        REQUIRE(config_manager.getProjectName() == "TestProject");
        REQUIRE(config_manager.getProjectDescription() == "Test Description");
        REQUIRE(config_manager.isHttpsEnforced() == true);
        REQUIRE(config_manager.isAuthEnabled() == false);

        const auto& connections = config_manager.getConnections();
        REQUIRE(connections.size() == 1);
        REQUIRE(connections.at("default").init == "SELECT 1;");
        REQUIRE(connections.at("default").properties.at("db_file") == "./data/test.db");
        REQUIRE(connections.at("default").log_queries == true);
        REQUIRE(connections.at("default").log_parameters == false);
        REQUIRE(connections.at("default").allow == "*");

        const auto& duckdb_config = config_manager.getDuckDBConfig();
        REQUIRE(duckdb_config.db_path == ":memory:");
        REQUIRE(duckdb_config.settings.at("max_memory") == "2GB");
        REQUIRE(duckdb_config.settings.at("threads") == "4");
    }

    SECTION("Load invalid configuration") {
        std::string yaml_content = R"(
invalid_key: value
)";

        std::string config_file = createTempYamlFile(yaml_content);
        REQUIRE_THROWS(config_manager.loadConfig(config_file));
    }

    std::filesystem::remove_all(temp_template_dir);
}

TEST_CASE("ConfigManager endpoint configuration", "[config_manager]") {
    ConfigManager config_manager;
    std::string temp_template_dir = createTempDir();

    SECTION("Load valid endpoint configuration") {
        std::string yaml_content = R"(
name: TestProject
description: Test Description
template:
  path: )" + temp_template_dir + R"(
connections:
  default:
    init: "SELECT 1;"
    properties:
      db_file: ./data/test.db
)";

        std::string config_file = createTempYamlFile(yaml_content);
        REQUIRE_NOTHROW(config_manager.loadConfig(config_file));

        std::string endpoint_yaml = R"(
urlPath: /test
templateSource: test.sql
request:
  - fieldName: id
    fieldIn: query
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
  cacheTableName: test_cache
  cacheSource: test_source
  refreshTime: 1h
  refreshEndpoint: true

)";

        std::string endpoint_file = createTempYamlFile(endpoint_yaml, "endpoint_config.yaml");
        REQUIRE_NOTHROW(config_manager.loadEndpointConfig(endpoint_file));

        const auto& endpoints = config_manager.getEndpoints();
        REQUIRE(endpoints.size() == 1);
        const auto& endpoint = endpoints[0];

        REQUIRE(endpoint.urlPath == "/test");
        REQUIRE(endpoint.templateSource == "test.sql");
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
        REQUIRE(endpoint.cache.cacheSource == "test_source");
        REQUIRE(endpoint.cache.refreshTime == "1h");
        REQUIRE(endpoint.cache.refreshEndpoint == true);
    }

    std::filesystem::remove_all(temp_template_dir);
}

TEST_CASE("ConfigManager getEndpointForPath", "[config_manager]") {
    ConfigManager config_manager;
    std::string temp_template_dir = createTempDir();

    std::string yaml_content = R"(
name: TestProject
description: Test Description
template:
  path: )" + temp_template_dir + R"(
connections:
  default:
    init: "SELECT 1;"
    properties:
      db_file: ./data/test.db
)";

    std::string config_file = createTempYamlFile(yaml_content);
    REQUIRE_NOTHROW(config_manager.loadConfig(config_file));

    std::string endpoint_yaml = R"(
urlPath: /users/:id
templateSource: user.sql
request:
  - fieldName: id
    fieldIn: path
    description: User ID
    required: true
    validators:
      - type: int
        min: 1
connection:
  - default
)";

    std::string endpoint_file = createTempYamlFile(endpoint_yaml, "endpoint_config.yaml");
    REQUIRE_NOTHROW(config_manager.loadEndpointConfig(endpoint_file));

    SECTION("Match existing endpoint") {
        const auto* endpoint = config_manager.getEndpointForPath("/users/42");
        REQUIRE(endpoint != nullptr);
        REQUIRE(endpoint->urlPath == "/users/:id");
    }

    SECTION("No match for non-existent endpoint") {
        const auto* endpoint = config_manager.getEndpointForPath("/non-existent");
        REQUIRE(endpoint == nullptr);
    }

    std::filesystem::remove_all(temp_template_dir);
}

TEST_CASE("ConfigManager template configuration", "[config_manager]") {
    ConfigManager config_manager;
    std::string temp_template_dir = createTempDir();

    std::string yaml_content = R"(
name: TestProject
description: Test Description
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
    REQUIRE_NOTHROW(config_manager.loadConfig(config_file));

    const auto& template_config = config_manager.getTemplateConfig();

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
    ConfigManager config_manager;
    std::string temp_template_dir = createTempDir();

    std::string yaml_content = R"(
name: TestProject
description: Test Description
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
    REQUIRE_NOTHROW(config_manager.loadConfig(config_file));

    SECTION("Get properties for existing connection") {
        auto props = config_manager.getPropertiesForTemplates("default");
        REQUIRE(props.size() == 3);
        REQUIRE(ends_with(props["db_file"], "/data/test.db"));
        REQUIRE(ends_with(props["relative_path"], "/relative/path"));
        REQUIRE(props["absolute_path"] == "/absolute/path");
    }

    SECTION("Get properties for non-existent connection") {
        auto props = config_manager.getPropertiesForTemplates("non_existent");
        REQUIRE(props.empty());
    }

    std::filesystem::remove_all(temp_template_dir);
}