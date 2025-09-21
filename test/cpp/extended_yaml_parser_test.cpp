#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "extended_yaml_parser.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>

using namespace flapi;

class ExtendedYamlTestFixture {
public:
    std::filesystem::path temp_dir;

    ExtendedYamlTestFixture() {
        temp_dir = std::filesystem::temp_directory_path() / "flapi_extended_yaml_test";
        std::filesystem::create_directories(temp_dir);

        // Create test files
        createTestFile("common/request.yaml", R"(
request:
  - field-name: id
    field-in: query
    description: Customer ID
    required: false
    validators:
      - type: int
        min: 1
        max: 1000000
        preventSqlInjection: true

  - field-name: segment
    field-in: query
    description: Customer segment (optional)
    required: false
    validators:
      - type: enum
        allowedValues: [retail, corporate, vip]

  - field-name: email
    field-in: query
    description: Customer email address
    required: false
    validators:
      - type: email
)");

        createTestFile("common/auth.yaml", R"(
auth:
  enabled: true
  type: basic
  users:
    - username: admin
      password: secret
      roles: [admin]
    - username: user
      password: password
      roles: [read]
)");

        createTestFile("common/rate_limit.yaml", R"(
rate-limit:
  enabled: true
  max: 100
  interval: 60
)");

        createTestFile("common/connection.yaml", R"(
connection:
  - customers-parquet
)");

        createTestFile("common/cache_config.yaml", R"(
cache:
  cache-table-name: customer_cache
  cache-source: cache.sql
  refresh-time: 1h
  refresh-endpoint: true
)");

        createTestFile("common/heartbeat.yaml", R"(
heartbeat:
  enabled: false
  params:
    id: 123
)");

        createTestFile("common/template.yaml", R"(
template-source: customers.sql
)");

        createTestFile("common/mcp_config.yaml", R"(
mcp-tool:
  name: customers
  description: Retrieve customer information
  result_mime_type: application/json
)");
    }

    ~ExtendedYamlTestFixture() {
        std::filesystem::remove_all(temp_dir);
    }

    void createTestFile(const std::string& relative_path, const std::string& content) {
        auto file_path = temp_dir / relative_path;
        std::filesystem::create_directories(file_path.parent_path());

        std::ofstream file(file_path);
        file << content;
        file.close();
    }

    std::string getFilePath(const std::string& relative_path) {
        return (temp_dir / relative_path).string();
    }

    std::string readFile(const std::string& relative_path) {
        std::ifstream file(temp_dir / relative_path);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }
};

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Basic file parsing", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("Parse simple YAML file") {
        auto result = parser.parseFile(getFilePath("common/request.yaml"));

        REQUIRE(result.success);
        REQUIRE(result.included_files.empty());

        const YAML::Node& node = result.node;
        REQUIRE(node["request"]);
        REQUIRE(node["request"].IsSequence());
        REQUIRE(node["request"].size() == 3);
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Include processing", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("Simple include from string") {
        std::string yaml_content = R"(
{{include from common/request.yaml}}
)";
        std::cout << "DEBUG: Testing simple include with content: " << yaml_content << std::endl;
        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 1);
        const YAML::Node& node = result.node;
        REQUIRE(node["request"]);
        REQUIRE(node["request"].IsSequence());
        REQUIRE(node["request"].size() == 3);
    }

    SECTION("Section include") {
        std::string yaml_content = R"(
config:
  {{include:request from common/request.yaml}}
  {{include:auth from common/auth.yaml}}
)";
        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 2);
        const YAML::Node& node = result.node;
        REQUIRE(node["config"]["request"]);
        REQUIRE(node["config"]["auth"]);
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: File inclusion", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("Include entire file") {
        std::string yaml_content = R"(
{{include from common/request.yaml}}
{{include from common/auth.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 2);

        const YAML::Node& node = result.node;
        REQUIRE(node["request"]);
        REQUIRE(node["auth"]);
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Section inclusion", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("Include specific section") {
        std::string yaml_content = R"(
config:
  {{include:request from common/request.yaml}}
  {{include:auth from common/auth.yaml}}
  {{include:rate-limit from common/rate_limit.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 3);

        const YAML::Node& node = result.node;
        REQUIRE(node["config"]["request"]);
        REQUIRE(node["config"]["auth"]);
        REQUIRE(node["config"]["rate-limit"]);
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Include resolution", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("Resolve relative include path") {
        std::string yaml_content = R"(
{{include from common/request.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 1);
        REQUIRE(result.included_files[0].find("common/request.yaml") != std::string::npos);
    }

    SECTION("Resolve include paths configuration") {
        ExtendedYamlParser::IncludeConfig config;
        config.include_paths = {"/nonexistent", temp_dir.string()};

        ExtendedYamlParser parser_with_paths(config);

        std::string yaml_content = R"(
{{include from nonexistent/file.yaml}}
)";

        auto result = parser_with_paths.parseString(yaml_content, "/some/path");

        // Should find file through include_paths
        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 1);
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Circular dependency detection", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("Detect simple circular dependency") {
        // Create files that include each other
        createTestFile("circular/a.yaml", R"(
{{include from ../circular/b.yaml}}
value: from_a
)");

        createTestFile("circular/b.yaml", R"(
{{include from ../circular/a.yaml}}
value: from_b
)");

        std::string yaml_content = R"(
{{include from circular/a.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_message.find("Circular dependency") != std::string::npos);
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Environment variable substitution", "[extended_yaml_parser]") {
    ExtendedYamlParser::IncludeConfig config;
    config.allow_environment_variables = true;
    config.environment_whitelist = {".*"}; // Allow all for testing

    ExtendedYamlParser parser(config);

    SECTION("Substitute existing environment variable") {
        setenv("TEST_VAR", "test_value", 1);

        std::string yaml_content = R"(
value: {{env.TEST_VAR}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        const YAML::Node& node = result.node;
        REQUIRE(node["value"].Scalar() == "test_value");

        // Check resolved variables
        REQUIRE(result.resolved_variables["TEST_VAR"] == "test_value");

        unsetenv("TEST_VAR");
    }

    SECTION("Substitute non-existent environment variable") {
        std::string yaml_content = R"(
value: {{env.NONEXISTENT_VAR}}
default: unchanged
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        const YAML::Node& node = result.node;
        REQUIRE(node["value"].Scalar() == ""); // Empty string for non-existent
        REQUIRE(node["default"].Scalar() == "unchanged");
    }

    SECTION("Environment variable in include path") {
        setenv("CONFIG_ENV", "auth", 1);

        std::string yaml_content = R"(
{{include from common/{{env.CONFIG_ENV}}.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 1);
        REQUIRE(result.included_files[0].find("common/auth.yaml") != std::string::npos);

        unsetenv("CONFIG_ENV");
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Conditional includes", "[extended_yaml_parser]") {
    ExtendedYamlParser::IncludeConfig config;
    config.allow_conditional_includes = true;

    ExtendedYamlParser parser(config);

    SECTION("Conditional include - true condition") {
        setenv("ENABLE_AUTH", "1", 1);

        std::string yaml_content = R"(
{{include from common/auth.yaml if env.ENABLE_AUTH}}
value: always_present
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 1);
        const YAML::Node& node = result.node;
        REQUIRE(node["auth"]);
        REQUIRE(node["value"].Scalar() == "always_present");

        unsetenv("ENABLE_AUTH");
    }

    SECTION("Conditional include - false condition") {
        setenv("ENABLE_AUTH", "", 1); // Empty string evaluates to false

        std::string yaml_content = R"(
{{include from common/auth.yaml if env.ENABLE_AUTH}}
value: always_present
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.empty()); // No includes processed
        const YAML::Node& node = result.node;
        REQUIRE_FALSE(node["auth"]); // Auth section not included
        REQUIRE(node["value"].Scalar() == "always_present");

        unsetenv("ENABLE_AUTH");
    }

    SECTION("Conditional include - true literal") {
        std::string yaml_content = R"(
{{include from common/auth.yaml if true}}
value: included
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 1);
        const YAML::Node& node = result.node;
        REQUIRE(node["auth"]);
        REQUIRE(node["value"].Scalar() == "included");
    }

    SECTION("Conditional include - false literal") {
        std::string yaml_content = R"(
{{include from common/auth.yaml if false}}
value: not_included
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.empty());
        const YAML::Node& node = result.node;
        REQUIRE_FALSE(node["auth"]);
        REQUIRE(node["value"].Scalar() == "not_included");
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Include in keys", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("Include in map keys") {
        std::string yaml_content = R"(
{{include:request from common/request.yaml}}: processed
value: unchanged
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        const YAML::Node& node = result.node;
        REQUIRE(node["request"]); // Key was processed
        REQUIRE(node["value"].Scalar() == "unchanged");
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Include in sequences", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("Include in sequence items") {
        std::string yaml_content = R"(
connections:
  - {{include from common/connection.yaml}}
  - custom_connection
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        const YAML::Node& node = result.node;
        REQUIRE(node["connections"].IsSequence());
        REQUIRE(node["connections"].size() == 2);
        REQUIRE(node["connections"][0].IsSequence());
        REQUIRE(node["connections"][1].Scalar() == "custom_connection");
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Nested includes", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("Multiple levels of includes") {
        // Create a file that includes another file
        createTestFile("nested/level1.yaml", R"(
level1_value: from_level1
{{include from ../common/auth.yaml}}
)");

        std::string yaml_content = R"(
root_value: from_root
{{include from nested/level1.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 2); // level1.yaml and auth.yaml

        const YAML::Node& node = result.node;
        REQUIRE(node["root_value"].Scalar() == "from_root");
        REQUIRE(node["level1_value"].Scalar() == "from_level1");
        REQUIRE(node["auth"]);
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Configuration options", "[extended_yaml_parser]") {
    SECTION("Disable environment variables") {
        ExtendedYamlParser::IncludeConfig config;
        config.allow_environment_variables = false;

        ExtendedYamlParser parser(config);

        setenv("TEST_VAR", "test_value", 1);

        std::string yaml_content = R"(
value: {{env.TEST_VAR}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        const YAML::Node& node = result.node;
        REQUIRE(node["value"].Scalar() == "{{env.TEST_VAR}}"); // Not substituted

        unsetenv("TEST_VAR");
    }

    SECTION("Disable conditional includes") {
        ExtendedYamlParser::IncludeConfig config;
        config.allow_conditional_includes = false;

        ExtendedYamlParser parser(config);

        setenv("ENABLE_AUTH", "1", 1);

        std::string yaml_content = R"(
{{include from common/auth.yaml if env.ENABLE_AUTH}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_message.find("Invalid include directive") != std::string::npos);

        unsetenv("ENABLE_AUTH");
    }

    SECTION("Environment whitelist") {
        ExtendedYamlParser::IncludeConfig config;
        config.allow_environment_variables = true;
        config.environment_whitelist = {"ALLOWED_.*"};

        ExtendedYamlParser parser(config);

        setenv("ALLOWED_VAR", "allowed", 1);
        setenv("DISALLOWED_VAR", "disallowed", 1);

        std::string yaml_content = R"(
allowed: {{env.ALLOWED_VAR}}
disallowed: {{env.DISALLOWED_VAR}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        const YAML::Node& node = result.node;
        REQUIRE(node["allowed"].Scalar() == "allowed");
        REQUIRE(node["disallowed"].Scalar() == ""); // Not substituted due to whitelist

        unsetenv("ALLOWED_VAR");
        unsetenv("DISALLOWED_VAR");
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Error handling", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("File not found") {
        std::string yaml_content = R"(
{{include from nonexistent/file.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_message.find("Could not resolve include path") != std::string::npos);
    }

    SECTION("Section not found") {
        std::string yaml_content = R"(
{{include:nonexistent_section from common/auth.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_message.find("Section 'nonexistent_section' not found") != std::string::npos);
    }

    SECTION("Invalid include directive") {
        std::string yaml_content = R"(
{{invalid_include_syntax}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE_FALSE(result.success);
        REQUIRE(result.error_message.find("Invalid include directive") != std::string::npos);
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: YAML merging", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("Merge included content with existing") {
        std::string yaml_content = R"(
existing_key: existing_value
{{include from common/auth.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        const YAML::Node& node = result.node;

        REQUIRE(node["existing_key"].Scalar() == "existing_value");
        REQUIRE(node["auth"]["enabled"].Scalar() == "true");
        REQUIRE(node["auth"]["type"].Scalar() == "basic");
    }

    SECTION("Merge nested structures") {
        std::string yaml_content = R"(
config:
  existing: value
  {{include:auth from common/auth.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        const YAML::Node& node = result.node;

        REQUIRE(node["config"]["existing"].Scalar() == "value");
        REQUIRE(node["config"]["auth"]["enabled"].Scalar() == "true");
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Real-world example", "[extended_yaml_parser]") {
    ExtendedYamlParser parser;

    SECTION("REST endpoint with includes") {
        std::string yaml_content = R"(
url-path: /customers/
method: GET
{{include:request from common/request.yaml}}
{{include:auth from common/auth.yaml}}
{{include:rate-limit from common/rate_limit.yaml}}
{{include:connection from common/connection.yaml}}
{{include:cache from common/cache_config.yaml}}
{{include:heartbeat from common/heartbeat.yaml}}
{{include:template from common/template.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 6);

        const YAML::Node& node = result.node;
        REQUIRE(node["url-path"].Scalar() == "/customers/");
        REQUIRE(node["method"].Scalar() == "GET");
        REQUIRE(node["request"]);
        REQUIRE(node["auth"]);
        REQUIRE(node["rate-limit"]);
        REQUIRE(node["connection"]);
        REQUIRE(node["cache"]);
        REQUIRE(node["heartbeat"]);
        REQUIRE(node["template-source"]);
    }

    SECTION("MCP tool with includes") {
        std::string yaml_content = R"(
{{include:mcp-tool from common/mcp_config.yaml}}
{{include:request from common/request.yaml}}
{{include:auth from common/auth.yaml}}
{{include:connection from common/connection.yaml}}
{{include:cache from common/cache_config.yaml}}
{{include:heartbeat from common/heartbeat.yaml}}
{{include:template from common/template.yaml}}
)";

        auto result = parser.parseString(yaml_content, temp_dir);

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 6);

        const YAML::Node& node = result.node;
        REQUIRE(node["mcp-tool"]["name"].Scalar() == "customers");
        REQUIRE(node["request"]);
        REQUIRE(node["auth"]);
        REQUIRE(node["connection"]);
        REQUIRE(node["cache"]);
        REQUIRE(node["heartbeat"]);
        REQUIRE(node["template-source"]);
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Include directive parsing", "[extended_yaml_parser]") {
    SECTION("Parse section include directive") {
        auto result = ExtendedYamlParser::parseIncludeDirective("request from common/request.yaml");
        REQUIRE(result.has_value());
        REQUIRE(result->section_name == "request");
        REQUIRE(result->file_path == "common/request.yaml");
        REQUIRE(result->is_section_include == true);
    }

    SECTION("Parse file include directive") {
        auto result = ExtendedYamlParser::parseIncludeDirective("from common/auth.yaml");
        REQUIRE(result.has_value());
        REQUIRE(result->section_name == "");
        REQUIRE(result->file_path == "common/auth.yaml");
        REQUIRE(result->is_section_include == false);
    }

    SECTION("Parse conditional include directive") {
        auto result = ExtendedYamlParser::parseIncludeDirective("request from common/request.yaml if env.ENABLE_FEATURE");
        REQUIRE(result.has_value());
        REQUIRE(result->section_name == "request");
        REQUIRE(result->file_path == "common/request.yaml");
        REQUIRE(result->is_section_include == true);
        REQUIRE(result->is_conditional == true);
        REQUIRE(result->condition == "env.ENABLE_FEATURE");
    }

    SECTION("Parse invalid directive") {
        auto result = ExtendedYamlParser::parseIncludeDirective("invalid syntax");
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Path resolution", "[extended_yaml_parser]") {
    SECTION("Resolve relative path") {
        std::filesystem::path base_path = temp_dir;
        std::filesystem::path include_path = "common/request.yaml";
        std::filesystem::path resolved_path;

        bool success = ExtendedYamlParser::resolveIncludePath(include_path, base_path, resolved_path, {});
        REQUIRE(success);
        REQUIRE(resolved_path == temp_dir / "common/request.yaml");
    }

    SECTION("Resolve absolute path") {
        std::filesystem::path base_path = "/some/base";
        std::filesystem::path include_path = getFilePath("common/auth.yaml"); // Absolute path
        std::filesystem::path resolved_path;

        bool success = ExtendedYamlParser::resolveIncludePath(include_path, base_path, resolved_path, {});
        REQUIRE(success);
        REQUIRE(resolved_path == include_path);
    }

    SECTION("Resolve through include paths") {
        std::vector<std::string> include_paths = {"/nonexistent", temp_dir.string()};
        std::filesystem::path base_path = "/some/base";
        std::filesystem::path include_path = "common/request.yaml";
        std::filesystem::path resolved_path;

        bool success = ExtendedYamlParser::resolveIncludePath(include_path, base_path, resolved_path, include_paths);
        REQUIRE(success);
        REQUIRE(resolved_path == temp_dir / "common/request.yaml");
    }
}

TEST_CASE_METHOD(ExtendedYamlTestFixture, "ExtendedYamlParser: Performance", "[extended_yaml_parser][performance]") {
    ExtendedYamlParser parser;

    SECTION("Large number of includes") {
        std::string yaml_content = "";
        for (int i = 0; i < 10; ++i) {
            yaml_content += "{{include from common/auth.yaml}}\n";
        }

        auto start = std::chrono::high_resolution_clock::now();
        auto result = parser.parseString(yaml_content, temp_dir);
        auto end = std::chrono::high_resolution_clock::now();

        REQUIRE(result.success);
        REQUIRE(result.included_files.size() == 10); // Should be 10 unique files, not 10 duplicates

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        REQUIRE(duration.count() < 1000); // Should complete within 1 second
    }
}
