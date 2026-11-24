// PathSchemeUtils::RedactUrl redacts every URL that reaches a flAPI log line
// (FlapiLogHandler runs it over each message). Review S1 found it leaking.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "vfs_adapter.hpp"

using flapi::PathSchemeUtils;

TEST_CASE("RedactUrl: userinfo is everything before the last @ of the authority", "[redaction][security]") {
    // No ':' in the userinfo, and an '@' in the path.
    CHECK(PathSchemeUtils::RedactUrl("https://ghp_SECRET@raw.githubusercontent.com/o/r/main/@latest.csv") ==
          "https://raw.githubusercontent.com/o/r/main/@latest.csv");
    // An '@' in the path must not swallow the host.
    CHECK(PathSchemeUtils::RedactUrl("s3://user:pw@bucket/a@b") == "s3://bucket/a@b");
    CHECK(PathSchemeUtils::RedactUrl("https://u:p@h/x") == "https://h/x");
    CHECK(PathSchemeUtils::RedactUrl("https://h@x?y") == "https://x?y");
    // No userinfo: an '@' in the path is data.
    CHECK(PathSchemeUtils::RedactUrl("s3://bucket/users/a@b.com.csv") == "s3://bucket/users/a@b.com.csv");
}

TEST_CASE("RedactUrl: query keys are matched after percent-decoding, and ';' separates too",
          "[redaction][security]") {
    const auto encoded = PathSchemeUtils::RedactUrl("https://h/x?%73ig=SECRET1&%74oken=SECRET2&page=2");
    CHECK(encoded.find("SECRET") == std::string::npos);
    CHECK(encoded.find("page=2") != std::string::npos);
    const auto semicolon = PathSchemeUtils::RedactUrl("https://h/x?a=1;X-Amz-Signature=SECRET3");
    CHECK(semicolon.find("SECRET") == std::string::npos);
    CHECK(semicolon.find("a=1") != std::string::npos);
    // Unchanged when nothing is secret.
    CHECK(PathSchemeUtils::RedactUrl("s3://bucket/path?page=2&format=csv") == "s3://bucket/path?page=2&format=csv");
}
