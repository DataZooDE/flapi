// Regression tests for configuration-parsing fixes (#211, cluster A):
// optional project-description, documented validator spellings, load-time
// "has no effect" warnings, the `warn` log level, and OIDC preset
// placeholders / the `azure` provider alias.
#include <catch2/catch_test_macros.hpp>
#include <crow/logging.h>

#include <mutex>
#include <string>
#include <vector>

#include "config_manager.hpp"
#include "oidc_provider_presets.hpp"
#include "test_utils.hpp"

namespace flapi {
namespace test {

namespace {

const char* kBase = R"(
project-name: parse-fixes
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

// Captures crow log lines for the lifetime of the object.
class LogCapture : public crow::ILogHandler {
public:
    LogCapture() {
        _previous_level = crow::logger::get_current_log_level();
        crow::logger::setLogLevel(crow::LogLevel::Warning);
        crow::logger::setHandler(this);
    }
    ~LogCapture() override {
        static crow::CerrLogHandler restore;
        crow::logger::setHandler(&restore);
        crow::logger::setLogLevel(_previous_level);
    }
    void log(const std::string& message, crow::LogLevel level) override {
        if (level >= crow::LogLevel::Warning) {
            std::lock_guard<std::mutex> lock(_mutex);
            _lines.push_back(std::move(message));
        }
    }
    bool contains(const std::string& needle) {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& line : _lines) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    std::mutex _mutex;
    std::vector<std::string> _lines;
    crow::LogLevel _previous_level;
};

}  // namespace

TEST_CASE("project-description is optional", "[config_manager][parse_fixes]") {
    TempTestConfig temp(kBase, "parse_no_description");
    auto cm = temp.createConfigManager();
    REQUIRE(cm->getProjectDescription().empty());
}

TEST_CASE("Validators accept the documented `values` and `pattern` keys", "[config_manager][parse_fixes][validators]") {
    TempTestConfig temp(kBase, "parse_validator_aliases");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("v.yaml", R"(
url-path: /v
template-source: q.sql
connection: [test]
request:
  - field-name: status
    field-in: query
    validators:
      - type: enum
        values: [active, inactive]
  - field-name: legacy_status
    field-in: query
    validators:
      - type: enum
        allowedValues: [a, b]
  - field-name: code
    field-in: query
    validators:
      - type: string
        pattern: "^[A-Z]{3}$"
  - field-name: legacy_code
    field-in: query
    validators:
      - type: string
        regex: "^[0-9]+$"
)");
    auto cm = temp.createConfigManager();
    auto ep = cm->getEndpointForPathAndMethod("/v", "GET");
    REQUIRE(ep);
    REQUIRE(ep->request_fields.size() == 4);
    REQUIRE(ep->request_fields[0].validators[0].allowedValues == std::vector<std::string>{"active", "inactive"});
    REQUIRE(ep->request_fields[1].validators[0].allowedValues == std::vector<std::string>{"a", "b"});
    REQUIRE(ep->request_fields[2].validators[0].regex == "^[A-Z]{3}$");
    REQUIRE(ep->request_fields[3].validators[0].regex == "^[0-9]+$");

    SECTION("the JSON (config-service) path accepts them too") {
        auto json = crow::json::load(R"({"url-path": "/w", "template-source": "q.sql",
            "request": [{"field-name": "s", "field-in": "query",
                         "validators": [{"type": "enum", "values": ["x", "y"]}]},
                        {"field-name": "p", "field-in": "query",
                         "validators": [{"type": "string", "pattern": "^a$"}]}]})");
        REQUIRE(json);
        auto parsed = cm->deserializeEndpointConfig(json);
        REQUIRE(parsed.request_fields.size() == 2);
        REQUIRE(parsed.request_fields[0].validators[0].allowedValues == std::vector<std::string>{"x", "y"});
        REQUIRE(parsed.request_fields[1].validators[0].regex == "^a$");
    }
}

TEST_CASE("An enum validator without values is a load error", "[config_manager][parse_fixes][validators]") {
    TempTestConfig temp(kBase, "parse_enum_missing");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("v.yaml", R"(
url-path: /v
template-source: q.sql
connection: [test]
request:
  - field-name: status
    field-in: query
    validators:
      - type: enum
)");
    // The configuration fails to load, and the error names the key.
    auto cm = std::make_shared<ConfigManager>(temp.configPath());
    try {
        cm->loadConfig();
        FAIL("loadConfig accepted an enum validator without values");
    } catch (const std::exception& e) {
        REQUIRE(std::string(e.what()).find("values") != std::string::npos);
    }
}

TEST_CASE("Inert keys warn at load", "[config_manager][parse_fixes][warnings]") {
    const std::string config = std::string(R"(
project-name: parse-fixes
server-name: my-server
storage:
  cache:
    enabled: true
template:
  path: ./sqls
connections:
  test:
    log-queries: true
    log-parameters: true
    allow: '*'
    properties:
      path: ./data.parquet
)");
    TempTestConfig temp(config, "parse_inert_keys");
    temp.writeSqlTemplate("q.sql", "SELECT 1");
    temp.writeEndpoint("e.yaml", R"(
url-path: /e
template-source: q.sql
connection: [test]
request:
  - field-name: id
    field-in: query
    field-type: integer
)");
    LogCapture capture;
    auto cm = temp.createConfigManager();
    REQUIRE(capture.contains("`server-name` has no effect"));
    REQUIRE(capture.contains("`storage:` has no effect"));
    REQUIRE(capture.contains("`connections.test.log-queries` has no effect"));
    REQUIRE(capture.contains("`connections.test.log-parameters` has no effect"));
    REQUIRE(capture.contains("`connections.test.allow` has no effect"));
    REQUIRE(capture.contains("`field-type` has no effect"));
}

TEST_CASE("log-level accepts warn and rejects unknown values", "[config_manager][parse_fixes][log_level]") {
    SECTION("warn is an alias of warning") {
        TempTestConfig temp(std::string(kBase) + "log-level: warn\n", "parse_log_warn");
        auto cm = temp.createConfigManager();
        REQUIRE(cm->getLogLevel() == "warning");
    }
    SECTION("an unknown value is a startup error naming the key") {
        TempTestConfig temp(std::string(kBase) + "log-level: verbose\n", "parse_log_bad");
        auto cm = std::make_shared<ConfigManager>(temp.configPath());
        try {
            cm->loadConfig();
            FAIL("loadConfig accepted log-level: verbose");
        } catch (const std::exception& e) {
            const std::string what = e.what();
            REQUIRE(what.find("log-level") != std::string::npos);
            REQUIRE(what.find("verbose") != std::string::npos);
        }
    }
}

TEST_CASE("OIDC presets substitute their issuer-url placeholders", "[oidc][parse_fixes]") {
    SECTION("microsoft tenant-id fills {tenant}") {
        OIDCConfig c;
        c.provider_type = "microsoft";
        c.client_id = "client";
        c.tenant_id = "contoso-tenant";
        OIDCProviderPresets::applyPreset(c);
        REQUIRE(c.issuer_url == "https://login.microsoftonline.com/contoso-tenant/v2.0");
        REQUIRE(OIDCProviderPresets::validateProviderConfig(c).empty());
    }
    SECTION("azure is an alias of microsoft") {
        OIDCConfig c;
        c.provider_type = "azure";
        c.client_id = "client";
        c.tenant_id = "t1";
        REQUIRE(OIDCProviderPresets::applyPreset(c));
        REQUIRE(c.provider_type == "microsoft");
        REQUIRE(c.issuer_url == "https://login.microsoftonline.com/t1/v2.0");
    }
    SECTION("keycloak domain + realm") {
        OIDCConfig c;
        c.provider_type = "keycloak";
        c.client_id = "client";
        c.domain = "sso.example.org";
        c.realm = "staff";
        OIDCProviderPresets::applyPreset(c);
        REQUIRE(c.issuer_url == "https://sso.example.org/realms/staff");
        REQUIRE(OIDCProviderPresets::validateProviderConfig(c).empty());
    }
    SECTION("a literal issuer URL is valid without any placeholder") {
        OIDCConfig c;
        c.provider_type = "keycloak";
        c.client_id = "client";
        c.issuer_url = "https://kc.example.org/realms/main";
        OIDCProviderPresets::applyPreset(c);
        REQUIRE(c.issuer_url == "https://kc.example.org/realms/main");
        REQUIRE(OIDCProviderPresets::validateProviderConfig(c).empty());
    }
    SECTION("an unfilled placeholder is reported with the key to set") {
        OIDCConfig c;
        c.provider_type = "microsoft";
        c.client_id = "client";
        OIDCProviderPresets::applyPreset(c);
        const auto problem = OIDCProviderPresets::validateProviderConfig(c);
        REQUIRE(problem.find("{tenant}") != std::string::npos);
        REQUIRE(problem.find("tenant-id") != std::string::npos);
    }
}

TEST_CASE("An OIDC block with an unfillable placeholder fails at load", "[oidc][parse_fixes]") {
    const std::string config = std::string(kBase) + R"(
auth:
  enabled: true
  type: oidc
  oidc:
    provider-type: okta
    client-id: abc
)";
    TempTestConfig temp(config, "parse_oidc_placeholder");
    auto cm = std::make_shared<ConfigManager>(temp.configPath());
    try {
        cm->loadConfig();
        FAIL("loadConfig accepted an okta preset without `domain`");
    } catch (const std::exception& e) {
        REQUIRE(std::string(e.what()).find("{domain}") != std::string::npos);
    }

    TempTestConfig ok(std::string(kBase) + R"(
auth:
  enabled: true
  type: oidc
  oidc:
    provider-type: okta
    client-id: abc
    domain: dev-123
)", "parse_oidc_placeholder_ok");
    auto cm_ok = ok.createConfigManager();
    REQUIRE(cm_ok->getGlobalOIDCConfig().has_value());
    REQUIRE(cm_ok->getGlobalOIDCConfig()->domain == "dev-123");
}

}  // namespace test
}  // namespace flapi
