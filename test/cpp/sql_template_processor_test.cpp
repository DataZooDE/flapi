#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <fstream>
#include <filesystem>
#include <random>

#include "sql_template_processor.hpp"
#include "config_manager.hpp"

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