#pragma once

#include <string>
#include <optional>
#include <memory>

namespace flapi {

/**
 * Credential type for cloud providers.
 */
enum class CredentialType {
    NONE,               // No credentials configured
    ENVIRONMENT,        // Use environment variables
    SECRET,             // Use DuckDB Secrets Manager
    INSTANCE_PROFILE,   // Use cloud instance profile (AWS IAM roles)
    SERVICE_ACCOUNT,    // Use service account (GCP)
    CONNECTION_STRING,  // Use connection string (Azure)
    MANAGED_IDENTITY    // Use managed identity (Azure)
};

/**
 * S3 credential configuration.
 */
struct S3Credentials {
    CredentialType type = CredentialType::ENVIRONMENT;
    std::string region;
    std::string access_key_id;      // Only for SECRET type
    std::string secret_access_key;  // Only for SECRET type
    std::string session_token;      // Optional, for temporary credentials
    std::string endpoint;           // Optional, for S3-compatible endpoints
    bool use_ssl = true;
};

/**
 * GCS credential configuration.
 */
struct GCSCredentials {
    CredentialType type = CredentialType::ENVIRONMENT;
    std::string project_id;
    std::string key_file;           // Path to service account key file
};

/**
 * Azure Blob Storage credential configuration.
 */
struct AzureCredentials {
    CredentialType type = CredentialType::CONNECTION_STRING;
    std::string account_name;
    std::string connection_string;  // For CONNECTION_STRING type
    std::string account_key;        // For direct key access
    std::string tenant_id;          // For managed identity
    std::string client_id;          // For service principal
};

/**
 * Credential Manager for cloud storage providers.
 *
 * This class handles credential loading, validation, and configuration
 * for S3, GCS, and Azure storage backends. Credentials can be loaded from:
 * - Environment variables (default for S3, GCS)
 * - DuckDB Secrets Manager
 * - Instance profiles (AWS IAM roles)
 * - Service accounts (GCP)
 * - Connection strings or managed identity (Azure)
 *
 * Usage:
 *   CredentialManager manager;
 *   manager.loadFromEnvironment();
 *   auto s3_creds = manager.getS3Credentials();
 */
class CredentialManager {
public:
    CredentialManager() = default;
    ~CredentialManager() = default;

    /**
     * Load all credentials from environment variables.
     * This is the simplest and most common approach.
     *
     * Environment variables checked:
     * - S3: AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_REGION, AWS_SESSION_TOKEN
     * - GCS: GOOGLE_APPLICATION_CREDENTIALS, GOOGLE_CLOUD_PROJECT
     * - Azure: AZURE_STORAGE_CONNECTION_STRING, AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_KEY
     */
    void loadFromEnvironment();

    /**
     * Configure S3 credentials explicitly.
     */
    void setS3Credentials(const S3Credentials& creds);

    /**
     * Configure GCS credentials explicitly.
     */
    void setGCSCredentials(const GCSCredentials& creds);

    /**
     * Configure Azure credentials explicitly.
     */
    void setAzureCredentials(const AzureCredentials& creds);

    /**
     * Get configured S3 credentials.
     * @return S3Credentials if configured, std::nullopt otherwise
     */
    std::optional<S3Credentials> getS3Credentials() const;

    /**
     * Get configured GCS credentials.
     * @return GCSCredentials if configured, std::nullopt otherwise
     */
    std::optional<GCSCredentials> getGCSCredentials() const;

    /**
     * Get configured Azure credentials.
     * @return AzureCredentials if configured, std::nullopt otherwise
     */
    std::optional<AzureCredentials> getAzureCredentials() const;

    /**
     * Check if S3 credentials are configured.
     */
    bool hasS3Credentials() const;

    /**
     * Check if GCS credentials are configured.
     */
    bool hasGCSCredentials() const;

    /**
     * Check if Azure credentials are configured.
     */
    bool hasAzureCredentials() const;

    /**
     * Configure DuckDB with the loaded credentials.
     * This sets up the httpfs extension with appropriate credentials.
     *
     * @return true if configuration was successful
     */
    bool configureDuckDB();

    /**
     * Get credential type as string for logging.
     */
    static std::string credentialTypeToString(CredentialType type);

    /**
     * Log credential status (without revealing secrets).
     */
    void logCredentialStatus() const;

private:
    std::optional<S3Credentials> _s3_credentials;
    std::optional<GCSCredentials> _gcs_credentials;
    std::optional<AzureCredentials> _azure_credentials;

    /**
     * Get environment variable value.
     */
    static std::string getEnv(const std::string& name);

    /**
     * Check if environment variable is set.
     */
    static bool hasEnv(const std::string& name);
};

/**
 * Global credential manager instance.
 * Initialize once at startup and reuse throughout the application.
 */
CredentialManager& getGlobalCredentialManager();

} // namespace flapi
