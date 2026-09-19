#include <catch2/catch_all.hpp>

#include "api_server.hpp"

using flapi::APIServer;

// Crow runs a handler on the io thread that owns its connection, so a slow
// synchronous query blocks every other connection on that thread - including
// GET /health, which is how a stalled instance is supposed to report itself
// (#120).
//
// Measured, one ~5s query in flight, 40 CONCURRENT health probes:
//
//   concurrency = 2 (1 io thread):  40/40 blocked,  0/40 reported the stall
//   concurrency = 8 (7 io threads):  5/40 blocked, 35/40 reported the stall
//
// Consistent with roughly 1-in-io-threads. The first row is the one that
// matters: on a 1-vCPU container - Cloud Run's default - readiness does not
// merely miss the stall occasionally, it NEVER reports it. Every probe waits
// out the query and then answers 200 ready.
//
// Note these must be measured with concurrent probes. Sequential ones cannot
// see it: the first probe to block waits out the query, and every later one
// runs after it has finished, so at most one can ever block regardless of
// thread count.

TEST_CASE("a small machine still gets enough io threads", "[api_server][threads]") {
    // The case the floor exists for. hardware_concurrency() reports 1 or 2 on
    // a small container, which without a floor leaves a single io thread.
    REQUIRE(APIServer::serverThreadCount(1) == 8);
    REQUIRE(APIServer::serverThreadCount(2) == 8);
    REQUIRE(APIServer::serverThreadCount(4) == 8);

    // 0 is what hardware_concurrency() returns when it cannot tell, which is
    // permitted by the standard and must not collapse to nothing.
    REQUIRE(APIServer::serverThreadCount(0) == 8);
}

TEST_CASE("a large machine keeps its own thread count", "[api_server][threads]") {
    // The floor raises, never lowers: a big host should still use what it has.
    REQUIRE(APIServer::serverThreadCount(16) == 16);
    REQUIRE(APIServer::serverThreadCount(32) == 32);
}

TEST_CASE("an implausible core count is capped", "[api_server][threads]") {
    // Crow takes a uint16_t. A machine reporting more cores than that - or a
    // container runtime reporting nonsense - must not wrap around to a tiny
    // number, which would be the exact failure the floor exists to prevent.
    REQUIRE(APIServer::serverThreadCount(256) == 64);
    REQUIRE(APIServer::serverThreadCount(100000) == 64);
}
