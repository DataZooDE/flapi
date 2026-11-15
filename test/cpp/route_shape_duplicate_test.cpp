#include <catch2/catch_test_macros.hpp>

#include "config_manager.hpp"
#include "test_utils.hpp"

using flapi::ConfigManager;
using flapi::EndpointConfig;

TEST_CASE("RouteShape ignores parameter names", "[config_manager][duplicates]") {
    CHECK(ConfigManager::RouteShape("/customers/:id") == ConfigManager::RouteShape("/customers/:cid"));
    CHECK(ConfigManager::RouteShape("/a/:x/b/:y") == ConfigManager::RouteShape("/a/:p/b/:q"));
    CHECK(ConfigManager::RouteShape("/customers/:id") != ConfigManager::RouteShape("/customers/7"));
    CHECK(ConfigManager::RouteShape("/customers/:id") != ConfigManager::RouteShape("/customers/:id/orders"));
    CHECK(ConfigManager::RouteShape("/customers") != ConfigManager::RouteShape("/customer"));
}

TEST_CASE("Endpoints with the same route shape are duplicates", "[config_manager][duplicates]") {
    // Security regression: an open /items/:item_id must not load next to an
    // authenticated /items/:id - load order would decide which one answers.
    flapi::test::TempTestConfig temp(R"(
project-name: shapes
template:
  path: ./sqls
connections:
  mem:
    properties:
      database: ':memory:'
)", "route_shape_duplicates");
    temp.writeSqlTemplate("a.sql", "SELECT 1 AS n\n");
    temp.writeEndpoint("items.yaml",
                       "url-path: /items/:id\nmethod: GET\ntemplate-source: a.sql\nconnection: [mem]\n");
    auto mgr = temp.createConfigManager();

    EndpointConfig open_copy;
    open_copy.urlPath = "/items/:item_id";
    open_copy.method = "GET";
    open_copy.config_file_path = "/elsewhere/items_open.yaml";
    const auto problem = mgr->DuplicateEndpointProblem(open_copy);
    REQUIRE_FALSE(problem.empty());
    CHECK(problem.find("Duplicate endpoint GET /items/:item_id") != std::string::npos);
    CHECK(problem.find("same route as /items/:id") != std::string::npos);

    EndpointConfig other_method = open_copy;
    other_method.method = "POST";
    CHECK(mgr->DuplicateEndpointProblem(other_method).empty());

    EndpointConfig literal = open_copy;
    literal.urlPath = "/items/special";
    CHECK(mgr->DuplicateEndpointProblem(literal).empty());
}
