#include <catch2/catch_test_macros.hpp>

#include "template_secrets.hpp"

using namespace flapi;

// TemplateSecrets sits on three hot paths - the REST read and write catch
// blocks, MCP tools/call and MCP resources/read - and decides whether a
// caller sees a diagnostic at all. It shipped with no unit tests; these pin
// the boundaries that otherwise live only in comments.

TEST_CASE("a credential-named value is scrubbed", "[secrets]") {
    TemplateSecrets secrets;
    secrets.add("password", "hunter2-and-then-some");
    REQUIRE_FALSE(secrets.withhold());
    REQUIRE(secrets.scrub("SELECT 'hunter2-and-then-some' AS pw") ==
            "SELECT '<redacted>' AS pw");
}

TEST_CASE("a non-credential name is left alone", "[secrets]") {
    // Over-broad scrubbing makes a preview useless, which is its own failure.
    TemplateSecrets secrets;
    secrets.add("path", "/data/public/file.parquet");
    REQUIRE(secrets.values().empty());
    REQUIRE(secrets.scrub("read_parquet('/data/public/file.parquet')") ==
            "read_parquet('/data/public/file.parquet')");
}

TEST_CASE("a server value too short to scrub withholds instead", "[secrets]") {
    // Measured: a one-byte secret rewrote the middle of an unrelated file
    // path. Neither leaking nor mangling is acceptable.
    TemplateSecrets secrets;
    secrets.add("password", "abc");
    REQUIRE(secrets.withhold());
    REQUIRE(secrets.values().empty());
}

TEST_CASE("the withhold boundary is four characters", "[secrets]") {
    TemplateSecrets three;
    three.add("api_key", "abc");
    REQUIRE(three.withhold());

    TemplateSecrets four;
    four.add("api_key", "abcd");
    REQUIRE_FALSE(four.withhold());
    REQUIRE(four.values().size() == 1);
}

TEST_CASE("a CALLER-supplied short value never withholds", "[secrets]") {
    // Otherwise `?token=ab` is a denial-of-diagnostics switch: it blanked
    // every error the endpoint could produce, validation errors included, on
    // every surface. A value the caller sent is not a secret being kept from
    // them.
    TemplateSecrets secrets;
    secrets.addCallerSupplied("token", "ab");
    REQUIRE_FALSE(secrets.withhold());
    REQUIRE(secrets.values().empty());
}

TEST_CASE("a caller-supplied long credential is still scrubbed", "[secrets]") {
    // It reaches the rendered SQL, so it must not be echoed back in one.
    TemplateSecrets secrets;
    secrets.addCallerSupplied("token", "a-long-caller-token");
    REQUIRE_FALSE(secrets.withhold());
    REQUIRE(secrets.scrub("WHERE t = 'a-long-caller-token'") ==
            "WHERE t = '<redacted>'");
}

TEST_CASE("a long environment value is scrubbed whatever it is called",
          "[secrets]") {
    // The name heuristic cannot recognise PAYMENT_VALUE or
    // SERVICE_ACCOUNT_JSON, and a whitelisted env var is server-configured -
    // an operator chose to expose it to templates.
    TemplateSecrets secrets;
    secrets.addEnv("PAYMENT_VALUE", "sk-live-0123456789abcdefghij");
    REQUIRE(secrets.scrub("x 'sk-live-0123456789abcdefghij'") == "x '<redacted>'");
}

TEST_CASE("a short neutral environment value stays visible", "[secrets]") {
    // Ordinary configuration - a region, a bucket, a tuning knob - must not
    // be redacted out of a preview.
    TemplateSecrets secrets;
    secrets.addEnv("AWS_REGION", "eu-central-1");
    REQUIRE(secrets.values().empty());
    REQUIRE(secrets.scrub("region = 'eu-central-1'") == "region = 'eu-central-1'");
}

TEST_CASE("the opaque-env boundary is twenty-four characters", "[secrets]") {
    const std::string twenty_three(23, 'x');
    const std::string twenty_four(24, 'y');

    TemplateSecrets below;
    below.addEnv("NEUTRAL_NAME", twenty_three);
    REQUIRE(below.values().empty());

    TemplateSecrets at;
    at.addEnv("NEUTRAL_NAME", twenty_four);
    REQUIRE(at.values().size() == 1);
}

TEST_CASE("the same value is recorded once", "[secrets]") {
    TemplateSecrets secrets;
    secrets.add("password", "shared-value-here");
    secrets.addEnv("API_KEY", "shared-value-here");
    REQUIRE(secrets.values().size() == 1);
}

TEST_CASE("scrubbing replaces every occurrence and does not loop", "[secrets]") {
    TemplateSecrets secrets;
    secrets.add("password", "aaaa");
    // The replacement contains no occurrence of the secret, so a naive
    // implementation cannot re-match its own output.
    REQUIRE(secrets.scrub("aaaa and aaaa") == "<redacted> and <redacted>");
}

TEST_CASE("publicErrorMessage scrubs, and withholds when it must", "[secrets]") {
    TemplateSecrets scrubbable;
    scrubbable.add("password", "hunter2-and-then-some");
    const auto scrubbed = publicErrorMessage(
        "Internal Server Error",
        "Binder Error: ... SELECT 'hunter2-and-then-some' AS pw", scrubbable);
    REQUIRE(scrubbed.find("hunter2") == std::string::npos);
    REQUIRE(scrubbed.find("Binder Error") != std::string::npos);

    TemplateSecrets unscrubbable;
    unscrubbable.add("password", "abc");
    const auto withheld = publicErrorMessage(
        "Internal Server Error", "Binder Error: ... 'abc' ...", unscrubbable);
    REQUIRE(withheld.find("abc'") == std::string::npos);
    REQUIRE(withheld.find("Binder Error") == std::string::npos);
}

TEST_CASE("a long neutrally-named connection property is scrubbed", "[secrets]") {
    // Connection properties got only the credential-NAME heuristic while
    // whitelisted env values got the stronger one, so a property called
    // `service_account_json` or `sas` sailed past a stem list. Both are
    // server-configured; both get the same rule.
    TemplateSecrets secrets;
    secrets.addConnectionProperty("service_account", std::string(40, 'k'));
    REQUIRE(secrets.values().size() == 1);
}

TEST_CASE("a credential-free path or URI stays visible however long", "[secrets]") {
    // `path` and `database` are exactly what an operator reads a preview to
    // confirm, and redacting them makes the preview useless.
    TemplateSecrets secrets;
    secrets.addConnectionProperty("path", "/very/long/data/lake/prefix/with/many/segments.parquet");
    secrets.addConnectionProperty("database", "postgresql://db.example.com:5432/analytics");
    REQUIRE(secrets.values().empty());
}

TEST_CASE("a URI carrying credentials is NOT treated as a location", "[secrets]") {
    // The first version of the location exemption skipped any value
    // containing '/' or '://', which is the shape the credentials cloud
    // deployments actually use - and none of `database`, `url` or `endpoint`
    // matches a credential stem, so all of these reached dry-run previews and
    // error messages verbatim. A test even pinned it as desired.
    SECTION("userinfo in the authority") {
        TemplateSecrets secrets;
        secrets.addConnectionProperty("database",
                                      "postgresql://alice:hunter2@db.example.com/prod");
        REQUIRE(secrets.values().size() == 1);
    }

    SECTION("an Azure SAS signature in the query string") {
        TemplateSecrets secrets;
        secrets.addConnectionProperty(
            "url", "https://acct.blob.core.windows.net/c/f?sv=2021&sig=ABCDEF0123456789");
        REQUIRE(secrets.values().size() == 1);
    }

    SECTION("a presigned S3 URL") {
        TemplateSecrets secrets;
        secrets.addConnectionProperty(
            "endpoint",
            "https://bucket.s3.amazonaws.com/key?X-Amz-Signature=deadbeefdeadbeefdead");
        REQUIRE(secrets.values().size() == 1);
    }
}

TEST_CASE("an overlapping secret is not partly disclosed", "[secrets]") {
    // Replacement was insertion-ordered, so a short secret that is a prefix
    // of a longer one consumed its start and left the tail in the output.
    // Both insertion orders, because the bug only showed in one.
    SECTION("short first") {
        TemplateSecrets secrets;
        secrets.add("access_key", "sk-live-");
        secrets.add("password", "sk-live-supersecret");
        const auto out = secrets.scrub("WHERE k = 'sk-live-supersecret'");
        REQUIRE(out.find("supersecret") == std::string::npos);
    }

    SECTION("long first") {
        TemplateSecrets secrets;
        secrets.add("password", "sk-live-supersecret");
        secrets.add("access_key", "sk-live-");
        const auto out = secrets.scrub("WHERE k = 'sk-live-supersecret'");
        REQUIRE(out.find("supersecret") == std::string::npos);
    }
}

TEST_CASE("a credential-named connection property is scrubbed whatever it looks like",
          "[secrets]") {
    // The location exemption widens what stays visible; it must not narrow
    // what is redacted.
    TemplateSecrets secrets;
    secrets.addConnectionProperty("password", "/not/really/a/path/but/named/password");
    REQUIRE(secrets.values().size() == 1);
}
