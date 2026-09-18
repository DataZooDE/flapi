// Section 4.1 - the endpoint table is mutated at runtime while readers hold
// pointers into it.
//
// ConfigManager::endpoints is a bare std::vector<EndpointConfig>.
// getEndpointForPathAndMethod returns &endpoint into that vector, and
// addEndpoint / removeEndpointByPath push_back and erase on it - reachable at
// runtime from the config service (config_service.cpp:831, :895) and the MCP
// config tools (config_tool_adapter.cpp:874, :1011), on request threads, with no
// lock against in-flight requests. A push_back that reallocates invalidates every
// outstanding pointer.
//
// Today that window is confined to one handler. The observability epic would
// widen it across before_handle -> handler -> after_handle and make it a
// documented design element, so it is fixed first.
//
// These tests are written against the snapshot API rather than by racing threads
// and hoping for a crash: reading freed memory is undefined, so a test that
// "detects" it is unreliable. Instead they pin the *property* that makes the
// pattern safe - a snapshot taken before a mutation stays valid and unchanged
// after it.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>

#include "config_manager.hpp"
#include "test_utils.hpp"

using namespace flapi;
using flapi::test::TempTestConfig;

namespace {

EndpointConfig makeEndpoint(const std::string& path) {
    EndpointConfig e;
    e.urlPath = path;
    e.method = "GET";
    e.templateSource = "x.sql";
    return e;
}

}  // namespace

TEST_CASE("an endpoint snapshot survives concurrent mutation", "[endpoints][snapshot]") {
    TempTestConfig temp("flapi_snapshot");
    auto cm_ptr = temp.createConfigManager();
    ConfigManager& cm = *cm_ptr;
    for (int i = 0; i < 8; ++i) {
        cm.addEndpoint(makeEndpoint("/seed/" + std::to_string(i)));
    }

    SECTION("a snapshot is unaffected by later additions") {
        auto snap = cm.endpointsSnapshot();
        const std::size_t before = snap->size();

        // Enough to force at least one reallocation of the live vector.
        for (int i = 0; i < 256; ++i) {
            cm.addEndpoint(makeEndpoint("/added/" + std::to_string(i)));
        }

        // The pinned snapshot still reads correctly - this is the whole point.
        REQUIRE(snap->size() == before);
        REQUIRE(snap->at(0).urlPath == "/seed/0");
        REQUIRE(cm.endpointsSnapshot()->size() == before + 256);
    }

    SECTION("a snapshot is unaffected by later removals") {
        auto snap = cm.endpointsSnapshot();
        const std::size_t before = snap->size();

        REQUIRE(cm.removeEndpointByPath("/seed/0"));

        REQUIRE(snap->size() == before);
        REQUIRE(snap->at(0).urlPath == "/seed/0");
        REQUIRE(cm.endpointsSnapshot()->size() == before - 1);
    }

    SECTION("lookups against a pinned snapshot stay valid across mutation") {
        auto snap = cm.endpointsSnapshot();
        const EndpointConfig* found = ConfigManager::findEndpoint(*snap, "/seed/3", "GET");
        REQUIRE(found != nullptr);

        for (int i = 0; i < 256; ++i) {
            cm.addEndpoint(makeEndpoint("/churn/" + std::to_string(i)));
        }

        // Reading through `found` after 256 reallocations is exactly the
        // use-after-free this fixes; it is safe only because `snap` pins it.
        REQUIRE(found->urlPath == "/seed/3");
    }
}

TEST_CASE("readers and a writer can run concurrently", "[endpoints][snapshot]") {
    // Under ASan/TSan this is the regression test for the data race. Without a
    // snapshot it reads a vector being reallocated underneath it.
    TempTestConfig temp("flapi_snapshot");
    auto cm_ptr = temp.createConfigManager();
    ConfigManager& cm = *cm_ptr;
    for (int i = 0; i < 16; ++i) {
        cm.addEndpoint(makeEndpoint("/r/" + std::to_string(i)));
    }

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                auto snap = cm.endpointsSnapshot();
                const EndpointConfig* e = ConfigManager::findEndpoint(*snap, "/r/7", "GET");
                if (e && e->urlPath == "/r/7") {
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (int i = 0; i < 400; ++i) {
        cm.addEndpoint(makeEndpoint("/w/" + std::to_string(i)));
    }
    stop.store(true);
    for (auto& t : readers) { t.join(); }

    REQUIRE(reads.load() > 0);
    REQUIRE(cm.endpointsSnapshot()->size() == 16 + 400);
}
