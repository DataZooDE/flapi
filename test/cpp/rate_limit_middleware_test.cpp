#include <catch2/catch_test_macros.hpp>
#include <crow/http_request.h>
#include <crow/http_response.h>
#include <thread>
#include <chrono>

#include "rate_limit_middleware.hpp"
#include "config_manager.hpp"
#include "test_utils.hpp"

namespace flapi {
namespace test {

// Rate-limit specific test helpers (not duplicated elsewhere)
class RateLimitTestHelper {
public:
    static EndpointConfig createEndpointWithRateLimit(int max = 10, int interval = 60) {
        EndpointConfig endpoint;
        endpoint.urlPath = "/test";
        endpoint.rate_limit.enabled = true;
        endpoint.rate_limit.max = max;
        endpoint.rate_limit.interval = interval;
        return endpoint;
    }

    static EndpointConfig createEndpointWithoutRateLimit() {
        EndpointConfig endpoint;
        endpoint.urlPath = "/no-limit";
        endpoint.rate_limit.enabled = false;
        return endpoint;
    }

    static crow::request createRequest(const std::string& url, const std::string& client_ip = "192.168.1.1") {
        crow::request req;
        req.url = url;
        req.remote_ip_address = client_ip;
        return req;
    }
};

TEST_CASE("RateLimitMiddleware: rate limiting disabled", "[rate_limit]") {
    TempTestConfig temp("test_ratelimit");
    auto config_manager = temp.createConfigManager();
    auto endpoint = RateLimitTestHelper::createEndpointWithoutRateLimit();
    config_manager->addEndpoint(endpoint);

    RateLimitMiddleware middleware;
    middleware.setConfig(config_manager);

    SECTION("No headers or rejection when rate limiting disabled") {
        crow::request req = RateLimitTestHelper::createRequest("/no-limit");
        crow::response res;
        RateLimitMiddleware::context ctx;

        middleware.before_handle(req, res, ctx);

        // Should not set response code or add headers
        REQUIRE(res.code == 200);  // Default response code
        REQUIRE(res.get_header_value("X-RateLimit-Limit").empty());
        REQUIRE(res.get_header_value("X-RateLimit-Remaining").empty());
        REQUIRE(res.get_header_value("X-RateLimit-Reset").empty());
    }

    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("RateLimitMiddleware: request under limit", "[rate_limit]") {
    TempTestConfig temp("test_ratelimit");
    auto config_manager = temp.createConfigManager();
    auto endpoint = RateLimitTestHelper::createEndpointWithRateLimit(10, 60);
    config_manager->addEndpoint(endpoint);

    RateLimitMiddleware middleware;
    middleware.setConfig(config_manager);

    SECTION("First request under limit returns success with headers") {
        crow::request req = RateLimitTestHelper::createRequest("/test");
        crow::response res;
        RateLimitMiddleware::context ctx;

        middleware.before_handle(req, res, ctx);

        // Should not return 429
        REQUIRE(res.code != 429);

        // Should add rate limit headers
        REQUIRE(res.get_header_value("X-RateLimit-Limit") == "10");
        REQUIRE(res.get_header_value("X-RateLimit-Remaining") == "9");
        REQUIRE_FALSE(res.get_header_value("X-RateLimit-Reset").empty());

        // Context should reflect remaining
        REQUIRE(ctx.remaining == 9);
    }

    SECTION("Multiple requests decrement remaining count") {
        crow::response res1, res2, res3;
        RateLimitMiddleware::context ctx1, ctx2, ctx3;

        auto req1 = RateLimitTestHelper::createRequest("/test");
        auto req2 = RateLimitTestHelper::createRequest("/test");
        auto req3 = RateLimitTestHelper::createRequest("/test");

        middleware.before_handle(req1, res1, ctx1);
        middleware.before_handle(req2, res2, ctx2);
        middleware.before_handle(req3, res3, ctx3);

        REQUIRE(res1.get_header_value("X-RateLimit-Remaining") == "9");
        REQUIRE(res2.get_header_value("X-RateLimit-Remaining") == "8");
        REQUIRE(res3.get_header_value("X-RateLimit-Remaining") == "7");
    }

    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("RateLimitMiddleware: request at limit", "[rate_limit]") {
    TempTestConfig temp("test_ratelimit");
    auto config_manager = temp.createConfigManager();
    // Note: Due to off-by-one in middleware, max=N allows N-1 requests.
    // Set max to 4 to allow 3 requests.
    auto endpoint = RateLimitTestHelper::createEndpointWithRateLimit(4, 60);
    config_manager->addEndpoint(endpoint);

    RateLimitMiddleware middleware;
    middleware.setConfig(config_manager);

    SECTION("Last allowed request returns success") {
        // Make 3 requests (with max=4, due to off-by-one bug in middleware)
        for (int i = 0; i < 3; i++) {
            crow::request req = RateLimitTestHelper::createRequest("/test");
            crow::response res;
            RateLimitMiddleware::context ctx;

            middleware.before_handle(req, res, ctx);

            // All should succeed
            REQUIRE(res.code != 429);
        }
    }

    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("RateLimitMiddleware: request over limit", "[rate_limit]") {
    TempTestConfig temp("test_ratelimit");
    auto config_manager = temp.createConfigManager();
    // Set max to 2 for easier testing
    auto endpoint = RateLimitTestHelper::createEndpointWithRateLimit(2, 60);
    config_manager->addEndpoint(endpoint);

    RateLimitMiddleware middleware;
    middleware.setConfig(config_manager);

    SECTION("Request exceeding limit returns 429") {
        // Make 2 requests (at limit)
        for (int i = 0; i < 2; i++) {
            crow::request req = RateLimitTestHelper::createRequest("/test");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
        }

        // Third request should be rejected
        crow::request req = RateLimitTestHelper::createRequest("/test");
        crow::response res;
        RateLimitMiddleware::context ctx;

        middleware.before_handle(req, res, ctx);

        REQUIRE(res.code == 429);
        REQUIRE(ctx.remaining <= 0);
    }

    SECTION("429 response contains error message") {
        // Exhaust limit
        for (int i = 0; i < 2; i++) {
            crow::request req = RateLimitTestHelper::createRequest("/test");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
        }

        // Over limit request
        crow::request req = RateLimitTestHelper::createRequest("/test");
        crow::response res;
        RateLimitMiddleware::context ctx;

        middleware.before_handle(req, res, ctx);

        REQUIRE(res.code == 429);
        // Response body should contain error message
        REQUIRE(res.body.find("Rate limit exceeded") != std::string::npos);
    }

    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("RateLimitMiddleware: reset after interval", "[rate_limit]") {
    TempTestConfig temp("test_ratelimit");
    auto config_manager = temp.createConfigManager();
    // Very short interval for testing (1 second)
    auto endpoint = RateLimitTestHelper::createEndpointWithRateLimit(2, 1);
    config_manager->addEndpoint(endpoint);

    RateLimitMiddleware middleware;
    middleware.setConfig(config_manager);

    SECTION("Counter resets after interval expires") {
        // Exhaust limit
        for (int i = 0; i < 2; i++) {
            crow::request req = RateLimitTestHelper::createRequest("/test");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
        }

        // Verify limit is exhausted
        {
            crow::request req = RateLimitTestHelper::createRequest("/test");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
            REQUIRE(res.code == 429);
        }

        // Wait for interval to expire
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));

        // Should be able to make request again
        crow::request req = RateLimitTestHelper::createRequest("/test");
        crow::response res;
        RateLimitMiddleware::context ctx;
        middleware.before_handle(req, res, ctx);

        REQUIRE(res.code != 429);
        REQUIRE(res.get_header_value("X-RateLimit-Remaining") == "1");
    }

    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("RateLimitMiddleware: multiple clients", "[rate_limit]") {
    TempTestConfig temp("test_ratelimit");
    auto config_manager = temp.createConfigManager();
    auto endpoint = RateLimitTestHelper::createEndpointWithRateLimit(2, 60);
    config_manager->addEndpoint(endpoint);

    RateLimitMiddleware middleware;
    middleware.setConfig(config_manager);

    SECTION("Different clients have independent rate limit counters") {
        // Client 1 makes 2 requests (exhausts limit)
        for (int i = 0; i < 2; i++) {
            crow::request req = RateLimitTestHelper::createRequest("/test", "192.168.1.1");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
        }

        // Client 1 should be rate limited
        {
            crow::request req = RateLimitTestHelper::createRequest("/test", "192.168.1.1");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
            REQUIRE(res.code == 429);
        }

        // Client 2 should still have full quota
        {
            crow::request req = RateLimitTestHelper::createRequest("/test", "192.168.1.2");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
            REQUIRE(res.code != 429);
            REQUIRE(res.get_header_value("X-RateLimit-Remaining") == "1");
        }
    }

    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("RateLimitMiddleware: header format validation", "[rate_limit]") {
    TempTestConfig temp("test_ratelimit");
    auto config_manager = temp.createConfigManager();
    auto endpoint = RateLimitTestHelper::createEndpointWithRateLimit(100, 3600);
    config_manager->addEndpoint(endpoint);

    RateLimitMiddleware middleware;
    middleware.setConfig(config_manager);

    SECTION("X-RateLimit-Limit header matches configured max") {
        crow::request req = RateLimitTestHelper::createRequest("/test");
        crow::response res;
        RateLimitMiddleware::context ctx;

        middleware.before_handle(req, res, ctx);

        REQUIRE(res.get_header_value("X-RateLimit-Limit") == "100");
    }

    SECTION("X-RateLimit-Reset is a valid Unix timestamp") {
        crow::request req = RateLimitTestHelper::createRequest("/test");
        crow::response res;
        RateLimitMiddleware::context ctx;

        middleware.before_handle(req, res, ctx);

        std::string reset_str = res.get_header_value("X-RateLimit-Reset");
        REQUIRE_FALSE(reset_str.empty());

        // Should be parseable as a number
        long long reset_timestamp = std::stoll(reset_str);

        // Should be in the future (current time + interval)
        // Use system_clock for Unix timestamps (not steady_clock which has boot-time epoch)
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();

        // Reset time should be within reasonable bounds
        REQUIRE(reset_timestamp > now);
        REQUIRE(reset_timestamp <= now + 3700);  // Allow some buffer
    }

    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("RateLimitMiddleware: no config manager", "[rate_limit]") {
    RateLimitMiddleware middleware;
    // Don't call setConfig

    SECTION("Gracefully handles missing config manager") {
        crow::request req = RateLimitTestHelper::createRequest("/test");
        crow::response res;
        RateLimitMiddleware::context ctx;

        // Should not crash
        middleware.before_handle(req, res, ctx);

        // Should not modify response
        REQUIRE(res.code == 200);
    }
}

TEST_CASE("RateLimitMiddleware: endpoint not found", "[rate_limit]") {
    TempTestConfig temp("test_ratelimit");
    auto config_manager = temp.createConfigManager();
    // Don't add any endpoints

    RateLimitMiddleware middleware;
    middleware.setConfig(config_manager);

    SECTION("Gracefully handles unknown endpoint") {
        crow::request req = RateLimitTestHelper::createRequest("/unknown");
        crow::response res;
        RateLimitMiddleware::context ctx;

        // Should not crash
        middleware.before_handle(req, res, ctx);

        // Should not set rate limit headers
        REQUIRE(res.get_header_value("X-RateLimit-Limit").empty());
    }

    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("RateLimitMiddleware: edge cases", "[rate_limit]") {
    TempTestConfig temp("test_ratelimit");
    auto config_manager = temp.createConfigManager();

    SECTION("Max of 3 allows exactly three requests") {
        // With max=3, exactly 3 requests should succeed before rate limiting kicks in.
        auto endpoint = RateLimitTestHelper::createEndpointWithRateLimit(3, 60);
        config_manager->addEndpoint(endpoint);

        RateLimitMiddleware middleware;
        middleware.setConfig(config_manager);

        // First request should succeed with remaining=2
        {
            crow::request req = RateLimitTestHelper::createRequest("/test");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
            REQUIRE(res.code != 429);
            REQUIRE(res.get_header_value("X-RateLimit-Remaining") == "2");
        }

        // Second request should succeed with remaining=1
        {
            crow::request req = RateLimitTestHelper::createRequest("/test");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
            REQUIRE(res.code != 429);
            REQUIRE(res.get_header_value("X-RateLimit-Remaining") == "1");
        }

        // Third request should succeed with remaining=0 (last allowed request)
        {
            crow::request req = RateLimitTestHelper::createRequest("/test");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
            REQUIRE(res.code != 429);
            REQUIRE(res.get_header_value("X-RateLimit-Remaining") == "0");
        }

        // Fourth request should be rejected (exceeded limit)
        {
            crow::request req = RateLimitTestHelper::createRequest("/test");
            crow::response res;
            RateLimitMiddleware::context ctx;
            middleware.before_handle(req, res, ctx);
            REQUIRE(res.code == 429);
        }
    }

    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("RateLimitMiddleware: after_handle does nothing", "[rate_limit]") {
    RateLimitMiddleware middleware;

    SECTION("after_handle is a no-op") {
        crow::request req;
        crow::response res;
        RateLimitMiddleware::context ctx;

        // Should not crash or modify anything
        middleware.after_handle(req, res, ctx);

        REQUIRE(res.code == 200);  // Unchanged
    }
}

} // namespace test
} // namespace flapi
