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

std::vector<SecurityWarning> withCode(const std::vector<SecurityWarning>& warnings, const std::string& code) {
    std::vector<SecurityWarning> out;
    for (const auto& w : warnings) {
        if (w.code == code) {
            out.push_back(w);
        }
    }
    return out;
}

const char* kMcpAuthOnConfig = R"(
project-name: mcp-roles-project
project-description: MCP auth on
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
cors:
  allow-origins: ["https://app.example.com"]
mcp:
  enabled: true
  auth:
    enabled: true
    type: bearer
    jwt-secret: test-secret-long-enough
    jwt-issuer: test-issuer
)";

const char* kMcpAuthOffConfig = R"(
project-name: mcp-bypass-project
project-description: MCP auth off
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
cors:
  allow-origins: ["https://app.example.com"]
mcp:
  enabled: true
  auth:
    enabled: false
)";

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

TEST_CASE("SecurityAuditor: bcrypt password is reported as unverifiable, not as plaintext", "[security][audit][password]") {
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
    // PasswordHasher refuses bcrypt, so this user can never log in: say so.
    REQUIRE(hasWarning(warnings, "AUTH_BCRYPT_UNSUPPORTED"));
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


TEST_CASE("SecurityAuditor: PBKDF2 hashes are not reported as plaintext", "[security][audit][password]") {
    REQUIRE(SecurityAuditor::classifyPassword(
                "$pbkdf2-sha256$600000$c2FsdHNhbHRzYWx0c2FsdA$aGFzaGhhc2hoYXNoaGFzaGhhc2hoYXNoaGFzaGhhc2g")
            .empty());
    // passlib's ab64 form too
    REQUIRE(SecurityAuditor::classifyPassword(
                "$pbkdf2-sha256$1000$....AQID.AARIjNEVWZ3iA$hK0WL5EIFIQeq3GChnG.dHp7OjTY5QJhSA3DQVVfOu4")
            .empty());
    REQUIRE(SecurityAuditor::classifyPassword("hunter2") == "AUTH_PLAINTEXT_PASSWORD");
    REQUIRE(SecurityAuditor::classifyPassword("68675fbd5f8a9f03341659489a70944f") == "AUTH_MD5_PASSWORD");
    // bcrypt is recognised but never verifies: report it, do not bless it
    REQUIRE(SecurityAuditor::classifyPassword("$2b$12$Qz5e9p1m2n3o4p5q6r7s8eABCDEFGHIJKLMNOPQRSTUVWXYZabcd") ==
            "AUTH_BCRYPT_UNSUPPORTED");
}

TEST_CASE("SecurityAuditor: advice names a real way to make a PBKDF2 hash", "[security][audit][password]") {
    TempTestConfig temp(R"(
project-name: advice
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)", "audit_advice");
    temp.writeEndpoint("e.yaml", R"(
url-path: /e
template-source: e.sql
connection: [test]
auth:
  enabled: true
  type: basic
  users:
    - username: u
      password: plain-text-pw
)");
    temp.writeSqlTemplate("e.sql", "SELECT 1");
    auto cm = temp.createConfigManager();
    auto warnings = SecurityAuditor{}.audit(*cm);
    auto it = std::find_if(warnings.begin(), warnings.end(),
                           [](const SecurityWarning& w) { return w.code == "AUTH_PLAINTEXT_PASSWORD"; });
    REQUIRE(it != warnings.end());
    REQUIRE(it->message.find("pbkdf2_sha256") != std::string::npos);
    REQUIRE(it->message.find("bcrypt") == std::string::npos);
    REQUIRE(it->message.find("flapii auth hash") == std::string::npos);
}

TEST_CASE("SecurityAuditor: wildcard CORS with auth is flagged", "[security][audit][cors]") {
    const std::string base = R"(
project-name: cors-audit
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";
    SECTION("explicit * with global auth") {
        TempTestConfig temp(base + R"(
cors:
  allow-origins: ["*"]
auth:
  enabled: true
  type: basic
  users:
    - username: u
      password: $pbkdf2-sha256$1000$....AQID.AARIjNEVWZ3iA$hK0WL5EIFIQeq3GChnG.dHp7OjTY5QJhSA3DQVVfOu4
)", "audit_cors_star");
        auto cm = temp.createConfigManager();
        REQUIRE(hasWarning(SecurityAuditor{}.audit(*cm), "CORS_WILDCARD_WITH_AUTH"));
    }
    SECTION("an allowlist with auth is fine") {
        TempTestConfig temp(base + R"(
cors:
  allow-origins: ["https://app.example.com"]
auth:
  enabled: true
  type: basic
  users:
    - username: u
      password: $pbkdf2-sha256$1000$....AQID.AARIjNEVWZ3iA$hK0WL5EIFIQeq3GChnG.dHp7OjTY5QJhSA3DQVVfOu4
)", "audit_cors_list");
        auto cm = temp.createConfigManager();
        REQUIRE_FALSE(hasWarning(SecurityAuditor{}.audit(*cm), "CORS_WILDCARD_WITH_AUTH"));
    }
    SECTION("wildcard without any auth is not flagged") {
        TempTestConfig temp(base + R"(
cors:
  allow-origins: ["*"]
)", "audit_cors_noauth");
        auto cm = temp.createConfigManager();
        REQUIRE_FALSE(hasWarning(SecurityAuditor{}.audit(*cm), "CORS_WILDCARD_WITH_AUTH"));
    }
}

TEST_CASE("SecurityAuditor: global users are audited once, not per inheriting endpoint", "[security][audit][global]") {
    TempTestConfig temp(R"(
project-name: global-audit
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
auth:
  enabled: true
  type: basic
  users:
    - username: u
      password: plain-text-pw
)", "audit_global_once");
    temp.writeSqlTemplate("e.sql", "SELECT 1");
    temp.writeEndpoint("a.yaml", "url-path: /a\ntemplate-source: e.sql\nconnection: [test]\n");
    temp.writeEndpoint("b.yaml", "url-path: /b\ntemplate-source: e.sql\nconnection: [test]\n");
    auto cm = temp.createConfigManager();
    auto warnings = SecurityAuditor{}.audit(*cm);
    const auto n = std::count_if(warnings.begin(), warnings.end(),
                                 [](const SecurityWarning& w) { return w.code == "AUTH_PLAINTEXT_PASSWORD"; });
    REQUIRE(n == 1);
}

// ---- #275: new auditor codes ------------------------------------------------

TEST_CASE("SecurityAuditor: an MCP tool without allowed-roles under mcp.auth is flagged",
          "[security][audit][mcp][issue275]") {
    // MCPAuthorizationPolicy denies such a tool to EVERY caller, so a tool
    // that reads as exposed is unusable; say so at startup.
    TempTestConfig temp(kMcpAuthOnConfig, "audit_mcp_no_roles");
    temp.writeEndpoint("open.yaml", R"(
template-source: q.sql
connection: [test]
mcp-tool:
  name: roleless_tool
  description: No allowed-roles
)");
    temp.writeEndpoint("gated.yaml", R"(
template-source: q.sql
connection: [test]
mcp-tool:
  name: gated_tool
  description: Has allowed-roles
  allowed-roles: [analyst]
)");
    temp.writeEndpoint("res.yaml", R"(
template-source: q.sql
connection: [test]
mcp-resource:
  name: roleless_resource
  description: No allowed-roles
)");
    temp.writeSqlTemplate("q.sql", "SELECT 1 AS id");

    auto warnings = SecurityAuditor().audit(*temp.createConfigManager());
    const auto found = withCode(warnings, "MCP_TOOL_NO_ALLOWED_ROLES");
    REQUIRE(found.size() == 2);
    const auto names = [&](const std::string& needle) {
        return std::any_of(found.begin(), found.end(), [&](const SecurityWarning& w) {
            return w.message.find(needle) != std::string::npos;
        });
    };
    REQUIRE(names("roleless_tool"));
    REQUIRE(names("roleless_resource"));
    REQUIRE_FALSE(names("gated_tool"));
}

TEST_CASE("SecurityAuditor: no MCP_TOOL_NO_ALLOWED_ROLES while mcp.auth is off",
          "[security][audit][mcp][issue275]") {
    TempTestConfig temp(kMcpAuthOffConfig, "audit_mcp_no_roles_off");
    temp.writeEndpoint("open.yaml", R"(
template-source: q.sql
connection: [test]
mcp-tool:
  name: roleless_tool
  description: No allowed-roles
)");
    temp.writeSqlTemplate("q.sql", "SELECT 1 AS id");

    auto warnings = SecurityAuditor().audit(*temp.createConfigManager());
    REQUIRE_FALSE(hasWarning(warnings, "MCP_TOOL_NO_ALLOWED_ROLES"));
}

TEST_CASE("SecurityAuditor: an MCP tool on a REST-protected endpoint with mcp.auth off is flagged",
          "[security][audit][mcp][issue275]") {
    // The REST route asks for credentials; the same SQL as a tool does not.
    TempTestConfig temp(kMcpAuthOffConfig, "audit_mcp_bypass");
    temp.writeEndpoint("orders.yaml", R"(
url-path: /orders
method: GET
template-source: q.sql
connection: [test]
auth:
  enabled: true
  type: bearer
  jwt-secret: rest-secret-long-enough
mcp-tool:
  name: orders_tool
  description: Orders
)");
    temp.writeEndpoint("public.yaml", R"(
url-path: /public
method: GET
template-source: q.sql
connection: [test]
mcp-tool:
  name: public_tool
  description: Public
)");
    temp.writeSqlTemplate("q.sql", "SELECT 1 AS id");

    auto warnings = SecurityAuditor().audit(*temp.createConfigManager());
    const auto found = withCode(warnings, "MCP_TOOL_BYPASSES_REST_AUTH");
    REQUIRE(found.size() == 1);
    REQUIRE(found[0].location == "endpoint GET /orders");
    REQUIRE(found[0].message.find("orders_tool") != std::string::npos);
}

TEST_CASE("SecurityAuditor: no MCP_TOOL_BYPASSES_REST_AUTH when mcp.auth is on",
          "[security][audit][mcp][issue275]") {
    TempTestConfig temp(kMcpAuthOnConfig, "audit_mcp_bypass_on");
    temp.writeEndpoint("orders.yaml", R"(
url-path: /orders
method: GET
template-source: q.sql
connection: [test]
auth:
  enabled: true
  type: bearer
  jwt-secret: rest-secret-long-enough
mcp-tool:
  name: orders_tool
  description: Orders
  allowed-roles: [analyst]
)");
    temp.writeSqlTemplate("q.sql", "SELECT 1 AS id");

    auto warnings = SecurityAuditor().audit(*temp.createConfigManager());
    REQUIRE_FALSE(hasWarning(warnings, "MCP_TOOL_BYPASSES_REST_AUTH"));
}

TEST_CASE("SecurityAuditor: an auth-like unknown key without an auth block is flagged",
          "[security][audit][issue275]") {
    // `authentication:` / `Auth:` is read by nothing: the endpoint is open (or
    // inherits the top-level auth) while its file reads as protected.
    TempTestConfig temp(kMcpAuthOffConfig, "audit_auth_key_ignored");
    temp.writeEndpoint("orders.yaml", R"(
url-path: /orders
method: GET
template-source: q.sql
connection: [test]
)");
    temp.writeEndpoint("declared.yaml", R"(
url-path: /declared
method: GET
template-source: q.sql
connection: [test]
auth:
  enabled: true
  type: bearer
  jwt-secret: rest-secret-long-enough
)");
    temp.writeSqlTemplate("q.sql", "SELECT 1 AS id");
    auto config_manager = temp.createConfigManager();

    // The parser records the unknown top-level keys (EndpointConfig::unknown_keys).
    const auto snapshot = config_manager->getEndpoints();  // pinned: replaceEndpoint swaps it
    for (const auto& endpoint : *snapshot) {
        EndpointConfig copy = endpoint;
        copy.unknown_keys = {"Authentication", "descripton"};
        REQUIRE(config_manager->replaceEndpoint(copy));
    }

    auto warnings = SecurityAuditor().audit(*config_manager);
    const auto found = withCode(warnings, "ENDPOINT_AUTH_KEY_IGNORED");
    // Only the endpoint WITHOUT an auth block: the other one's block is read.
    REQUIRE(found.size() == 1);
    REQUIRE(found[0].location == "endpoint GET /orders");
    REQUIRE(found[0].message.find("Authentication") != std::string::npos);
    REQUIRE(found[0].message.find("descripton") == std::string::npos);
}

} // namespace test
} // namespace flapi
