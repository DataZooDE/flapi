#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <filesystem>

#include "config_manager.hpp"
#include "test_utils.hpp"

namespace flapi {
namespace test {

// No HttpsConfigTestHelper needed - using TempTestConfig from test_utils.hpp

TEST_CASE("HttpsConfig: HTTPS disabled by default", "[https][config]") {
    std::string config_content = R"(
project-name: test-project
project-description: Test project for HTTPS configuration
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

    TempTestConfig temp(config_content, "test_https");

    SECTION("HTTPS is disabled when not configured") {
        try {
            auto config_manager = temp.createConfigManager();
            REQUIRE(config_manager->isHttpsEnforced() == false);
        } catch (...) {
            // TempTestConfig automatically cleans up
            throw;
        }
    }

    // TempTestConfig automatically cleans up
}

TEST_CASE("HttpsConfig: HTTPS explicitly disabled", "[https][config]") {
    std::string config_content = R"(
project-name: test-project
project-description: Test project for HTTPS configuration
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
enforce-https:
  enabled: false
)";

    TempTestConfig temp(config_content, "test_https");

    SECTION("HTTPS disabled when enabled is false") {
        try {
            auto config_manager = temp.createConfigManager();
            REQUIRE(config_manager->isHttpsEnforced() == false);

            const auto& https_config = config_manager->getHttpsConfig();
            REQUIRE(https_config.enabled == false);
        } catch (...) {
            // TempTestConfig automatically cleans up
            throw;
        }
    }

    // TempTestConfig automatically cleans up
}

TEST_CASE("HttpsConfig: HTTPS enabled with valid paths", "[https][config]") {
    // Create temporary cert and key files for testing
    std::string cert_path = std::filesystem::temp_directory_path().string() + "/test_cert_" + std::to_string(rand()) + ".pem";
    std::string key_path = std::filesystem::temp_directory_path().string() + "/test_key_" + std::to_string(rand()) + ".pem";

    // Create dummy files
    {
        std::ofstream cert_file(cert_path);
        cert_file << "-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----\n";
        std::ofstream key_file(key_path);
        key_file << "-----BEGIN PRIVATE KEY-----\ntest\n-----END PRIVATE KEY-----\n";
    }

    std::string config_content =
        "project-name: test-project\n"
        "project-description: Test project for HTTPS configuration\n"
        "http-port: 8080\n"
        "template:\n"
        "  path: ./sqls\n"
        "connections:\n"
        "  test:\n"
        "    properties:\n"
        "      path: ./data.parquet\n"
        "enforce-https:\n"
        "  enabled: true\n"
        "  ssl-cert-file: " + cert_path + "\n"
        "  ssl-key-file: " + key_path + "\n";

    TempTestConfig temp(config_content, "test_https");

    SECTION("HTTPS enabled with valid certificate paths") {
        try {
            auto config_manager = temp.createConfigManager();
            REQUIRE(config_manager->isHttpsEnforced() == true);

            const auto& https_config = config_manager->getHttpsConfig();
            REQUIRE(https_config.enabled == true);
            REQUIRE(https_config.ssl_cert_file == cert_path);
            REQUIRE(https_config.ssl_key_file == key_path);
        } catch (const std::exception& e) {
            // TempTestConfig automatically cleans up
            std::filesystem::remove(cert_path);
            std::filesystem::remove(key_path);
            FAIL("Unexpected exception: " << e.what());
        }
    }

    // TempTestConfig automatically cleans up
    std::filesystem::remove(cert_path);
    std::filesystem::remove(key_path);
}

TEST_CASE("HttpsConfig: HTTPS enabled missing cert file throws error", "[https][config]") {
    std::string config_content = R"(
project-name: test-project
project-description: Test project for HTTPS configuration
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
enforce-https:
  enabled: true
  ssl-key-file: /path/to/key.pem
)";

    TempTestConfig temp(config_content, "test_https");

    SECTION("Throws ConfigurationError when cert file missing") {
        try {
            REQUIRE_THROWS_AS(
                temp.createConfigManager(),
                ConfigurationError
            );
        } catch (...) {
            // TempTestConfig automatically cleans up
            throw;
        }
    }

    // TempTestConfig automatically cleans up
}

TEST_CASE("HttpsConfig: HTTPS enabled missing key file throws error", "[https][config]") {
    std::string config_content = R"(
project-name: test-project
project-description: Test project for HTTPS configuration
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
enforce-https:
  enabled: true
  ssl-cert-file: /path/to/cert.pem
)";

    TempTestConfig temp(config_content, "test_https");

    SECTION("Throws ConfigurationError when key file missing") {
        try {
            REQUIRE_THROWS_AS(
                temp.createConfigManager(),
                ConfigurationError
            );
        } catch (...) {
            // TempTestConfig automatically cleans up
            throw;
        }
    }

    // TempTestConfig automatically cleans up
}

TEST_CASE("HttpsConfig: HTTPS enabled missing both files throws error", "[https][config]") {
    std::string config_content = R"(
project-name: test-project
project-description: Test project for HTTPS configuration
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
enforce-https:
  enabled: true
)";

    TempTestConfig temp(config_content, "test_https");

    SECTION("Throws ConfigurationError when both cert and key files missing") {
        try {
            REQUIRE_THROWS_AS(
                temp.createConfigManager(),
                ConfigurationError
            );
        } catch (...) {
            // TempTestConfig automatically cleans up
            throw;
        }
    }

    // TempTestConfig automatically cleans up
}

TEST_CASE("HttpsConfig: enforce-https not a map throws error", "[https][config]") {
    std::string config_content = R"(
project-name: test-project
project-description: Test project for HTTPS configuration
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
enforce-https: true
)";

    TempTestConfig temp(config_content, "test_https");

    SECTION("Throws ConfigurationError when enforce-https is not a map") {
        try {
            REQUIRE_THROWS_AS(
                temp.createConfigManager(),
                ConfigurationError
            );
        } catch (...) {
            // TempTestConfig automatically cleans up
            throw;
        }
    }

    // TempTestConfig automatically cleans up
}

TEST_CASE("HttpsConfig: isHttpsEnforced returns correct value", "[https][config]") {
    std::string disabled_config = R"(
project-name: test-project
project-description: Test project for HTTPS configuration
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
)";

    TempTestConfig temp(disabled_config, "test_https");

    SECTION("isHttpsEnforced returns false when disabled") {
        try {
            auto config_manager = temp.createConfigManager();
            REQUIRE(config_manager->isHttpsEnforced() == false);
        } catch (...) {
            // TempTestConfig automatically cleans up
            throw;
        }
    }

    // TempTestConfig automatically cleans up
}

TEST_CASE("HttpsConfig: getHttpsConfig returns complete struct", "[https][config]") {
    // Create temporary cert and key files for testing
    std::string cert_path = std::filesystem::temp_directory_path().string() + "/test_cert2_" + std::to_string(rand()) + ".pem";
    std::string key_path = std::filesystem::temp_directory_path().string() + "/test_key2_" + std::to_string(rand()) + ".pem";

    // Create dummy files
    {
        std::ofstream cert_file(cert_path);
        cert_file << "cert content";
        std::ofstream key_file(key_path);
        key_file << "key content";
    }

    std::string config_content =
        "project-name: test-project\n"
        "project-description: Test project for HTTPS configuration\n"
        "http-port: 8080\n"
        "template:\n"
        "  path: ./sqls\n"
        "connections:\n"
        "  test:\n"
        "    properties:\n"
        "      path: ./data.parquet\n"
        "enforce-https:\n"
        "  enabled: true\n"
        "  ssl-cert-file: " + cert_path + "\n"
        "  ssl-key-file: " + key_path + "\n";

    TempTestConfig temp(config_content, "test_https");

    SECTION("getHttpsConfig returns all fields correctly") {
        try {
            auto config_manager = temp.createConfigManager();
            const auto& https_config = config_manager->getHttpsConfig();

            REQUIRE(https_config.enabled == true);
            REQUIRE_FALSE(https_config.ssl_cert_file.empty());
            REQUIRE_FALSE(https_config.ssl_key_file.empty());
            REQUIRE(https_config.ssl_cert_file == cert_path);
            REQUIRE(https_config.ssl_key_file == key_path);
        } catch (const std::exception& e) {
            // TempTestConfig automatically cleans up
            std::filesystem::remove(cert_path);
            std::filesystem::remove(key_path);
            FAIL("Unexpected exception: " << e.what());
        }
    }

    // TempTestConfig automatically cleans up
    std::filesystem::remove(cert_path);
    std::filesystem::remove(key_path);
}

TEST_CASE("HttpsConfig: disabled HTTPS has empty cert/key paths", "[https][config]") {
    std::string config_content = R"(
project-name: test-project
project-description: Test project for HTTPS configuration
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
enforce-https:
  enabled: false
)";

    TempTestConfig temp(config_content, "test_https");

    SECTION("Disabled HTTPS does not require cert/key paths") {
        try {
            auto config_manager = temp.createConfigManager();
            const auto& https_config = config_manager->getHttpsConfig();

            REQUIRE(https_config.enabled == false);
            REQUIRE(https_config.ssl_cert_file.empty());
            REQUIRE(https_config.ssl_key_file.empty());
        } catch (const std::exception& e) {
            // TempTestConfig automatically cleans up
            FAIL("Unexpected exception: " << e.what());
        }
    }

    // TempTestConfig automatically cleans up
}

TEST_CASE("HttpsConfig: empty string cert/key paths treated as missing", "[https][config]") {
    std::string config_content = R"(
project-name: test-project
project-description: Test project for HTTPS configuration
http-port: 8080
template:
  path: ./sqls
connections:
  test:
    properties:
      path: ./data.parquet
enforce-https:
  enabled: true
  ssl-cert-file: ""
  ssl-key-file: ""
)";

    TempTestConfig temp(config_content, "test_https");

    SECTION("Empty cert/key paths throw ConfigurationError when enabled") {
        try {
            REQUIRE_THROWS_AS(
                temp.createConfigManager(),
                ConfigurationError
            );
        } catch (...) {
            // TempTestConfig automatically cleans up
            throw;
        }
    }

    // TempTestConfig automatically cleans up
}

} // namespace test
} // namespace flapi
