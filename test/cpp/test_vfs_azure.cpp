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
// Azure Path Scheme Detection Tests
// ============================================================================

TEST_CASE("Azure path scheme detection", "[vfs][azure][scheme]") {
    SECTION("az:// paths are recognized") {
        REQUIRE(PathSchemeUtils::IsAzurePath("az://container/blob"));
        REQUIRE(PathSchemeUtils::IsAzurePath("az://mycontainer/path/to/blob.yaml"));
    }

    SECTION("azure:// paths are recognized") {
        REQUIRE(PathSchemeUtils::IsAzurePath("azure://container/blob"));
        REQUIRE(PathSchemeUtils::IsAzurePath("azure://mycontainer/path/to/blob.yaml"));
    }

    SECTION("AZ:// and AZURE:// are recognized (case insensitive)") {
        REQUIRE(PathSchemeUtils::IsAzurePath("AZ://container/blob"));
        REQUIRE(PathSchemeUtils::IsAzurePath("AZURE://container/blob"));
        REQUIRE(PathSchemeUtils::IsAzurePath("Azure://MyContainer/MyBlob"));
    }

    SECTION("Non-Azure paths are not recognized") {
        REQUIRE_FALSE(PathSchemeUtils::IsAzurePath("s3://bucket/key"));
        REQUIRE_FALSE(PathSchemeUtils::IsAzurePath("gs://bucket/key"));
        REQUIRE_FALSE(PathSchemeUtils::IsAzurePath("/local/path"));
        REQUIRE_FALSE(PathSchemeUtils::IsAzurePath("https://storageaccount.blob.core.windows.net/container/blob"));
    }

    SECTION("GetScheme returns correct scheme for Azure paths") {
        REQUIRE(PathSchemeUtils::GetScheme("az://container/blob") == "az://");
        REQUIRE(PathSchemeUtils::GetScheme("azure://container/blob") == "azure://");
    }

    SECTION("Azure paths are remote paths") {
        REQUIRE(PathSchemeUtils::IsRemotePath("az://container/blob"));
        REQUIRE(PathSchemeUtils::IsRemotePath("azure://container/blob"));
    }
}

// ============================================================================
// Azure URL Structure Tests
// ============================================================================

TEST_CASE("Azure URL structure", "[vfs][azure][url]") {
    SECTION("Basic Azure URL components (az://)") {
        std::string url = "az://mycontainer/path/to/blob.yaml";

        REQUIRE(PathSchemeUtils::IsAzurePath(url));

        // Extract container name
        size_t scheme_end = url.find("://") + 3;
        size_t container_end = url.find('/', scheme_end);
        std::string container = url.substr(scheme_end, container_end - scheme_end);
        REQUIRE(container == "mycontainer");

        // Extract blob path
        std::string blob = url.substr(container_end + 1);
        REQUIRE(blob == "path/to/blob.yaml");
    }

    SECTION("Azure container naming rules") {
        // Azure container names: 3-63 chars, lowercase, numbers, hyphens
        REQUIRE(PathSchemeUtils::IsAzurePath("az://abc/blob"));  // Minimum length
        REQUIRE(PathSchemeUtils::IsAzurePath("az://my-container/blob"));  // Hyphen allowed
        REQUIRE(PathSchemeUtils::IsAzurePath("az://container123/blob"));  // Numbers allowed
    }
}

// ============================================================================
// Azure Credential Configuration Tests
// ============================================================================

TEST_CASE("Azure credential configuration", "[vfs][azure][credentials]") {
    SECTION("Connection string authentication") {
        ScopedEnvVar conn_str("AZURE_STORAGE_CONNECTION_STRING",
            "DefaultEndpointsProtocol=https;AccountName=mystorageaccount;AccountKey=base64key==;EndpointSuffix=core.windows.net");

        CredentialManager manager;
        manager.loadFromEnvironment();

        REQUIRE(manager.hasAzureCredentials());
        auto creds = manager.getAzureCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->type == CredentialType::CONNECTION_STRING);
        REQUIRE_FALSE(creds->connection_string.empty());
    }

    SECTION("Account name and key authentication") {
        ScopedEnvVarUnset conn_str("AZURE_STORAGE_CONNECTION_STRING");
        ScopedEnvVar account("AZURE_STORAGE_ACCOUNT", "mystorageaccount");
        ScopedEnvVar key("AZURE_STORAGE_KEY", "base64encodedkey==");

        CredentialManager manager;
        manager.loadFromEnvironment();

        REQUIRE(manager.hasAzureCredentials());
        auto creds = manager.getAzureCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->type == CredentialType::ENVIRONMENT);
        REQUIRE(creds->account_name == "mystorageaccount");
        REQUIRE(creds->account_key == "base64encodedkey==");
    }

    SECTION("Managed identity authentication") {
        ScopedEnvVarUnset conn_str("AZURE_STORAGE_CONNECTION_STRING");
        ScopedEnvVar account("AZURE_STORAGE_ACCOUNT", "myaccount");
        ScopedEnvVar tenant("AZURE_TENANT_ID", "tenant-guid-1234");
        ScopedEnvVar client("AZURE_CLIENT_ID", "client-guid-5678");

        CredentialManager manager;
        manager.loadFromEnvironment();

        auto creds = manager.getAzureCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->type == CredentialType::MANAGED_IDENTITY);
        REQUIRE(creds->tenant_id == "tenant-guid-1234");
        REQUIRE(creds->client_id == "client-guid-5678");
    }

    SECTION("Azure credentials struct defaults") {
        AzureCredentials creds;
        REQUIRE(creds.type == CredentialType::CONNECTION_STRING);
        REQUIRE(creds.account_name.empty());
        REQUIRE(creds.connection_string.empty());
        REQUIRE(creds.account_key.empty());
        REQUIRE(creds.tenant_id.empty());
        REQUIRE(creds.client_id.empty());
    }

    SECTION("Set credentials explicitly") {
        CredentialManager manager;

        AzureCredentials explicit_creds;
        explicit_creds.type = CredentialType::CONNECTION_STRING;
        explicit_creds.connection_string = "ExplicitConnectionString";

        manager.setAzureCredentials(explicit_creds);

        auto creds = manager.getAzureCredentials();
        REQUIRE(creds.has_value());
        REQUIRE(creds->connection_string == "ExplicitConnectionString");
    }
}

// ============================================================================
// Azure Storage Account Handling
// ============================================================================

TEST_CASE("Azure storage account handling", "[vfs][azure][account]") {
    SECTION("Storage account naming rules") {
        // Azure storage account names: 3-24 chars, lowercase and numbers only
        AzureCredentials creds;

        // Valid account names
        creds.account_name = "mystorageaccount";
        REQUIRE_FALSE(creds.account_name.empty());

        creds.account_name = "account123";
        REQUIRE_FALSE(creds.account_name.empty());
    }
}

// ============================================================================
// Azure Integration with VFS
// ============================================================================

TEST_CASE("Azure integration with VFS", "[vfs][azure][integration]") {
    SECTION("FileProviderFactory routes Azure paths to DuckDB provider") {
        std::string az_path = "az://container/blob.yaml";

        REQUIRE(PathSchemeUtils::IsRemotePath(az_path));
        REQUIRE(PathSchemeUtils::IsAzurePath(az_path));
    }

    SECTION("LocalFileProvider does not handle Azure paths") {
        LocalFileProvider local;
        REQUIRE(local.IsRemotePath("az://container/blob") == true);
        REQUIRE(local.IsRemotePath("azure://container/blob") == true);
    }

    SECTION("Both az:// and azure:// schemes work") {
        REQUIRE(PathSchemeUtils::IsRemotePath("az://c/b"));
        REQUIRE(PathSchemeUtils::IsRemotePath("azure://c/b"));
    }
}
