#include <catch2/catch_test_macros.hpp>
#include <crow/http_request.h>
#include <crow/http_response.h>
#include <crow/utility.h>
#include <jwt-cpp/jwt.h>

#include "auth_middleware.hpp"
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "test_utils.hpp"

namespace flapi {
namespace test {

// Auth-specific test helpers (not duplicated elsewhere)
class AuthTestHelper {
public:
    static std::string createBasicAuthHeader(const std::string& username, const std::string& password) {
        std::string auth_string = username + ":" + password;
        return "Basic " + crow::utility::base64encode(auth_string, auth_string.size());
    }

    static std::string createBearerAuthHeader(const std::string& token) {
        return "Bearer " + token;
    }

    static EndpointConfig createEndpointWithInlineUsers() {
        EndpointConfig endpoint;
        endpoint.urlPath = "/test";
        endpoint.auth.enabled = true;
        endpoint.auth.type = "basic";

        AuthUser user1;
        user1.username = "test_user";
        user1.password = "test_password";
        user1.roles = {"user", "admin"};
        endpoint.auth.users.push_back(user1);

        AuthUser user2;
        user2.username = "md5_user";
        // MD5 hash of "md5_password"
        user2.password = "68675fbd5f8a9f03341659489a70944f";
        user2.roles = {"user"};
        endpoint.auth.users.push_back(user2);

        return endpoint;
    }

    static EndpointConfig createEndpointWithAwsSecrets() {
        EndpointConfig endpoint;
        endpoint.urlPath = "/test";
        endpoint.auth.enabled = true;
        endpoint.auth.type = "basic";

        AuthFromSecretManagerConfig aws_config;
        aws_config.secret_name = "prod/flapi/test";
        aws_config.secret_table = ConfigManager::secretNameToTableName(aws_config.secret_name);
        aws_config.init = ConfigManager::createDefaultAuthInit(aws_config.secret_name, "", "", "");

        endpoint.auth.from_aws_secretmanager = aws_config;
        return endpoint;
    }
};

TEST_CASE("AuthMiddleware basic functionality", "[auth]") {
    TempTestConfig temp("test_auth");
    auto config_manager = temp.createConfigManager();

    crow::logger::setLogLevel(crow::LogLevel::Debug);
    auto auth_middleware = std::make_unique<AuthMiddleware>();
    auth_middleware->initialize(config_manager);

    SECTION("Request without authentication header") {
        auto endpoint = AuthTestHelper::createEndpointWithInlineUsers();
        config_manager->addEndpoint(endpoint);

        crow::request req;
        crow::response res;
        AuthMiddleware::context ctx;
        req.url = "/test";

        auth_middleware->before_handle(req, res, ctx);

        REQUIRE_FALSE(ctx.authenticated);
        REQUIRE(ctx.username.empty());
        REQUIRE(ctx.roles.empty());
        REQUIRE(res.code == 401);
        REQUIRE(res.get_header_value("WWW-Authenticate") == "Basic realm=\"flAPI\"");
    }

    SECTION("Valid basic authentication") {
        auto endpoint = AuthTestHelper::createEndpointWithInlineUsers();
        config_manager->addEndpoint(endpoint);

        crow::request req;
        crow::response res;
        AuthMiddleware::context ctx;
        req.url = "/test";
        req.add_header("Authorization", AuthTestHelper::createBasicAuthHeader("test_user", "test_password"));

        auth_middleware->before_handle(req, res, ctx);

        REQUIRE(ctx.authenticated);
        REQUIRE(ctx.username == "test_user");
        REQUIRE(ctx.roles.size() == 2);
        REQUIRE(ctx.roles[0] == "user");
        REQUIRE(ctx.roles[1] == "admin");
    }

    SECTION("MD5 hashed password authentication") {
        auto endpoint = AuthTestHelper::createEndpointWithInlineUsers();
        config_manager->addEndpoint(endpoint);

        auto auth_header = AuthTestHelper::createBasicAuthHeader("md5_user", "md5_password");

        crow::request req;
        crow::response res;
        AuthMiddleware::context ctx;
        req.url = "/test";
        req.add_header("Authorization", auth_header);

        auth_middleware->before_handle(req, res, ctx);

        REQUIRE(ctx.authenticated);
        REQUIRE(ctx.username == "md5_user");
        REQUIRE(ctx.roles.size() == 1);
        REQUIRE(ctx.roles[0] == "user");
    }

    SECTION("Invalid password") {
        auto endpoint = AuthTestHelper::createEndpointWithInlineUsers();
        config_manager->addEndpoint(endpoint);

        crow::request req;
        crow::response res;
        AuthMiddleware::context ctx;
        req.url = "/test";
        req.add_header("Authorization", AuthTestHelper::createBasicAuthHeader("test_user", "wrong_password"));

        auth_middleware->before_handle(req, res, ctx);

        REQUIRE_FALSE(ctx.authenticated);
        REQUIRE(res.code == 401);
    }
    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("AuthMiddleware AWS Secrets Manager authentication", "[auth]") {
    SKIP();

    TempTestConfig temp("test_auth_aws");
    auto config_manager = temp.createConfigManager();

    crow::logger::setLogLevel(crow::LogLevel::Debug);
    auto db_manager = DatabaseManager::getInstance();
    db_manager->initializeDBManagerFromConfig(config_manager);

    SECTION("Valid AWS Secrets Manager credentials") {
        auto endpoint = AuthTestHelper::createEndpointWithAwsSecrets();
        config_manager->addEndpoint(endpoint);

        auto auth_middleware = std::make_unique<AuthMiddleware>();
        auth_middleware->initialize(config_manager);

        crow::request req;
        crow::response res;
        AuthMiddleware::context ctx;
        req.url = "/test";
        req.add_header("Authorization", AuthTestHelper::createBasicAuthHeader("admin", "admin_secret"));

        auth_middleware->before_handle(req, res, ctx);

        REQUIRE(ctx.authenticated);
        REQUIRE(ctx.username == "admin");
        REQUIRE(ctx.roles.size() == 1);
        REQUIRE(ctx.roles[0] == "admin");
    }

    SECTION("AWS Secrets Manager with MD5 password") {
        auto endpoint = AuthTestHelper::createEndpointWithAwsSecrets();
        config_manager->addEndpoint(endpoint);

        auto auth_middleware = std::make_unique<AuthMiddleware>();
        auth_middleware->initialize(config_manager);

        crow::request req;
        crow::response res;
        AuthMiddleware::context ctx;

        req.url = "/test";
        req.add_header("Authorization", AuthTestHelper::createBasicAuthHeader("md5_user", "md5_password"));

        auth_middleware->before_handle(req, res, ctx);

        REQUIRE(ctx.authenticated);
        REQUIRE(ctx.username == "md5_user");
        REQUIRE(ctx.roles.size() == 1);
        REQUIRE(ctx.roles[0] == "developer");
    }
    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("AuthMiddleware JWT bearer authentication", "[auth]") {
    TempTestConfig temp("test_auth_jwt");
    auto config_manager = temp.createConfigManager();

    crow::logger::setLogLevel(crow::LogLevel::Debug);
    auto auth_middleware = std::make_unique<AuthMiddleware>();
    auth_middleware->initialize(config_manager);

    SECTION("Valid JWT token") {
        crow::request req;
        crow::response res;
        AuthMiddleware::context ctx;

        auto endpoint = AuthTestHelper::createEndpointWithInlineUsers();
        endpoint.auth.type = "bearer";
        endpoint.auth.jwt_secret = "your-256-bit-secret";
        endpoint.auth.jwt_issuer = "test-issuer";
        config_manager->addEndpoint(endpoint);

        auto token = jwt::create()
            .set_issuer("test-issuer")
            .set_payload_claim("sub", jwt::claim(std::string("test_user")))
            .set_payload_claim("roles", jwt::claim(std::set<std::string>{"user", "admin"}))
            .sign(jwt::algorithm::hs256{"your-256-bit-secret"});

        req.url = "/test";
        req.add_header("Authorization", AuthTestHelper::createBearerAuthHeader(token));

        auth_middleware->before_handle(req, res, ctx);

        REQUIRE(ctx.authenticated);
        REQUIRE(ctx.username == "test_user");
        REQUIRE(ctx.roles.size() == 2);
    }
    // TempTestConfig automatically cleans up on destruction
}

TEST_CASE("AuthMiddleware md5 hash", "[auth]") {
    REQUIRE(AuthMiddleware::md5Hash("test_password") == "16ec1ebb01fe02ded9b7d5447d3dfc65");
    REQUIRE(AuthMiddleware::md5Hash("md5_password") == "68675fbd5f8a9f03341659489a70944f");
    REQUIRE(AuthMiddleware::md5Hash("foo123$Xyz") == "e709075713658cae1cdbcf7965ab8528");
}

} // namespace test
} // namespace flapi