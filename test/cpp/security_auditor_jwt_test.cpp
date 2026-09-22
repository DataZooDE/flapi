#include <catch2/catch_test_macros.hpp>

#include "config_manager.hpp"
#include "security_auditor.hpp"
#include "test_utils.hpp"

using namespace flapi;
using flapi::test::TempTestConfig;

// An empty HMAC key is not a weak secret, it is NO secret: a token signed with
// the empty key verifies, so any caller can mint one claiming any subject and
// any roles.
//
// The usual way to arrive there is `jwt-secret: '{{env.API_JWT_SECRET}}'` with
// the variable unset - it resolves to "" without complaint, so the config
// reads as protected and is not. Measured against the shipped binary before
// the fix: a token forged with an empty key and roles ["admin"] was accepted
// with 200 and returned the protected rows.
//
// The runtime refuses it (auth_middleware.cpp); these tests cover the startup
// warning, which is what tells an operator BEFORE a request arrives.
namespace {

EndpointConfig bearerEndpoint(const std::string& secret, const std::string& type = "bearer") {
    EndpointConfig e;
    e.urlPath = "/protected";
    e.method = "GET";
    e.templateSource = "x.sql";
    e.auth.enabled = true;
    e.auth.type = type;
    e.auth.jwt_secret = secret;
    return e;
}

bool hasCode(const std::vector<SecurityWarning>& warnings, const std::string& code) {
    for (const auto& w : warnings) {
        if (w.code == code) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("an empty jwt-secret is reported at startup", "[security][auditor][jwt]") {
    TempTestConfig temp("flapi_auditor_jwt");
    auto cm = temp.createConfigManager();
    cm->addEndpoint(bearerEndpoint(""));

    const auto warnings = SecurityAuditor{}.audit(*cm);
    REQUIRE(hasCode(warnings, "AUTH_EMPTY_JWT_SECRET"));
}

TEST_CASE("a configured jwt-secret is not reported", "[security][auditor][jwt]") {
    // The warning must be specific, or it becomes noise an operator learns to
    // scroll past - which is how the real one would be missed.
    TempTestConfig temp("flapi_auditor_jwt");
    auto cm = temp.createConfigManager();
    cm->addEndpoint(bearerEndpoint("a-real-secret"));

    const auto warnings = SecurityAuditor{}.audit(*cm);
    REQUIRE_FALSE(hasCode(warnings, "AUTH_EMPTY_JWT_SECRET"));
}

TEST_CASE("an unknown auth type is reported", "[security][auditor][jwt]") {
    // AuthMiddleware dispatches on basic / bearer / oidc. Anything else
    // matches no branch and the endpoint refuses every request - it fails
    // closed, but silently. `type: jwt` is the case that matters: it is what
    // the shipped customer example uses.
    TempTestConfig temp("flapi_auditor_jwt");
    auto cm = temp.createConfigManager();
    cm->addEndpoint(bearerEndpoint("a-real-secret", "jwt"));

    const auto warnings = SecurityAuditor{}.audit(*cm);
    REQUIRE(hasCode(warnings, "AUTH_UNKNOWN_TYPE"));
}

TEST_CASE("the recognised auth types are not reported", "[security][auditor][jwt]") {
    TempTestConfig temp("flapi_auditor_jwt");
    auto cm = temp.createConfigManager();
    cm->addEndpoint(bearerEndpoint("s", "basic"));
    cm->addEndpoint(bearerEndpoint("s", "oidc"));
    cm->addEndpoint(bearerEndpoint("s", "bearer"));

    const auto warnings = SecurityAuditor{}.audit(*cm);
    REQUIRE_FALSE(hasCode(warnings, "AUTH_UNKNOWN_TYPE"));
}
