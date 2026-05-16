#include <catch2/catch_test_macros.hpp>
#include <algorithm>

#include "config_manager.hpp"
#include "security_auditor.hpp"
#include "test_utils.hpp"

namespace flapi {
namespace test {

namespace {

bool hasWarning(const std::vector<SecurityWarning>& warnings, const std::string& code) {
    return std::any_of(warnings.begin(), warnings.end(),
                       [&](const SecurityWarning& w) { return w.code == code; });
}

} // namespace

TEST_CASE("SecurityAuditor: clean config produces no warnings", "[security][audit]") {
    std::string config_content = R"(
project-name: clean-project
project-description: Project with no inline users and no MCP tools
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

    TempTestConfig temp(config_content, "audit_clean");
    auto config_manager = temp.createConfigManager();

    SecurityAuditor auditor;
    auto warnings = auditor.audit(*config_manager);

    REQUIRE(warnings.empty());
}

TEST_CASE("SecurityAuditor: plaintext password on endpoint is flagged", "[security][audit][password]") {
    std::string config_content = R"(
project-name: plaintext-project
project-description: Endpoint configured with plaintext password
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

    TempTestConfig temp(config_content, "audit_plaintext");

    // Write an endpoint that configures Basic auth with a plaintext password
    temp.writeEndpoint("secret.yaml", R"(
url-path: /secret
method: GET
template-source: secret.sql
connection: [test]
auth:
  enabled: true
  type: basic
  users:
    - username: alice
      password: hunter2
      roles: [admin]
)");
    temp.writeSqlTemplate("secret.sql", "SELECT 1 AS ok");

    auto config_manager = temp.createConfigManager();
    SecurityAuditor auditor;
    auto warnings = auditor.audit(*config_manager);

    REQUIRE(hasWarning(warnings, "AUTH_PLAINTEXT_PASSWORD"));
}

TEST_CASE("SecurityAuditor: MD5-hashed password is flagged as deprecated", "[security][audit][password]") {
    std::string config_content = R"(
project-name: md5-project
project-description: Endpoint configured with MD5 password hash
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

    TempTestConfig temp(config_content, "audit_md5");

    // MD5("hunter2") = 2ab96390c7dbe3439de74d0c9b0b1767
    temp.writeEndpoint("secret.yaml", R"(
url-path: /secret
method: GET
template-source: secret.sql
connection: [test]
auth:
  enabled: true
  type: basic
  users:
    - username: alice
      password: 2ab96390c7dbe3439de74d0c9b0b1767
      roles: [admin]
)");
    temp.writeSqlTemplate("secret.sql", "SELECT 1 AS ok");

    auto config_manager = temp.createConfigManager();
    SecurityAuditor auditor;
    auto warnings = auditor.audit(*config_manager);

    REQUIRE(hasWarning(warnings, "AUTH_MD5_PASSWORD"));
    REQUIRE_FALSE(hasWarning(warnings, "AUTH_PLAINTEXT_PASSWORD"));
}

TEST_CASE("SecurityAuditor: bcrypt password produces no warning", "[security][audit][password]") {
    std::string config_content = R"(
project-name: bcrypt-project
project-description: Endpoint configured with bcrypt password hash
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

    TempTestConfig temp(config_content, "audit_bcrypt");

    // bcrypt hash for "hunter2" (cost 4): $2b$04$abcdefghijklmnopqrstuv...
    // Use a plausible-shape bcrypt string; classifier only inspects prefix.
    temp.writeEndpoint("secret.yaml", R"(
url-path: /secret
method: GET
template-source: secret.sql
connection: [test]
auth:
  enabled: true
  type: basic
  users:
    - username: alice
      password: $2b$12$Qz5e9p1m2n3o4p5q6r7s8eABCDEFGHIJKLMNOPQRSTUVWXYZabcd
      roles: [admin]
)");
    temp.writeSqlTemplate("secret.sql", "SELECT 1 AS ok");

    auto config_manager = temp.createConfigManager();
    SecurityAuditor auditor;
    auto warnings = auditor.audit(*config_manager);

    REQUIRE_FALSE(hasWarning(warnings, "AUTH_PLAINTEXT_PASSWORD"));
    REQUIRE_FALSE(hasWarning(warnings, "AUTH_MD5_PASSWORD"));
}

TEST_CASE("SecurityAuditor: MCP tool without auth is flagged", "[security][audit][mcp]") {
    std::string config_content = R"(
project-name: mcp-unauth-project
project-description: MCP tool exposed without authentication
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
mcp:
  enabled: true
  auth:
    enabled: false
)";

    TempTestConfig temp(config_content, "audit_mcp_noauth");

    temp.writeEndpoint("lookup.yaml", R"(
template-source: lookup.sql
connection: [test]
mcp-tool:
  name: customer_lookup
  description: Look up a customer by id
)");
    temp.writeSqlTemplate("lookup.sql", "SELECT 1 AS id");

    auto config_manager = temp.createConfigManager();
    SecurityAuditor auditor;
    auto warnings = auditor.audit(*config_manager);

    REQUIRE(hasWarning(warnings, "MCP_UNAUTHENTICATED_TOOLS"));
}

TEST_CASE("SecurityAuditor: MCP tool with auth enabled produces no MCP warning",
          "[security][audit][mcp]") {
    std::string config_content = R"(
project-name: mcp-auth-project
project-description: MCP tool with auth enabled
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
mcp:
  enabled: true
  auth:
    enabled: true
    type: bearer
    jwt-secret: test-secret
    jwt-issuer: test-issuer
)";

    TempTestConfig temp(config_content, "audit_mcp_auth");

    temp.writeEndpoint("lookup.yaml", R"(
template-source: lookup.sql
connection: [test]
mcp-tool:
  name: customer_lookup
  description: Look up a customer by id
)");
    temp.writeSqlTemplate("lookup.sql", "SELECT 1 AS id");

    auto config_manager = temp.createConfigManager();
    SecurityAuditor auditor;
    auto warnings = auditor.audit(*config_manager);

    REQUIRE_FALSE(hasWarning(warnings, "MCP_UNAUTHENTICATED_TOOLS"));
}

TEST_CASE("SecurityAuditor: MCP disabled overall produces no MCP warning even without auth",
          "[security][audit][mcp]") {
    std::string config_content = R"(
project-name: mcp-off-project
project-description: MCP is disabled at the top level
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
mcp:
  enabled: false
  auth:
    enabled: false
)";

    TempTestConfig temp(config_content, "audit_mcp_off");

    auto config_manager = temp.createConfigManager();
    SecurityAuditor auditor;
    auto warnings = auditor.audit(*config_manager);

    REQUIRE_FALSE(hasWarning(warnings, "MCP_UNAUTHENTICATED_TOOLS"));
}

TEST_CASE("SecurityAuditor: warning carries a non-empty stable code and message",
          "[security][audit]") {
    std::string config_content = R"(
project-name: shape-project
project-description: Verifying warning shape
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

    TempTestConfig temp(config_content, "audit_shape");
    temp.writeEndpoint("endpoint.yaml", R"(
url-path: /endpoint
method: GET
template-source: endpoint.sql
connection: [test]
auth:
  enabled: true
  type: basic
  users:
    - username: bob
      password: plaintext-password
)");
    temp.writeSqlTemplate("endpoint.sql", "SELECT 1");

    auto config_manager = temp.createConfigManager();
    SecurityAuditor auditor;
    auto warnings = auditor.audit(*config_manager);

    REQUIRE_FALSE(warnings.empty());
    for (const auto& w : warnings) {
        REQUIRE_FALSE(w.code.empty());
        REQUIRE_FALSE(w.message.empty());
    }
}

} // namespace test
} // namespace flapi
