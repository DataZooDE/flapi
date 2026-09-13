#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <any>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
// crow must be parsed with its real access specifiers: including it under the
// hack below changes crow::response's layout in this TU only, so a default-
// constructed response disagrees with the one request_handler.cpp sees.
#include <crow.h>
#define private public
#include "database_manager.hpp"
#undef private
#include "cache_database_adapter.hpp"
#include "config_manager.hpp"
#include "config_service.hpp"
#include "query_executor.hpp"

using namespace flapi;

namespace {

std::pair<std::filesystem::path, std::filesystem::path> createTestConfig(bool with_cache = true) {
    auto temp_dir = std::filesystem::temp_directory_path() / std::filesystem::path("config_service_tests");
    std::filesystem::create_directories(temp_dir);

    auto config_path = temp_dir / "config.yaml";
    std::ofstream config_file(config_path);
    config_file << "project-name: test\nproject-description: desc\ntemplate:\n  path: " << temp_dir << "\n";
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

class SuccessfulCacheAdapter : public ICacheDatabaseAdapter {
public:
    std::string renderCacheTemplate(const EndpointConfig& endpoint,
                                    const CacheConfig& cacheConfig,
                                    std::map<std::string, std::string>& params) override {
        (void)endpoint;
        (void)cacheConfig;
        (void)params;
        return "SELECT 1";
    }

    void executeDuckLakeQuery(const std::string& query,
                              const std::map<std::string, std::string>& params = {}) override {
        (void)query;
        (void)params;
    }

    QueryResult executeDuckLakeQueryWithResult(const std::string& query) override {
        (void)query;
        QueryResult result;
        result.data = crow::json::wvalue::list();
        return result;
    }
};

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

TEST_CASE("ConfigService: manual cache refresh updates shared readiness state", "[config_service][cache][readiness]") {
    auto [config_path, endpoint_path] = createTestConfig(true);

    auto config_mgr = std::make_shared<ConfigManager>(config_path);
    config_mgr->loadConfig();
    config_mgr->loadEndpointConfig(endpoint_path);
    auto* endpoint = config_mgr->getEndpointForPath("/test");
    REQUIRE(endpoint != nullptr);

    auto db_manager = DatabaseManager::getInstance();
    db_manager->reset();
    auto shared_cache_manager = std::make_shared<CacheManager>(std::make_shared<SuccessfulCacheAdapter>());
    db_manager->cache_manager = shared_cache_manager;

    shared_cache_manager->initializeReadiness(config_mgr);
    shared_cache_manager->markCacheFailed(config_mgr, *endpoint, "warmup failed");

    CacheConfigHandler handler(config_mgr);
    auto response = handler.refreshCache(crow::request{}, "/test");

    REQUIRE(response.code == crow::status::OK);
    auto readiness = shared_cache_manager->getEndpointReadiness(config_mgr, *endpoint);
    REQUIRE(readiness.state == CacheManager::ReadinessState::Ready);
}
