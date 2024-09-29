#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "config_manager.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>

// Helper function to create a temporary YAML file
std::string create_temp_yaml(const std::string& content) {
    std::string filename = "temp_config_" + std::to_string(rand()) + ".yaml";
    std::ofstream file(filename);
    file << content;
    file.close();
    return filename;
}

TEST_CASE("ConfigManager basic configuration parsing", "[config]") {
    std::string yaml_content = R"(
name: test-project
description: A test project
template-path: 'test_sqls'
connections:
  test-connection:
    init: 'LOAD extension;'
    properties:
      url: 'test-url'
    log-queries: true
    log-parameters: false
    allow: '*'
rate-limit:
  options:
    interval:
      min: 1
    max: 100
enforce-https:
  enabled: true
auth:
  enabled: false
response-format:
  enabled: true
  options:
    default: json
    formats:
      - json
      - csv
    )";

    std::string filename = create_temp_yaml(yaml_content);
    flapi::ConfigManager config_manager;
    config_manager.load_config(filename);

    SECTION("Project information") {
        CHECK(config_manager.get_project_name() == "test-project");
        CHECK(config_manager.get_project_description() == "A test project");
        CHECK(config_manager.get_template_path() == "test_sqls");
    }

    SECTION("Connections") {
        const auto& connections = config_manager.get_connections();
        REQUIRE(connections.size() == 1);
        REQUIRE(connections.count("test-connection") == 1);
        const auto& conn = connections.at("test-connection");
        CHECK(conn.init == "LOAD extension;");
        CHECK(conn.properties.at("url") == "test-url");
        CHECK(conn.log_queries == true);
        CHECK(conn.log_parameters == false);
        CHECK(conn.allow == "*");
    }

    SECTION("Rate limit") {
        const auto& rate_limit = config_manager.get_rate_limit_config();
        CHECK(rate_limit.interval_min == 1);
        CHECK(rate_limit.max == 100);
    }

    SECTION("HTTPS and Auth") {
        CHECK(config_manager.is_https_enforced() == true);
        CHECK(config_manager.is_auth_enabled() == false);
    }

    SECTION("Response format") {
        const auto& response_format = config_manager.get_response_format_config();
        CHECK(response_format.enabled == true);
        CHECK(response_format.default_format == "json");
        REQUIRE(response_format.formats.size() == 2);
        CHECK(response_format.formats[0] == "json");
        CHECK(response_format.formats[1] == "csv");
    }

    std::remove(filename.c_str());
}

TEST_CASE("ConfigManager endpoint parsing", "[config]") {
    std::string main_yaml_content = R"(
name: test-project
description: A test project
template-path: 'test_sqls'
connections:
  test-connection:
    init: 'LOAD extension;'
    properties:
      url: 'test-url'
    )";

    std::string endpoint_yaml_content = R"(
urlPath: /test/:id
request:
  - fieldName: id
    fieldIn: path
    description: Test ID
    validators:
      - required
  - fieldName: filter
    fieldIn: query
    description: Optional filter
templateSource: test.sql
connection:
  - test-connection
    )";

    std::string main_filename = create_temp_yaml(main_yaml_content);
    std::string endpoint_filename = create_temp_yaml(endpoint_yaml_content);

    // Create the test_sqls directory and move the endpoint YAML file into it
    std::filesystem::create_directories("test_sqls");
    std::filesystem::rename(endpoint_filename, "test_sqls/test_endpoint.yaml");

    flapi::ConfigManager config_manager;
    config_manager.load_config(main_filename);

    SECTION("Endpoint configuration") {
        const auto& endpoints = config_manager.get_endpoints();
        REQUIRE(endpoints.size() == 1);
        const auto& endpoint = endpoints[0];

        CHECK(endpoint.urlPath == "/test/:id");
        REQUIRE(endpoint.requestFields.size() == 2);

        const auto& id_field = endpoint.requestFields[0];
        CHECK(id_field.fieldName == "id");
        CHECK(id_field.fieldIn == "path");
        CHECK(id_field.description == "Test ID");
        REQUIRE(id_field.validators.size() == 1);
        CHECK(id_field.validators[0] == "required");

        const auto& filter_field = endpoint.requestFields[1];
        CHECK(filter_field.fieldName == "filter");
        CHECK(filter_field.fieldIn == "query");
        CHECK(filter_field.description == "Optional filter");
        CHECK(filter_field.validators.empty());

        CHECK(endpoint.templateSource == "test.sql");
        REQUIRE(endpoint.connection.size() == 1);
        CHECK(endpoint.connection[0] == "test-connection");
    }

    std::remove(main_filename.c_str());
    std::filesystem::remove("test_sqls");
}

TEST_CASE("ConfigManager error handling", "[config]") {
    SECTION("Invalid YAML syntax") {
        std::string invalid_yaml = R"(
name: test-project
description: A test project
template-path: 'test_sqls'
connections:
  - invalid: yaml
    syntax: here
        )";

        std::string filename = create_temp_yaml(invalid_yaml);
        flapi::ConfigManager config_manager;

        REQUIRE_THROWS_AS(config_manager.load_config(filename), YAML::Exception);

        std::remove(filename.c_str());
    }

    SECTION("Missing required fields") {
        std::string missing_fields_yaml = R"(
name: test-project
# description is missing
template-path: 'test_sqls'
        )";

        std::string filename = create_temp_yaml(missing_fields_yaml);
        flapi::ConfigManager config_manager;

        REQUIRE_THROWS_MATCHES(
            config_manager.load_config(filename),
            std::runtime_error,
            Catch::Matchers::Message("Missing required field")
        );

        std::remove(filename.c_str());
    }

    SECTION("Invalid connection configuration") {
        std::string invalid_connection_yaml = R"(
name: test-project
description: A test project
template-path: 'test_sqls'
connections:
  test-connection:
    init: 123  # Should be a string
    properties:
      url: 'test-url'
        )";

        std::string filename = create_temp_yaml(invalid_connection_yaml);
        flapi::ConfigManager config_manager;

        REQUIRE_THROWS_MATCHES(
            config_manager.load_config(filename),
            std::runtime_error,
            Catch::Matchers::Message("Invalid connection configuration")
        );

        std::remove(filename.c_str());
    }
}