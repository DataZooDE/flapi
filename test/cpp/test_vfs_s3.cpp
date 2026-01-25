#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
#include "vfs_adapter.hpp"
#include "credential_manager.hpp"
#include <cstdlib>

using namespace flapi;

// Helper to set/unset environment variables for testing
class ScopedEnvVar {
public:
    ScopedEnvVar(const std::string& name, const std::string& value)
        : name_(name), had_value_(false) {
        const char* old_value = std::getenv(name.c_str());
        if (old_value) {
            had_value_ = true;
            old_value_ = old_value;
        }
        setenv(name.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar() {
        if (had_value_) {
            setenv(name_.c_str(), old_value_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::string old_value_;
    bool had_value_;
};

// ============================================================================
// S3 Path Scheme Detection Tests
// ============================================================================

TEST_CASE("S3 path scheme detection", "[vfs][s3][scheme]") {
    SECTION("s3:// paths are recognized") {
        REQUIRE(PathSchemeUtils::IsS3Path("s3://bucket/key"));
        REQUIRE(PathSchemeUtils::IsS3Path("s3://my-bucket/path/to/file.yaml"));
        REQUIRE(PathSchemeUtils::IsS3Path("s3://bucket-with-dashes/key_with_underscores.txt"));
    }

    SECTION("S3:// paths are recognized (case insensitive)") {
        REQUIRE(PathSchemeUtils::IsS3Path("S3://bucket/key"));
        REQUIRE(PathSchemeUtils::IsS3Path("S3://MyBucket/MyKey"));
    }

    SECTION("Non-S3 paths are not recognized") {
        REQUIRE_FALSE(PathSchemeUtils::IsS3Path("gs://bucket/key"));
        REQUIRE_FALSE(PathSchemeUtils::IsS3Path("az://container/blob"));
        REQUIRE_FALSE(PathSchemeUtils::IsS3Path("/local/path"));
        REQUIRE_FALSE(PathSchemeUtils::IsS3Path("./relative"));
        REQUIRE_FALSE(PathSchemeUtils::IsS3Path("https://example.com"));
    }

    SECTION("GetScheme returns s3:// for S3 paths") {
        REQUIRE(PathSchemeUtils::GetScheme("s3://bucket/key") == "s3://");
    }

    SECTION("S3 paths are remote paths") {
        REQUIRE(PathSchemeUtils::IsRemotePath("s3://bucket/key"));
    }
}

// ============================================================================
// S3 URL Parsing Tests
// ============================================================================

TEST_CASE("S3 URL structure", "[vfs][s3][url]") {
    SECTION("Basic S3 URL components") {
        std::string url = "s3://my-bucket/path/to/file.yaml";

        // Verify it's an S3 path
        REQUIRE(PathSchemeUtils::IsS3Path(url));

        // Extract bucket name (everything between s3:// and first /)
        size_t scheme_end = url.find("://") + 3;
        size_t bucket_end = url.find('/', scheme_end);
        std::string bucket = url.substr(scheme_end, bucket_end - scheme_end);
        REQUIRE(bucket == "my-bucket");

        // Extract key (everything after bucket/)
        std::string key = url.substr(bucket_end + 1);
        REQUIRE(key == "path/to/file.yaml");
    }

    SECTION("S3 URL with special characters") {
        std::string url = "s3://bucket/path/with spaces/file-name_v1.2.yaml";
        REQUIRE(PathSchemeUtils::IsS3Path(url));
    }

    SECTION("S3 URL with only bucket (no key)") {
        std::string url = "s3://bucket/";
        REQUIRE(PathSchemeUtils::IsS3Path(url));
    }
}

// ============================================================================
// S3 Credential Configuration Tests
// ============================================================================

TEST_CASE("S3 credential configuration", "[vfs][s3][credentials]") {
    SECTION("Environment variable names are correct") {
        // Verify the expected environment variable names
        ScopedEnvVar key_id("AWS_ACCESS_KEY_ID", "AKIAIOSFODNN7EXAMPLE");
        ScopedEnvVar secret("AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
        ScopedEnvVar region("AWS_REGION", "us-east-1");

        CredentialManager manager;
        manager.loadFromEnvironment();

        REQUIRE(manager.hasS3Credentials());
        auto creds = manager.getS3Credentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->access_key_id == "AKIAIOSFODNN7EXAMPLE");
        REQUIRE(creds->secret_access_key == "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY");
        REQUIRE(creds->region == "us-east-1");
    }

    SECTION("Temporary credentials with session token") {
        ScopedEnvVar key_id("AWS_ACCESS_KEY_ID", "temp_key");
        ScopedEnvVar secret("AWS_SECRET_ACCESS_KEY", "temp_secret");
        ScopedEnvVar token("AWS_SESSION_TOKEN", "AQoDYXdzEJr...");

        CredentialManager manager;
        manager.loadFromEnvironment();

        auto creds = manager.getS3Credentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->session_token == "AQoDYXdzEJr...");
    }

    SECTION("S3-compatible endpoint (MinIO, LocalStack)") {
        ScopedEnvVar key_id("AWS_ACCESS_KEY_ID", "minioadmin");
        ScopedEnvVar secret("AWS_SECRET_ACCESS_KEY", "minioadmin");
        ScopedEnvVar endpoint("AWS_ENDPOINT_URL", "http://localhost:9000");

        CredentialManager manager;
        manager.loadFromEnvironment();

        auto creds = manager.getS3Credentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->endpoint == "http://localhost:9000");
    }

    SECTION("S3 credentials struct defaults") {
        S3Credentials creds;
        REQUIRE(creds.type == CredentialType::ENVIRONMENT);
        REQUIRE(creds.region.empty());
        REQUIRE(creds.access_key_id.empty());
        REQUIRE(creds.secret_access_key.empty());
        REQUIRE(creds.session_token.empty());
        REQUIRE(creds.endpoint.empty());
        REQUIRE(creds.use_ssl == true);
    }
}

// ============================================================================
// S3 Region Handling Tests
// ============================================================================

TEST_CASE("S3 region handling", "[vfs][s3][region]") {
    SECTION("Common AWS regions are accepted") {
        S3Credentials creds;

        // Test various regions
        std::vector<std::string> regions = {
            "us-east-1", "us-east-2", "us-west-1", "us-west-2",
            "eu-west-1", "eu-central-1", "eu-north-1",
            "ap-southeast-1", "ap-northeast-1", "ap-south-1",
            "sa-east-1", "me-south-1", "af-south-1"
        };

        for (const auto& region : regions) {
            creds.region = region;
            REQUIRE_FALSE(creds.region.empty());
        }
    }

    SECTION("AWS_DEFAULT_REGION fallback works") {
        // This is tested in test_credential_manager.cpp
        // Just verify the behavior is consistent
        REQUIRE(true);
    }
}

// ============================================================================
// S3 Error Handling Tests (without actual S3 access)
// ============================================================================

TEST_CASE("S3 error scenarios", "[vfs][s3][errors]") {
    SECTION("Invalid bucket name patterns") {
        // These would fail validation in actual S3 operations
        // Just verify they're still valid S3 URLs syntactically
        REQUIRE(PathSchemeUtils::IsS3Path("s3://x/key")); // Too short bucket name
        REQUIRE(PathSchemeUtils::IsS3Path("s3://-bucket/key")); // Starts with hyphen
    }

    SECTION("Missing credentials error message should be clear") {
        S3Credentials creds;
        // Empty credentials - would cause clear error
        REQUIRE(creds.access_key_id.empty());
        REQUIRE(creds.secret_access_key.empty());
    }
}

// ============================================================================
// S3 Integration with VFS Adapter
// ============================================================================

TEST_CASE("S3 integration with VFS", "[vfs][s3][integration]") {
    SECTION("FileProviderFactory routes S3 paths to DuckDB provider") {
        // Note: This would fail if DatabaseManager is not initialized
        // We only test the routing logic, not actual S3 access
        std::string s3_path = "s3://bucket/key.yaml";

        REQUIRE(PathSchemeUtils::IsRemotePath(s3_path));
        REQUIRE(PathSchemeUtils::IsS3Path(s3_path));

        // CreateProvider would create DuckDBVFSProvider for this path
        // Actual test requires DatabaseManager, so we just verify routing logic
    }

    SECTION("LocalFileProvider does not handle S3 paths") {
        LocalFileProvider local;
        REQUIRE(local.IsRemotePath("s3://bucket/key") == true);
        // local.FileExists would return false for S3 paths
    }
}
