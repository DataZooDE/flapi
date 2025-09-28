#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "config_service.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace flapi;

namespace {

std::pair<std::filesystem::path, std::filesystem::path> createTestConfig(bool with_cache = true) {
    auto temp_dir = std::filesystem::temp_directory_path() / std::filesystem::path("config_service_tests");
    std::filesystem::create_directories(temp_dir);

    auto config_path = temp_dir / "config.yaml";
    std::ofstream config_file(config_path);
    config_file << "project_name: test\nproject_description: desc\ntemplate:\n  path: " << temp_dir << "\n";
    config_file.close();

    auto endpoint_path = temp_dir / "endpoint.yaml";
    std::ofstream endpoint_file(endpoint_path);
    endpoint_file << "url-path: /test\n";
    endpoint_file << "template-source: test.sql\n";
    endpoint_file << "method: GET\n";
    if (with_cache) {
        endpoint_file << "cache:\n  enabled: true\n  table: test_cache\n  schema: cache\n";
    }
    endpoint_file.close();

    return {config_path, endpoint_path};
} 

}

TEST_CASE("ConfigService: Get cache config when disabled", "[config_service]") {
    auto [config_path, endpoint_path] = createTestConfig(false);

    auto config_mgr = std::make_shared<ConfigManager>(config_path);
    config_mgr->loadConfig();
    config_mgr->loadEndpointConfig(endpoint_path);

    ConfigService service(config_mgr);

    auto response = service.getCacheConfig(crow::request{}, "/test");
    REQUIRE(response.code == crow::status::OK);
    auto json = crow::json::load(response.body);
    REQUIRE(json["enabled"].b() == false);
}