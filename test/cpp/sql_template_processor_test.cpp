#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <fstream>
#include <filesystem>
#include <random>
#include <algorithm>
#include <chrono>

#include "sql_template_processor.hpp"
#include "config_manager.hpp"
#include "mustache_syntax_check.hpp"

#ifdef _WIN32
#include <cstdlib>
#define setenv(name, value, overwrite) _putenv_s(name, value)
#define unsetenv(name) _putenv((std::string(name) + "=").c_str())
#else
#include <stdlib.h>
#endif


using namespace flapi;

class MockConfigManager : public ConfigManager {
public:
    MockConfigManager() : ConfigManager(std::filesystem::path("path/to/mock_config.yaml")) {}

    void setTemplatePath(const std::string& path) {
        template_path = path;
    }

    std::string getTemplatePath() const override {
        return template_path;
    }

    void addConnection(const std::string& name, const ConnectionConfig& config) {
        connections[name] = config;
    }

    void setTemplateConfig(const TemplateConfig& config) {
        template_config = config;
    }

private:
    std::string template_path;
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::string temp_dir = std::filesystem::temp_directory_path().string();
        std::string random_suffix = generate_random_string(8);
        path = std::filesystem::path(temp_dir) / ("flapi_test_" + random_suffix);
        std::filesystem::create_directory(path);
    }

    ~TemporaryDirectory() {
        std::filesystem::remove_all(path);
    }

    std::filesystem::path getPath() const {
        return path;
    }

private:
    std::filesystem::path path;

    std::string generate_random_string(size_t length) {
        const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, chars.size() - 1);
        std::string result;
        result.reserve(length);
        for (size_t i = 0; i < length; ++i) {
            result += chars[dis(gen)];
        }
        return result;
    }
};

TEST_CASE("SQLTemplateProcessor: Basic template processing", "[sql_template_processor]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("Simple template") {
        std::string template_content = "SELECT * FROM users WHERE name = {{params.name}}";
        auto template_file_path = temp_dir.getPath() / "simple_template.sql";
        std::ofstream template_file(template_file_path);
        template_file << template_content;
        template_file.close();

        std::cout << "Temporary directory: " << temp_dir.getPath() << std::endl;
        std::cout << "Template file path: " << template_file_path << std::endl;

        EndpointConfig endpoint;
        endpoint.templateSource = "simple_template.sql";
        endpoint.connection = {"default"};

        std::cout << "Configuration Template Path: " << config_manager->getTemplatePath() << std::endl;

        std::map<std::string, std::string> params = {{"name", "John"}};

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "SELECT * FROM users WHERE name = John");
    }
}

TEST_CASE("SQLTemplateProcessor: Template with connection properties", "[sql_template_processor]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    ConnectionConfig conn_config;
    conn_config.properties["database"] = "mydb";
    conn_config.properties["schema"] = "public";
    config_manager->addConnection("default", conn_config);

    SQLTemplateProcessor processor(config_manager);

    SECTION("Template with connection properties") {
        std::string template_content = "SELECT * FROM {{conn.schema}}.users WHERE database = '{{conn.database}}'";
        std::ofstream template_file(temp_dir.getPath() / "conn_template.sql");
        template_file << template_content;
        template_file.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "conn_template.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params;

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "SELECT * FROM public.users WHERE database = 'mydb'");
    }
}

TEST_CASE("SQLTemplateProcessor: Template with environment variables", "[sql_template_processor]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    TemplateConfig template_config;
    template_config.environment_whitelist = {"TEST_ENV_.*"};
    config_manager->setTemplateConfig(template_config);

    SQLTemplateProcessor processor(config_manager);

    SECTION("Template with allowed environment variable") {
        std::string template_content = "SELECT * FROM users WHERE region = '{{env.TEST_ENV_REGION}}'";
        std::ofstream template_file(temp_dir.getPath() / "env_template.sql");
        template_file << template_content;
        template_file.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "env_template.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params;

        // Set environment variable
        setenv("TEST_ENV_REGION", "us-west", 1);

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "SELECT * FROM users WHERE region = 'us-west'");

        // Unset environment variable
        unsetenv("TEST_ENV_REGION");
    }

    SECTION("Template with disallowed environment variable") {
        std::string template_content = "SELECT * FROM users WHERE api_key = '{{env.API_KEY}}'";
        std::ofstream template_file(temp_dir.getPath() / "disallowed_env_template.sql");
        template_file << template_content;
        template_file.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "disallowed_env_template.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params;

        // Set environment variable
        setenv("API_KEY", "secret", 1);

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "SELECT * FROM users WHERE api_key = ''");

        // Unset environment variable
        unsetenv("API_KEY");
    }
}

TEST_CASE("SQLTemplateProcessor: Template with cache parameters", "[sql_template_processor]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("Template with cache parameters") {
        std::string template_content = "CREATE TABLE {{cache.schema}}.{{cache.table}} AS SELECT * FROM source_table WHERE updated_at > '{{cache.snapshotTimestamp}}'";
        std::ofstream template_file(temp_dir.getPath() / "cache_template.sql");
        template_file << template_content;
        template_file.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "cache_template.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params = {
            {"cacheCatalog", "cache"},
            {"cacheSchema", "cache_schema"},
            {"cacheTable", "cache_table"},
            {"cacheSnapshotTimestamp", "2023-05-01 00:00:00"}
        };

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "CREATE TABLE cache_schema.cache_table AS SELECT * FROM source_table WHERE updated_at > '2023-05-01 00:00:00'");
    }
}

TEST_CASE("SQLTemplateProcessor: Template with request parameters", "[sql_template_processor]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("Template with request parameters") {
        std::string template_content = "SELECT * FROM users WHERE name = '{{params.name}}' AND age > {{params.min_age}}";
        std::ofstream template_file(temp_dir.getPath() / "params_template.sql");
        template_file << template_content;
        template_file.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "params_template.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params = {
            {"name", "John"},
            {"min_age", "18"}
        };

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "SELECT * FROM users WHERE name = 'John' AND age > 18");
    }
}

TEST_CASE("SQLTemplateProcessor: Template not found", "[sql_template_processor]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("Non-existent template") {
        EndpointConfig endpoint;
        endpoint.templateSource = "non_existent_template.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params;

        REQUIRE_THROWS_AS(processor.loadAndProcessTemplate(endpoint, params), std::runtime_error);
    }
}

TEST_CASE("SQLTemplateProcessor: Cache template processing", "[sql_template_processor]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("Cache template") {
        std::string template_content = "CREATE TABLE {{cache.schema}}.{{cache.table}} AS SELECT * FROM source_table WHERE updated_at > '{{cache.snapshotTimestamp}}'";
        std::ofstream template_file(temp_dir.getPath() / "cache_template.sql");
        template_file << template_content;
        template_file.close();

        EndpointConfig endpoint;
        endpoint.connection = {"default"};
        endpoint.cache.enabled = true;
        endpoint.cache.table = "cache_table";
        endpoint.cache.schema = "cache_schema";
        endpoint.cache.template_file = "cache_template.sql";

        CacheConfig cache_config;
        cache_config.enabled = true;
        cache_config.table = "cache_table";
        cache_config.schema = "cache_schema";
        cache_config.template_file = "cache_template.sql";

        std::map<std::string, std::string> params = {
            {"cacheCatalog", "cache"},
            {"cacheSchema", "cache_schema"},
            {"cacheTable", "cache_table"},
            {"cacheSnapshotTimestamp", "2023-05-01 00:00:00"}
        };

        std::string result = processor.loadAndProcessTemplate(endpoint, cache_config, params);
        REQUIRE(result == "CREATE TABLE cache_schema.cache_table AS SELECT * FROM source_table WHERE updated_at > '2023-05-01 00:00:00'");
    }
}

TEST_CASE("SQLTemplateProcessor: Complex template with multiple features", "[sql_template_processor]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    ConnectionConfig conn_config;
    conn_config.properties["database"] = "mydb";
    conn_config.properties["schema"] = "public";
    config_manager->addConnection("default", conn_config);

    TemplateConfig template_config;
    template_config.environment_whitelist = {"TEST_ENV_.*"};
    config_manager->setTemplateConfig(template_config);

    SQLTemplateProcessor processor(config_manager);

    SECTION("Complex template") {
        std::string template_content = R"(
            WITH cache_data AS (
                SELECT * FROM {{cache.schema}}.{{cache.table}}
                WHERE updated_at > '{{cache.snapshotTimestamp}}'
            )
            SELECT cd.*, u.email
            FROM cache_data cd
            JOIN {{conn.schema}}.users u ON cd.user_id = u.id
            WHERE cd.region = '{{env.TEST_ENV_REGION}}'
              AND cd.status = '{{params.status}}'
            LIMIT {{params.limit}}
        )";
        std::ofstream template_file(temp_dir.getPath() / "complex_template.sql");
        template_file << template_content;
        template_file.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "complex_template.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params = {
            {"cacheCatalog", "cache"},
            {"cacheSchema", "cache_schema"},
            {"cacheTable", "cache_table"},
            {"cacheSnapshotTimestamp", "2023-05-01 00:00:00"},
            {"status", "active"},
            {"limit", "100"}
        };

        // Set environment variable
        setenv("TEST_ENV_REGION", "us-west", 1);

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        std::string expected = R"(
            WITH cache_data AS (
                SELECT * FROM cache_schema.cache_table
                WHERE updated_at > '2023-05-01 00:00:00'
            )
            SELECT cd.*, u.email
            FROM cache_data cd
            JOIN public.users u ON cd.user_id = u.id
            WHERE cd.region = 'us-west'
              AND cd.status = 'active'
            LIMIT 100
        )";
        REQUIRE(result == expected);

        // Unset environment variable
        unsetenv("TEST_ENV_REGION");
    }
}

// ============================================================================
// VFS Integration Tests for SQLTemplateProcessor
// ============================================================================

#include "vfs_adapter.hpp"

// Extended MockConfigManager that supports remote template paths
class VFSMockConfigManager : public ConfigManager {
public:
    VFSMockConfigManager() : ConfigManager(std::filesystem::path("path/to/mock_config.yaml")) {}

    void setTemplatePath(const std::string& path) {
        template_path = path;
    }

    std::string getTemplatePath() const override {
        return template_path;
    }

    void addConnection(const std::string& name, const ConnectionConfig& config) {
        connections[name] = config;
    }

private:
    std::string template_path;
};

TEST_CASE("SQLTemplateProcessor: Path resolution with remote templates", "[sql_template_processor][vfs]") {
    auto config_manager = std::make_shared<VFSMockConfigManager>();
    SQLTemplateProcessor processor(config_manager);

    SECTION("Remote S3 base path with relative template") {
        config_manager->setTemplatePath("s3://bucket/templates/");

        EndpointConfig endpoint;
        endpoint.templateSource = "queries/customers.sql";
        endpoint.connection = {"default"};

        // Test path resolution by calling getFullTemplatePath indirectly
        // via loadTemplate which will throw for non-existent remote file
        // but the path in the error message should be correct
        std::map<std::string, std::string> params;

        try {
            processor.loadAndProcessTemplate(endpoint, params);
            FAIL("Expected exception for non-existent remote template");
        } catch (const std::runtime_error& e) {
            std::string error_msg = e.what();
            // Verify the path resolution happened correctly
            REQUIRE(error_msg.find("s3://bucket/templates/queries/customers.sql") != std::string::npos);
        }
    }

    SECTION("Remote GCS base path with relative template") {
        config_manager->setTemplatePath("gs://bucket/templates");

        EndpointConfig endpoint;
        endpoint.templateSource = "analytics.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params;

        try {
            processor.loadAndProcessTemplate(endpoint, params);
            FAIL("Expected exception for non-existent remote template");
        } catch (const std::runtime_error& e) {
            std::string error_msg = e.what();
            // Verify slash is added between base and template
            REQUIRE(error_msg.find("gs://bucket/templates/analytics.sql") != std::string::npos);
        }
    }

    SECTION("Remote HTTPS base path with relative template") {
        config_manager->setTemplatePath("https://example.com/api/templates/");

        EndpointConfig endpoint;
        endpoint.templateSource = "endpoint.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params;

        try {
            processor.loadAndProcessTemplate(endpoint, params);
            FAIL("Expected exception for non-existent remote template");
        } catch (const std::runtime_error& e) {
            std::string error_msg = e.what();
            REQUIRE(error_msg.find("https://example.com/api/templates/endpoint.sql") != std::string::npos);
        }
    }

    SECTION("Absolute remote template source is used directly") {
        config_manager->setTemplatePath("/local/templates/");

        EndpointConfig endpoint;
        endpoint.templateSource = "s3://other-bucket/special.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params;

        try {
            processor.loadAndProcessTemplate(endpoint, params);
            FAIL("Expected exception for non-existent remote template");
        } catch (const std::runtime_error& e) {
            std::string error_msg = e.what();
            // Remote template source should be used as-is, not combined with local base
            REQUIRE(error_msg.find("s3://other-bucket/special.sql") != std::string::npos);
        }
    }

    SECTION("Local absolute path is preserved") {
        config_manager->setTemplatePath("s3://bucket/templates/");

        EndpointConfig endpoint;
        endpoint.templateSource = "/absolute/local/template.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params;

        try {
            processor.loadAndProcessTemplate(endpoint, params);
            FAIL("Expected exception for non-existent template");
        } catch (const std::runtime_error& e) {
            std::string error_msg = e.what();
            // Absolute local path should be used as-is
            REQUIRE(error_msg.find("/absolute/local/template.sql") != std::string::npos);
        }
    }
}

TEST_CASE("SQLTemplateProcessor: PathSchemeUtils integration", "[sql_template_processor][vfs]") {
    SECTION("Verify remote path detection used by SQLTemplateProcessor") {
        // These tests verify that PathSchemeUtils is correctly used
        REQUIRE(PathSchemeUtils::IsRemotePath("s3://bucket/file.sql"));
        REQUIRE(PathSchemeUtils::IsRemotePath("gs://bucket/file.sql"));
        REQUIRE(PathSchemeUtils::IsRemotePath("https://example.com/file.sql"));
        REQUIRE(PathSchemeUtils::IsRemotePath("http://example.com/file.sql"));
        REQUIRE(PathSchemeUtils::IsRemotePath("az://container/file.sql"));

        REQUIRE_FALSE(PathSchemeUtils::IsRemotePath("/local/path/file.sql"));
        REQUIRE_FALSE(PathSchemeUtils::IsRemotePath("relative/path/file.sql"));
        REQUIRE_FALSE(PathSchemeUtils::IsRemotePath("file:///local/file.sql"));
    }
}

// ── Bug #17 ────────────────────────────────────────────────────────────────
// auth.* template variables not injected into Mustache context
// ──────────────────────────────────────────────────────────────────────────

TEST_CASE("SQLTemplateProcessor: auth.username injected from reserved param", "[sql_template_processor][auth][bug17]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("auth.username rendered in template") {
        std::string template_content = "SELECT * FROM t WHERE user = '{{{auth.username}}}'";
        std::ofstream f(temp_dir.getPath() / "auth_username.sql");
        f << template_content;
        f.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "auth_username.sql";

        std::map<std::string, std::string> params = {{"__auth_username", "alice"}};

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "SELECT * FROM t WHERE user = 'alice'");
    }

    SECTION("__auth_username not exposed as params.* variable") {
        std::string template_content = "{{params.__auth_username}}";
        std::ofstream f(temp_dir.getPath() / "auth_leak.sql");
        f << template_content;
        f.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "auth_leak.sql";

        std::map<std::string, std::string> params = {{"__auth_username", "alice"}};

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "");  // must NOT leak into params.*
    }
}

TEST_CASE("SQLTemplateProcessor: auth.roles injected from reserved param", "[sql_template_processor][auth][bug17]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("auth.roles rendered in template") {
        std::string template_content = "SELECT '{{{auth.roles}}}'";
        std::ofstream f(temp_dir.getPath() / "auth_roles.sql");
        f << template_content;
        f.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "auth_roles.sql";

        std::map<std::string, std::string> params = {{"__auth_roles", "admin,user"}};

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "SELECT 'admin,user'");
    }
}

TEST_CASE("SQLTemplateProcessor: auth.email injected from reserved param", "[sql_template_processor][auth][bug17]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("auth.email rendered in template") {
        std::string template_content = "SELECT email = '{{{auth.email}}}'";
        std::ofstream f(temp_dir.getPath() / "auth_email.sql");
        f << template_content;
        f.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "auth_email.sql";

        std::map<std::string, std::string> params = {{"__auth_email", "alice@example.com"}};

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "SELECT email = 'alice@example.com'");
    }
}

TEST_CASE("SQLTemplateProcessor: missing auth params render as empty string", "[sql_template_processor][auth][bug17]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("no auth params present — auth.username is empty string not error") {
        std::string template_content = "SELECT '{{{auth.username}}}'";
        std::ofstream f(temp_dir.getPath() / "auth_missing.sql");
        f << template_content;
        f.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "auth_missing.sql";

        std::map<std::string, std::string> params;  // no __auth_* keys

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "SELECT ''");
    }
}

TEST_CASE("SQLTemplateProcessor: auth and regular params coexist", "[sql_template_processor][auth][bug17]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("auth.username and params.id both rendered correctly") {
        std::string template_content =
            "SELECT * FROM orders WHERE id = {{params.id}} AND owner = '{{{auth.username}}}'";
        std::ofstream f(temp_dir.getPath() / "auth_and_params.sql");
        f << template_content;
        f.close();

        EndpointConfig endpoint;
        endpoint.templateSource = "auth_and_params.sql";

        std::map<std::string, std::string> params = {
            {"id", "42"},
            {"__auth_username", "bob"}
        };

        std::string result = processor.loadAndProcessTemplate(endpoint, params);
        REQUIRE(result == "SELECT * FROM orders WHERE id = 42 AND owner = 'bob'");
    }
}
TEST_CASE("SQLTemplateProcessor: prepared rendering replaces typed params with ?", "[sql_template_processor][prepared]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());

    SQLTemplateProcessor processor(config_manager);

    SECTION("int-typed param becomes ?, untyped param stays inline") {
        std::string template_content =
            "SELECT * FROM t WHERE id = {{params.id}} AND status = '{{params.status}}'";
        std::ofstream(temp_dir.getPath() / "tpl.sql") << template_content;

        EndpointConfig endpoint;
        endpoint.templateSource = "tpl.sql";

        RequestFieldConfig id_field;
        id_field.fieldName = "id";
        ValidatorConfig int_v;
        int_v.type = "int";
        id_field.validators.push_back(int_v);
        endpoint.request_fields.push_back(id_field);

        // status has no validator -> stays on Mustache path
        RequestFieldConfig status_field;
        status_field.fieldName = "status";
        endpoint.request_fields.push_back(status_field);

        std::map<std::string, std::string> params = {{"id", "42"}, {"status", "ok"}};

        auto prepared = processor.loadAndProcessTemplatePrepared(endpoint, params);
        REQUIRE(prepared.bindings.size() == 1);
        REQUIRE(prepared.bindings[0].field_name == "id");
        REQUIRE(prepared.bindings[0].type == SqlParameterType::Integer);
        REQUIRE(prepared.sql == "SELECT * FROM t WHERE id = ? AND status = 'ok'");
    }

    SECTION("template with only conn.* references produces no bindings") {
        std::string template_content = "SELECT 1 FROM {{conn.schema}}.users";
        std::ofstream(temp_dir.getPath() / "no_params.sql") << template_content;

        ConnectionConfig cc;
        cc.properties["schema"] = "public";
        config_manager->addConnection("default", cc);

        EndpointConfig endpoint;
        endpoint.templateSource = "no_params.sql";
        endpoint.connection = {"default"};

        std::map<std::string, std::string> params;
        auto prepared = processor.loadAndProcessTemplatePrepared(endpoint, params);
        REQUIRE(prepared.bindings.empty());
        REQUIRE(prepared.sql == "SELECT 1 FROM public.users");
    }

    SECTION("a quoted triple-brace site of a typed param is bound (#192)") {
        std::string template_content = "SELECT '{{{params.name}}}'";
        std::ofstream(temp_dir.getPath() / "triple.sql") << template_content;

        EndpointConfig endpoint;
        endpoint.templateSource = "triple.sql";
        RequestFieldConfig f;
        f.fieldName = "name";
        ValidatorConfig sv;
        sv.type = "string";
        f.validators.push_back(sv);
        endpoint.request_fields.push_back(f);

        std::map<std::string, std::string> params = {{"name", "alice"}};
        auto prepared = processor.loadAndProcessTemplatePrepared(endpoint, params);
        REQUIRE(prepared.bindings.size() == 1);
        REQUIRE(prepared.sql == "SELECT ?");
    }

    SECTION("auth.username inside template is rendered, not bound") {
        // __auth_username is stripped from params by createTemplateContext
        // and surfaces as auth.username. The rewriter never sees it as a
        // request field, so it stays on the Mustache path. Verifying
        // here so the auth-context plumbing keeps working under prepared.
        std::string template_content =
            "SELECT * FROM t WHERE id = {{params.id}} AND owner = '{{{auth.username}}}'";
        std::ofstream(temp_dir.getPath() / "auth_prep.sql") << template_content;

        EndpointConfig endpoint;
        endpoint.templateSource = "auth_prep.sql";
        RequestFieldConfig f;
        f.fieldName = "id";
        ValidatorConfig iv;
        iv.type = "int";
        f.validators.push_back(iv);
        endpoint.request_fields.push_back(f);

        std::map<std::string, std::string> params = {
            {"id", "7"},
            {"__auth_username", "alice"}
        };
        auto prepared = processor.loadAndProcessTemplatePrepared(endpoint, params);
        REQUIRE(prepared.bindings.size() == 1);
        REQUIRE(prepared.bindings[0].field_name == "id");
        REQUIRE(prepared.sql == "SELECT * FROM t WHERE id = ? AND owner = 'alice'");
    }
}

// ──────────────────────────────────────────────────────────────────────────
// auth.* values are SQL-quoted for use inside a '...' literal
// ──────────────────────────────────────────────────────────────────────────
//
// auth.* is never bound as a prepared parameter, and comes from the identity
// provider (Basic username, JWT sub/roles, OIDC claims), so a value with a
// single quote must stay inside the literal the template wrote around it.

namespace {

std::string RenderAuthTemplate(SQLTemplateProcessor& processor,
                               const std::filesystem::path& dir,
                               const std::string& file_name,
                               const std::string& template_content,
                               std::map<std::string, std::string> params,
                               bool prepared) {
    std::ofstream(dir / file_name) << template_content;
    EndpointConfig endpoint;
    endpoint.templateSource = file_name;
    if (prepared) {
        return processor.loadAndProcessTemplatePrepared(endpoint, params).sql;
    }
    return processor.loadAndProcessTemplate(endpoint, params);
}

} // namespace

TEST_CASE("SQLTemplateProcessor: auth.* values stay inside their quoted literal",
          "[sql_template_processor][auth][auth_quoting]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());
    SQLTemplateProcessor processor(config_manager);

    const std::string hostile = "x' OR 1=1 --";
    const std::string quoted = "'x'' OR 1=1 --'";

    for (const bool prepared : {false, true}) {
        DYNAMIC_SECTION("auth.username, prepared=" << prepared) {
            const auto sql = RenderAuthTemplate(processor, temp_dir.getPath(), "u.sql",
                "SELECT * FROM t WHERE owner = '{{{ auth.username }}}'",
                {{"__auth_username", hostile}}, prepared);
            REQUIRE(sql == "SELECT * FROM t WHERE owner = " + quoted);
        }
        DYNAMIC_SECTION("auth.email, prepared=" << prepared) {
            const auto sql = RenderAuthTemplate(processor, temp_dir.getPath(), "e.sql",
                "SELECT * FROM t WHERE email = '{{{ auth.email }}}'",
                {{"__auth_email", hostile}}, prepared);
            REQUIRE(sql == "SELECT * FROM t WHERE email = " + quoted);
        }
        DYNAMIC_SECTION("auth.roles, prepared=" << prepared) {
            const auto sql = RenderAuthTemplate(processor, temp_dir.getPath(), "r.sql",
                "SELECT * FROM t WHERE role = '{{{ auth.roles }}}'",
                {{"__auth_roles", hostile}}, prepared);
            REQUIRE(sql == "SELECT * FROM t WHERE role = " + quoted);
        }
        DYNAMIC_SECTION("auth.type, prepared=" << prepared) {
            const auto sql = RenderAuthTemplate(processor, temp_dir.getPath(), "ty.sql",
                "SELECT * FROM t WHERE kind = '{{{ auth.type }}}'",
                {{"__auth_type", hostile}}, prepared);
            REQUIRE(sql == "SELECT * FROM t WHERE kind = " + quoted);
        }
    }

    SECTION("a legitimate quote in a name still matches as a literal") {
        const auto sql = RenderAuthTemplate(processor, temp_dir.getPath(), "ob.sql",
            "SELECT * FROM t WHERE owner = '{{{ auth.username }}}'",
            {{"__auth_username", "O'Brien"}}, false);
        REQUIRE(sql == "SELECT * FROM t WHERE owner = 'O''Brien'");
    }

    SECTION("values without a quote are unchanged") {
        const auto sql = RenderAuthTemplate(processor, temp_dir.getPath(), "plain.sql",
            "'{{{ auth.username }}}' '{{{ auth.roles }}}' '{{{ auth.email }}}' '{{{ auth.type }}}' {{{ auth.authenticated }}}",
            {{"__auth_username", "alice"}, {"__auth_roles", "admin,user"},
             {"__auth_email", "alice@example.com"}, {"__auth_type", "bearer"},
             {"__auth_authenticated", "true"}}, false);
        REQUIRE(sql == "'alice' 'admin,user' 'alice@example.com' 'bearer' true");
    }

    SECTION("the double-brace form renders the same quote-doubled value") {
        const auto sql = RenderAuthTemplate(processor, temp_dir.getPath(), "dbl.sql",
            "SELECT * FROM t WHERE owner = '{{ auth.username }}'",
            {{"__auth_username", hostile}}, false);
        REQUIRE(sql == "SELECT * FROM t WHERE owner = " + quoted);
        const auto obrien = RenderAuthTemplate(processor, temp_dir.getPath(), "dbl2.sql",
            "SELECT * FROM t WHERE owner = '{{ auth.username }}'",
            {{"__auth_username", "O'Brien"}}, false);
        REQUIRE(obrien == "SELECT * FROM t WHERE owner = 'O''Brien'");
    }
}

TEST_CASE("FindUnquotedAuthVariables: reports auth.* anywhere but a plain '...' literal",
          "[sql_template_processor][auth][auth_quoting]") {
    const auto names = [](const std::string& body) {
        std::vector<std::string> out;
        for (const auto& v : FindUnquotedAuthVariables(body)) {
            out.push_back(v.name);
        }
        return out;
    };
    const std::vector<std::string> username{"auth.username"};

    SECTION("inside a plain '...' literal is fine, in either brace form") {
        REQUIRE(names("SELECT * FROM t WHERE owner = '{{{ auth.username }}}'").empty());
        REQUIRE(names("WHERE e = '{{ auth.email }}' AND r = 'x,{{{auth.roles}}},y'").empty());
        REQUIRE(names("WHERE name LIKE'{{{ auth.username }}}'").empty());
        REQUIRE(names("{{#auth.username}}AND owner = '{{{auth.username}}}'{{/auth.username}}").empty());
        REQUIRE(names("{{#auth}}AND owner = '{{{username}}}'{{/auth}}").empty());
        REQUIRE(names("{{#auth.username}}AND owner = '{{.}}'{{/auth.username}}").empty());
    }

    SECTION("a bare use is reported with its position") {
        const auto found = FindUnquotedAuthVariables("SELECT 1\nWHERE id = {{{ auth.username }}}");
        REQUIRE(found.size() == 1);
        REQUIRE(found[0].name == "auth.username");
        REQUIRE(found[0].line == 2);
        REQUIRE(found[0].column == 12);
        REQUIRE(names("WHERE x IN ({{ auth.roles }})") == std::vector<std::string>{"auth.roles"});
        REQUIRE(names("'a' || {{&auth.email}}") == std::vector<std::string>{"auth.email"});
    }

    SECTION("sections themselves, auth.authenticated and other names are not reported") {
        REQUIRE(names("{{^auth.roles}}AND 1=0{{/auth.roles}}").empty());
        REQUIRE(names("WHERE {{ auth.authenticated }}").empty());
        REQUIRE(names("WHERE id = {{ params.id }}").empty());
        REQUIRE(names("{{#params.x}}WHERE id = {{.}}{{/params.x}}").empty());
    }

    SECTION("comments are reported: a value can end them") {
        REQUIRE(names("-- owner {{{ auth.username }}}\nSELECT 1") == username);
        REQUIRE(names("/* {{{ auth.username }}} */ SELECT 1") == username);
        REQUIRE(names("SELECT 1 -- '{{{ auth.username }}}'") == username);
        REQUIRE(names("/* a /* nested */ '{{{ auth.username }}}' */") == username);
    }

    SECTION("an E'...' escape string is reported: backslash escapes the doubled quote") {
        REQUIRE(names("WHERE owner = E'{{{ auth.username }}}'") == username);
        REQUIRE(names("WHERE owner = e'{{{ auth.username }}}'") == username);
        REQUIRE(names("WHERE owner = E'a\\'b{{{ auth.username }}}'") == username);
    }

    SECTION("a double-quoted identifier is reported") {
        REQUIRE(names("SELECT 1 AS \"{{{ auth.username }}}\"") == username);
        REQUIRE(names("SELECT \"'\" AS c WHERE owner = '{{{ auth.username }}}'").empty());
    }

    SECTION("dollar-quoted strings are tracked, and a tag inside one is reported") {
        REQUIRE(names("SELECT $$it's$$ AS c WHERE id = {{{ auth.username }}}") == username);
        REQUIRE(names("SELECT $t$it's$t$ AS c WHERE owner = '{{{ auth.username }}}'").empty());
        REQUIRE(names("SELECT $${{{ auth.username }}}$$") == username);
        REQUIRE(names("SELECT $tag${{{ auth.username }}}$tag$") == username);
    }

    SECTION("a carriage return ends a -- comment, as it does in DuckDB") {
        REQUIRE(names("-- x\r'{{{ auth.username }}}'").empty());
        REQUIRE(names("-- don't\nWHERE id = {{ auth.username }}") == username);
    }

    SECTION("section scoping: names resolved inside an auth section are auth values") {
        REQUIRE(names("{{#auth}}WHERE id = {{{username}}}{{/auth}}") == username);
        REQUIRE(names("{{#auth}}WHERE e = {{email}}{{/auth}}") == std::vector<std::string>{"auth.email"});
        REQUIRE(names("{{#auth.username}}WHERE id = {{.}}{{/auth.username}}") == username);
        REQUIRE(names("{{#auth}}{{#username}}-- {{.}}{{/username}}{{/auth}}") == username);
        REQUIRE(names("{{#auth}}WHERE id = {{{auth}}}{{/auth}}") == std::vector<std::string>{"auth"});
    }

    SECTION("a section that changes the quoting around an auth value is reported") {
        REQUIRE_FALSE(names("{{#params.x}}'{{/params.x}}{{{ auth.username }}}{{#params.x}}'{{/params.x}}").empty());
    }

    SECTION("a set-delimiter tag hides tags from the check, so it is reported") {
        REQUIRE_FALSE(names("{{=<% %>=}}WHERE id = <% auth.username %>").empty());
    }
}

TEST_CASE("SQLTemplateProcessor: a template with auth.* outside a plain literal is refused",
          "[sql_template_processor][auth][auth_quoting]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());
    SQLTemplateProcessor processor(config_manager);

    const std::vector<std::string> templates{
        "SELECT * FROM t WHERE id = {{{ auth.username }}}",
        "SELECT * FROM t -- {{{ auth.username }}}",
        "SELECT * FROM t WHERE o = E'{{{ auth.username }}}'",
        "SELECT 1 AS \"{{{ auth.username }}}\"",
        "{{#auth}}SELECT {{{username}}}{{/auth}}"};
    for (const bool prepared : {false, true}) {
        for (std::size_t i = 0; i < templates.size(); ++i) {
            DYNAMIC_SECTION("prepared=" << prepared << " template #" << i) {
                std::ofstream(temp_dir.getPath() / "unsafe.sql") << templates[i];
                EndpointConfig endpoint;
                endpoint.templateSource = "unsafe.sql";
                std::map<std::string, std::string> params = {{"__auth_username", "bob"}};
                if (prepared) {
                    REQUIRE_THROWS_AS(processor.loadAndProcessTemplatePrepared(endpoint, params),
                                      std::invalid_argument);
                } else {
                    REQUIRE_THROWS_AS(processor.loadAndProcessTemplate(endpoint, params),
                                      std::invalid_argument);
                }
            }
        }
    }
}

TEST_CASE("SQLTemplateProcessor: an auth.* value with a control character is refused",
          "[sql_template_processor][auth][auth_quoting]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());
    SQLTemplateProcessor processor(config_manager);
    std::ofstream(temp_dir.getPath() / "ctl.sql")
        << "SELECT '{{{ auth.username }}}' '{{{ auth.email }}}' '{{{ auth.roles }}}' '{{{ auth.type }}}'";
    EndpointConfig endpoint;
    endpoint.templateSource = "ctl.sql";

    const std::vector<std::string> keys{"__auth_username", "__auth_email", "__auth_roles", "__auth_type"};
    const std::vector<std::string> values{"bob\nOR 1=1", "bob\r", std::string("bob\0x", 5), "bob\x1f", "\tbob",
                                          "bob\x7f"};
    for (const auto& key : keys) {
        for (std::size_t i = 0; i < values.size(); ++i) {
            DYNAMIC_SECTION(key << " value #" << i) {
                std::map<std::string, std::string> params = {{key, values[i]}};
                REQUIRE_THROWS_AS(processor.loadAndProcessTemplate(endpoint, params), std::invalid_argument);
            }
        }
    }

    SECTION("printable non-ASCII is kept") {
        std::map<std::string, std::string> params = {{"__auth_username", "J\xC3\xBCrgen"}};
        REQUIRE(processor.loadAndProcessTemplate(endpoint, params) == "SELECT 'J\xC3\xBCrgen' '' '' ''");
    }
}

TEST_CASE("FindUnquotedAuthVariables: SQL text that another function executes is refused",
          "[sql_template_processor][auth][auth_quoting]") {
    const auto names = [](const std::string& body) {
        std::vector<std::string> out;
        for (const auto& v : FindUnquotedAuthVariables(body)) {
            out.push_back(v.name);
        }
        return out;
    };
    const std::vector<std::string> username{"auth.username"};

    SECTION("a literal that also holds a doubled quote is SQL text") {
        REQUIRE(names("WHERE a = 'it''s {{{ auth.username }}}'") == username);
        REQUIRE(names("SELECT * FROM query('SELECT * FROM d WHERE o = ''{{{ auth.username }}}''')") == username);
    }

    SECTION("a literal argument of a SQL-executing function, at any depth") {
        REQUIRE(names("SELECT * FROM bigquery_query('p', concat('SELECT * FROM d WHERE o = ''', "
                      "'{{{ auth.username }}}', ''''))") == username);
        REQUIRE(names("SELECT * FROM postgres_query('db', '{{{ auth.username }}}')") == username);
        REQUIRE(names("SELECT * FROM mysql_execute('db', '{{{ auth.username }}}')") == username);
        REQUIRE(names("SELECT * FROM snowflake_query ( '{{{ auth.username }}}' )") == username);
        REQUIRE(names("SELECT * FROM odbc_query('dsn', '{{{ auth.username }}}')") == username);
        REQUIRE(names("SELECT * FROM QUERY('{{{ auth.username }}}')") == username);
        REQUIRE(names("SELECT * FROM query_table('{{{ auth.username }}}')") == username);
        REQUIRE(names("SELECT * FROM sqlite_query('f', lower('{{{ auth.username }}}'))") == username);
    }

    SECTION("filtering in the outer query is fine") {
        REQUIRE(names("SELECT * FROM bigquery_query('p', 'SELECT id, owner FROM d') "
                      "WHERE owner = '{{{ auth.username }}}'").empty());
        REQUIRE(names("SELECT * FROM query('SELECT 1 AS x') WHERE lower('{{{ auth.username }}}') = 'a'").empty());
        REQUIRE(names("SELECT concat('a', '{{{ auth.username }}}') FROM t").empty());
    }
}

TEST_CASE("CheckAuthPlacement: a Mustache tag between SQL characters does not hide a position",
          "[sql_template_processor][auth][auth_quoting]") {
    for (const std::string tpl : {
             std::string("SELECT * FROM t WHERE o = E{{! c }}'{{{ auth.username }}}'"),
             std::string("SELECT * FROM t WHERE o = {{#auth.authenticated}}E{{/auth.authenticated}}'{{{ auth.username }}}'"),
             std::string("SELECT * FROM t WHERE o = ${{! }}$ {{{ auth.username }}} $$"),
             std::string("SELECT * FROM t -{{! }}- '{{{ auth.username }}}'"),
             std::string("SELECT * FROM t /{{! }}* '{{{ auth.username }}}' */"),
             std::string("SELECT * FROM query{{! }}('SELECT 1 WHERE o = ''{{{ auth.username }}}''')")}) {
        DYNAMIC_SECTION(tpl) {
            REQUIRE(CheckAuthPlacement(tpl, "t.sql").has_value());
        }
    }
    REQUIRE_FALSE(CheckAuthPlacement("SELECT * FROM t WHERE o = '{{{ auth.username }}}'", "t.sql").has_value());
}

TEST_CASE("SQLTemplateProcessor: the rendered SQL is checked, not only the source",
          "[sql_template_processor][auth][auth_quoting]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());
    SQLTemplateProcessor processor(config_manager);

    const std::vector<std::string> templates{
        "SELECT * FROM t WHERE o = E{{! c }}'{{{ auth.username }}}'",
        "SELECT * FROM t WHERE o = {{#auth.authenticated}}E{{/auth.authenticated}}'{{{ auth.username }}}'",
        "SELECT * FROM t WHERE o = ${{! }}$ {{{ auth.username }}} $$",
        "SELECT * FROM t -{{! }}- '{{{ auth.username }}}'",
        "SELECT * FROM t /{{! }}* '{{{ auth.username }}}' */",
        "SELECT * FROM query('SELECT * FROM d WHERE o = ''{{{ auth.username }}}''')",
        "SELECT * FROM t WHERE o = {{{ params.q }}}'{{{ auth.username }}}'"};
    for (const bool prepared : {false, true}) {
        for (std::size_t i = 0; i < templates.size(); ++i) {
            DYNAMIC_SECTION("prepared=" << prepared << " template #" << i) {
                std::ofstream(temp_dir.getPath() / "r.sql") << templates[i];
                EndpointConfig endpoint;
                endpoint.templateSource = "r.sql";
                std::map<std::string, std::string> params = {
                    {"__auth_username", "bob"}, {"__auth_authenticated", "true"}, {"q", "E"}};
                if (prepared) {
                    REQUIRE_THROWS_AS(processor.loadAndProcessTemplatePrepared(endpoint, params),
                                      std::invalid_argument);
                } else {
                    REQUIRE_THROWS_AS(processor.loadAndProcessTemplate(endpoint, params),
                                      std::invalid_argument);
                }
            }
        }
    }
}

TEST_CASE("Auth placement checks are linear in the template size",
          "[sql_template_processor][auth][auth_quoting][perf]") {
    std::string body = "SELECT * FROM t WHERE owner IN (";
    for (int i = 0; i < 20000; ++i) {
        body += "'{{{ auth.username }}}', ";
    }
    body += "'x')";

    auto t0 = std::chrono::steady_clock::now();
    REQUIRE_FALSE(CheckAuthPlacement(body, "big.sql").has_value());
    REQUIRE(FindUnquotedAuthVariables(body).empty());
    const auto check_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(check_ms < 1000);

    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());
    SQLTemplateProcessor processor(config_manager);
    std::ofstream(temp_dir.getPath() / "big.sql") << body;
    EndpointConfig endpoint;
    endpoint.templateSource = "big.sql";
    std::map<std::string, std::string> params = {{"__auth_username", "O'Brien"}};
    t0 = std::chrono::steady_clock::now();
    const auto sql = processor.loadAndProcessTemplate(endpoint, params);
    const auto render_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(sql.find("'O''Brien', 'x')") != std::string::npos);
    REQUIRE(render_ms < 1000);
}

TEST_CASE("Auth placement: SQL text is recognised by its content and across comments",
          "[sql_template_processor][auth][auth_quoting]") {
    const std::vector<std::string> refused{
        // A function the name list does not know, handed SQL text.
        "SELECT * FROM tenant_rows('SELECT id FROM d WHERE o = $${{{ auth.username }}}$$')",
        "SELECT * FROM tenant_rows('SELECT id FROM d WHERE o = $q${{{ auth.username }}}$q$')",
        "SELECT * FROM tenant_rows('SELECT id FROM d WHERE \"o\" = {{{ auth.username }}}')",
        "SELECT * FROM tenant_rows('SELECT id FROM d WHERE o = $q$' || '{{{ auth.username }}}' || '$q$')",
        "SELECT * FROM tenant_rows(concat('SELECT 1 WHERE o = $$', lower('{{{ auth.username }}}'), '$$'))",
        // The executing function's name across a comment, or quoted.
        "SELECT * FROM query/**/('{{{ auth.username }}}')",
        "SELECT * FROM query /* a */ /* b */ ('{{{ auth.username }}}')",
        "SELECT * FROM query -- c\n('{{{ auth.username }}}')",
        "SELECT * FROM \"query\"('{{{ auth.username }}}')",
        "SELECT * FROM main.\"bigquery_query\"('p', '{{{ auth.username }}}')",
        // E'...' and $$ after a number, as DuckDB lexes them.
        "SELECT 1E'{{{ auth.username }}}'",
        "SELECT 1$${{{ auth.username }}}$$",
    };
    for (std::size_t i = 0; i < refused.size(); ++i) {
        DYNAMIC_SECTION("refused #" << i << ": " << refused[i]) {
            REQUIRE(CheckAuthPlacement(refused[i], "t.sql").has_value());
        }
    }

    const std::vector<std::string> allowed{
        "SELECT * FROM t WHERE o = '{{{ auth.username }}}' AND n = '$$'",
        "SELECT $\xC3\xA9$it's$\xC3\xA9$ AS c FROM t WHERE o = '{{{ auth.username }}}'",
        "SELECT * FROM tenant_rows('SELECT 1') WHERE o = '{{{ auth.username }}}'",
        "SELECT * FROM t WHERE lower(o) = lower('{{{ auth.username }}}')",
        "SELECT * FROM t WHERE o IN ('{{{ auth.username }}}', 'x')",
        "SELECT a$$b FROM t WHERE o = '{{{ auth.username }}}'",
    };
    for (std::size_t i = 0; i < allowed.size(); ++i) {
        DYNAMIC_SECTION("allowed #" << i << ": " << allowed[i]) {
            REQUIRE_FALSE(CheckAuthPlacement(allowed[i], "t.sql").has_value());
        }
    }
}

TEST_CASE("SQLTemplateProcessor: a marker inside a token is still found at render",
          "[sql_template_processor][auth][auth_quoting]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    TemporaryDirectory temp_dir;
    config_manager->setTemplatePath(temp_dir.getPath().string());
    SQLTemplateProcessor processor(config_manager);

    const std::vector<std::string> templates{
        "SELECT $${{{ auth.username }}}$ FROM t",
        "SELECT ${{{ auth.username }}}$ x $$",
        "SELECT x{{{ auth.username }}} FROM t",
        "SELECT 1{{{ auth.username }}} FROM t",
        "SELECT * FROM tenant_rows('SELECT 1 WHERE o = $$' || '{{{ auth.username }}}' || '$$')"};
    for (std::size_t i = 0; i < templates.size(); ++i) {
        DYNAMIC_SECTION("template #" << i) {
            std::ofstream(temp_dir.getPath() / "m.sql") << templates[i];
            EndpointConfig endpoint;
            endpoint.templateSource = "m.sql";
            std::map<std::string, std::string> params = {{"__auth_username", "x$ OR 1=1 OR $"}};
            REQUIRE_THROWS_AS(processor.loadAndProcessTemplate(endpoint, params), std::invalid_argument);
        }
    }
}

TEST_CASE("SQLTemplateProcessor: {{ }} doubles single quotes instead of HTML-escaping (#277)",
          "[sql_template_processor][escaping]") {
    auto config_manager = std::make_shared<MockConfigManager>();
    SQLTemplateProcessor processor(config_manager);
    crow::mustache::context ctx;
    ctx["params"]["name"] = "O'Brien";
    ctx["params"]["sym"] = "a&b<c>\"d\"=`e`/f\\g";
    ctx["conn"]["path"] = "/data/x.parquet";

    SECTION("a quote is doubled, so the value stays inside its literal") {
        REQUIRE(processor.processTemplate("SELECT '{{ params.name }}'", ctx) == "SELECT 'O''Brien'");
        REQUIRE(processor.processTemplate("SELECT '{{params.name}}' AS n", ctx) == "SELECT 'O''Brien' AS n");
    }
    SECTION("nothing else is escaped: a path is a real path") {
        REQUIRE(processor.processTemplate("FROM read_parquet('{{ conn.path }}')", ctx) ==
                "FROM read_parquet('/data/x.parquet')");
        REQUIRE(processor.processTemplate("'{{ params.sym }}'", ctx) == "'a&b<c>\"d\"=`e`/f\\g'");
    }
    SECTION("inside a section too") {
        REQUIRE(processor.processTemplate("{{#params.name}}c = '{{ params.name }}'{{/params.name}}", ctx) ==
                "c = 'O''Brien'");
    }
    SECTION("triple braces and {{& }} still paste the value as it is") {
        REQUIRE(processor.processTemplate("{{{ params.name }}} {{& params.name }}", ctx) == "O'Brien O'Brien");
    }
}

TEST_CASE("crow mustache keeps HTML escaping by default (the Swagger UI page) (#277)",
          "[sql_template_processor][escaping]") {
    crow::mustache::context ctx;
    ctx["x"] = "<b>'&\"/=`";
    REQUIRE(crow::mustache::compile("{{x}}").render_string(ctx) == "&lt;b&gt;&#39;&amp;&quot;&#x2F;&#x3D;&#x60;");
    REQUIRE(crow::mustache::compile("{{{x}}}").render_string(ctx) == "<b>'&\"/=`");
}
