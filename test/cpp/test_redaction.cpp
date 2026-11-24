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

TEST_CASE("short credential names are matched whole, not as substrings",
          "[redaction][security]") {
    // `pass` and `sas` are real credential key names - `pass`, `user_pass`,
    // `db_pass`, and the Azure shared-access signature - and none of them
    // contains "password". Added as SUBSTRING stems they immediately
    // swallowed `passenger_count`, which the ordinary-field-names test above
    // caught. So they are matched on the whole normalised key.
    for (const char* key : {"pass", "user_pass", "db_pass",
                            "sas", "sas_token", "sas_key",
                            "pw", "key",
                            "service_account", "service_account_json"}) {
        INFO("key = " << key);
        REQUIRE(isCredentialKey(key));
    }

    for (const char* key : {"passenger_count", "passengers", "compass",
                            "sassafras", "passage",
                            // `pw` and `key` are whole-key, so these stay
                            // visible - `sort_key` and `primary_key` are
                            // ordinary data-API field names.
                            "pw_bucket", "keyword", "sort_key", "primary_key",
                            "monkey"}) {
        INFO("key = " << key);
        REQUIRE_FALSE(isCredentialKey(key));
    }
}

TEST_CASE("key normalisation strips separators and case", "[redaction]") {
    REQUIRE(normaliseKey("X-Api-Key") == "xapikey");
    REQUIRE(normaliseKey("client_secret") == "clientsecret");
    REQUIRE(normaliseKey("") == "");
}

// An init script carries credentials, and DuckDB's error about it quotes the
// statement: `LINE 1: ...SECRET 'abc'def'...` and `at or near "def"` (#254).
TEST_CASE("SQL error messages lose the statement text", "[redaction][security]") {
    const std::string parse_error =
        "Parser Error: syntax error at or near \"defKEY\"\n\n"
        "LINE 1: ...KEY_ID 'AKID', SECRET 'abc'defKEY', REGION 'eu'...\n"
        "                                       ^";
    const auto redacted = redactSqlErrorMessage(parse_error);
    REQUIRE(redacted == "Parser Error: syntax error at or near \"<redacted>\"");

    const auto literal = redactSqlErrorMessage("Invalid Input Error: bad value 'hunter2' for SECRET");
    REQUIRE(literal == "Invalid Input Error: bad value '<redacted>' for SECRET");

    // Identifiers are kept: they name what went wrong.
    const std::string missing = "IO Error: Extension \"/x/postgres.duckdb_extension\" not found";
    REQUIRE(redactSqlErrorMessage(missing) == missing);

    // An unterminated literal is masked to the end.
    REQUIRE(redactSqlErrorMessage("oops 'abc") == "oops '<redacted>");
}
