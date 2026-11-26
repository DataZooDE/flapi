// OIDC claim mapping and configuration (#281): explicit claims beat presets,
// the Auth0 issuer and role defaults, the refused github preset, keys that are
// parsed but have no effect, and auth.groups in templates.
#include <catch2/catch_test_macros.hpp>
#include <crow/logging.h>

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "config_manager.hpp"
#include "mustache_syntax_check.hpp"
#include "oidc_auth_handler.hpp"
#include "oidc_provider_presets.hpp"
#include "sql_template_processor.hpp"
#include "test_utils.hpp"

namespace flapi {
namespace test {

namespace {

const char* kBase = R"(
project-name: oidc-claims
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

class WarningCapture : public crow::ILogHandler {
public:
    WarningCapture() {
        _previous_level = crow::logger::get_current_log_level();
        crow::logger::setLogLevel(crow::LogLevel::Warning);
        crow::logger::setHandler(this);
    }
    ~WarningCapture() override {
        static crow::CerrLogHandler restore;
        crow::logger::setHandler(&restore);
        crow::logger::setLogLevel(_previous_level);
    }
    void log(const std::string& message, crow::LogLevel level) override {
        if (level >= crow::LogLevel::Warning) {
            std::lock_guard<std::mutex> lock(_mutex);
            _lines.push_back(message);
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

OIDCConfig loadGlobalOidc(const std::string& oidc_block, const std::string& name) {
    TempTestConfig temp(std::string(kBase) + "auth:\n  enabled: true\n  type: oidc\n  oidc:\n" + oidc_block, name);
    auto cm = temp.createConfigManager();
    REQUIRE(cm->getGlobalOIDCConfig().has_value());
    return *cm->getGlobalOIDCConfig();
}

}  // namespace

TEST_CASE("An explicit username-claim wins over the provider preset", "[oidc][oidc_claims]") {
    for (const std::string provider : {"google", "microsoft", "keycloak", "auth0", "okta"}) {
        DYNAMIC_SECTION(provider) {
            auto config = loadGlobalOidc("    provider-type: " + provider +
                                             "\n    issuer-url: https://idp.example.com/x\n"
                                             "    client-id: api\n    username-claim: sub\n",
                                         "oidc_explicit_username_" + provider);
            OIDCProviderPresets::applyPreset(config);
            REQUIRE(config.username_claim == "sub");
        }
    }
    SECTION("without one, the preset still applies") {
        auto config = loadGlobalOidc("    provider-type: keycloak\n    issuer-url: https://kc.example.com/realms/a\n"
                                     "    client-id: api\n",
                                     "oidc_preset_username");
        OIDCProviderPresets::applyPreset(config);
        REQUIRE(config.username_claim == "preferred_username");
    }
}

TEST_CASE("The Auth0 preset issuer ends with a slash, as Auth0's iss does", "[oidc][oidc_claims]") {
    SECTION("a tenant name") {
        OIDCConfig c;
        c.provider_type = "auth0";
        c.client_id = "api";
        c.domain = "acme";
        OIDCProviderPresets::applyPreset(c);
        REQUIRE(c.issuer_url == "https://acme.auth0.com/");
    }
    SECTION("a full domain (regional or custom)") {
        OIDCConfig c;
        c.provider_type = "auth0";
        c.client_id = "api";
        c.domain = "acme.eu.auth0.com";
        OIDCProviderPresets::applyPreset(c);
        REQUIRE(c.issuer_url == "https://acme.eu.auth0.com/");
    }
    SECTION("a literal issuer-url gets the slash too") {
        OIDCConfig c;
        c.provider_type = "auth0";
        c.client_id = "api";
        c.issuer_url = "https://login.acme.com";
        OIDCProviderPresets::applyPreset(c);
        REQUIRE(c.issuer_url == "https://login.acme.com/");
    }
    SECTION("no placeholder role path is invented") {
        OIDCConfig c;
        c.provider_type = "auth0";
        c.client_id = "api";
        c.domain = "acme";
        OIDCProviderPresets::applyPreset(c);
        REQUIRE(c.role_claim_path.empty());
        REQUIRE(c.roles_claim == "roles");
    }
}

TEST_CASE("provider-type github is refused at load", "[oidc][oidc_claims]") {
    TempTestConfig temp(std::string(kBase) + R"(
auth:
  enabled: true
  type: oidc
  oidc:
    provider-type: github
    client-id: abc
)", "oidc_github_refused");
    auto cm = std::make_shared<ConfigManager>(temp.configPath());
    try {
        cm->loadConfig();
        FAIL("loadConfig accepted provider-type: github");
    } catch (const std::exception& e) {
        REQUIRE(std::string(e.what()).find("https://token.actions.githubusercontent.com") != std::string::npos);
    }
}

TEST_CASE("OIDC keys flAPI never reads are reported at load", "[oidc][oidc_claims]") {
    WarningCapture capture;
    loadGlobalOidc("    issuer-url: https://idp.example.com\n    client-id: api\n"
                   "    client-secret: s3cret\n    scopes: [openid]\n"
                   "    enable-client-credentials: true\n    enable-refresh-tokens: true\n",
                   "oidc_unused_keys");
    for (const std::string key : {"client-secret", "scopes", "enable-client-credentials", "enable-refresh-tokens"}) {
        DYNAMIC_SECTION(key) {
            REQUIRE(capture.contains("`" + key + "` is parsed but has no effect"));
        }
    }
}

// Groups travel as one __auth_groups value, items separated by U+001F (a
// control character, which no identity value may contain).
#define US "\x1f"

TEST_CASE("auth.groups and auth.role_list are lists, one quote-doubled item each",
          "[oidc][oidc_claims][auth_quoting][auth_lists]") {
    TempTestConfig temp(kBase, "oidc_auth_groups_render");
    auto cm = temp.createConfigManager();
    SQLTemplateProcessor processor(cm);
    const auto Render = [&](const std::string& body, std::map<std::string, std::string> params) {
        temp.writeSqlTemplate("g.sql", body);
        EndpointConfig endpoint;
        endpoint.templateSource = "g.sql";
        return processor.loadAndProcessTemplate(endpoint, params);
    };
    const std::string hostile = "a'b,c]d\\e \xC3\xBC";   // ' , ] \ and a UTF-8 letter
    const std::string doubled = "a''b,c]d\\e \xC3\xBC";

    SECTION("the section form renders each item in its own literal") {
        REQUIRE(Render("SELECT {{#auth.groups}}'{{.}}', {{/auth.groups}}1",
                       {{"__auth_groups", "CN=x,OU=y" US "O'Brien"}}) == "SELECT 'CN=x,OU=y', 'O''Brien', 1");
        REQUIRE(Render("SELECT {{#auth.groups}}'{{.}}' {{/auth.groups}}", {{"__auth_groups", hostile}}) ==
                "SELECT '" + doubled + "' ");
    }
    SECTION("auth.groups_sql is a ready list literal") {
        REQUIRE(Render("SELECT list_contains({{{ auth.groups_sql }}}, 'admin')",
                       {{"__auth_groups", "CN=x,OU=y" US + hostile}}) ==
                "SELECT list_contains(['CN=x,OU=y','" + doubled + "']::VARCHAR[], 'admin')");
        REQUIRE(Render("SELECT {{ auth.groups_sql }}", {{"__auth_authenticated", "true"}}) ==
                "SELECT []::VARCHAR[]");
        REQUIRE(Render("SELECT {{{ auth.groups_sql }}}", {}) == "SELECT []::VARCHAR[]");
    }
    SECTION("roles: the comma string stays, role_list and roles_sql are new") {
        const std::map<std::string, std::string> p = {{"__auth_roles", "analyst,O'Neil"}};
        REQUIRE(Render("SELECT '{{{ auth.roles }}}'", p) == "SELECT 'analyst,O''Neil'");
        REQUIRE(Render("SELECT {{#auth.role_list}}'{{.}}' {{/auth.role_list}}", p) == "SELECT 'analyst' 'O''Neil' ");
        REQUIRE(Render("SELECT {{{ auth.roles_sql }}}", p) == "SELECT ['analyst','O''Neil']::VARCHAR[]");
    }
    SECTION("no groups: the section is skipped, the inverted one renders") {
        REQUIRE(Render("SELECT {{#auth.groups}}'{{.}}'{{/auth.groups}}{{^auth.groups}}0{{/auth.groups}}",
                       {{"__auth_authenticated", "true"}}) == "SELECT 0");
    }
    SECTION("a control character in a group is refused") {
        REQUIRE_THROWS_AS(Render("SELECT {{#auth.groups}}'{{.}}'{{/auth.groups}}",
                                 {{"__auth_groups", "staff" US "bad\nOR 1=1"}}),
                          std::invalid_argument);
        REQUIRE_THROWS_AS(Render("SELECT {{{ auth.groups_sql }}}", {{"__auth_groups", "bad\rx"}}),
                          std::invalid_argument);
    }
}

TEST_CASE("Placement of list items and SQL list literals", "[oidc][oidc_claims][auth_quoting][auth_lists]") {
    const auto refused = [](const std::string& tpl) { return CheckAuthPlacement(tpl, "t.sql").has_value(); };
    SECTION("items must sit in a plain literal") {
        REQUIRE_FALSE(refused("SELECT * FROM t WHERE g IN ({{#auth.groups}}'{{.}}',{{/auth.groups}} '')"));
        REQUIRE(refused("SELECT * FROM t WHERE g IN ({{#auth.groups}}{{.}},{{/auth.groups}} '')"));
        REQUIRE(refused("SELECT {{#auth.groups}}E'{{.}}'{{/auth.groups}}"));
        REQUIRE(refused("SELECT {{#auth.role_list}}\"{{.}}\"{{/auth.role_list}}"));
        REQUIRE(refused("SELECT * FROM query('SELECT 1 WHERE g = ''{{#auth.groups}}{{.}}{{/auth.groups}}''')"));
    }
    SECTION("a list is not a value: a plain tag on it is refused") {
        REQUIRE(refused("SELECT '{{{ auth.groups }}}'"));
        REQUIRE(refused("SELECT {{{ auth.role_list }}}"));
        REQUIRE(refused("{{#auth}}SELECT '{{groups}}'{{/auth}}"));
    }
    SECTION("the _sql forms stand where a value does") {
        REQUIRE_FALSE(refused("SELECT * FROM t WHERE list_contains({{{ auth.groups_sql }}}, 'admin')"));
        REQUIRE_FALSE(refused("SELECT * FROM t WHERE region IN (SELECT unnest({{ auth.roles_sql }}))"));
        REQUIRE_FALSE(refused("{{#auth}}SELECT {{{groups_sql}}}{{/auth}}"));
    }
    SECTION("...and nowhere else") {
        REQUIRE(refused("SELECT '{{{ auth.groups_sql }}}'"));
        REQUIRE(refused("SELECT 1 -- {{{ auth.groups_sql }}}"));
        REQUIRE(refused("SELECT 1 /* {{{ auth.roles_sql }}} */"));
        REQUIRE(refused("SELECT 1 AS \"{{{ auth.groups_sql }}}\""));
        REQUIRE(refused("SELECT $${{{ auth.groups_sql }}}$$"));
        REQUIRE(refused("SELECT * FROM query({{{ auth.groups_sql }}})"));
        REQUIRE(refused("SELECT * FROM postgres_query('db', {{{ auth.roles_sql }}})"));
        REQUIRE(refused("SELECT * FROM query('SELECT ' || {{{ auth.groups_sql }}})"));
        REQUIRE(refused("SELECT my_macro('SELECT $q$', {{{ auth.groups_sql }}})"));
        REQUIRE(refused("SELECT E{{! c }}'{{{ auth.groups_sql }}}'"));
    }
}

TEST_CASE("OIDC accepts RS*, PS* and ES* and nothing else", "[oidc][oidc_algorithms]") {
    for (const std::string name : {"RS256", "RS384", "RS512", "PS256", "PS384", "PS512"}) {
        DYNAMIC_SECTION(name) {
            const auto* alg = FindJwsAlgorithm(name);
            REQUIRE(alg != nullptr);
            REQUIRE(alg->ec_curve == nullptr);
            REQUIRE(alg->pss == (name[0] == 'P'));
        }
    }
    REQUIRE(std::string(FindJwsAlgorithm("ES256")->ec_curve) == "P-256");
    REQUIRE(FindJwsAlgorithm("ES384")->ec_size == 48);
    REQUIRE(FindJwsAlgorithm("ES512")->ec_size == 66);
    for (const std::string refused : {"none", "None", "HS256", "HS384", "HS512", "EdDSA", "ES256K", "rs256", ""}) {
        DYNAMIC_SECTION("refused: " << refused) {
            REQUIRE(FindJwsAlgorithm(refused) == nullptr);
        }
    }
}

}  // namespace test
}  // namespace flapi
