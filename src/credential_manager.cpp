#include "credential_manager.hpp"
#include "extension_installer.hpp"
#include <algorithm>
#include <crow/logging.h>
#include <cstdlib>

// Include DatabaseManager for DuckDB configuration
#include "database_manager.hpp"

namespace flapi {

// Environment variable names
namespace {

bool HasPrefixNoCase(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), text.begin(),
                      [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
}

bool ExtensionsOffline() {
    return ExtensionsOfflineRequested();
}

// INSTALL (unless offline) and LOAD an extension; false with a log line when it is not available.
bool EnsureExtension(duckdb_connection conn, const std::string& name) {
    for (const std::string verb : {"INSTALL ", "LOAD "}) {
        if (verb == "INSTALL " && ExtensionsOffline()) {
            continue;
        }
        duckdb_result result;
        if (duckdb_query(conn, (verb + name).c_str(), &result) != DuckDBSuccess) {
            CROW_LOG_WARNING << verb << name << " failed: " << duckdb_result_error(&result)
                             << (ExtensionsOffline() ? " (offline: pre-install it into FLAPI_EXTENSION_DIRECTORY)" : "");
            duckdb_destroy_result(&result);
            return false;
        }
        duckdb_destroy_result(&result);
    }
    return true;
}

bool RunStatement(duckdb_connection conn, const std::string& sql, const char* what) {
    duckdb_result result;
    const bool ok = duckdb_query(conn, sql.c_str(), &result) == DuckDBSuccess;
    if (!ok) {
        CROW_LOG_WARNING << "Failed to create the " << what << " secret: " << duckdb_result_error(&result);
    }
    duckdb_destroy_result(&result);
    return ok;
}

// The value DuckDB currently holds for a setting, or "" when it is unset (NULL).
std::string CurrentSetting(duckdb_connection conn, const std::string& name) {
    duckdb_result result;
    // current_setting(), not duckdb_settings(): DuckDB 2.0 no longer lists extension settings such as
    // httpfs's s3_* there. An unknown setting is an error, which reads as "not set".
    const std::string query = "SELECT current_setting('" + name + "')::VARCHAR";
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

// What the effective value of an `s3_*` setting is: one the user configured (`duckdb:` settings, a
// connection init) wins over the environment's value.
std::string UserOrEnv(duckdb_connection conn, const std::string& setting, const std::string& env_value,
                      const std::string& builtin_default = "") {
    const std::string current = CurrentSetting(conn, setting);
    if (!current.empty() && current != builtin_default) {
        CROW_LOG_DEBUG << "Keeping user-configured " << setting << "; the environment value is not used";
        return current;
    }
    return env_value;
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
    constexpr const char* GCS_ACCESS_KEY_ID = "GCS_ACCESS_KEY_ID";
    constexpr const char* GCS_SECRET_ACCESS_KEY = "GCS_SECRET_ACCESS_KEY";
    constexpr const char* GS_ACCESS_KEY_ID = "GS_ACCESS_KEY_ID";            // boto / gsutil spelling
    constexpr const char* GS_SECRET_ACCESS_KEY = "GS_SECRET_ACCESS_KEY";
    constexpr const char* GCS_ENDPOINT_URL = "GCS_ENDPOINT_URL";
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
    if (hasEnv(GOOGLE_APPLICATION_CREDENTIALS) || hasEnv(GOOGLE_CLOUD_PROJECT) || hasEnv(GCS_ACCESS_KEY_ID) ||
        hasEnv(GS_ACCESS_KEY_ID)) {
        GCSCredentials gcs;
        gcs.type = CredentialType::ENVIRONMENT;
        gcs.key_file = getEnv(GOOGLE_APPLICATION_CREDENTIALS);
        gcs.hmac_key_id = hasEnv(GCS_ACCESS_KEY_ID) ? getEnv(GCS_ACCESS_KEY_ID) : getEnv(GS_ACCESS_KEY_ID);
        gcs.hmac_secret = hasEnv(GCS_SECRET_ACCESS_KEY) ? getEnv(GCS_SECRET_ACCESS_KEY) : getEnv(GS_SECRET_ACCESS_KEY);
        gcs.endpoint = getEnv(GCS_ENDPOINT_URL);

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
            // The credentials go into a DuckDB SECRET, not `SET GLOBAL s3_secret_access_key`: a setting
            // can be read back with `SELECT * FROM duckdb_settings()` by any SQL template, a secret
            // is redacted. A value the user configured themselves wins over the environment.
            std::string host = creds.endpoint;
            bool use_ssl = creds.use_ssl;
            for (const std::string scheme : {"https://", "http://"}) {
                if (HasPrefixNoCase(host, scheme)) {
                    use_ssl = (scheme == "https://");
                    host.erase(0, scheme.size());
                    break;
                }
            }
            while (!host.empty() && host.back() == '/') {
                host.pop_back();
            }
            const std::string region = UserOrEnv(conn, "s3_region", creds.region);
            const bool have_keys = !creds.access_key_id.empty() && !creds.secret_access_key.empty();
            if (!have_keys && host.empty()) {
                // Instance profile / credential chain, or only a region: nothing secret to hide.
                if (!region.empty() && CurrentSetting(conn, "s3_region").empty()) {
                    RunStatement(conn, "SET GLOBAL s3_region = '" + SqlLiteral(region) + "';", "S3 region");
                }
            } else {
                std::string sql = "CREATE OR REPLACE SECRET zz_flapi_s3 (TYPE s3";
                if (have_keys) {
                    sql += ", KEY_ID '" + SqlLiteral(UserOrEnv(conn, "s3_access_key_id", creds.access_key_id)) +
                           "', SECRET '" + SqlLiteral(UserOrEnv(conn, "s3_secret_access_key", creds.secret_access_key)) + "'";
                    if (!creds.session_token.empty()) {
                        sql += ", SESSION_TOKEN '" + SqlLiteral(UserOrEnv(conn, "s3_session_token", creds.session_token)) + "'";
                    }
                }
                if (!region.empty()) {
                    sql += ", REGION '" + SqlLiteral(region) + "'";
                }
                const std::string endpoint = UserOrEnv(conn, "s3_endpoint", host);
                if (!endpoint.empty()) {
                    sql += ", ENDPOINT '" + SqlLiteral(endpoint) + "'";
                    // A custom endpoint (MinIO, a local fake, an S3-compatible store) is addressed by path:
                    // `<bucket>.<host>` rarely resolves there.
                    const bool real_aws = endpoint.find("amazonaws.com") != std::string::npos;  // incl. .com.cn
                    const std::string style = UserOrEnv(conn, "s3_url_style", real_aws ? "" : "path", "vhost");
                    if (!style.empty()) {
                        sql += ", URL_STYLE '" + SqlLiteral(style) + "'";
                    }
                    const std::string ssl = UserOrEnv(conn, "s3_use_ssl", use_ssl ? "true" : "false", "true");
                    sql += ", USE_SSL " + std::string(ssl == "false" ? "false" : "true");
                }
                sql += ")";
                // httpfs copies AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN from the
                // environment into its legacy s3_* settings when it loads, where any SQL template can
                // read them back. Load it now (a later LOAD is then a no-op), create the secret, and
                // blank the settings that only mirror the environment.
                const bool httpfs_ready = EnsureExtension(conn, "httpfs");
                if (!RunStatement(conn, sql, "S3")) {
                    success = false;
                    errors += "S3: secret could not be created; ";
                } else if (httpfs_ready) {
                    const std::pair<const char*, const std::string*> mirrored[] = {
                        {"s3_access_key_id", &creds.access_key_id},
                        {"s3_secret_access_key", &creds.secret_access_key},
                        {"s3_session_token", &creds.session_token}};
                    for (const auto& [name, env_value] : mirrored) {
                        if (!env_value->empty() && CurrentSetting(conn, name) == *env_value) {
                            RunStatement(conn, std::string("SET GLOBAL ") + name + " = '';", "legacy S3 setting");
                        }
                    }
                }
            }

            CROW_LOG_INFO << "S3 credentials configured in DuckDB";
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "Error configuring S3 credentials: " << e.what();
            errors += "S3: " + std::string(e.what()) + "; ";
            success = false;
        }
    }

    // GCS: DuckDB's httpfs reads gs:// through Google's S3-interoperability API with an
    // HMAC key pair (a secret of TYPE gcs). A service-account key file is not usable.
    if (_gcs_credentials.has_value() && !_gcs_credentials->hmac_key_id.empty() &&
        !_gcs_credentials->hmac_secret.empty()) {
        const auto& creds = _gcs_credentials.value();
        std::string sql = "CREATE OR REPLACE SECRET zz_flapi_gcs (TYPE gcs, KEY_ID '" + SqlLiteral(creds.hmac_key_id) +
                          "', SECRET '" + SqlLiteral(creds.hmac_secret) + "'";
        if (!creds.endpoint.empty()) {
            std::string host = creds.endpoint;
            bool use_ssl = true;
            for (const std::string scheme : {"https://", "http://"}) {
                if (HasPrefixNoCase(host, scheme)) {
                    use_ssl = (scheme == "https://");
                    host.erase(0, scheme.size());
                    break;
                }
            }
            while (!host.empty() && host.back() == '/') {
                host.pop_back();
            }
            sql += ", ENDPOINT '" + SqlLiteral(host) + "', URL_STYLE 'path', USE_SSL " + (use_ssl ? "true" : "false");
        }
        sql += ")";
        if (RunStatement(conn, sql, "GCS")) {
            CROW_LOG_INFO << "GCS HMAC credentials configured in DuckDB";
        } else {
            success = false;
            errors += "GCS: secret could not be created; ";
        }
    } else if (_gcs_credentials.has_value() && _gcs_referenced) {
        const auto& creds = _gcs_credentials.value();
        CROW_LOG_WARNING << "GCS: " << (creds.key_file.empty() ? "a Google Cloud project" : "GOOGLE_APPLICATION_CREDENTIALS")
                         << " is set, but DuckDB's httpfs does not use service-account credentials, "
                            "so gs:// access is NOT configured from it. Create an HMAC key and set "
                            "GCS_ACCESS_KEY_ID / GCS_SECRET_ACCESS_KEY (or add "
                            "`CREATE SECRET (TYPE gcs, KEY_ID '...', SECRET '...')` to a connection's `init`).";
    }

    // Azure: only when something reads az:// (see setAzureReferenced / the bootstrap).
    if (_azure_credentials.has_value() && _azure_referenced) {
        if (!configureAzure(conn)) {
            success = false;
            errors += "Azure: extension or secret unavailable; ";
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

bool CredentialManager::configureAzure(duckdb_connection conn) {
    if (!_azure_credentials.has_value()) {
        return true;
    }
    const auto& creds = _azure_credentials.value();
    // DuckDB reads az:// / azure:// through the `azure` extension and a secret; the legacy
    // azure_* settings are not used for that.
    std::string sql;
    if (!creds.connection_string.empty()) {
        sql = "CREATE OR REPLACE SECRET zz_flapi_azure (TYPE azure, CONNECTION_STRING '" +
              SqlLiteral(creds.connection_string) + "')";
    } else if (!creds.account_name.empty() && !creds.account_key.empty()) {
        sql = "CREATE OR REPLACE SECRET zz_flapi_azure (TYPE azure, CONNECTION_STRING '" +
              SqlLiteral("DefaultEndpointsProtocol=https;AccountName=" + creds.account_name +
                         ";AccountKey=" + creds.account_key + ";EndpointSuffix=core.windows.net") + "')";
    } else if (!creds.account_name.empty()) {
        sql = "CREATE OR REPLACE SECRET zz_flapi_azure (TYPE azure, PROVIDER credential_chain, ACCOUNT_NAME '" +
              SqlLiteral(creds.account_name) + "')";
    }
    if (sql.empty()) {
        CROW_LOG_WARNING << "Azure: AZURE_STORAGE_* is set but holds no usable credentials "
                            "(connection string, account + key, or account for the credential chain).";
        return false;
    }
    if (!EnsureExtension(conn, "azure") || !RunStatement(conn, sql, "Azure")) {
        return false;
    }
    CROW_LOG_INFO << "Azure credentials configured in DuckDB";
    return true;
}

CredentialManager& getGlobalCredentialManager() {
    return global_credential_manager;
}

} // namespace flapi
