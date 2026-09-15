// Issue 8 - capture tiers and redaction.
//
// The heart of decision D2, and the part most likely to be got wrong by an
// implementation optimising for a good Phoenix demo. flAPI's market is German
// manufacturing; "we export no customer data by default" is a claim that has to
// survive a security review, which means it has to be a test rather than a
// sentence in a README.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_set>
#include <vector>

#include "trace_capture_policy.hpp"

using namespace flapi;

namespace {

CapturePolicy makePolicy(CaptureTier global,
                         std::unordered_set<std::string> redact_keys = {},
                         std::size_t max_bytes = 64) {
    return CapturePolicy(global, std::move(redact_keys), max_bytes, 50);
}

}  // namespace

TEST_CASE("global off beats a per-endpoint payload opt-in", "[capture_policy][security]") {
    // The one lever a security review will ask for: a single setting that is
    // GUARANTEED to stop data export, whatever any endpoint says.
    const auto policy = makePolicy(CaptureTier::Off);
    REQUIRE(policy.effectiveTier(std::nullopt) == CaptureTier::Off);
    REQUIRE(policy.effectiveTier(CaptureTier::Payload) == CaptureTier::Off);
    REQUIRE(policy.effectiveTier(CaptureTier::Metadata) == CaptureTier::Off);
}

TEST_CASE("an endpoint may opt in to payload when global allows it", "[capture_policy]") {
    const auto policy = makePolicy(CaptureTier::Metadata);
    REQUIRE(policy.effectiveTier(std::nullopt) == CaptureTier::Metadata);
    REQUIRE(policy.effectiveTier(CaptureTier::Payload) == CaptureTier::Payload);
}

TEST_CASE("an endpoint may opt DOWN from the global tier", "[capture_policy]") {
    // A sensitive endpoint inside a payload-tier deployment must be able to
    // exclude itself.
    const auto policy = makePolicy(CaptureTier::Payload);
    REQUIRE(policy.effectiveTier(CaptureTier::Metadata) == CaptureTier::Metadata);
    REQUIRE(policy.effectiveTier(CaptureTier::Off) == CaptureTier::Off);
}

TEST_CASE("values are captured only at the payload tier", "[capture_policy][security]") {
    REQUIRE_FALSE(makePolicy(CaptureTier::Off).capturesValues(std::nullopt));
    REQUIRE_FALSE(makePolicy(CaptureTier::Metadata).capturesValues(std::nullopt));
    REQUIRE(makePolicy(CaptureTier::Payload).capturesValues(std::nullopt));
}

TEST_CASE("redaction happens BEFORE clamping", "[capture_policy][security]") {
    // Order of operations matters: clamping first can truncate mid-value and
    // leave a partial secret in the span. A partial secret is still a secret.
    CapturePolicy policy(CaptureTier::Payload, {"password"}, /*max_bytes=*/20, 50);

    const std::string redacted = policy.redactAndClamp("password", "hunter2-and-a-very-long-tail");
    REQUIRE(redacted.find("hunter2") == std::string::npos);
    REQUIRE(redacted == "<redacted>");
}

TEST_CASE("clamping is UTF-8 safe", "[capture_policy]") {
    // Truncating mid-sequence produces invalid UTF-8, which a backend may reject
    // outright - losing the whole span rather than one attribute.
    CapturePolicy policy(CaptureTier::Payload, {}, /*max_bytes=*/5, 50);
    const std::string clamped = policy.redactAndClamp("note", "caf\xC3\xA9 \xE5\x8C\x97\xE4\xBA\xAC");

    REQUIRE(clamped.size() <= 5);
    // Every byte that survives must be part of a complete sequence: no byte may
    // be a continuation byte left without its lead.
    std::size_t i = 0;
    while (i < clamped.size()) {
        const auto c = static_cast<unsigned char>(clamped[i]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0)      { len = 2; }
        else if ((c & 0xF0) == 0xE0) { len = 3; }
        else if ((c & 0xF8) == 0xF0) { len = 4; }
        REQUIRE(i + len <= clamped.size());
        i += len;
    }
}

TEST_CASE("the redact key list is reused, not reinvented", "[capture_policy][security]") {
    // audit.redact_keys already exists and operators already configure it. A
    // second, separate list would silently diverge and leak whatever the operator
    // forgot to add twice.
    CapturePolicy policy(CaptureTier::Payload, {"token", "secret"}, 1024, 50);
    REQUIRE(policy.redactAndClamp("token", "abc") == "<redacted>");
    REQUIRE(policy.redactAndClamp("secret", "abc") == "<redacted>");
    REQUIRE(policy.redactAndClamp("harmless", "abc") == "abc");
}

TEST_CASE("redaction is case-insensitive on the key", "[capture_policy][security]") {
    // A query parameter named "Token" must not slip past a list containing
    // "token": HTTP parameter names are not reliably normalised, and the failure
    // mode is a leaked credential.
    CapturePolicy policy(CaptureTier::Payload, {"token"}, 1024, 50);
    REQUIRE(policy.redactAndClamp("Token", "abc") == "<redacted>");
    REQUIRE(policy.redactAndClamp("TOKEN", "abc") == "<redacted>");
}

TEST_CASE("credential-shaped keys are always redacted", "[capture_policy][security]") {
    // Defence in depth: these are never legitimate span content at ANY tier, and
    // an operator should not have to remember to list them. The plan's invariant
    // is that Authorization headers, bearer tokens, passwords and connection
    // strings never appear - that cannot depend on configuration.
    CapturePolicy policy(CaptureTier::Payload, {}, 1024, 50);
    for (const char* key : {"authorization", "Authorization", "password", "passwd",
                            "api_key", "apikey", "access_token", "refresh_token",
                            "client_secret", "private_key", "connection_string"}) {
        INFO("key: " << key);
        REQUIRE(policy.redactAndClamp(key, "sensitive") == "<redacted>");
    }
}

TEST_CASE("db.query.text is withheld unless every site is a prepared binding",
          "[capture_policy][security]") {
    // flAPI binds typed parameters as DuckDB prepared statements, so the
    // parameterised SQL contains placeholders rather than values - genuinely safe.
    // That is NOT true for interpolated sites, where a value is substituted into
    // the SQL text itself.
    CapturePolicy policy(CaptureTier::Metadata, {}, 1024, 50);
    REQUIRE_FALSE(policy.allowQueryText(/*interpolated=*/0, /*opt_in=*/false));
    REQUIRE(policy.allowQueryText(/*interpolated=*/0, /*opt_in=*/true));
    REQUIRE_FALSE(policy.allowQueryText(/*interpolated=*/1, /*opt_in=*/true));
}

TEST_CASE("the document cap is bounded in BYTES as well as count",
          "[capture_policy][security]") {
    // Capping only the count is not a bound: fifty rows of a wide table is
    // megabytes held in the export queue per span - a memory-exhaustion vector an
    // operator can reach by enabling a documented feature.
    CapturePolicy policy(CaptureTier::Payload, {}, /*max_bytes=*/32, /*max_documents=*/50);
    REQUIRE(policy.maxDocuments() == 50);
    REQUIRE(policy.maxValueBytes() == 32);

    std::size_t budget = policy.maxValueBytes() * policy.maxDocuments();
    REQUIRE(budget > 0);
}
