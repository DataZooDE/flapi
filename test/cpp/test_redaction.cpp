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
    const std::string statement = "CREATE SECRET s (TYPE S3, KEY_ID 'AKID', SECRET 'abc'defKEY', REGION 'eu')";
    const std::string parse_error =
        "Parser Error: syntax error at or near \"defKEY\"\n\n"
        "LINE 1: ...KEY_ID 'AKID', SECRET 'abc'defKEY', REGION 'eu'...\n"
        "                                       ^";
    REQUIRE(redactSqlErrorMessage(parse_error, statement) == "Parser Error: syntax error at or near <redacted>");

    const auto literal = redactSqlErrorMessage("Invalid Input Error: bad value 'hunter2' for SECRET",
                                               "CREATE SECRET x (TYPE S3, SECRET 'hunter2')");
    REQUIRE(literal == "Invalid Input Error: bad value '<redacted>' for SECRET");

    // Identifiers the statement does not contain are kept: they name what went wrong.
    const std::string missing = "IO Error: Extension \"/x/postgres.duckdb_extension\" not found";
    REQUIRE(redactSqlErrorMessage(missing, "LOAD postgres") == missing);
}

TEST_CASE("SQL error redaction with the statement", "[redaction][security]") {
    SECTION("a PEG error double-quoting a literal's value") {
        const auto out = redactSqlErrorMessage(
            "Parser Error: syntax error at or near \"\"wJalrXUtnFEMIK7MDENG\"\"",
            "CREATE SECRET s (TYPE S3, SECRET 'wJalrXUtnFEMIK7MDENG')");
        REQUIRE(out.find("wJal") == std::string::npos);
    }
    SECTION("a double quote inside a literal") {
        const std::string statement = "CREATE SECRET s (TYPE S3, SECRET 'ab\"cdSECRET99')";
        const auto out = redactSqlErrorMessage(
            "Invalid Input Error: could not use \"ab\"cdSECRET99\" as a key", statement);
        REQUIRE(out.find("SECRET99") == std::string::npos);
    }
    SECTION("nested and odd apostrophes") {
        const std::string statement = "CREATE SECRET s (TYPE S3, SECRET 'it''s S3CRETVALUE')";
        const auto out = redactSqlErrorMessage(
            "Invalid Input Error: Failed to cast option 'secret' to VARCHAR: can't use 'it's S3CRETVALUE' here",
            statement);
        REQUIRE(out.find("S3CRETVALUE") == std::string::npos);
        REQUIRE(out.find("can't use") != std::string::npos);
    }
    SECTION("an extension echoing a DSN unquoted") {
        const std::string statement = "ATTACH 'host=db user=app password=s3cr3t dbname=x' AS pg (TYPE postgres)";
        const auto out = redactSqlErrorMessage(
            "IO Error: Unable to connect to Postgres at host=db user=app password=s3cr3t dbname=x", statement);
        REQUIRE(out.find("s3cr3t") == std::string::npos);
    }
    SECTION("a dollar-quoted body") {
        const auto out = redactSqlErrorMessage("Binder Error: bad DOLLARBODY1",
                                               "SELECT $tag$DOLLARBODY1$tag$");
        REQUIRE(out.find("DOLLARBODY1") == std::string::npos);
    }
}

TEST_CASE("an init statement is shown with its literals redacted", "[redaction][security]") {
    REQUIRE(redactSqlLiterals("SET VARIABLE x = 'INITCANARY'; ATTACH 'it''s' AS \"db\"") ==
            "SET VARIABLE x = '<redacted>'; ATTACH '<redacted>' AS \"db\"");
    REQUIRE(redactSqlLiterals("SELECT $$body$$ -- 'c'") == "SELECT $$<redacted>$$ -- 'c'");
    REQUIRE(redactSqlLiterals("") == "");
}

// Review of #254: four ways the message-only redaction leaked or mangled.
TEST_CASE("SQL error redaction: what the message alone gives away", "[redaction][security]") {
    SECTION("a PEG syntax error double-quotes the token") {
        const auto out = redactSqlErrorMessage(
            "Parser Error: syntax error at or near \"\"wJalrXUtnFEMIK7MDENG\"\"");
        REQUIRE(out.find("wJal") == std::string::npos);
    }
    SECTION("an apostrophe in prose is not a literal") {
        const std::string prose = "Catalog Error: Table with name foo doesn't exist! Did you mean \"bar\"?";
        REQUIRE(redactSqlErrorMessage(prose) == prose);
    }
    SECTION("a DSN echoed unquoted by an extension") {
        const auto out = redactSqlErrorMessage(
            "IO Error: Unable to connect to Postgres at host=db user=app password=s3cr3t dbname=x");
        REQUIRE(out.find("s3cr3t") == std::string::npos);
        REQUIRE(out.find("host=db") != std::string::npos);
    }
    SECTION("a URL with credentials") {
        const auto out = redactSqlErrorMessage(
            "HTTP Error: GET https://user:pa55word@example.com/x?X-Amz-Signature=abc123def failed");
        REQUIRE(out.find("pa55word") == std::string::npos);
        REQUIRE(out.find("abc123def") == std::string::npos);
    }
}
