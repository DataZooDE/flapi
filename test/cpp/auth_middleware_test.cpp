#include <catch2/catch_test_macros.hpp>
#include <crow/http_request.h>
#include <crow/http_response.h>
#include <crow/utility.h>
#include <jwt-cpp/jwt.h>

#include <map>
#include <optional>

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

// The Secrets-Manager user list used to live in a table of the main DuckDB,
// where every SQL template could read it, and was looked up with SQL (#254).
// It is parsed once, into memory; these pin the parsing and the AWS settings.
TEST_CASE("AwsHelper parses the Secrets-Manager user list", "[auth][aws]") {
    SECTION("users with and without roles") {
        auto users = AwsHelper::parseUserList(
            R"({"auth":[{"username":"alice","password":"pw","roles":["reader","writer"]},)"
            R"({"username":"o'brien","password":"it's"}]})");
        REQUIRE(users.size() == 2);
        REQUIRE(users[0].username == "alice");
        REQUIRE(users[0].password == "pw");
        REQUIRE(users[0].roles == std::vector<std::string>{"reader", "writer"});
        // A quote is data: it once broke the SQL the lookup was made with.
        REQUIRE(users[1].username == "o'brien");
        REQUIRE(users[1].password == "it's");
        REQUIRE(users[1].roles.empty());
    }

    SECTION("a document of the wrong shape is refused without quoting it") {
        const char* bad[] = {
            "not json SECRETPW",
            R"({"users":[{"username":"a","password":"SECRETPW"}]})",
            R"({"auth":{"username":"a","password":"SECRETPW"}})",
            R"({"auth":[{"username":"a"}]})",
            R"({"auth":[{"username":"","password":"SECRETPW"}]})",
            R"({"auth":[{"username":"a","password":"SECRETPW","roles":"admin"}]})",
            R"({"auth":[{"username":"a","password":"SECRETPW","roles":[1]}]})",
            R"({"auth":["SECRETPW"]})",
        };
        for (const auto* doc : bad) {
            INFO(doc);
            try {
                AwsHelper::parseUserList(doc);
                FAIL("accepted");
            } catch (const std::runtime_error& e) {
                REQUIRE(std::string(e.what()).find("SECRETPW") == std::string::npos);
            }
        }
    }
}

namespace {
AwsHelper::EnvLookup FakeEnv(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](const std::string& name) -> std::optional<std::string> {
        const auto it = vars.find(name);
        if (it == vars.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}
}  // namespace

TEST_CASE("AwsHelper resolves the Secrets-Manager endpoint override", "[auth][aws]") {
    REQUIRE(AwsHelper::resolveEndpointOverride(FakeEnv({})).empty());
    // AWS_ENDPOINT_URL is usually MinIO for S3: it must not decide who logs in.
    REQUIRE(AwsHelper::resolveEndpointOverride(FakeEnv({{"AWS_ENDPOINT_URL", "http://global"}})).empty());
    REQUIRE(AwsHelper::resolveEndpointOverride(FakeEnv({{"AWS_ENDPOINT_URL", "http://global"},
                                                        {"AWS_ENDPOINT_URL_SECRETS_MANAGER", "http://sm"}})) ==
            "http://sm");
    REQUIRE(AwsHelper::resolveEndpointOverride(FakeEnv({{"AWS_ENDPOINT_URL", "http://global"},
                                                        {"AWS_ENDPOINT_URL_SECRETS_MANAGER", ""}}))
                .empty());
    for (const char* yes : {"true", "TRUE", "1"}) {
        REQUIRE(AwsHelper::resolveEndpointOverride(FakeEnv({{"AWS_ENDPOINT_URL_SECRETS_MANAGER", "http://sm"},
                                                            {"AWS_IGNORE_CONFIGURED_ENDPOINT_URLS", yes}}))
                    .empty());
    }
    REQUIRE(AwsHelper::resolveEndpointOverride(FakeEnv({{"AWS_ENDPOINT_URL_SECRETS_MANAGER", "http://sm"},
                                                        {"AWS_IGNORE_CONFIGURED_ENDPOINT_URLS", "false"}})) ==
            "http://sm");
}

TEST_CASE("AwsHelper resolves the Secrets-Manager region", "[auth][aws]") {
    const auto env = FakeEnv({{"AWS_REGION", "eu-west-1"}, {"AWS_DEFAULT_REGION", "us-east-2"}});
    REQUIRE(AwsHelper::resolveRegion("eu-central-1", env) == "eu-central-1");
    REQUIRE(AwsHelper::resolveRegion("", env) == "eu-west-1");
    REQUIRE(AwsHelper::resolveRegion("", FakeEnv({{"AWS_DEFAULT_REGION", "us-east-2"}})) == "us-east-2");
    try {
        AwsHelper::resolveRegion("", FakeEnv({}));
        FAIL("no region, no error");
    } catch (const std::runtime_error& e) {
        REQUIRE(std::string(e.what()).find("auth.from-aws-secretmanager.region") != std::string::npos);
    }
}

TEST_CASE("AuthMiddleware refuses a Secrets-Manager user it never loaded", "[auth][aws]") {
    TempTestConfig temp("test_auth_aws");
    auto config_manager = temp.createConfigManager();
    auto auth_middleware = std::make_unique<AuthMiddleware>();
    auth_middleware->initialize(config_manager);

    // Added after initialize(): its secret was never fetched. Fails closed.
    EndpointConfig endpoint;
    endpoint.urlPath = "/test";
    endpoint.auth.enabled = true;
    endpoint.auth.type = "basic";
    AuthFromSecretManagerConfig aws_config;
    aws_config.secret_name = "prod/flapi/test";
    endpoint.auth.from_aws_secretmanager = aws_config;
    config_manager->addEndpoint(endpoint);

    for (const char* username : {"admin", "' OR '1'='1", "x' UNION SELECT 'admin', 'pw', ['admin'] --"}) {
        crow::request req;
        crow::response res;
        AuthMiddleware::context ctx;
        req.url = "/test";
        req.add_header("Authorization", AuthTestHelper::createBasicAuthHeader(username, "pw"));
        auth_middleware->before_handle(req, res, ctx);
        REQUIRE_FALSE(ctx.authenticated);
        REQUIRE(res.code == 401);
    }
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