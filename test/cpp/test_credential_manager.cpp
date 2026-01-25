#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_all.hpp>
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

class ScopedEnvVarUnset {
public:
    explicit ScopedEnvVarUnset(const std::string& name)
        : name_(name), had_value_(false) {
        const char* old_value = std::getenv(name.c_str());
        if (old_value) {
            had_value_ = true;
            old_value_ = old_value;
            unsetenv(name.c_str());
        }
    }

    ~ScopedEnvVarUnset() {
        if (had_value_) {
            setenv(name_.c_str(), old_value_.c_str(), 1);
        }
    }

private:
    std::string name_;
    std::string old_value_;
    bool had_value_;
};

// ============================================================================
// CredentialType String Conversion Tests
// ============================================================================

TEST_CASE("CredentialManager::credentialTypeToString", "[vfs][credentials]") {
    SECTION("Converts all types correctly") {
        REQUIRE(CredentialManager::credentialTypeToString(CredentialType::NONE) == "none");
        REQUIRE(CredentialManager::credentialTypeToString(CredentialType::ENVIRONMENT) == "environment");
        REQUIRE(CredentialManager::credentialTypeToString(CredentialType::SECRET) == "secret");
        REQUIRE(CredentialManager::credentialTypeToString(CredentialType::INSTANCE_PROFILE) == "instance_profile");
        REQUIRE(CredentialManager::credentialTypeToString(CredentialType::SERVICE_ACCOUNT) == "service_account");
        REQUIRE(CredentialManager::credentialTypeToString(CredentialType::CONNECTION_STRING) == "connection_string");
        REQUIRE(CredentialManager::credentialTypeToString(CredentialType::MANAGED_IDENTITY) == "managed_identity");
    }
}

// ============================================================================
// S3 Credential Tests
// ============================================================================

TEST_CASE("CredentialManager S3 credentials", "[vfs][credentials][s3]") {
    CredentialManager manager;

    SECTION("No credentials by default") {
        REQUIRE_FALSE(manager.hasS3Credentials());
        REQUIRE_FALSE(manager.getS3Credentials().has_value());
    }

    SECTION("Load from environment variables") {
        // Set environment variables
        ScopedEnvVar key_id("AWS_ACCESS_KEY_ID", "test_key_id");
        ScopedEnvVar secret("AWS_SECRET_ACCESS_KEY", "test_secret");
        ScopedEnvVar region("AWS_REGION", "us-west-2");

        CredentialManager fresh_manager;
        fresh_manager.loadFromEnvironment();

        REQUIRE(fresh_manager.hasS3Credentials());
        auto creds = fresh_manager.getS3Credentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->type == CredentialType::ENVIRONMENT);
        REQUIRE(creds->access_key_id == "test_key_id");
        REQUIRE(creds->secret_access_key == "test_secret");
        REQUIRE(creds->region == "us-west-2");
    }

    SECTION("AWS_DEFAULT_REGION fallback") {
        ScopedEnvVar key_id("AWS_ACCESS_KEY_ID", "key");
        ScopedEnvVarUnset region("AWS_REGION");  // Ensure not set
        ScopedEnvVar default_region("AWS_DEFAULT_REGION", "eu-central-1");

        CredentialManager fresh_manager;
        fresh_manager.loadFromEnvironment();

        auto creds = fresh_manager.getS3Credentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->region == "eu-central-1");
    }

    SECTION("Session token is optional") {
        ScopedEnvVar key_id("AWS_ACCESS_KEY_ID", "key");
        ScopedEnvVar secret("AWS_SECRET_ACCESS_KEY", "secret");
        ScopedEnvVar token("AWS_SESSION_TOKEN", "temp_token");

        CredentialManager fresh_manager;
        fresh_manager.loadFromEnvironment();

        auto creds = fresh_manager.getS3Credentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->session_token == "temp_token");
    }

    SECTION("Set credentials explicitly") {
        S3Credentials explicit_creds;
        explicit_creds.type = CredentialType::SECRET;
        explicit_creds.access_key_id = "explicit_key";
        explicit_creds.secret_access_key = "explicit_secret";
        explicit_creds.region = "ap-southeast-1";

        manager.setS3Credentials(explicit_creds);

        REQUIRE(manager.hasS3Credentials());
        auto creds = manager.getS3Credentials();
        REQUIRE(creds->type == CredentialType::SECRET);
        REQUIRE(creds->access_key_id == "explicit_key");
        REQUIRE(creds->region == "ap-southeast-1");
    }

    SECTION("Custom endpoint for S3-compatible storage") {
        ScopedEnvVar key_id("AWS_ACCESS_KEY_ID", "minio_key");
        ScopedEnvVar secret("AWS_SECRET_ACCESS_KEY", "minio_secret");
        ScopedEnvVar endpoint("AWS_ENDPOINT_URL", "http://localhost:9000");

        CredentialManager fresh_manager;
        fresh_manager.loadFromEnvironment();

        auto creds = fresh_manager.getS3Credentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->endpoint == "http://localhost:9000");
    }
}

// ============================================================================
// GCS Credential Tests
// ============================================================================

TEST_CASE("CredentialManager GCS credentials", "[vfs][credentials][gcs]") {
    CredentialManager manager;

    SECTION("No credentials by default") {
        REQUIRE_FALSE(manager.hasGCSCredentials());
    }

    SECTION("Load from environment variables") {
        ScopedEnvVar creds_file("GOOGLE_APPLICATION_CREDENTIALS", "/path/to/service-account.json");
        ScopedEnvVar project("GOOGLE_CLOUD_PROJECT", "my-gcp-project");

        CredentialManager fresh_manager;
        fresh_manager.loadFromEnvironment();

        REQUIRE(fresh_manager.hasGCSCredentials());
        auto creds = fresh_manager.getGCSCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->type == CredentialType::ENVIRONMENT);
        REQUIRE(creds->key_file == "/path/to/service-account.json");
        REQUIRE(creds->project_id == "my-gcp-project");
    }

    SECTION("GCLOUD_PROJECT fallback") {
        ScopedEnvVar creds_file("GOOGLE_APPLICATION_CREDENTIALS", "/path/to/key.json");
        ScopedEnvVarUnset project1("GOOGLE_CLOUD_PROJECT");
        ScopedEnvVar project2("GCLOUD_PROJECT", "fallback-project");

        CredentialManager fresh_manager;
        fresh_manager.loadFromEnvironment();

        auto creds = fresh_manager.getGCSCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->project_id == "fallback-project");
    }

    SECTION("Set credentials explicitly") {
        GCSCredentials explicit_creds;
        explicit_creds.type = CredentialType::SERVICE_ACCOUNT;
        explicit_creds.key_file = "/explicit/path/key.json";
        explicit_creds.project_id = "explicit-project";

        manager.setGCSCredentials(explicit_creds);

        REQUIRE(manager.hasGCSCredentials());
        auto creds = manager.getGCSCredentials();
        REQUIRE(creds->type == CredentialType::SERVICE_ACCOUNT);
        REQUIRE(creds->key_file == "/explicit/path/key.json");
    }
}

// ============================================================================
// Azure Credential Tests
// ============================================================================

TEST_CASE("CredentialManager Azure credentials", "[vfs][credentials][azure]") {
    CredentialManager manager;

    SECTION("No credentials by default") {
        REQUIRE_FALSE(manager.hasAzureCredentials());
    }

    SECTION("Load from connection string") {
        ScopedEnvVar conn_str("AZURE_STORAGE_CONNECTION_STRING",
                               "DefaultEndpointsProtocol=https;AccountName=test;AccountKey=key==");

        CredentialManager fresh_manager;
        fresh_manager.loadFromEnvironment();

        REQUIRE(fresh_manager.hasAzureCredentials());
        auto creds = fresh_manager.getAzureCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->type == CredentialType::CONNECTION_STRING);
        REQUIRE_FALSE(creds->connection_string.empty());
    }

    SECTION("Load from account name and key") {
        ScopedEnvVarUnset conn_str("AZURE_STORAGE_CONNECTION_STRING");
        ScopedEnvVar account("AZURE_STORAGE_ACCOUNT", "mystorageaccount");
        ScopedEnvVar key("AZURE_STORAGE_KEY", "base64key==");

        CredentialManager fresh_manager;
        fresh_manager.loadFromEnvironment();

        REQUIRE(fresh_manager.hasAzureCredentials());
        auto creds = fresh_manager.getAzureCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->type == CredentialType::ENVIRONMENT);
        REQUIRE(creds->account_name == "mystorageaccount");
        REQUIRE(creds->account_key == "base64key==");
    }

    SECTION("Managed identity detection") {
        ScopedEnvVarUnset conn_str("AZURE_STORAGE_CONNECTION_STRING");
        ScopedEnvVar account("AZURE_STORAGE_ACCOUNT", "myaccount");
        ScopedEnvVar tenant("AZURE_TENANT_ID", "tenant-id-123");
        ScopedEnvVar client("AZURE_CLIENT_ID", "client-id-456");

        CredentialManager fresh_manager;
        fresh_manager.loadFromEnvironment();

        auto creds = fresh_manager.getAzureCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->type == CredentialType::MANAGED_IDENTITY);
        REQUIRE(creds->tenant_id == "tenant-id-123");
        REQUIRE(creds->client_id == "client-id-456");
    }

    SECTION("Set credentials explicitly") {
        AzureCredentials explicit_creds;
        explicit_creds.type = CredentialType::CONNECTION_STRING;
        explicit_creds.connection_string = "explicit-connection-string";

        manager.setAzureCredentials(explicit_creds);

        REQUIRE(manager.hasAzureCredentials());
        auto creds = manager.getAzureCredentials();
        REQUIRE(creds->connection_string == "explicit-connection-string");
    }
}

// ============================================================================
// Global Credential Manager Tests
// ============================================================================

TEST_CASE("Global credential manager", "[vfs][credentials]") {
    SECTION("Returns same instance") {
        auto& manager1 = getGlobalCredentialManager();
        auto& manager2 = getGlobalCredentialManager();

        REQUIRE(&manager1 == &manager2);
    }
}

// ============================================================================
// Mixed Credentials Tests
// ============================================================================

TEST_CASE("CredentialManager with multiple providers", "[vfs][credentials]") {
    ScopedEnvVar aws_key("AWS_ACCESS_KEY_ID", "aws_key");
    ScopedEnvVar aws_secret("AWS_SECRET_ACCESS_KEY", "aws_secret");
    ScopedEnvVar gcs_creds("GOOGLE_APPLICATION_CREDENTIALS", "/gcs/key.json");
    ScopedEnvVar azure_conn("AZURE_STORAGE_CONNECTION_STRING", "conn_string");

    CredentialManager manager;
    manager.loadFromEnvironment();

    SECTION("All providers loaded") {
        REQUIRE(manager.hasS3Credentials());
        REQUIRE(manager.hasGCSCredentials());
        REQUIRE(manager.hasAzureCredentials());
    }

    SECTION("Log credential status does not throw") {
        REQUIRE_NOTHROW(manager.logCredentialStatus());
    }
}
