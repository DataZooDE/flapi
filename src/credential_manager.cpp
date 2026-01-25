#include "credential_manager.hpp"
#include <crow/logging.h>
#include <cstdlib>

// Include DatabaseManager for DuckDB configuration
#include "database_manager.hpp"

namespace flapi {

// Environment variable names
namespace {
    // S3 / AWS
    constexpr const char* AWS_ACCESS_KEY_ID = "AWS_ACCESS_KEY_ID";
    constexpr const char* AWS_SECRET_ACCESS_KEY = "AWS_SECRET_ACCESS_KEY";
    constexpr const char* AWS_REGION = "AWS_REGION";
    constexpr const char* AWS_DEFAULT_REGION = "AWS_DEFAULT_REGION";
    constexpr const char* AWS_SESSION_TOKEN = "AWS_SESSION_TOKEN";
    constexpr const char* AWS_ENDPOINT_URL = "AWS_ENDPOINT_URL";

    // GCS / Google Cloud
    constexpr const char* GOOGLE_APPLICATION_CREDENTIALS = "GOOGLE_APPLICATION_CREDENTIALS";
    constexpr const char* GOOGLE_CLOUD_PROJECT = "GOOGLE_CLOUD_PROJECT";
    constexpr const char* GCLOUD_PROJECT = "GCLOUD_PROJECT";
    constexpr const char* GCP_PROJECT = "GCP_PROJECT";

    // Azure
    constexpr const char* AZURE_STORAGE_CONNECTION_STRING = "AZURE_STORAGE_CONNECTION_STRING";
    constexpr const char* AZURE_STORAGE_ACCOUNT = "AZURE_STORAGE_ACCOUNT";
    constexpr const char* AZURE_STORAGE_KEY = "AZURE_STORAGE_KEY";
    constexpr const char* AZURE_TENANT_ID = "AZURE_TENANT_ID";
    constexpr const char* AZURE_CLIENT_ID = "AZURE_CLIENT_ID";
}

std::string CredentialManager::getEnv(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    return value ? std::string(value) : "";
}

bool CredentialManager::hasEnv(const std::string& name) {
    return std::getenv(name.c_str()) != nullptr;
}

std::string CredentialManager::credentialTypeToString(CredentialType type) {
    switch (type) {
        case CredentialType::NONE:
            return "none";
        case CredentialType::ENVIRONMENT:
            return "environment";
        case CredentialType::SECRET:
            return "secret";
        case CredentialType::INSTANCE_PROFILE:
            return "instance_profile";
        case CredentialType::SERVICE_ACCOUNT:
            return "service_account";
        case CredentialType::CONNECTION_STRING:
            return "connection_string";
        case CredentialType::MANAGED_IDENTITY:
            return "managed_identity";
        default:
            return "unknown";
    }
}

void CredentialManager::loadFromEnvironment() {
    CROW_LOG_DEBUG << "Loading cloud credentials from environment variables";

    // Load S3/AWS credentials
    if (hasEnv(AWS_ACCESS_KEY_ID) || hasEnv(AWS_SECRET_ACCESS_KEY) || hasEnv(AWS_REGION)) {
        S3Credentials s3;
        s3.type = CredentialType::ENVIRONMENT;
        s3.access_key_id = getEnv(AWS_ACCESS_KEY_ID);
        s3.secret_access_key = getEnv(AWS_SECRET_ACCESS_KEY);
        s3.region = getEnv(AWS_REGION);
        if (s3.region.empty()) {
            s3.region = getEnv(AWS_DEFAULT_REGION);
        }
        s3.session_token = getEnv(AWS_SESSION_TOKEN);
        s3.endpoint = getEnv(AWS_ENDPOINT_URL);

        _s3_credentials = s3;
        CROW_LOG_DEBUG << "S3 credentials loaded from environment (region: "
                       << (s3.region.empty() ? "default" : s3.region) << ")";
    }

    // Load GCS credentials
    if (hasEnv(GOOGLE_APPLICATION_CREDENTIALS) || hasEnv(GOOGLE_CLOUD_PROJECT)) {
        GCSCredentials gcs;
        gcs.type = CredentialType::ENVIRONMENT;
        gcs.key_file = getEnv(GOOGLE_APPLICATION_CREDENTIALS);

        // Try multiple project ID environment variables
        gcs.project_id = getEnv(GOOGLE_CLOUD_PROJECT);
        if (gcs.project_id.empty()) {
            gcs.project_id = getEnv(GCLOUD_PROJECT);
        }
        if (gcs.project_id.empty()) {
            gcs.project_id = getEnv(GCP_PROJECT);
        }

        _gcs_credentials = gcs;
        CROW_LOG_DEBUG << "GCS credentials loaded from environment (key_file: "
                       << (gcs.key_file.empty() ? "not set" : "set")
                       << ", project: " << (gcs.project_id.empty() ? "not set" : gcs.project_id) << ")";
    }

    // Load Azure credentials
    if (hasEnv(AZURE_STORAGE_CONNECTION_STRING) || hasEnv(AZURE_STORAGE_ACCOUNT)) {
        AzureCredentials azure;

        // Check for connection string first (simplest)
        if (hasEnv(AZURE_STORAGE_CONNECTION_STRING)) {
            azure.type = CredentialType::CONNECTION_STRING;
            azure.connection_string = getEnv(AZURE_STORAGE_CONNECTION_STRING);
        } else if (hasEnv(AZURE_TENANT_ID) && hasEnv(AZURE_CLIENT_ID)) {
            // Managed identity / service principal
            azure.type = CredentialType::MANAGED_IDENTITY;
            azure.tenant_id = getEnv(AZURE_TENANT_ID);
            azure.client_id = getEnv(AZURE_CLIENT_ID);
            azure.account_name = getEnv(AZURE_STORAGE_ACCOUNT);
        } else {
            // Direct key access
            azure.type = CredentialType::ENVIRONMENT;
            azure.account_name = getEnv(AZURE_STORAGE_ACCOUNT);
            azure.account_key = getEnv(AZURE_STORAGE_KEY);
        }

        _azure_credentials = azure;
        CROW_LOG_DEBUG << "Azure credentials loaded from environment (type: "
                       << credentialTypeToString(azure.type) << ")";
    }
}

void CredentialManager::setS3Credentials(const S3Credentials& creds) {
    _s3_credentials = creds;
}

void CredentialManager::setGCSCredentials(const GCSCredentials& creds) {
    _gcs_credentials = creds;
}

void CredentialManager::setAzureCredentials(const AzureCredentials& creds) {
    _azure_credentials = creds;
}

std::optional<S3Credentials> CredentialManager::getS3Credentials() const {
    return _s3_credentials;
}

std::optional<GCSCredentials> CredentialManager::getGCSCredentials() const {
    return _gcs_credentials;
}

std::optional<AzureCredentials> CredentialManager::getAzureCredentials() const {
    return _azure_credentials;
}

bool CredentialManager::hasS3Credentials() const {
    return _s3_credentials.has_value();
}

bool CredentialManager::hasGCSCredentials() const {
    return _gcs_credentials.has_value();
}

bool CredentialManager::hasAzureCredentials() const {
    return _azure_credentials.has_value();
}

bool CredentialManager::configureDuckDB() {
    auto db_manager = DatabaseManager::getInstance();
    if (!db_manager) {
        CROW_LOG_WARNING << "CredentialManager::configureDuckDB: DatabaseManager not initialized";
        return false;
    }

    auto conn = db_manager->getConnection();
    if (!conn) {
        CROW_LOG_WARNING << "CredentialManager::configureDuckDB: Could not get DuckDB connection";
        return false;
    }

    bool success = true;
    std::string errors;

    // Configure S3 if credentials are available
    if (_s3_credentials.has_value()) {
        const auto& creds = _s3_credentials.value();

        try {
            // Set S3 region
            if (!creds.region.empty()) {
                std::string query = "SET s3_region = '" + creds.region + "';";
                auto result = duckdb_query(conn, query.c_str(), nullptr);
                if (result != DuckDBSuccess) {
                    CROW_LOG_WARNING << "Failed to set s3_region";
                }
            }

            // Set S3 credentials if provided (for non-instance-profile auth)
            if (!creds.access_key_id.empty() && !creds.secret_access_key.empty()) {
                std::string query = "SET s3_access_key_id = '" + creds.access_key_id + "';";
                duckdb_query(conn, query.c_str(), nullptr);

                query = "SET s3_secret_access_key = '" + creds.secret_access_key + "';";
                duckdb_query(conn, query.c_str(), nullptr);

                if (!creds.session_token.empty()) {
                    query = "SET s3_session_token = '" + creds.session_token + "';";
                    duckdb_query(conn, query.c_str(), nullptr);
                }
            }

            // Set custom endpoint if provided
            if (!creds.endpoint.empty()) {
                std::string query = "SET s3_endpoint = '" + creds.endpoint + "';";
                duckdb_query(conn, query.c_str(), nullptr);

                // If custom endpoint, typically disable SSL verification for local testing
                if (!creds.use_ssl) {
                    query = "SET s3_use_ssl = false;";
                    duckdb_query(conn, query.c_str(), nullptr);
                }
            }

            CROW_LOG_INFO << "S3 credentials configured in DuckDB";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "Error configuring S3 credentials: " << e.what();
            errors += "S3: " + std::string(e.what()) + "; ";
            success = false;
        }
    }

    // Configure GCS if credentials are available
    if (_gcs_credentials.has_value()) {
        const auto& creds = _gcs_credentials.value();

        try {
            // GCS credentials are typically handled via GOOGLE_APPLICATION_CREDENTIALS
            // DuckDB's httpfs extension will automatically use these
            // But we can set project ID if available
            if (!creds.project_id.empty()) {
                // Note: DuckDB may not have a direct setting for GCS project
                // The project is typically inferred from credentials
                CROW_LOG_DEBUG << "GCS project ID: " << creds.project_id;
            }

            CROW_LOG_INFO << "GCS credentials configured (using environment)";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "Error configuring GCS credentials: " << e.what();
            errors += "GCS: " + std::string(e.what()) + "; ";
            success = false;
        }
    }

    // Configure Azure if credentials are available
    if (_azure_credentials.has_value()) {
        const auto& creds = _azure_credentials.value();

        try {
            // Azure credentials can be configured via connection string
            if (!creds.connection_string.empty()) {
                std::string query = "SET azure_storage_connection_string = '" +
                                    creds.connection_string + "';";
                duckdb_query(conn, query.c_str(), nullptr);
            } else if (!creds.account_name.empty() && !creds.account_key.empty()) {
                std::string query = "SET azure_account_name = '" + creds.account_name + "';";
                duckdb_query(conn, query.c_str(), nullptr);

                query = "SET azure_account_key = '" + creds.account_key + "';";
                duckdb_query(conn, query.c_str(), nullptr);
            }

            CROW_LOG_INFO << "Azure credentials configured in DuckDB";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "Error configuring Azure credentials: " << e.what();
            errors += "Azure: " + std::string(e.what()) + "; ";
            success = false;
        }
    }

    duckdb_disconnect(&conn);

    if (!success) {
        CROW_LOG_WARNING << "Some credentials failed to configure: " << errors;
    }

    return success;
}

void CredentialManager::logCredentialStatus() const {
    CROW_LOG_INFO << "Credential Manager Status:";

    if (_s3_credentials.has_value()) {
        const auto& creds = _s3_credentials.value();
        CROW_LOG_INFO << "  S3: configured (type: " << credentialTypeToString(creds.type)
                      << ", region: " << (creds.region.empty() ? "default" : creds.region)
                      << ", access_key: " << (creds.access_key_id.empty() ? "not set" : "****")
                      << ")";
    } else {
        CROW_LOG_INFO << "  S3: not configured";
    }

    if (_gcs_credentials.has_value()) {
        const auto& creds = _gcs_credentials.value();
        CROW_LOG_INFO << "  GCS: configured (type: " << credentialTypeToString(creds.type)
                      << ", key_file: " << (creds.key_file.empty() ? "not set" : "****")
                      << ", project: " << (creds.project_id.empty() ? "not set" : creds.project_id)
                      << ")";
    } else {
        CROW_LOG_INFO << "  GCS: not configured";
    }

    if (_azure_credentials.has_value()) {
        const auto& creds = _azure_credentials.value();
        CROW_LOG_INFO << "  Azure: configured (type: " << credentialTypeToString(creds.type)
                      << ", account: " << (creds.account_name.empty() ? "not set" : creds.account_name)
                      << ")";
    } else {
        CROW_LOG_INFO << "  Azure: not configured";
    }
}

// Global credential manager instance
static CredentialManager global_credential_manager;

CredentialManager& getGlobalCredentialManager() {
    return global_credential_manager;
}

} // namespace flapi
