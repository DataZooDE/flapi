#include <catch2/catch_all.hpp>

#include "redaction.hpp"

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
        "tokenizer_version",  // deliberately arguable; see note below
    };
    for (const auto* key : ordinary) {
        INFO("key = " << key);
        if (std::string(key) == "tokenizer_version") {
            // Contains the "token" stem. We accept this false positive: the
            // alternative is missing `auth_token`, and a redacted version string
            // is a far cheaper mistake than a leaked bearer token.
            REQUIRE(isCredentialKey(key));
            continue;
        }
        REQUIRE_FALSE(isCredentialKey(key));
    }
}

TEST_CASE("key normalisation strips separators and case", "[redaction]") {
    REQUIRE(normaliseKey("X-Api-Key") == "xapikey");
    REQUIRE(normaliseKey("client_secret") == "clientsecret");
    REQUIRE(normaliseKey("") == "");
}
