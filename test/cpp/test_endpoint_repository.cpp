#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "endpoint_repository.hpp"

using namespace flapi;

// Helper function to create a test REST endpoint
EndpointConfig createRestEndpoint(
    const std::string& url_path,
    const std::string& method = "GET",
    const std::string& template_source = "test.sql") {
    EndpointConfig ep;
    ep.urlPath = url_path;
    ep.method = method;
    ep.templateSource = template_source;
    ep.connection = {"default"};
    return ep;
}

// Helper function to create a test MCP tool endpoint
EndpointConfig createMCPToolEndpoint(
    const std::string& name,
    const std::string& description = "Test tool") {
    EndpointConfig ep;
    ep.mcp_tool = EndpointConfig::MCPToolInfo{
        name,
        description,
        "application/json"
    };
    ep.templateSource = "test.sql";
    ep.connection = {"default"};
    return ep;
}

// Helper function to create a REST endpoint that's also an MCP tool
EndpointConfig createDualEndpoint(
    const std::string& url_path,
    const std::string& mcp_name) {
    EndpointConfig ep = createRestEndpoint(url_path);
    ep.method = "POST";
    ep.mcp_tool = EndpointConfig::MCPToolInfo{
        mcp_name,
        "Dual endpoint",
        "application/json"
    };
    return ep;
}

TEST_CASE("EndpointRepository basic operations", "[endpoints][repository]") {
    EndpointRepository repo;

    SECTION("Empty repository has zero endpoints") {
        REQUIRE(repo.count() == 0);
        REQUIRE(repo.countRestEndpoints() == 0);
        REQUIRE(repo.countMCPEndpoints() == 0);
    }

    SECTION("Add single REST endpoint") {
        EndpointConfig ep = createRestEndpoint("/customers", "GET");
        repo.addEndpoint(ep);

        REQUIRE(repo.count() == 1);
        REQUIRE(repo.countRestEndpoints() == 1);
        REQUIRE(repo.countMCPEndpoints() == 0);
    }

    SECTION("Add single MCP endpoint") {
        EndpointConfig ep = createMCPToolEndpoint("customer_lookup");
        repo.addEndpoint(ep);

        REQUIRE(repo.count() == 1);
        REQUIRE(repo.countRestEndpoints() == 0);
        REQUIRE(repo.countMCPEndpoints() == 1);
    }

    SECTION("Add multiple endpoints") {
        repo.addEndpoint(createRestEndpoint("/customers", "GET"));
        repo.addEndpoint(createRestEndpoint("/customers", "POST"));
        repo.addEndpoint(createRestEndpoint("/orders", "GET"));
        repo.addEndpoint(createMCPToolEndpoint("customer_lookup"));

        REQUIRE(repo.count() == 4);
        REQUIRE(repo.countRestEndpoints() == 3);
        REQUIRE(repo.countMCPEndpoints() == 1);
    }

    SECTION("Clear repository") {
        repo.addEndpoint(createRestEndpoint("/customers"));
        repo.addEndpoint(createMCPToolEndpoint("customer_lookup"));

        REQUIRE(repo.count() == 2);

        repo.clear();

        REQUIRE(repo.count() == 0);
        REQUIRE(repo.countRestEndpoints() == 0);
        REQUIRE(repo.countMCPEndpoints() == 0);
    }
}

TEST_CASE("EndpointRepository REST endpoint operations", "[endpoints][repository]") {
    EndpointRepository repo;

    SECTION("Get REST endpoint by path and method") {
        EndpointConfig ep = createRestEndpoint("/customers", "GET");
        repo.addEndpoint(ep);

        auto retrieved = repo.getEndpointByRestPath("/customers", "GET");

        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->urlPath == "/customers");
        REQUIRE(retrieved->method == "GET");
    }

    SECTION("Get non-existent REST endpoint returns empty") {
        auto retrieved = repo.getEndpointByRestPath("/nonexistent", "GET");

        REQUIRE_FALSE(retrieved.has_value());
    }

    SECTION("Different methods are separate endpoints") {
        repo.addEndpoint(createRestEndpoint("/customers", "GET"));
        repo.addEndpoint(createRestEndpoint("/customers", "POST"));

        REQUIRE(repo.hasRestEndpoint("/customers", "GET"));
        REQUIRE(repo.hasRestEndpoint("/customers", "POST"));
        REQUIRE_FALSE(repo.hasRestEndpoint("/customers", "DELETE"));
    }

    SECTION("Replace REST endpoint with same path/method") {
        EndpointConfig ep1 = createRestEndpoint("/customers", "GET", "customers.sql");
        repo.addEndpoint(ep1);

        EndpointConfig ep2 = createRestEndpoint("/customers", "GET", "customers_v2.sql");
        repo.addEndpoint(ep2);

        REQUIRE(repo.countRestEndpoints() == 1);
        auto retrieved = repo.getEndpointByRestPath("/customers", "GET");
        REQUIRE(retrieved->templateSource == "customers_v2.sql");
    }

    SECTION("Remove REST endpoint") {
        repo.addEndpoint(createRestEndpoint("/customers", "GET"));
        repo.addEndpoint(createRestEndpoint("/orders", "GET"));

        bool removed = repo.removeRestEndpoint("/customers", "GET");

        REQUIRE(removed);
        REQUIRE(repo.countRestEndpoints() == 1);
        REQUIRE_FALSE(repo.hasRestEndpoint("/customers", "GET"));
        REQUIRE(repo.hasRestEndpoint("/orders", "GET"));
    }

    SECTION("Remove non-existent REST endpoint returns false") {
        bool removed = repo.removeRestEndpoint("/nonexistent", "GET");

        REQUIRE_FALSE(removed);
    }
}

TEST_CASE("EndpointRepository MCP endpoint operations", "[endpoints][repository]") {
    EndpointRepository repo;

    SECTION("Get MCP endpoint by name") {
        EndpointConfig ep = createMCPToolEndpoint("customer_lookup", "Get customer info");
        repo.addEndpoint(ep);

        auto retrieved = repo.getEndpointByMCPName("customer_lookup");

        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->mcp_tool->name == "customer_lookup");
        REQUIRE(retrieved->mcp_tool->description == "Get customer info");
    }

    SECTION("Get non-existent MCP endpoint returns empty") {
        auto retrieved = repo.getEndpointByMCPName("nonexistent");

        REQUIRE_FALSE(retrieved.has_value());
    }

    SECTION("Check MCP endpoint existence") {
        repo.addEndpoint(createMCPToolEndpoint("customer_lookup"));

        REQUIRE(repo.hasMCPEndpoint("customer_lookup"));
        REQUIRE_FALSE(repo.hasMCPEndpoint("nonexistent"));
    }

    SECTION("Remove MCP endpoint") {
        repo.addEndpoint(createMCPToolEndpoint("customer_lookup"));
        repo.addEndpoint(createMCPToolEndpoint("order_lookup"));

        bool removed = repo.removeMCPEndpoint("customer_lookup");

        REQUIRE(removed);
        REQUIRE(repo.countMCPEndpoints() == 1);
        REQUIRE_FALSE(repo.hasMCPEndpoint("customer_lookup"));
        REQUIRE(repo.hasMCPEndpoint("order_lookup"));
    }

    SECTION("Remove non-existent MCP endpoint returns false") {
        bool removed = repo.removeMCPEndpoint("nonexistent");

        REQUIRE_FALSE(removed);
    }
}

TEST_CASE("EndpointRepository dual (REST + MCP) endpoints", "[endpoints][repository]") {
    EndpointRepository repo;

    SECTION("Add endpoint that's both REST and MCP") {
        EndpointConfig ep = createDualEndpoint("/customers", "create_customer");
        repo.addEndpoint(ep);

        // Should be retrievable as both REST and MCP
        REQUIRE(repo.hasRestEndpoint("/customers", "POST"));
        REQUIRE(repo.hasMCPEndpoint("create_customer"));

        // Count should be 1 (not 2), as it's the same endpoint
        REQUIRE(repo.count() == 1);

        // But counts should reflect both categories
        REQUIRE(repo.countRestEndpoints() == 1);
        REQUIRE(repo.countMCPEndpoints() == 1);
    }

    SECTION("Retrieve dual endpoint by REST path") {
        EndpointConfig ep = createDualEndpoint("/customers", "create_customer");
        repo.addEndpoint(ep);

        auto retrieved = repo.getEndpointByRestPath("/customers", "POST");

        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->urlPath == "/customers");
        REQUIRE(retrieved->mcp_tool.has_value());
    }

    SECTION("Retrieve dual endpoint by MCP name") {
        EndpointConfig ep = createDualEndpoint("/customers", "create_customer");
        repo.addEndpoint(ep);

        auto retrieved = repo.getEndpointByMCPName("create_customer");

        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->mcp_tool->name == "create_customer");
        REQUIRE(retrieved->urlPath == "/customers");
    }

    SECTION("Remove REST endpoint independently from MCP") {
        EndpointConfig ep = createDualEndpoint("/customers", "create_customer");
        repo.addEndpoint(ep);

        repo.removeRestEndpoint("/customers", "POST");

        // After removing REST, MCP endpoint remains (they're independent indices)
        REQUIRE(repo.countRestEndpoints() == 0);
        REQUIRE(repo.countMCPEndpoints() == 1);
        REQUIRE(repo.hasMCPEndpoint("create_customer"));
    }

    SECTION("Remove MCP endpoint independently from REST") {
        EndpointConfig ep = createDualEndpoint("/customers", "create_customer");
        repo.addEndpoint(ep);

        repo.removeMCPEndpoint("create_customer");

        // After removing MCP, REST endpoint remains (they're independent indices)
        REQUIRE(repo.countRestEndpoints() == 1);
        REQUIRE(repo.countMCPEndpoints() == 0);
        REQUIRE(repo.hasRestEndpoint("/customers", "POST"));
    }
}

TEST_CASE("EndpointRepository get all endpoints", "[endpoints][repository]") {
    EndpointRepository repo;

    SECTION("Get all endpoints includes REST and MCP") {
        repo.addEndpoint(createRestEndpoint("/customers", "GET"));
        repo.addEndpoint(createRestEndpoint("/customers", "POST"));
        repo.addEndpoint(createMCPToolEndpoint("customer_lookup"));
        repo.addEndpoint(createDualEndpoint("/orders", "create_order"));

        auto all = repo.getAllEndpoints();

        // Should have 4 total: 2 REST only + 1 MCP only + 1 dual (counted once)
        // Actually: 3 REST + 1 MCP_only + 1 dual = 5, but dual counted in both so 4 unique
        REQUIRE(all.size() == 4);
    }

    SECTION("Get all REST endpoints") {
        repo.addEndpoint(createRestEndpoint("/customers", "GET"));
        repo.addEndpoint(createRestEndpoint("/customers", "POST"));
        repo.addEndpoint(createMCPToolEndpoint("customer_lookup"));

        auto rest = repo.getAllRestEndpoints();

        REQUIRE(rest.size() == 2);
        for (const auto& ep : rest) {
            REQUIRE_FALSE(ep.urlPath.empty());
        }
    }

    SECTION("Get all MCP endpoints") {
        repo.addEndpoint(createRestEndpoint("/customers", "GET"));
        repo.addEndpoint(createMCPToolEndpoint("customer_lookup"));
        repo.addEndpoint(createMCPToolEndpoint("order_lookup"));

        auto mcp = repo.getAllMCPEndpoints();

        REQUIRE(mcp.size() == 2);
        for (const auto& ep : mcp) {
            REQUIRE(ep.mcp_tool.has_value());
        }
    }
}

TEST_CASE("EndpointRepository find endpoints", "[endpoints][repository]") {
    EndpointRepository repo;

    repo.addEndpoint(createRestEndpoint("/customers", "GET"));
    repo.addEndpoint(createRestEndpoint("/customers", "POST"));
    repo.addEndpoint(createRestEndpoint("/orders", "GET"));
    repo.addEndpoint(createMCPToolEndpoint("customer_lookup"));

    SECTION("Find endpoints by predicate") {
        auto results = repo.findEndpoints([](const EndpointConfig& ep) {
            return ep.method == "POST";
        });

        REQUIRE(results.size() == 1);
        REQUIRE(results[0].urlPath == "/customers");
        REQUIRE(results[0].method == "POST");
    }

    SECTION("Find endpoints matching path prefix") {
        auto results = repo.findEndpoints([](const EndpointConfig& ep) {
            return ep.urlPath.find("/customers") == 0;
        });

        REQUIRE(results.size() == 2);
    }

    SECTION("Find all GET endpoints") {
        auto results = repo.findEndpoints([](const EndpointConfig& ep) {
            return ep.method == "GET";
        });

        REQUIRE(results.size() == 2);
        for (const auto& ep : results) {
            REQUIRE(ep.method == "GET");
        }
    }

    SECTION("Find endpoints with no matches") {
        auto results = repo.findEndpoints([](const EndpointConfig& ep) {
            return ep.method == "DELETE";
        });

        REQUIRE(results.empty());
    }

    SECTION("Find MCP endpoints only") {
        auto results = repo.findEndpoints([](const EndpointConfig& ep) {
            return ep.mcp_tool.has_value();
        });

        REQUIRE(results.size() == 1);
        REQUIRE(results[0].mcp_tool->name == "customer_lookup");
    }
}

TEST_CASE("EndpointRepository complex scenarios", "[endpoints][repository]") {
    EndpointRepository repo;

    SECTION("Many endpoints of same path, different methods") {
        repo.addEndpoint(createRestEndpoint("/customers", "GET"));
        repo.addEndpoint(createRestEndpoint("/customers", "POST"));
        repo.addEndpoint(createRestEndpoint("/customers", "PUT"));
        repo.addEndpoint(createRestEndpoint("/customers", "DELETE"));
        repo.addEndpoint(createRestEndpoint("/customers", "PATCH"));

        REQUIRE(repo.countRestEndpoints() == 5);

        auto getEp = repo.getEndpointByRestPath("/customers", "GET");
        auto postEp = repo.getEndpointByRestPath("/customers", "POST");

        REQUIRE(getEp->method == "GET");
        REQUIRE(postEp->method == "POST");
    }

    SECTION("Mix of REST paths and MCP names") {
        repo.addEndpoint(createRestEndpoint("/customers", "GET"));
        repo.addEndpoint(createRestEndpoint("/customers/{id}", "GET"));
        repo.addEndpoint(createRestEndpoint("/orders", "GET"));
        repo.addEndpoint(createMCPToolEndpoint("get_customer"));
        repo.addEndpoint(createMCPToolEndpoint("get_orders"));
        repo.addEndpoint(createMCPToolEndpoint("create_order"));

        REQUIRE(repo.countRestEndpoints() == 3);
        REQUIRE(repo.countMCPEndpoints() == 3);
        REQUIRE(repo.count() == 6);
    }

    SECTION("Stress test: many endpoints") {
        // Add 50 REST endpoints
        for (int i = 0; i < 50; i++) {
            repo.addEndpoint(createRestEndpoint("/resource" + std::to_string(i), "GET"));
        }

        // Add 50 MCP endpoints
        for (int i = 0; i < 50; i++) {
            repo.addEndpoint(createMCPToolEndpoint("tool_" + std::to_string(i)));
        }

        REQUIRE(repo.countRestEndpoints() == 50);
        REQUIRE(repo.countMCPEndpoints() == 50);
        REQUIRE(repo.count() == 100);

        // Verify retrieval still works
        REQUIRE(repo.hasRestEndpoint("/resource25", "GET"));
        REQUIRE(repo.hasMCPEndpoint("tool_25"));
    }

    SECTION("Remove and re-add endpoint") {
        EndpointConfig ep = createRestEndpoint("/customers", "GET");

        repo.addEndpoint(ep);
        REQUIRE(repo.hasRestEndpoint("/customers", "GET"));

        repo.removeRestEndpoint("/customers", "GET");
        REQUIRE_FALSE(repo.hasRestEndpoint("/customers", "GET"));

        repo.addEndpoint(ep);
        REQUIRE(repo.hasRestEndpoint("/customers", "GET"));
    }

    SECTION("Replace endpoint with different config") {
        EndpointConfig ep1 = createRestEndpoint("/customers", "GET", "customers.sql");
        repo.addEndpoint(ep1);

        EndpointConfig ep2 = createRestEndpoint("/customers", "GET", "customers_v2.sql");
        repo.addEndpoint(ep2);

        auto retrieved = repo.getEndpointByRestPath("/customers", "GET");
        REQUIRE(retrieved->templateSource == "customers_v2.sql");
    }
}

TEST_CASE("EndpointRepository edge cases", "[endpoints][repository]") {
    EndpointRepository repo;

    SECTION("Empty path handling") {
        EndpointConfig ep;
        ep.method = "GET";
        ep.connection = {"default"};
        // Empty urlPath means this is MCP-only

        repo.addEndpoint(ep);

        // Should not be added to REST endpoints
        REQUIRE(repo.countRestEndpoints() == 0);
    }

    SECTION("Case sensitive path matching") {
        repo.addEndpoint(createRestEndpoint("/Customers", "GET"));
        repo.addEndpoint(createRestEndpoint("/customers", "GET"));

        REQUIRE(repo.hasRestEndpoint("/Customers", "GET"));
        REQUIRE(repo.hasRestEndpoint("/customers", "GET"));
        REQUIRE(repo.countRestEndpoints() == 2);
    }

    SECTION("Method case sensitivity") {
        repo.addEndpoint(createRestEndpoint("/customers", "GET"));

        REQUIRE(repo.hasRestEndpoint("/customers", "GET"));
        REQUIRE_FALSE(repo.hasRestEndpoint("/customers", "get"));
    }

    SECTION("Empty MCP name") {
        EndpointConfig ep;
        ep.urlPath = "/test";
        ep.method = "GET";
        ep.connection = {"default"};

        repo.addEndpoint(ep);

        // Should only be added as REST endpoint
        REQUIRE(repo.countRestEndpoints() == 1);
        REQUIRE(repo.countMCPEndpoints() == 0);
    }
}
