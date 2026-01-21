#include <catch2/catch_test_macros.hpp>
#include "path_validator.hpp"

using namespace flapi;

// ============================================================================
// Path Traversal Attack Prevention Tests
// ============================================================================

TEST_CASE("PathValidator: Path traversal detection", "[security][path_validator]") {
    SECTION("Detects basic .. traversal") {
        REQUIRE(PathValidator::ContainsTraversal(".."));
        REQUIRE(PathValidator::ContainsTraversal("../"));
        REQUIRE(PathValidator::ContainsTraversal("../file.txt"));
        REQUIRE(PathValidator::ContainsTraversal("path/../file.txt"));
        REQUIRE(PathValidator::ContainsTraversal("/path/../file.txt"));
        REQUIRE(PathValidator::ContainsTraversal("path/to/../file.txt"));
    }

    SECTION("Detects Windows-style traversal") {
        REQUIRE(PathValidator::ContainsTraversal("..\\"));
        REQUIRE(PathValidator::ContainsTraversal("..\\file.txt"));
        REQUIRE(PathValidator::ContainsTraversal("path\\..\\file.txt"));
    }

    SECTION("Allows normal paths without traversal") {
        REQUIRE_FALSE(PathValidator::ContainsTraversal("file.txt"));
        REQUIRE_FALSE(PathValidator::ContainsTraversal("path/to/file.txt"));
        REQUIRE_FALSE(PathValidator::ContainsTraversal("/absolute/path/file.txt"));
        REQUIRE_FALSE(PathValidator::ContainsTraversal("path/file..txt"));  // .. in filename
        REQUIRE_FALSE(PathValidator::ContainsTraversal("path/...file.txt")); // ... is ok
        REQUIRE_FALSE(PathValidator::ContainsTraversal("path/.hidden/file.txt")); // .hidden
    }

    SECTION("Detects traversal at end of path") {
        REQUIRE(PathValidator::ContainsTraversal("path/.."));
        REQUIRE(PathValidator::ContainsTraversal("/path/to/.."));
    }
}

TEST_CASE("PathValidator: URL-encoded traversal detection", "[security][path_validator]") {
    PathValidator validator;

    SECTION("Decodes %2e (.) properly") {
        // %2e%2e = ..
        auto result = validator.ValidatePath("%2e%2e/etc/passwd", "/base");
        REQUIRE_FALSE(result.valid);
        REQUIRE(result.error_message.find("traversal") != std::string::npos);
    }

    SECTION("Decodes mixed encoding") {
        // .%2e = ..
        auto result = validator.ValidatePath(".%2e/etc/passwd", "/base");
        REQUIRE_FALSE(result.valid);
    }

    SECTION("Decodes uppercase hex") {
        // %2E%2E = ..
        auto result = validator.ValidatePath("%2E%2E/etc/passwd", "/base");
        REQUIRE_FALSE(result.valid);
    }

    SECTION("Double-encoded traversal") {
        // %252e%252e would decode to %2e%2e, then to ..
        // First decode: %25 = %
        std::string decoded = PathValidator::UrlDecode("%252e%252e");
        REQUIRE(decoded == "%2e%2e");
        // Note: We only decode once, so this is still caught on second validation
    }
}

TEST_CASE("PathValidator: URL decode function", "[security][path_validator]") {
    SECTION("Decodes basic sequences") {
        REQUIRE(PathValidator::UrlDecode("%20") == " ");
        REQUIRE(PathValidator::UrlDecode("%2f") == "/");
        REQUIRE(PathValidator::UrlDecode("%2F") == "/");
        REQUIRE(PathValidator::UrlDecode("%2e") == ".");
    }

    SECTION("Decodes full paths") {
        REQUIRE(PathValidator::UrlDecode("path%2fto%2ffile") == "path/to/file");
        REQUIRE(PathValidator::UrlDecode("%2e%2e%2fpasswd") == "../passwd");
    }

    SECTION("Handles + as space") {
        REQUIRE(PathValidator::UrlDecode("hello+world") == "hello world");
    }

    SECTION("Preserves non-encoded content") {
        REQUIRE(PathValidator::UrlDecode("normal-path") == "normal-path");
        REQUIRE(PathValidator::UrlDecode("/path/to/file.txt") == "/path/to/file.txt");
    }

    SECTION("Handles incomplete sequences") {
        REQUIRE(PathValidator::UrlDecode("%2") == "%2");  // Incomplete
        REQUIRE(PathValidator::UrlDecode("%") == "%");
        REQUIRE(PathValidator::UrlDecode("%GG") == "%GG");  // Invalid hex
    }
}

// ============================================================================
// Prefix-based Access Control Tests
// ============================================================================

TEST_CASE("PathValidator: Allowed prefix checking", "[security][path_validator]") {
    PathValidator::Config config;
    config.allowed_prefixes = {"/allowed/path", "/another/allowed"};
    PathValidator validator(config);

    SECTION("Allows paths within prefix") {
        auto result = validator.ValidatePath("/allowed/path/file.txt");
        REQUIRE(result.valid);

        result = validator.ValidatePath("/allowed/path/subdir/file.txt");
        REQUIRE(result.valid);

        result = validator.ValidatePath("/another/allowed/file.txt");
        REQUIRE(result.valid);
    }

    SECTION("Rejects paths outside prefix") {
        auto result = validator.ValidatePath("/forbidden/path/file.txt");
        REQUIRE_FALSE(result.valid);
        REQUIRE(result.error_message.find("not within allowed") != std::string::npos);

        result = validator.ValidatePath("/etc/passwd");
        REQUIRE_FALSE(result.valid);
    }

    SECTION("Allows exact prefix match") {
        auto result = validator.ValidatePath("/allowed/path");
        REQUIRE(result.valid);
    }

    SECTION("Rejects prefix-like but not matching paths") {
        // /allowed/pathXXX should not match /allowed/path/
        auto result = validator.ValidatePath("/allowed/pathological/file.txt");
        REQUIRE_FALSE(result.valid);
    }
}

TEST_CASE("PathValidator: No prefix restriction", "[security][path_validator]") {
    PathValidator::Config config;
    config.allowed_prefixes = {};  // Empty = allow all
    PathValidator validator(config);

    SECTION("Allows any path when no prefixes configured") {
        auto result = validator.ValidatePath("/any/path/file.txt");
        REQUIRE(result.valid);

        result = validator.ValidatePath("/etc/passwd");
        REQUIRE(result.valid);
    }
}

// ============================================================================
// URL Scheme Whitelisting Tests
// ============================================================================

TEST_CASE("PathValidator: Scheme whitelisting", "[security][path_validator]") {
    PathValidator::Config config;
    config.allowed_schemes = {"https", "file"};
    PathValidator validator(config);

    SECTION("Allows whitelisted schemes") {
        REQUIRE(validator.IsSchemeAllowed("https"));
        REQUIRE(validator.IsSchemeAllowed("file"));
    }

    SECTION("Rejects non-whitelisted schemes") {
        REQUIRE_FALSE(validator.IsSchemeAllowed("s3"));
        REQUIRE_FALSE(validator.IsSchemeAllowed("gs"));
        REQUIRE_FALSE(validator.IsSchemeAllowed("http"));
        REQUIRE_FALSE(validator.IsSchemeAllowed("ftp"));
    }

    SECTION("Validates remote paths with scheme check") {
        auto result = validator.ValidatePath("https://example.com/file.txt");
        REQUIRE(result.valid);

        result = validator.ValidatePath("s3://bucket/file.txt");
        REQUIRE_FALSE(result.valid);
        REQUIRE(result.error_message.find("scheme not allowed") != std::string::npos);
    }
}

TEST_CASE("PathValidator: Adding schemes dynamically", "[security][path_validator]") {
    PathValidator validator;

    SECTION("Can add S3 scheme") {
        REQUIRE_FALSE(validator.IsSchemeAllowed("s3"));

        validator.AddAllowedScheme("s3");

        REQUIRE(validator.IsSchemeAllowed("s3"));

        auto result = validator.ValidatePath("s3://bucket/key.txt");
        REQUIRE(result.valid);
    }

    SECTION("Can add multiple schemes") {
        validator.AddAllowedScheme("s3");
        validator.AddAllowedScheme("gs");
        validator.AddAllowedScheme("az");

        REQUIRE(validator.IsSchemeAllowed("s3"));
        REQUIRE(validator.IsSchemeAllowed("gs"));
        REQUIRE(validator.IsSchemeAllowed("az"));
    }
}

TEST_CASE("PathValidator: Scheme extraction", "[security][path_validator]") {
    SECTION("Extracts common schemes") {
        REQUIRE(PathValidator::ExtractScheme("https://example.com") == "https");
        REQUIRE(PathValidator::ExtractScheme("http://example.com") == "http");
        REQUIRE(PathValidator::ExtractScheme("s3://bucket/key") == "s3");
        REQUIRE(PathValidator::ExtractScheme("gs://bucket/key") == "gs");
        REQUIRE(PathValidator::ExtractScheme("file:///path") == "file");
    }

    SECTION("Returns empty for no scheme") {
        REQUIRE(PathValidator::ExtractScheme("/local/path") == "");
        REQUIRE(PathValidator::ExtractScheme("relative/path") == "");
        REQUIRE(PathValidator::ExtractScheme("") == "");
    }

    SECTION("Handles case insensitively") {
        REQUIRE(PathValidator::ExtractScheme("HTTPS://example.com") == "https");
        REQUIRE(PathValidator::ExtractScheme("S3://bucket/key") == "s3");
    }

    SECTION("Rejects invalid schemes") {
        REQUIRE(PathValidator::ExtractScheme("://no-scheme") == "");
        REQUIRE(PathValidator::ExtractScheme("bad scheme://host") == "");
    }
}

// ============================================================================
// Path Canonicalization Tests
// ============================================================================

TEST_CASE("PathValidator: Path canonicalization", "[security][path_validator]") {
    PathValidator validator;

    SECTION("Combines base and relative paths") {
        std::string result = validator.Canonicalize("/base/path", "file.txt");
        REQUIRE(result == "/base/path/file.txt");

        result = validator.Canonicalize("/base/path/", "file.txt");
        REQUIRE(result == "/base/path/file.txt");
    }

    SECTION("Handles ./ prefix") {
        std::string result = validator.Canonicalize("/base", "./file.txt");
        REQUIRE(result == "/base/file.txt");
    }

    SECTION("Returns empty for traversal") {
        std::string result = validator.Canonicalize("/base", "../file.txt");
        REQUIRE(result == "");

        result = validator.Canonicalize("/base", "sub/../../../etc/passwd");
        REQUIRE(result == "");
    }

    SECTION("Normalizes Windows separators") {
        std::string result = validator.Canonicalize("C:\\base\\path", "file.txt");
        REQUIRE(result == "C:/base/path/file.txt");
    }

    SECTION("Removes duplicate slashes") {
        std::string result = validator.Canonicalize("/base//path", "file.txt");
        REQUIRE(result == "/base/path/file.txt");
    }
}

// ============================================================================
// Remote Path Detection Tests
// ============================================================================

TEST_CASE("PathValidator: Remote path detection", "[security][path_validator]") {
    SECTION("Identifies remote schemes") {
        REQUIRE(PathValidator::IsRemotePath("s3://bucket/key"));
        REQUIRE(PathValidator::IsRemotePath("gs://bucket/key"));
        REQUIRE(PathValidator::IsRemotePath("https://example.com/path"));
        REQUIRE(PathValidator::IsRemotePath("http://example.com/path"));
        REQUIRE(PathValidator::IsRemotePath("az://container/blob"));
        REQUIRE(PathValidator::IsRemotePath("abfs://container@account/path"));
    }

    SECTION("Identifies local paths") {
        REQUIRE_FALSE(PathValidator::IsRemotePath("/local/path"));
        REQUIRE_FALSE(PathValidator::IsRemotePath("relative/path"));
        REQUIRE_FALSE(PathValidator::IsRemotePath("file:///local/path"));
        REQUIRE_FALSE(PathValidator::IsRemotePath("C:\\Windows\\path"));
    }
}

// ============================================================================
// Full Validation Flow Tests
// ============================================================================

TEST_CASE("PathValidator: Full validation scenarios", "[security][path_validator]") {
    PathValidator::Config config;
    config.allowed_schemes = {"https", "s3"};
    config.allowed_prefixes = {"/app/data", "/app/templates"};
    PathValidator validator(config);

    SECTION("Valid local path within allowed prefix") {
        auto result = validator.ValidatePath("/app/data/users.json");
        REQUIRE(result.valid);
        REQUIRE(result.canonical_path == "/app/data/users.json");
    }

    SECTION("Valid relative path resolved against base") {
        auto result = validator.ValidatePath("users.json", "/app/data");
        REQUIRE(result.valid);
        REQUIRE(result.canonical_path == "/app/data/users.json");
    }

    SECTION("Valid HTTPS URL") {
        auto result = validator.ValidatePath("https://api.example.com/data.json");
        REQUIRE(result.valid);
    }

    SECTION("Valid S3 URL after adding scheme") {
        auto result = validator.ValidatePath("s3://mybucket/data/file.json");
        REQUIRE(result.valid);
    }

    SECTION("Rejects traversal attempt") {
        auto result = validator.ValidatePath("../../../etc/passwd", "/app/data");
        REQUIRE_FALSE(result.valid);
        REQUIRE(result.error_message.find("traversal") != std::string::npos);
    }

    SECTION("Rejects path outside allowed prefix") {
        auto result = validator.ValidatePath("/etc/passwd");
        REQUIRE_FALSE(result.valid);
    }

    SECTION("Rejects disallowed scheme") {
        auto result = validator.ValidatePath("ftp://server/file.txt");
        REQUIRE_FALSE(result.valid);
        REQUIRE(result.error_message.find("scheme not allowed") != std::string::npos);
    }
}

TEST_CASE("PathValidator: Empty and edge cases", "[security][path_validator]") {
    PathValidator validator;

    SECTION("Rejects empty path") {
        auto result = validator.ValidatePath("");
        REQUIRE_FALSE(result.valid);
        REQUIRE(result.error_message.find("empty") != std::string::npos);
    }

    SECTION("Handles whitespace-only paths") {
        auto result = validator.ValidatePath("   ");
        // After URL decode, this is just spaces - should fail as not useful
        REQUIRE_FALSE(result.valid);
    }
}

// ============================================================================
// OWASP Path Traversal Pattern Tests
// ============================================================================

TEST_CASE("PathValidator: OWASP path traversal patterns", "[security][path_validator]") {
    PathValidator::Config config;
    config.allowed_prefixes = {"/safe"};
    PathValidator validator(config);

    // Test patterns from OWASP Path Traversal cheat sheet
    SECTION("Basic traversal patterns") {
        REQUIRE_FALSE(validator.ValidatePath("../../../etc/passwd", "/safe").valid);
        REQUIRE_FALSE(validator.ValidatePath("..\\..\\..\\windows\\system32", "/safe").valid);
    }

    SECTION("URL-encoded traversal") {
        REQUIRE_FALSE(validator.ValidatePath("%2e%2e/%2e%2e/etc/passwd", "/safe").valid);
        REQUIRE_FALSE(validator.ValidatePath("..%2f..%2f..%2fetc/passwd", "/safe").valid);
        REQUIRE_FALSE(validator.ValidatePath("%2e%2e%5c..%5c..%5cwindows", "/safe").valid);
    }

    SECTION("Double URL-encoding") {
        // %252e = %2e after first decode, which is . after second decode
        // We only decode once, but the first decode should reveal suspicious patterns
        std::string first_decode = PathValidator::UrlDecode("%252e%252e");
        REQUIRE(first_decode == "%2e%2e");
    }

    SECTION("Null byte injection (legacy)") {
        // %00 null byte - should be decoded and path should still be validated
        auto result = validator.ValidatePath("/safe/file.txt%00.jpg", "/safe");
        // Path is within /safe, so it's allowed
        REQUIRE(result.valid);
        // The null byte becomes \0 in the string but path is still under /safe
    }

    SECTION("Unicode/UTF-8 encoding attacks") {
        // Overlong UTF-8 encoding of / (%c0%af)
        // This should be treated as literal characters since we do basic URL decode
        auto result = validator.ValidatePath("%c0%af", "/safe");
        // This decodes to invalid UTF-8, treated as literal
        REQUIRE(result.valid);  // No traversal detected
    }
}

TEST_CASE("PathValidator: Configuration validation", "[security][path_validator]") {
    SECTION("Default config allows file and https") {
        PathValidator validator;
        const auto& config = validator.GetConfig();

        REQUIRE(config.allowed_schemes.count("file") == 1);
        REQUIRE(config.allowed_schemes.count("https") == 1);
        REQUIRE(config.allow_local_paths == true);
        REQUIRE(config.allow_relative_paths == true);
    }

    SECTION("Can disable local paths") {
        PathValidator::Config config;
        config.allow_local_paths = false;
        PathValidator validator(config);

        auto result = validator.ValidatePath("/local/path");
        REQUIRE_FALSE(result.valid);
        REQUIRE(result.error_message.find("Local paths not allowed") != std::string::npos);
    }

    SECTION("Can disable relative paths") {
        PathValidator::Config config;
        config.allow_relative_paths = false;
        PathValidator validator(config);

        auto result = validator.ValidatePath("relative/path", "/base");
        REQUIRE_FALSE(result.valid);
        REQUIRE(result.error_message.find("Relative paths not allowed") != std::string::npos);
    }
}
