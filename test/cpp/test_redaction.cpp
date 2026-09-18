#include <catch2/catch_all.hpp>

#include "redaction.hpp"

#include <string>

using namespace flapi;

// The guarantee documented in docs/OBSERVABILITY.md §4 is that credential-shaped
// keys are redacted "regardless of your configuration". An exact-match list
// cannot deliver that: HTTP parameter names are not normalised, and the failure
// mode is a leaked credential in a persistent file.

TEST_CASE("credential keys are matched regardless of case and separators",
          "[redaction][security]") {
    // Each of these was reachable past the previous exact-match list.
    const char* leaky[] = {
        "token",      "Token",        "auth_token",   "id_token",
        "x-api-key",  "api-key",      "API_KEY",      "user_password",
        "authorization-token",        "client-secret", "pwd",
        "jwt",        "Bearer",       "access-key",   "signature",
        "connection-string",          "private-key",  "Set-Cookie",
        // Found missing by the documentation crew review.
        "passphrase", "passcode",     "session_id",   "jsessionid",
        "auth_key",   "dsn",          "database_url", "conn_str",
        "privkey",    "hmac",         "subscription_key",
        "x-functions-key",            "csrf_token",   "xsrf-token",
    };
    for (const auto* key : leaky) {
        INFO("key = " << key);
        REQUIRE(isCredentialKey(key));
    }
}

TEST_CASE("ordinary field names are not redacted", "[redaction][security]") {
    // Over-redaction is the safe direction for a denylist, but it still costs
    // real observability. A stem that swallows ordinary data-API field names
    // (author, design, session_count) is a bad stem, not a cautious one.
    const char* ordinary[] = {
        "author",   "authors",   "design",    "signal",   "customer_id",
        "email",    "country",   "price",     "passenger_count",
        "sort_key", "primary_key",           "session_count",
        // LLM token counters. This product IS an MCP/LLM tool surface, so these
        // field names are far more likely than a credential called "token
        // count", and redacting them would gut the payload tier for exactly the
        // workload it exists to observe.
        "max_tokens", "input_tokens", "output_tokens", "token_count",
        "tokens_used",
        "secretary",
    };
    for (const auto* key : ordinary) {
        INFO("key = " << key);
        REQUIRE_FALSE(isCredentialKey(key));
    }
}

TEST_CASE("key normalisation strips separators and case", "[redaction]") {
    REQUIRE(normaliseKey("X-Api-Key") == "xapikey");
    REQUIRE(normaliseKey("client_secret") == "clientsecret");
    REQUIRE(normaliseKey("") == "");
}
