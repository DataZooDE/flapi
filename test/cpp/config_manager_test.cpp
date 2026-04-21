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

crow::json::rvalue wvalueToRValue(const crow::json::wvalue& wval) {
    return crow::json::load(wval.dump());
}

TEST_CASE("ConfigManager basic functionality", "[config_manager]") {
    
    std::string temp_template_dir = createTempDir();

    SECTION("Load valid configuration") {
        std::string yaml_content = R"(
project-name: TestProject
project-description: Test Description
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
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./data/cache.ducklake
  data-path: ./data/cache
  retention:
    keep-last-snapshots: 10
    max-snapshot-age: 30d
  compaction:
    enabled: true
    schedule: '@daily'
  scheduler:
    enabled: true
    scan-interval: 5m
)";

        std::string config_file = createTempYamlFile(yaml_content);
        ConfigManager mgr{std::filesystem::path(config_file)};
        mgr.loadConfig();

        REQUIRE(mgr.getProjectName() == "TestProject");
        REQUIRE(mgr.getProjectDescription() == "Test Description");
        REQUIRE(mgr.isHttpsEnforced() == true);
        REQUIRE(mgr.isAuthEnabled() == false);

        const auto& ducklake_config = mgr.getDuckLakeConfig();
        REQUIRE(ducklake_config.enabled);
        REQUIRE(ducklake_config.alias == "cache");
        REQUIRE(ends_with(ducklake_config.metadata_path, "data/cache.ducklake"));
        REQUIRE(ends_with(ducklake_config.data_path, "data/cache"));
        REQUIRE(ducklake_config.retention.keep_last_snapshots == 10);
        REQUIRE(ducklake_config.retention.max_snapshot_age.value() == "30d");
        REQUIRE(ducklake_config.compaction.enabled);
        REQUIRE(ducklake_config.compaction.schedule.value() == "@daily");
        REQUIRE(ducklake_config.scheduler.enabled);
        REQUIRE(ducklake_config.scheduler.scan_interval.value() == "5m");

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
project-name: TestProject
project-description: Test Description
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
  enabled: true
  table: test_cache
  schema: analytics
  schedule: 10m
  primary-key: [id]
  cursor:
    column: updated_at
    type: timestamp
  rollback-window: 2d
  retention:
    keep-last-snapshots: 5
    max-snapshot-age: 14d
  delete-handling: soft

)";

        std::string endpoint_file = createTempYamlFile(endpoint_yaml, "endpoint_config.yaml");
        mgr.loadEndpointConfig(endpoint_file);

        const auto& endpoints = mgr.getEndpoints();
        REQUIRE(endpoints.size() == 1);
        const auto& endpoint = endpoints[0];

        REQUIRE(endpoint.urlPath == "/test");
        // Template source is resolved relative to endpoint config file's directory
        // Note: templateSource is not canonicalized, so we compare without canonical()
        std::filesystem::path endpoint_dir = std::filesystem::path(endpoint_file).parent_path();
        std::filesystem::path expected_template = endpoint_dir / "test.sql";
        REQUIRE(endpoint.templateSource == expected_template.string());
        REQUIRE(endpoint.connection == std::vector<std::string>{"default"});

        REQUIRE(endpoint.request_fields.size() == 1);
        const auto& field = endpoint.request_fields[0];
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

        REQUIRE(endpoint.cache.enabled == true);
        REQUIRE(endpoint.cache.table == "test_cache");
        REQUIRE(endpoint.cache.schema == "analytics");
        REQUIRE(endpoint.cache.schedule.value() == "10m");
        REQUIRE(endpoint.cache.primary_keys == std::vector<std::string>{"id"});
        REQUIRE(endpoint.cache.cursor.has_value());
        REQUIRE(endpoint.cache.cursor->column == "updated_at");
        REQUIRE(endpoint.cache.cursor->type == "timestamp");
        REQUIRE(endpoint.cache.rollback_window.value() == "2d");
        REQUIRE(endpoint.cache.retention.keep_last_snapshots.value() == 5);
        REQUIRE(endpoint.cache.retention.max_snapshot_age.value() == "14d");
        REQUIRE(endpoint.cache.delete_handling.value() == "soft");
    }

    std::filesystem::remove_all(temp_template_dir);
}

TEST_CASE("ConfigManager getEndpointForPath", "[config_manager]") {
    std::string temp_template_dir = createTempDir();

    std::string yaml_content = R"(
project-name: TestProject
project-description: Test Description
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
project-name: TestProject
project-description: Test Description
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
project-name: TestProject
project-description: Test Description
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

TEST_CASE("ConfigManager replace and remove endpoints", "[config_manager]") {
    ConfigManager mgr{std::filesystem::path("/tmp/flapi_config.yaml")};

    EndpointConfig restEndpoint;
    restEndpoint.urlPath = "/rest";
    restEndpoint.method = "GET";
    restEndpoint.templateSource = "rest.sql";
    restEndpoint.connection = {"default"};
    restEndpoint.request_fields_validation = true;
    restEndpoint.with_pagination = true;

    RequestFieldConfig field;
    field.fieldName = "id";
    field.fieldIn = "query";
    field.description = "Identifier";
    field.required = true;
    restEndpoint.request_fields.push_back(field);

    // Initialize auth config
    restEndpoint.auth.enabled = false;
    restEndpoint.auth.type = "";

    // Initialize cache config
    restEndpoint.cache.enabled = true;
    restEndpoint.cache.table = "cache_table";
    restEndpoint.cache.schema = "cache";
    restEndpoint.cache.schedule = "1h";
    restEndpoint.cache.template_file = "cache.sql";

    // Initialize rate limit config
    restEndpoint.rate_limit.enabled = false;
    restEndpoint.rate_limit.max = 0;
    restEndpoint.rate_limit.interval = 0;

    // Initialize heartbeat config
    restEndpoint.heartbeat.enabled = false;

    mgr.addEndpoint(restEndpoint);

    SECTION("replace existing rest endpoint") {
        EndpointConfig replacement = restEndpoint;
        replacement.templateSource = "updated.sql";

        REQUIRE(mgr.replaceEndpoint(replacement));
        const auto* found = mgr.getEndpointForPath("/rest");
        REQUIRE(found != nullptr);
        REQUIRE(found->templateSource == "updated.sql");
    }

    SECTION("replace fails when endpoint missing") {
        EndpointConfig missing = restEndpoint;
        missing.urlPath = "/missing";
        REQUIRE_FALSE(mgr.replaceEndpoint(missing));
    }

    SECTION("remove by path succeeds") {
        REQUIRE(mgr.removeEndpointByPath("/rest"));
        REQUIRE(mgr.getEndpointForPath("/rest") == nullptr);
    }

    SECTION("remove is idempotent") {
        REQUIRE(mgr.removeEndpointByPath("/rest"));
        REQUIRE_FALSE(mgr.removeEndpointByPath("/rest"));
    }

    SECTION("serialize hyphen case") {
        auto json = mgr.serializeEndpointConfig(restEndpoint, EndpointJsonStyle::HyphenCase);
        auto rjson = wvalueToRValue(json);
        REQUIRE(rjson["url-path"].s() == "/rest");
        REQUIRE(rjson["template-source"].s() == "rest.sql");
        REQUIRE(rjson["cache"]["template-file"].s() == "cache.sql");
    }

    SECTION("serialize camel case") {
        auto json = mgr.serializeEndpointConfig(restEndpoint, EndpointJsonStyle::CamelCase);
        auto rjson = wvalueToRValue(json);
        REQUIRE(rjson["urlPath"].s() == "/rest");
        REQUIRE(rjson["templateSource"].s() == "rest.sql");
        REQUIRE(rjson["cache"]["templateFile"].s() == "cache.sql");
    }

    SECTION("deserialize accepts aliases") {
        crow::json::wvalue payload;
        payload["url_path"] = "/alias";
        payload["template-source"] = "alias.sql";
        payload["connection"] = std::vector<std::string>{"default"};
        payload["request-fields-validation"] = true;
        payload["request"][0]["fieldName"] = "name";
        payload["request"][0]["fieldIn"] = "query";
        payload["request"][0]["description"] = "Name";
        payload["request"][0]["required"] = true;

        auto config = mgr.deserializeEndpointConfig(wvalueToRValue(payload));
        REQUIRE(config.urlPath == "/alias");
        REQUIRE(config.templateSource == "alias.sql");
        REQUIRE(config.request_fields_validation);
        REQUIRE(config.request_fields.size() == 1);
        REQUIRE(config.request_fields[0].fieldName == "name");
    }
}

TEST_CASE("ConfigManager telemetry config", "[config_manager]") {
    std::string temp_template_dir = createTempDir();

    SECTION("telemetry.enabled: false disables telemetry") {
        std::string yaml_content = R"(
project-name: TelemetryTest
project-description: Test
template:
  path: )" + temp_template_dir + R"(
telemetry:
  enabled: false
)";
        std::string config_file = createTempYamlFile(yaml_content, "telemetry_false.yaml");
        ConfigManager mgr{std::filesystem::path(config_file)};
        mgr.loadConfig();

        REQUIRE(mgr.isTelemetryEnabled() == false);
    }

    SECTION("telemetry.enabled: true keeps telemetry on") {
        std::string yaml_content = R"(
project-name: TelemetryTest
project-description: Test
template:
  path: )" + temp_template_dir + R"(
telemetry:
  enabled: true
)";
        std::string config_file = createTempYamlFile(yaml_content, "telemetry_true.yaml");
        ConfigManager mgr{std::filesystem::path(config_file)};
        mgr.loadConfig();

        REQUIRE(mgr.isTelemetryEnabled() == true);
    }

    SECTION("no telemetry key defaults to enabled") {
        std::string yaml_content = R"(
project-name: TelemetryTest
project-description: Test
template:
  path: )" + temp_template_dir + R"(
)";
        std::string config_file = createTempYamlFile(yaml_content, "telemetry_absent.yaml");
        ConfigManager mgr{std::filesystem::path(config_file)};
        mgr.loadConfig();

        REQUIRE(mgr.isTelemetryEnabled() == true);
    }
}

// ── Bug #17 ────────────────────────────────────────────────────────────────
// OIDC config not parsed at endpoint level; global OIDC not propagated
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("ConfigManager: OIDC block parsed at endpoint level", "[config_manager][oidc][bug17]") {
    // Use a unique temp dir to avoid shared-state collisions between tests
    namespace fs = std::filesystem;
    fs::path temp_ep_dir = fs::temp_directory_path() / "flapi_oidc_ep_test";
    fs::create_directories(temp_ep_dir);

    // Write endpoint YAML into the template dir before loadConfig
    std::string endpoint_yaml = R"(
url-path: /secure
template-source: secure.sql
connection:
  - default
auth:
  enabled: true
  type: oidc
  oidc:
    issuer-url: https://accounts.google.com
    allowed-audiences:
      - my-client-id
    username-claim: email
)";
    {
        std::ofstream f(temp_ep_dir / "oidc_secure.yaml");
        f << endpoint_yaml;
    }

    std::string config_yaml = R"(
project-name: OIDCTest
project-description: Test
template:
  path: )" + temp_ep_dir.string() + R"(
connections:
  default:
    properties:
      db_file: ./data/test.db
)";

    std::string config_file = createTempYamlFile(config_yaml, "oidc_ep_test_config.yaml");
    ConfigManager mgr{fs::path(config_file)};
    mgr.loadConfig();

    SECTION("endpoint with oidc block has oidc config populated") {
        const auto& endpoints = mgr.getEndpoints();
        REQUIRE(endpoints.size() == 1);
        const auto& ep = endpoints[0];

        REQUIRE(ep.auth.enabled == true);
        REQUIRE(ep.auth.type == "oidc");
        REQUIRE(ep.auth.oidc.has_value());
        REQUIRE(ep.auth.oidc->issuer_url == "https://accounts.google.com");
        REQUIRE(ep.auth.oidc->allowed_audiences == std::vector<std::string>{"my-client-id"});
        REQUIRE(ep.auth.oidc->username_claim == "email");
    }

    fs::remove_all(temp_ep_dir);
}

TEST_CASE("ConfigManager: global OIDC config propagated to endpoints", "[config_manager][oidc][bug17]") {
    namespace fs = std::filesystem;
    fs::path temp_template_dir = fs::temp_directory_path() / "flapi_oidc_global_test";
    fs::create_directories(temp_template_dir);
    std::string temp_template_dir_str = temp_template_dir.string();

    // Global flapi.yaml with auth.oidc block
    std::string config_yaml = R"(
project-name: OIDCPropagationTest
project-description: Test
template:
  path: )" + temp_template_dir_str + R"(
connections:
  default:
    properties:
      db_file: ./data/test.db
auth:
  enabled: true
  type: oidc
  oidc:
    issuer-url: https://global-idp.example.com
    allowed-audiences:
      - global-audience
    username-claim: email
)";

    // Write endpoint YAML into template dir so loadConfig picks it up
    std::string endpoint_yaml = R"(
url-path: /protected
template-source: protected.sql
connection:
  - default
auth:
  enabled: true
  type: oidc
)";
    // Write endpoint into the template directory
    std::filesystem::path ep_path = temp_template_dir / "protected.yaml";
    {
        std::ofstream f(ep_path);
        f << endpoint_yaml;
    }

    std::string config_file = createTempYamlFile(config_yaml, "oidc_global_test_config.yaml");
    ConfigManager mgr{std::filesystem::path(config_file)};
    mgr.loadConfig();

    SECTION("global OIDC config is accessible via accessor") {
        auto global_oidc = mgr.getGlobalOIDCConfig();
        REQUIRE(global_oidc.has_value());
        REQUIRE(global_oidc->issuer_url == "https://global-idp.example.com");
    }

    SECTION("endpoint without local oidc block inherits global config") {
        const auto& endpoints = mgr.getEndpoints();
        REQUIRE(endpoints.size() == 1);
        const auto& ep = endpoints[0];

        REQUIRE(ep.auth.type == "oidc");
        REQUIRE(ep.auth.oidc.has_value());
        REQUIRE(ep.auth.oidc->issuer_url == "https://global-idp.example.com");
        REQUIRE(ep.auth.oidc->allowed_audiences == std::vector<std::string>{"global-audience"});
    }

    fs::remove_all(temp_template_dir);
}