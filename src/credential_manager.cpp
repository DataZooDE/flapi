#include "credential_manager.hpp"
#include <crow/logging.h>
#include <cstdlib>

// Include DatabaseManager for DuckDB configuration
#include "database_manager.hpp"

namespace flapi {

// Environment variable names
namespace {

// The value DuckDB currently holds for a setting, or "" when it is unset (NULL).
std::string CurrentSetting(duckdb_connection conn, const std::string& name) {
    duckdb_result result;
    const std::string query = "SELECT value FROM duckdb_settings() WHERE name = '" + name + "'";
    std::string value;
    if (duckdb_query(conn, query.c_str(), &result) == DuckDBSuccess && duckdb_row_count(&result) > 0 &&
        !duckdb_value_is_null(&result, 0, 0)) {
        if (char* text = duckdb_value_varchar(&result, 0, 0)) {
            value = text;
            duckdb_free(text);
        }
    }
    duckdb_destroy_result(&result);
    return value;
}

// A setting the user chose (`duckdb.settings`, a connection init) wins over the
// environment: it is applied only when DuckDB still holds the built-in default.
bool ApplyUnlessUserSet(duckdb_connection conn, const std::string& name, const std::string& sql_value,
                        const std::string& builtin_default = "") {
    const std::string current = CurrentSetting(conn, name);
    if (!current.empty() && current != builtin_default) {
        CROW_LOG_DEBUG << "Keeping user-configured " << name << "; the environment value is not applied";
        return false;
    }
    const std::string query = "SET GLOBAL " + name + " = " + sql_value + ";";
    if (duckdb_query(conn, query.c_str(), nullptr) != DuckDBSuccess) {
        CROW_LOG_WARNING << "Failed to set " << name;
        return false;
    }
    return true;
}

// Credentials come from the environment; a quote in one must not end the literal.
std::string SqlLiteral(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        out += ch;
        if (ch == '\'') {
            out += '\'';
        }
    }
    return out;
}
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

    const bool ok = configureConnection(conn);
    duckdb_disconnect(&conn);
    return ok;
}

bool CredentialManager::configureConnection(duckdb_connection conn) {
    bool success = true;
    std::string errors;

    // Configure S3 if credentials are available
    if (_s3_credentials.has_value()) {
        const auto& creds = _s3_credentials.value();

        try {
            if (!creds.region.empty()) {
                ApplyUnlessUserSet(conn, "s3_region", "'" + SqlLiteral(creds.region) + "'");
            }
            // Credentials for non-instance-profile auth
            if (!creds.access_key_id.empty() && !creds.secret_access_key.empty()) {
                ApplyUnlessUserSet(conn, "s3_access_key_id", "'" + SqlLiteral(creds.access_key_id) + "'");
                ApplyUnlessUserSet(conn, "s3_secret_access_key", "'" + SqlLiteral(creds.secret_access_key) + "'");
                if (!creds.session_token.empty()) {
                    ApplyUnlessUserSet(conn, "s3_session_token", "'" + SqlLiteral(creds.session_token) + "'");
                }
            }
            if (!creds.endpoint.empty()) {
                // DuckDB's s3_endpoint is `host[:port]`; the URL scheme decides SSL.
                std::string host = creds.endpoint;
                bool use_ssl = creds.use_ssl;
                for (const std::string scheme : {"https://", "http://"}) {
                    if (host.rfind(scheme, 0) == 0) {
                        use_ssl = (scheme == "https://");
                        host.erase(0, scheme.size());
                        break;
                    }
                }
                while (!host.empty() && host.back() == '/') {
                    host.pop_back();
                }
                ApplyUnlessUserSet(conn, "s3_endpoint", "'" + SqlLiteral(host) + "'");
                // A custom endpoint (MinIO, a local fake, an S3-compatible store) is
                // addressed by path: `<bucket>.<host>` rarely resolves there.
                const bool real_aws = host.size() >= 13 && host.compare(host.size() - 13, 13, "amazonaws.com") == 0;
                if (!real_aws) {
                    ApplyUnlessUserSet(conn, "s3_url_style", "'path'", "vhost");
                }
                ApplyUnlessUserSet(conn, "s3_use_ssl", use_ssl ? "true" : "false", "true");
            }

            CROW_LOG_INFO << "S3 credentials configured in DuckDB";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "Error configuring S3 credentials: " << e.what();
            errors += "S3: " + std::string(e.what()) + "; ";
            success = false;
        }
    }

    // GCS: nothing is applied to DuckDB, and the log says so. DuckDB's httpfs
    // reads gs:// through Google's S3-interoperability API, which takes an
    // HMAC key pair (a DuckDB secret of TYPE gcs); it does not read a
    // service-account key file. GOOGLE_APPLICATION_CREDENTIALS therefore has
    // no effect on gs:// reads - and startup used to log "GCS credentials
    // configured", which sent operators looking everywhere but here.
    if (_gcs_credentials.has_value() && _gcs_referenced) {
        const auto& creds = _gcs_credentials.value();
        CROW_LOG_WARNING << "GCS: " << (creds.key_file.empty() ? "a Google Cloud project" : "GOOGLE_APPLICATION_CREDENTIALS")
                         << " is set, but DuckDB's httpfs does not use service-account credentials, "
                            "so gs:// access is NOT configured from it. Create an HMAC key and add "
                            "`CREATE SECRET (TYPE gcs, KEY_ID '...', SECRET '...')` to a connection's "
                            "`init` (with the values from {{env.NAME}}).";
    }

    // Configure Azure if credentials are available
    if (_azure_credentials.has_value()) {
        const auto& creds = _azure_credentials.value();

        try {
            // Azure credentials can be configured via connection string
            if (!creds.connection_string.empty()) {
                std::string query = "SET GLOBAL azure_storage_connection_string = '" +
                                    SqlLiteral(creds.connection_string) + "';";
                duckdb_query(conn, query.c_str(), nullptr);
            } else if (!creds.account_name.empty() && !creds.account_key.empty()) {
                std::string query = "SET GLOBAL azure_account_name = '" + SqlLiteral(creds.account_name) + "';";
                duckdb_query(conn, query.c_str(), nullptr);

                query = "SET GLOBAL azure_account_key = '" + SqlLiteral(creds.account_key) + "';";
                duckdb_query(conn, query.c_str(), nullptr);
            }

            CROW_LOG_INFO << "Azure credentials configured in DuckDB";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "Error configuring Azure credentials: " << e.what();
            errors += "Azure: " + std::string(e.what()) + "; ";
            success = false;
        }
    }

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
        CROW_LOG_INFO << "  GCS: detected in the environment, NOT applied to DuckDB (type: "
                      << credentialTypeToString(creds.type)
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
