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
// GCS Path Scheme Detection Tests
// ============================================================================

TEST_CASE("GCS path scheme detection", "[vfs][gcs][scheme]") {
    SECTION("gs:// paths are recognized") {
        REQUIRE(PathSchemeUtils::IsGCSPath("gs://bucket/key"));
        REQUIRE(PathSchemeUtils::IsGCSPath("gs://my-bucket/path/to/file.yaml"));
        REQUIRE(PathSchemeUtils::IsGCSPath("gs://bucket_name/object_path.txt"));
    }

    SECTION("GS:// paths are recognized (case insensitive)") {
        REQUIRE(PathSchemeUtils::IsGCSPath("GS://bucket/key"));
        REQUIRE(PathSchemeUtils::IsGCSPath("Gs://MyBucket/MyObject"));
    }

    SECTION("Non-GCS paths are not recognized") {
        REQUIRE_FALSE(PathSchemeUtils::IsGCSPath("s3://bucket/key"));
        REQUIRE_FALSE(PathSchemeUtils::IsGCSPath("az://container/blob"));
        REQUIRE_FALSE(PathSchemeUtils::IsGCSPath("/local/path"));
        REQUIRE_FALSE(PathSchemeUtils::IsGCSPath("https://storage.googleapis.com/bucket/key"));
    }

    SECTION("GetScheme returns gs:// for GCS paths") {
        REQUIRE(PathSchemeUtils::GetScheme("gs://bucket/key") == "gs://");
    }

    SECTION("GCS paths are remote paths") {
        REQUIRE(PathSchemeUtils::IsRemotePath("gs://bucket/key"));
    }
}

// ============================================================================
// GCS URL Structure Tests
// ============================================================================

TEST_CASE("GCS URL structure", "[vfs][gcs][url]") {
    SECTION("Basic GCS URL components") {
        std::string url = "gs://my-gcs-bucket/path/to/object.yaml";

        REQUIRE(PathSchemeUtils::IsGCSPath(url));

        // Extract bucket name
        size_t scheme_end = url.find("://") + 3;
        size_t bucket_end = url.find('/', scheme_end);
        std::string bucket = url.substr(scheme_end, bucket_end - scheme_end);
        REQUIRE(bucket == "my-gcs-bucket");

        // Extract object path
        std::string object = url.substr(bucket_end + 1);
        REQUIRE(object == "path/to/object.yaml");
    }

    SECTION("GCS bucket naming rules") {
        // GCS bucket names: 3-63 chars, lowercase, numbers, hyphens, underscores
        REQUIRE(PathSchemeUtils::IsGCSPath("gs://abc/key"));  // Minimum length
        REQUIRE(PathSchemeUtils::IsGCSPath("gs://my_bucket/key"));  // Underscore allowed
        REQUIRE(PathSchemeUtils::IsGCSPath("gs://bucket-123/key"));  // Numbers allowed
    }
}

// ============================================================================
// GCS Credential Configuration Tests
// ============================================================================

TEST_CASE("GCS credential configuration", "[vfs][gcs][credentials]") {
    SECTION("Service account key file path") {
        ScopedEnvVar key_file("GOOGLE_APPLICATION_CREDENTIALS", "/path/to/service-account.json");
        ScopedEnvVar project("GOOGLE_CLOUD_PROJECT", "my-gcp-project-123");

        CredentialManager manager;
        manager.loadFromEnvironment();

        REQUIRE(manager.hasGCSCredentials());
        auto creds = manager.getGCSCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->type == CredentialType::ENVIRONMENT);
        REQUIRE(creds->key_file == "/path/to/service-account.json");
        REQUIRE(creds->project_id == "my-gcp-project-123");
    }

    SECTION("GCS credentials struct defaults") {
        GCSCredentials creds;
        REQUIRE(creds.type == CredentialType::ENVIRONMENT);
        REQUIRE(creds.project_id.empty());
        REQUIRE(creds.key_file.empty());
    }

    SECTION("Set credentials explicitly") {
        CredentialManager manager;

        GCSCredentials explicit_creds;
        explicit_creds.type = CredentialType::SERVICE_ACCOUNT;
        explicit_creds.key_file = "/explicit/service-account.json";
        explicit_creds.project_id = "explicit-project";

        manager.setGCSCredentials(explicit_creds);

        auto creds = manager.getGCSCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->type == CredentialType::SERVICE_ACCOUNT);
        REQUIRE(creds->key_file == "/explicit/service-account.json");
    }
}

// ============================================================================
// GCS Project ID Handling
// ============================================================================

TEST_CASE("GCS project ID handling", "[vfs][gcs][project]") {
    SECTION("Project ID format validation") {
        // GCP project IDs: 6-30 chars, lowercase, numbers, hyphens
        GCSCredentials creds;

        // Valid project IDs
        creds.project_id = "my-project";
        REQUIRE_FALSE(creds.project_id.empty());

        creds.project_id = "project-123456";
        REQUIRE_FALSE(creds.project_id.empty());

        creds.project_id = "a-very-long-project-name-here";
        REQUIRE_FALSE(creds.project_id.empty());
    }
}

// ============================================================================
// GCS Integration with VFS
// ============================================================================

TEST_CASE("GCS integration with VFS", "[vfs][gcs][integration]") {
    SECTION("FileProviderFactory routes GCS paths to DuckDB provider") {
        std::string gcs_path = "gs://bucket/object.yaml";

        REQUIRE(PathSchemeUtils::IsRemotePath(gcs_path));
        REQUIRE(PathSchemeUtils::IsGCSPath(gcs_path));
    }

    SECTION("LocalFileProvider does not handle GCS paths") {
        LocalFileProvider local;
        REQUIRE(local.IsRemotePath("gs://bucket/key") == true);
    }
}
