#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "config_serializer.hpp"
#include <filesystem>
#include <fstream>

using namespace flapi;

// Helper: Create temp test file
class TempFile {
public:
    explicit TempFile(const std::string& content = "") {
        path_ = std::filesystem::temp_directory_path() / ("test_serializer_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".yaml");
        if (!content.empty()) {
            std::ofstream file(path_);
            file << content;
        }
    }

    ~TempFile() {
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
        }
    }

    std::filesystem::path path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Helper: Create test REST endpoint
EndpointConfig createRestEndpoint(
    const std::string& url_path = "/test",
    const std::string& method = "GET") {
    EndpointConfig ep;
    ep.urlPath = url_path;
    ep.method = method;
    ep.templateSource = "test.sql";
    ep.connection = {"default"};
    return ep;
}

// Helper: Create test MCP endpoint
EndpointConfig createMCPEndpoint(const std::string& name = "test_tool") {
    EndpointConfig ep;
    ep.mcp_tool = EndpointConfig::MCPToolInfo{
        name,
        "Test tool",
        "application/json"
    };
    ep.templateSource = "test.sql";
    ep.connection = {"default"};
    return ep;
}

TEST_CASE("ConfigSerializer basic serialization", "[serializer][config]") {
    ConfigSerializer serializer;

    SECTION("Serialize simple REST endpoint") {
        EndpointConfig ep = createRestEndpoint("/customers", "GET");

        std::string yaml = serializer.serializeEndpointConfigToYaml(ep);

        REQUIRE(yaml.find("url-path: /customers") != std::string::npos);
        REQUIRE(yaml.find("method: GET") != std::string::npos);
        REQUIRE(yaml.find("template-source: test.sql") != std::string::npos);
    }

    SECTION("Serialize REST endpoint with multiple methods") {
        EndpointConfig ep = createRestEndpoint("/customers", "POST");

        std::string yaml = serializer.serializeEndpointConfigToYaml(ep);

        REQUIRE(yaml.find("method: POST") != std::string::npos);
    }

    SECTION("Serialize MCP tool endpoint") {
        EndpointConfig ep = createMCPEndpoint("customer_lookup");

        std::string yaml = serializer.serializeEndpointConfigToYaml(ep);

        REQUIRE(yaml.find("mcp-tool:") != std::string::npos);
        REQUIRE(yaml.find("name: customer_lookup") != std::string::npos);
        REQUIRE(yaml.find("description: Test tool") != std::string::npos);
    }

    SECTION("Serialize endpoint with connections") {
        EndpointConfig ep = createRestEndpoint();
        ep.connection = {"primary", "cache"};

        std::string yaml = serializer.serializeEndpointConfigToYaml(ep);

        REQUIRE(yaml.find("connection:") != std::string::npos);
        REQUIRE(yaml.find("primary") != std::string::npos);
        REQUIRE(yaml.find("cache") != std::string::npos);
    }
}

TEST_CASE("ConfigSerializer request field serialization", "[serializer][config]") {
    ConfigSerializer serializer;

    SECTION("Serialize endpoint with request fields") {
        EndpointConfig ep = createRestEndpoint();

        RequestFieldConfig field;
        field.fieldName = "id";
        field.fieldIn = "query";
        field.required = true;

        ValidatorConfig validator;
        validator.type = "int";
        validator.min = 1;
        validator.max = 999;
        field.validators.push_back(validator);

        ep.request_fields.push_back(field);

        std::string yaml = serializer.serializeEndpointConfigToYaml(ep);

        REQUIRE(yaml.find("request:") != std::string::npos);
        REQUIRE(yaml.find("field-name: id") != std::string::npos);
        REQUIRE(yaml.find("field-in: query") != std::string::npos);
        REQUIRE(yaml.find("required: true") != std::string::npos);
        REQUIRE(yaml.find("validators:") != std::string::npos);
    }

    SECTION("Serialize multiple request fields") {
        EndpointConfig ep = createRestEndpoint();

        RequestFieldConfig field1;
        field1.fieldName = "id";
        field1.fieldIn = "path";
        ep.request_fields.push_back(field1);

        RequestFieldConfig field2;
        field2.fieldName = "limit";
        field2.fieldIn = "query";
        ep.request_fields.push_back(field2);

        std::string yaml = serializer.serializeEndpointConfigToYaml(ep);

        REQUIRE(yaml.find("field-name: id") != std::string::npos);
        REQUIRE(yaml.find("field-name: limit") != std::string::npos);
    }
}

TEST_CASE("ConfigSerializer cache configuration serialization", "[serializer][config]") {
    ConfigSerializer serializer;

    SECTION("Serialize endpoint with cache enabled") {
        EndpointConfig ep = createRestEndpoint();
        ep.cache.enabled = true;
        ep.cache.table = "customers_cache";
        ep.cache.schema = "cache";
        ep.cache.schedule = "6h";

        std::string yaml = serializer.serializeEndpointConfigToYaml(ep);

        REQUIRE(yaml.find("cache:") != std::string::npos);
        REQUIRE(yaml.find("enabled: true") != std::string::npos);
        REQUIRE(yaml.find("table: customers_cache") != std::string::npos);
        REQUIRE(yaml.find("schedule: 6h") != std::string::npos);
    }

    SECTION("Disabled cache not serialized") {
        EndpointConfig ep = createRestEndpoint();
        ep.cache.enabled = false;

        std::string yaml = serializer.serializeEndpointConfigToYaml(ep);

        REQUIRE(yaml.find("cache:") == std::string::npos);
    }
}

TEST_CASE("ConfigSerializer deserialization", "[serializer][config]") {
    ConfigSerializer serializer;

    SECTION("Deserialize simple REST endpoint") {
        std::string yaml = R"(
url-path: /customers
method: GET
template-source: customers.sql
connection: [default]
)";

        EndpointConfig ep = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(ep.urlPath == "/customers");
        REQUIRE(ep.method == "GET");
        REQUIRE(ep.templateSource == "customers.sql");
        REQUIRE(ep.connection.size() == 1);
        REQUIRE(ep.connection[0] == "default");
    }

    SECTION("Deserialize MCP tool endpoint") {
        std::string yaml = R"(
mcp-tool:
  name: customer_lookup
  description: Get customer info
template-source: test.sql
connection: [default]
)";

        EndpointConfig ep = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(ep.mcp_tool.has_value());
        REQUIRE(ep.mcp_tool->name == "customer_lookup");
        REQUIRE(ep.mcp_tool->description == "Get customer info");
    }

    SECTION("Deserialize multiple connections") {
        std::string yaml = R"(
url-path: /customers
method: GET
template-source: test.sql
connection: [primary, cache, analytics]
)";

        EndpointConfig ep = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(ep.connection.size() == 3);
        REQUIRE(ep.connection[0] == "primary");
        REQUIRE(ep.connection[2] == "analytics");
    }

    SECTION("Deserialize with request fields") {
        std::string yaml = R"(
url-path: /customers
method: GET
template-source: test.sql
connection: [default]
request:
  - field-name: id
    field-in: query
    required: true
    validators:
      - type: int
        min: 1
)";

        EndpointConfig ep = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(ep.request_fields.size() == 1);
        REQUIRE(ep.request_fields[0].fieldName == "id");
        REQUIRE(ep.request_fields[0].fieldIn == "query");
        REQUIRE(ep.request_fields[0].required);
    }

    SECTION("Deserialize with cache config") {
        std::string yaml = R"(
url-path: /test
method: GET
template-source: test.sql
connection: [default]
cache:
  enabled: true
  table: test_cache
  schedule: 1h
)";

        EndpointConfig ep = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(ep.cache.enabled);
        REQUIRE(ep.cache.table == "test_cache");
        REQUIRE(ep.cache.schedule.has_value());
        REQUIRE(ep.cache.schedule.value() == "1h");
    }
}

TEST_CASE("ConfigSerializer round-trip serialization", "[serializer][config]") {
    ConfigSerializer serializer;

    SECTION("REST endpoint round-trip") {
        EndpointConfig original = createRestEndpoint("/customers", "GET");

        // Serialize
        std::string yaml = serializer.serializeEndpointConfigToYaml(original);

        // Deserialize
        EndpointConfig restored = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(restored.urlPath == original.urlPath);
        REQUIRE(restored.method == original.method);
        REQUIRE(restored.templateSource == original.templateSource);
    }

    SECTION("MCP endpoint round-trip") {
        EndpointConfig original = createMCPEndpoint("my_tool");

        std::string yaml = serializer.serializeEndpointConfigToYaml(original);
        EndpointConfig restored = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(restored.mcp_tool.has_value());
        REQUIRE(restored.mcp_tool->name == original.mcp_tool->name);
    }

    SECTION("Complex endpoint with cache and request fields round-trip") {
        EndpointConfig original = createRestEndpoint("/data", "POST");
        original.cache.enabled = true;
        original.cache.table = "data_cache";

        RequestFieldConfig field;
        field.fieldName = "limit";
        field.fieldIn = "query";
        original.request_fields.push_back(field);

        std::string yaml = serializer.serializeEndpointConfigToYaml(original);
        EndpointConfig restored = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(restored.cache.enabled);
        REQUIRE(restored.cache.table == "data_cache");
        REQUIRE(restored.request_fields.size() == 1);
    }
}

TEST_CASE("ConfigSerializer file operations", "[serializer][config]") {
    ConfigSerializer serializer;

    SECTION("Persist and load configuration") {
        EndpointConfig original = createRestEndpoint("/customers", "GET");

        TempFile temp_file;

        // Persist
        serializer.persistEndpointConfigToFile(original, temp_file.path());

        // Verify file was created
        REQUIRE(std::filesystem::exists(temp_file.path()));

        // Load
        std::string yaml_content = serializer.loadEndpointConfigYamlFromFile(temp_file.path());

        // Verify content
        REQUIRE_FALSE(yaml_content.empty());
        REQUIRE(yaml_content.find("url-path") != std::string::npos);
    }

    SECTION("Load from non-existent file throws error") {
        REQUIRE_THROWS_AS(
            serializer.loadEndpointConfigYamlFromFile("/nonexistent/file.yaml"),
            std::runtime_error
        );
    }

    SECTION("Load from directory throws error") {
        std::filesystem::path temp_dir = std::filesystem::temp_directory_path();

        REQUIRE_THROWS_AS(
            serializer.loadEndpointConfigYamlFromFile(temp_dir),
            std::runtime_error
        );
    }

    SECTION("Persist creates parent directories") {
        EndpointConfig ep = createRestEndpoint();

        std::filesystem::path parent_dir = std::filesystem::temp_directory_path() / "test_serializer_nested";
        std::filesystem::path file_path = parent_dir / "endpoint.yaml";

        // Clean up if exists
        if (std::filesystem::exists(parent_dir)) {
            std::filesystem::remove_all(parent_dir);
        }

        try {
            serializer.persistEndpointConfigToFile(ep, file_path);

            REQUIRE(std::filesystem::exists(file_path));

            // Clean up
            std::filesystem::remove_all(parent_dir);
        } catch (...) {
            // Clean up on error
            if (std::filesystem::exists(parent_dir)) {
                std::filesystem::remove_all(parent_dir);
            }
            throw;
        }
    }
}

TEST_CASE("ConfigSerializer error handling", "[serializer][config]") {
    ConfigSerializer serializer;

    SECTION("Deserialize invalid YAML throws error") {
        std::string bad_yaml = "bad: [yaml: syntax";

        REQUIRE_THROWS_AS(
            serializer.deserializeEndpointConfigFromYaml(bad_yaml),
            std::runtime_error
        );
    }

    SECTION("Error message includes YAML parse error") {
        std::string bad_yaml = "invalid: yaml: content:";

        try {
            serializer.deserializeEndpointConfigFromYaml(bad_yaml);
            REQUIRE(false);  // Should throw
        } catch (const std::runtime_error& e) {
            std::string error_msg(e.what());
            REQUIRE((error_msg.find("YAML") != std::string::npos || error_msg.find("parsing") != std::string::npos));
        }
    }
}

TEST_CASE("ConfigSerializer special cases", "[serializer][config]") {
    ConfigSerializer serializer;

    SECTION("Serialize endpoint with special characters in path") {
        EndpointConfig ep = createRestEndpoint("/api/v1/customers-{id}/details");

        std::string yaml = serializer.serializeEndpointConfigToYaml(ep);
        EndpointConfig restored = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(restored.urlPath == "/api/v1/customers-{id}/details");
    }

    SECTION("Serialize endpoint with empty optional fields") {
        EndpointConfig ep = createRestEndpoint();
        // All optional fields are empty

        std::string yaml = serializer.serializeEndpointConfigToYaml(ep);

        // Should still be valid YAML
        EndpointConfig restored = serializer.deserializeEndpointConfigFromYaml(yaml);
        REQUIRE(restored.urlPath == ep.urlPath);
    }

    SECTION("Default method is GET") {
        std::string yaml = R"(
url-path: /test
template-source: test.sql
connection: [default]
)";
        // No method specified

        EndpointConfig ep = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(ep.method == "GET");
    }

    SECTION("Single connection as string") {
        std::string yaml = R"(
url-path: /test
method: GET
template-source: test.sql
connection: default
)";

        EndpointConfig ep = serializer.deserializeEndpointConfigFromYaml(yaml);

        REQUIRE(ep.connection.size() == 1);
        REQUIRE(ep.connection[0] == "default");
    }
}

TEST_CASE("ConfigSerializer complex endpoint", "[serializer][config]") {
    ConfigSerializer serializer;

    SECTION("Complex endpoint with all features") {
        std::string yaml = R"(
url-path: /api/v1/customers
method: POST
mcp-tool:
  name: create_customer
  description: Create a new customer
  result-mime-type: application/json
template-source: customers/create.sql
connection: [primary, cache]
request:
  - field-name: name
    field-in: body
    required: true
    validators:
      - type: string
        min-length: 1
        max-length: 200
  - field-name: email
    field-in: body
    required: true
    validators:
      - type: email
cache:
  enabled: true
  table: customers_cache
  schedule: 1h
auth:
  enabled: true
  type: jwt
rate-limit:
  enabled: true
  max: 100
  interval: 60
)";

        EndpointConfig ep = serializer.deserializeEndpointConfigFromYaml(yaml);

        // Verify REST
        REQUIRE(ep.urlPath == "/api/v1/customers");
        REQUIRE(ep.method == "POST");

        // Verify MCP
        REQUIRE(ep.mcp_tool.has_value());
        REQUIRE(ep.mcp_tool->name == "create_customer");

        // Verify request fields
        REQUIRE(ep.request_fields.size() == 2);

        // Verify cache
        REQUIRE(ep.cache.enabled);

        // Verify auth
        REQUIRE(ep.auth.enabled);

        // Verify rate limit
        REQUIRE(ep.rate_limit.enabled);

        // Round-trip
        std::string yaml_out = serializer.serializeEndpointConfigToYaml(ep);
        EndpointConfig ep2 = serializer.deserializeEndpointConfigFromYaml(yaml_out);

        REQUIRE(ep2.urlPath == ep.urlPath);
        REQUIRE(ep2.method == ep.method);
        REQUIRE(ep2.request_fields.size() == ep.request_fields.size());
    }
}
