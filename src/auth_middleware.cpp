#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/config/ConfigAndCredentialsCacheManager.h>
#include <aws/secretsmanager/SecretsManagerClient.h>
#include <aws/secretsmanager/model/GetSecretValueRequest.h>
#include <aws/secretsmanager/model/GetSecretValueResult.h>
#include <crow/http_request.h>
#include <crow/http_response.h>
#include <crow/utility.h>
#include <crow/json.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <jwt-cpp/jwt.h>
#include <openssl/evp.h>
#include <sstream>
#include <iomanip>
#include <regex>

#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include "auth_middleware.hpp"
#include "auth_params.hpp"
#include "bearer_token_verifier.hpp"
#include "request_context_middleware.hpp"

namespace {
// RequestContext::auth_kind is a const char* into STATIC storage, because it is
// read in after_handle - long after the EndpointRef pinning the endpoint's
// strings has gone out of scope here. Mapping to literals keeps it valid and
// keeps the value a bounded enumeration rather than free-form config text.
const char* staticAuthKind(const std::string& configured) {
    if (configured == "basic")  { return "basic"; }
    if (configured == "bearer" || configured == "jwt") { return "bearer"; }
    if (configured == "oidc")   { return "oidc"; }
    return "none";
}
}  // namespace
#include "password_hasher.hpp"
#include "database_manager.hpp"
#include "flapi_telemetry.hpp"
#include "runtime_metrics.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/blob.hpp"
#include "oidc_provider_presets.hpp"
#include "extension_installer.hpp"
#include "external_access_policy.hpp"
#include "redaction.hpp"
#include "vfs_adapter.hpp"

namespace flapi 
{

namespace {

std::optional<std::string> ProcessEnv(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

// A key the secret does not carry is a NULL Value, and NULL.ToString() is the
// four characters "NULL": every secret without a session token sent
// `X-Amz-Security-Token: NULL`, which AWS rejects, and one without a region
// asked for the region "NULL".
std::string SecretString(const duckdb::KeyValueSecret& secret, const char* key) {
    const auto value = secret.TryGetValue(key, false);
    if (value.IsNull()) {
        return "";
    }
    return value.ToString();
}

bool IsTruthy(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    return lower == "true" || lower == "1" || lower == "yes";
}

}  // namespace

std::string AwsHelper::resolveEndpointOverride(const EnvLookup& env) {
    // Only the service-specific variable. AWS_ENDPOINT_URL is commonly set for
    // S3 (MinIO): it must not also pick the service that decides who logs in.
    if (const auto ignore = env("AWS_IGNORE_CONFIGURED_ENDPOINT_URLS"); ignore && IsTruthy(*ignore)) {
        return "";
    }
    if (const auto value = env("AWS_ENDPOINT_URL_SECRETS_MANAGER"); value && !value->empty()) {
        return *value;
    }
    return "";
}

bool AwsHelper::isLoopbackHost(const std::string& url) {
    auto rest = url;
    if (const auto scheme = rest.find("://"); scheme != std::string::npos) {
        rest = rest.substr(scheme + 3);
    }
    if (const auto at = rest.find('@'); at != std::string::npos && at < rest.find('/')) {
        rest = rest.substr(at + 1);
    }
    std::string host;
    if (!rest.empty() && rest.front() == '[') {
        host = rest.substr(1, rest.find(']') == std::string::npos ? std::string::npos : rest.find(']') - 1);
    } else {
        host = rest.substr(0, rest.find_first_of(":/?#"));
    }
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    if (host == "localhost" || host == "::1") {
        return true;
    }
    // 127.0.0.0/8 as an IPv4 literal - not a name that merely starts with "127.".
    static const std::regex loopback_v4(R"(127\.\d{1,3}\.\d{1,3}\.\d{1,3})");
    return std::regex_match(host, loopback_v4);
}

void AwsHelper::checkEndpointOverride(const std::string& url, const EnvLookup& env) {
    std::string scheme = url.substr(0, url.find("://"));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
    if (scheme != "http" || isLoopbackHost(url)) {
        return;
    }
    if (const auto allow = env("FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT"); allow && IsTruthy(*allow)) {
        return;
    }
    throw std::runtime_error(
        "AWS_ENDPOINT_URL_SECRETS_MANAGER is plain http to a host that is not loopback (" +
        RedactUrlsInText(url) + "): the answer decides who can log in, and anyone on the path could change it. "
        "Use https, or set FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT=1 to accept that.");
}

std::string AwsHelper::resolveRegion(const std::string& configured, const EnvLookup& env,
                                     const std::string& profile_region) {
    if (!configured.empty()) {
        return configured;
    }
    for (const char* name : {"AWS_REGION", "AWS_DEFAULT_REGION"}) {
        if (const auto value = env(name); value && !value->empty()) {
            return *value;
        }
    }
    if (!profile_region.empty()) {
        return profile_region;
    }
    throw std::runtime_error(
        "auth.from-aws-secretmanager.region is not set, nor AWS_REGION, AWS_DEFAULT_REGION or the AWS profile's "
        "region: set the region of the Secrets Manager secret");
}

std::string AwsHelper::sanitiseAwsErrorMessage(const std::string& message) {
    // AWS messages can quote an access key id or an encoded authorization
    // message; only the first line is wanted in a log anyway.
    std::string out = message.substr(0, message.find_first_of("\r\n"));
    static const std::regex access_key_id("A[SK]IA[0-9A-Z]{16}");
    static const std::regex long_base64("[A-Za-z0-9+/=_-]{32,}");
    out = std::regex_replace(out, access_key_id, "<key-id>");
    out = std::regex_replace(out, long_base64, "<redacted>");
    if (out.size() > 300) {
        out = out.substr(0, 300) + "...";
    }
    return out;
}

AwsHelper::AwsHelper(std::shared_ptr<DatabaseManager> db_manager)
    : _db_manager(std::move(db_manager))
{ }

namespace {

// Aws::InitAPI once per process, and no ShutdownAPI: the SDK's global state
// (HTTP client factory, curl) would otherwise be torn down and rebuilt around
// every fetch, and shutting it down during static destruction races other
// threads the way closing the bootstrap DuckDB did. The process exit reclaims it.
void EnsureAwsSdkInitialised() {
    static std::once_flag once;
    std::call_once(once, [] {
        static Aws::SDKOptions* const options = new Aws::SDKOptions();
        Aws::InitAPI(*options);
    });
}

std::optional<AwsAuthParams> ReadS3Secret(duckdb::SecretManager& secret_manager,
                                          duckdb::CatalogTransaction transaction, const std::string& name) {
    auto secret_entry = secret_manager.GetSecretByName(transaction, name);
    if (!secret_entry) {
        return std::nullopt;
    }
    auto kv_secret = dynamic_cast<const duckdb::KeyValueSecret*>(secret_entry->secret.get());
    if (!kv_secret) {
        return std::nullopt;
    }
    AwsAuthParams auth_params;
    auth_params.access_key = SecretString(*kv_secret, "key_id");
    auth_params.secret_key = SecretString(*kv_secret, "secret");
    auth_params.session_token = SecretString(*kv_secret, "session_token");
    auth_params.region = SecretString(*kv_secret, "region");
    return auth_params;
}

// Every statement of `sql` on `conn`. Errors say which statement failed and
// what kind of error it was, never the statement: this is credential SQL.
void RunStatements(duckdb_connection conn, const std::string& sql, const std::string& what) {
    duckdb_extracted_statements stmts = nullptr;
    const idx_t n = duckdb_extract_statements(conn, sql.c_str(), &stmts);
    if (n == 0) {
        const char* error = duckdb_extract_statements_error(stmts);
        const std::string detail = redactSqlErrorMessage(error ? error : "(no detail)", sql);
        duckdb_destroy_extracted(&stmts);
        throw std::runtime_error(what + ": could not parse it: " + detail);
    }
    for (idx_t i = 0; i < n; i++) {
        const std::string position = "statement " + std::to_string(i + 1) + " of " + std::to_string(n);
        duckdb_prepared_statement stmt = nullptr;
        if (duckdb_prepare_extracted_statement(conn, stmts, i, &stmt) == DuckDBError) {
            const char* error = duckdb_prepare_error(stmt);
            const std::string detail = redactSqlErrorMessage(error ? error : "(no detail)", sql);
            duckdb_destroy_prepare(&stmt);
            duckdb_destroy_extracted(&stmts);
            throw std::runtime_error(what + ": " + position + " failed: " + detail);
        }
        duckdb_result result;
        const bool failed = duckdb_execute_prepared(stmt, &result) == DuckDBError;
        const std::string detail = failed ? redactSqlErrorMessage(duckdb_result_error(&result), sql) : "";
        duckdb_destroy_result(&result);
        duckdb_destroy_prepare(&stmt);
        if (failed) {
            duckdb_destroy_extracted(&stmts);
            throw std::runtime_error(what + ": " + position + " failed: " + detail);
        }
    }
    duckdb_destroy_extracted(&stmts);
}

// Owns a private DuckDB instance for the length of a scope.
struct PrivateDuckDb {
    duckdb_database db = nullptr;
    duckdb_connection conn = nullptr;
    ~PrivateDuckDb() {
        if (conn) {
            duckdb_disconnect(&conn);
        }
        if (db) {
            duckdb_close(&db);
        }
    }
};

}  // namespace

std::optional<AwsAuthParams> AwsHelper::credentialsFromLegacyInit(const AuthFromSecretManagerConfig& config) {
    const std::string duck_secret_id = ConfigManager::secretNameToSecretId(config.secret_name);
    if (!config.init.empty()) {
        // A DuckDB of its own: a secret created in the main database is listed by
        // duckdb_secrets() to every SQL template (#254).
        PrivateDuckDb private_db;
        private_db.db = OpenPrivateDuckDb(/*allow_persistent_secrets=*/false,
                                          "private DuckDB for auth.from-aws-secretmanager.init");
        if (duckdb_connect(private_db.db, &private_db.conn) != DuckDBSuccess) {
            throw std::runtime_error("Could not connect to the private DuckDB for auth.from-aws-secretmanager.init");
        }
        // httpfs registers the S3 secret type; offline, nothing autoloads it.
        const bool offline = ExtensionsOfflineRequested();
        if (const auto error = LoadOrInstallExtension(private_db.conn, "httpfs", offline); !error.empty()) {
            CROW_LOG_WARNING << "auth.from-aws-secretmanager.init: loading httpfs failed: " << error;
        }
        // PROVIDER CREDENTIAL_CHAIN lives in the aws extension. Loaded only when
        // the init asks for it, so no other init downloads anything.
        std::string lower_init = config.init;
        std::transform(lower_init.begin(), lower_init.end(), lower_init.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower_init.find("credential_chain") != std::string::npos) {
            if (const auto error = LoadOrInstallExtension(private_db.conn, "aws", offline); !error.empty()) {
                CROW_LOG_WARNING << "auth.from-aws-secretmanager.init: loading aws (for CREDENTIAL_CHAIN) failed: "
                                 << error;
            }
        }
        // duckdb-hardening (#253): an endpoint YAML carries this SQL, and for a remote
        // config that YAML is in the very bucket `restricted` does not trust. The
        // init only has to create a secret, so it gets no file or network access and
        // cannot change that; extensions it needs were loaded just above.
        if (ExternalAccessRestricted()) {
            RunStatements(private_db.conn,
                          "SET enable_external_access = false; SET lock_configuration = true;",
                          "hardening the private DuckDB for auth.from-aws-secretmanager.init");
        }
        RunStatements(private_db.conn, config.init,
                      "auth.from-aws-secretmanager.init for secret '" + config.secret_name + "'");
        auto& instance = *reinterpret_cast<duckdb::DatabaseWrapper*>(private_db.db)->database->instance;
        if (auto params = ReadS3Secret(duckdb::SecretManager::Get(instance),
                                       duckdb::CatalogTransaction::GetSystemTransaction(instance), duck_secret_id)) {
            return params;
        }
    }
    // Created by a connection `init:` or persisted in ~/.duckdb/stored_secrets:
    // the main database's secret of that name. It keeps working, but such a
    // secret is listed (redacted) to every template, so it is deprecated.
    if (_db_manager && _db_manager->isInitialized()) {
        auto [secret_manager, transaction] = _db_manager->getSecretManagerAndTransaction();
        if (auto params = ReadS3Secret(secret_manager, transaction, duck_secret_id)) {
            CROW_LOG_WARNING << "auth.from-aws-secretmanager for secret '" << config.secret_name
                             << "': using the DuckDB secret '" << duck_secret_id
                             << "' of the main database, which SQL templates can list. This is deprecated: set "
                                "secret-id/secret-key, or neither to use the AWS SDK's default credential chain, "
                                "or create the secret in auth.from-aws-secretmanager.init.";
            return params;
        }
    }
    return std::nullopt;
}

std::string AwsHelper::getSecretJson(const AuthFromSecretManagerConfig& config)
{
    const auto& secret_name = config.secret_name;
    CROW_LOG_DEBUG << "Retrieving secret '" << secret_name << "' from AWS Secrets Manager";
    EnsureAwsSdkInitialised();

    const auto endpoint_override = resolveEndpointOverride(ProcessEnv);
    if (!endpoint_override.empty()) {
        checkEndpointOverride(endpoint_override, ProcessEnv);
        CROW_LOG_WARNING << "AWS Secrets Manager endpoint overridden by AWS_ENDPOINT_URL_SECRETS_MANAGER: "
                         << RedactUrlsInText(endpoint_override)
                         << ". This endpoint decides who can log in (secret '" << secret_name << "').";
    }

    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentials;
    std::string region = config.region;
    if (config.init_declared) {
        const auto params = credentialsFromLegacyInit(config);
        if (!params) {
            throw std::runtime_error(
                "auth.from-aws-secretmanager for secret '" + secret_name + "': `init` created no DuckDB S3 secret "
                "named '" + ConfigManager::secretNameToSecretId(secret_name) + "'. Set secret-id/secret-key "
                "instead, or neither to use the AWS SDK's default credential chain.");
        }
        credentials = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(
            params->access_key, params->secret_key, params->session_token);
        if (region.empty()) {
            region = params->region;
        }
    } else if (!config.secret_id.empty()) {
        // Straight to the SDK: no DuckDB secret, no SQL, nothing to quote.
        credentials = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(config.secret_id, config.secret_key);
    } else {
        // Environment, shared config/credentials files, SSO, web identity,
        // container and instance roles - the standard AWS precedence.
        credentials = std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>();
    }

    Aws::Client::ClientConfigurationInitValues init_values;
    init_values.shouldDisableIMDS = true;   // the region is resolved below, not probed
    Aws::Client::ClientConfiguration client_config(init_values);
    // The active profile (AWS_PROFILE, else default) of AWS_CONFIG_FILE / ~/.aws/config.
    client_config.region = resolveRegion(region, ProcessEnv, Aws::Config::GetCachedConfigValue("region"));
    if (!endpoint_override.empty()) {
        client_config.endpointOverride = endpoint_override;
    }

    Aws::SecretsManager::SecretsManagerClient client(credentials, client_config);
    Aws::SecretsManager::Model::GetSecretValueRequest request;
    request.SetSecretId(secret_name);

    auto response = client.GetSecretValue(request);
    if (!response.IsSuccess()) {
        const auto& error = response.GetError();
        throw std::runtime_error("Error retrieving secret '" + secret_name + "' from AWS Secrets Manager (" +
                                 client_config.region + "): " + error.GetExceptionName() + ": " +
                                 sanitiseAwsErrorMessage(error.GetMessage()));
    }

    auto secret_json = response.GetResult().GetSecretString();
    CROW_LOG_DEBUG << "Retrieved secret '" << secret_name << "': *****[" << secret_json.size() << "]";
    return secret_json;
}

std::vector<SecretUser> AwsHelper::parseUserList(const std::string& secret_json) {
    // The document is never quoted in an error: it holds the passwords.
    static const std::string kShape =
        "expected a JSON object {\"auth\": [{\"username\": ..., \"password\": ..., \"roles\": [...]}]}";
    auto doc = crow::json::load(secret_json);
    if (!doc || doc.t() != crow::json::type::Object || !doc.has("auth") ||
        doc["auth"].t() != crow::json::type::List) {
        throw std::runtime_error("The secret is not a flAPI user list: " + kShape);
    }
    std::vector<SecretUser> users;
    idx_t index = 0;
    for (const auto& entry : doc["auth"]) {
        const std::string where = "entry " + std::to_string(index++) + " of \"auth\"";
        if (entry.t() != crow::json::type::Object) {
            throw std::runtime_error("The secret's " + where + " is not an object: " + kShape);
        }
        SecretUser user;
        for (const auto& [key, target] : {std::pair<const char*, std::string*>{"username", &user.username},
                                          std::pair<const char*, std::string*>{"password", &user.password}}) {
            if (!entry.has(key) || entry[key].t() != crow::json::type::String) {
                throw std::runtime_error("The secret's " + where + " has no string \"" + key + "\"");
            }
            *target = std::string(entry[key].s());
            if (target->empty()) {
                throw std::runtime_error("The secret's " + where + " has an empty \"" + key + "\"");
            }
        }
        if (entry.has("roles")) {
            if (entry["roles"].t() != crow::json::type::List) {
                throw std::runtime_error("The secret's " + where + ": \"roles\" is not a list of strings");
            }
            for (const auto& role : entry["roles"]) {
                if (role.t() != crow::json::type::String) {
                    throw std::runtime_error("The secret's " + where + ": \"roles\" is not a list of strings");
                }
                user.roles.emplace_back(role.s());
            }
        }
        users.push_back(std::move(user));
    }
    return users;
}

void AwsHelper::loadUsers(const AuthFromSecretManagerConfig& config) {
    if (hasUsers(config.secret_name)) {
        return;
    }
    std::vector<SecretUser> users;
    try {
        users = parseUserList(getSecretJson(config));
    } catch (const std::exception& e) {
        throw std::runtime_error("auth.from-aws-secretmanager, secret '" + config.secret_name + "': " + e.what());
    }
    CROW_LOG_INFO << "Loaded " << users.size() << " user(s) from AWS Secrets Manager secret '"
                  << config.secret_name << "'";
    // Built now, not on the first unknown login: a PBKDF2 hash takes as long
    // to make as to check.
    auto dummy = PasswordHasher::dummyHashLike(users.empty() ? std::string() : users.front().password);
    std::lock_guard<std::mutex> lock(_mutex);
    _users.emplace(config.secret_name, std::move(users));
    _dummy_hashes.emplace(config.secret_name, std::move(dummy));
}

std::string AwsHelper::dummyHashFor(const std::string& secret_name) const {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (const auto it = _dummy_hashes.find(secret_name); it != _dummy_hashes.end()) {
            return it->second;
        }
    }
    return PasswordHasher::dummyHashLike("");
}

bool AwsHelper::hasUsers(const std::string& secret_name) const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _users.count(secret_name) > 0;
}

std::optional<SecretUser> AwsHelper::findUser(const std::string& secret_name, const std::string& username) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto it = _users.find(secret_name);
    if (it == _users.end()) {
        return std::nullopt;
    }
    for (const auto& user : it->second) {
        if (user.username == username) {
            return user;
        }
    }
    return std::nullopt;
}

// ------------------------------------------------------------------------------------------------
void AuthMiddleware::initialize(std::shared_ptr<ConfigManager> config_manager) {
    this->config_manager = config_manager;
    this->db_manager = DatabaseManager::getInstance();
    this->aws_helper = std::make_shared<AwsHelper>(db_manager);

    initializeAwsSecretsManager();
}

namespace {

// What decides which users a block gets, for the secret-name it shares.
struct SecretSource {
    std::string region;
    std::string credentials;   // compared only, never logged
    std::string init;
    std::string endpoint;
};

std::string LegacyDefaultTableName(const std::string& secret_name) {
    std::string sanitized = secret_name;
    std::replace_if(sanitized.begin(), sanitized.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    return "auth_" + sanitized;
}

}  // namespace

void AuthMiddleware::dropLegacyUserTables(const AuthFromSecretManagerConfig& config) {
    // Older versions kept the fetched user list in a table of the main
    // database. On a file db_path it outlives the upgrade, readable by every
    // template and listed by the schema API, so it goes.
    if (!db_manager || !db_manager->isInitialized()) {
        return;
    }
    std::vector<std::string> names{LegacyDefaultTableName(config.secret_name)};
    if (!config.secret_table.empty() && config.secret_table != names.front()) {
        names.push_back(config.secret_table);
    }
    for (const auto& name : names) {
        try {
            if (db_manager->dropTableIfExists(name)) {
                CROW_LOG_WARNING << "Dropped table '" << name << "' left by an older flAPI: it held the user list "
                                 << "of AWS Secrets Manager secret '" << config.secret_name
                                 << "', readable by SQL templates. If this database file was in use, rotate "
                                    "those passwords.";
            }
        } catch (const std::exception& e) {
            CROW_LOG_ERROR << "Could not drop table '" << name << "' left by an older flAPI (it holds the user list "
                           << "of secret '" << config.secret_name << "' and SQL templates can read it): "
                           << e.what();
        }
    }
}

void AuthMiddleware::initializeAwsSecretsManager() {
    // Once per secret-name: endpoints sharing one (or inheriting the top-level
    // `auth:`) share its user list - so they must agree on where it comes from.
    std::unordered_map<std::string, SecretSource> sources;
    std::vector<const EndpointConfig*> to_load;
    const auto endpoints = config_manager->getEndpoints();
    for (const auto& endpoint : *endpoints) {
        if (!endpoint.auth.from_aws_secretmanager) {
            continue;
        }
        const auto& aws_config = *endpoint.auth.from_aws_secretmanager;
        dropLegacyUserTables(aws_config);
        // Only Basic reads it; a bearer or OIDC block with a leftover
        // from-aws-secretmanager (a load warning names it) fetches nothing.
        if (!endpoint.auth.enabled || endpoint.auth.type != "basic") {
            continue;
        }
        if (aws_config.secret_name.empty()) {
            throw std::runtime_error("AWS Secrets Manager secret name in endpoint " + endpoint.urlPath + " is not set");
        }
        SecretSource source;
        source.region = aws_config.region;
        source.credentials = aws_config.init_declared ? "init"
                             : aws_config.secret_id.empty() ? "chain"
                                                            : "keys:" + aws_config.secret_id + ":" + aws_config.secret_key;
        source.init = aws_config.init;
        source.endpoint = endpoint.getIdentifier();
        const auto [it, inserted] = sources.emplace(aws_config.secret_name, source);
        if (!inserted) {
            std::string differs;
            if (it->second.region != source.region) {
                differs += "region";
            }
            if (it->second.credentials != source.credentials) {
                differs += std::string(differs.empty() ? "" : ", ") + "credentials";
            }
            if (it->second.init != source.init) {
                differs += std::string(differs.empty() ? "" : ", ") + "init";
            }
            if (!differs.empty()) {
                throw std::runtime_error(
                    "auth.from-aws-secretmanager: " + it->second.endpoint + " and " + source.endpoint +
                    " both name secret '" + aws_config.secret_name + "' but differ in " + differs +
                    ". The secret is fetched once and shared, so the blocks must agree.");
            }
        }
        to_load.push_back(&endpoint);
    }
    for (const auto* endpoint : to_load) {
        aws_helper->loadUsers(*endpoint->auth.from_aws_secretmanager);
    }
}

std::string AuthMiddleware::challengeFor(const std::string& auth_type, bool credentials_sent) {
    if (auth_type == "basic") {
        return "Basic realm=\"flAPI\"";
    }
    if (auth_type == "bearer" || auth_type == "jwt" || auth_type == "oidc") {
        // RFC 6750 §3.1: a request that carried a token which was refused
        // says so; one that carried none gets the bare challenge.
        return credentials_sent ? "Bearer realm=\"flAPI\", error=\"invalid_token\""
                                : "Bearer realm=\"flAPI\"";
    }
    // No scheme that could ever succeed (a load warning names it). Still a
    // valid challenge, so clients see a 401 that conforms to RFC 9110.
    return "Basic realm=\"flAPI\"";
}

namespace {

// Every 401 the same shape: the configured scheme's challenge and the JSON
// body a 403 already had. The body used to be Crow's plain "401 Unauthorized".
void rejectUnauthorized(crow::response& res, const std::string& auth_type, bool credentials_sent,
                        const std::string& message) {
    res.code = 401;
    res.set_header("WWW-Authenticate", AuthMiddleware::challengeFor(auth_type, credentials_sent));
    res.set_header("Content-Type", "application/json");
    crow::json::wvalue body;
    body["error"] = "Unauthorized";
    body["message"] = message;
    res.body = body.dump();
}

}  // namespace

void AuthMiddleware::before_handle(crow::request& req, crow::response& res, context& ctx) {
    if (!config_manager) return;

    // Skip if response already completed (e.g., by rate limit middleware)
    if (res.is_completed()) return;

    const auto endpoint = config_manager->getEndpointForRequest(
        req.url,
        crow::method_name(req.method)
    );
    if (!endpoint || !endpoint->auth.enabled) {
        return;
    }

    CROW_LOG_DEBUG << "Auth enabled for endpoint: " << req.url;

    // A 401/403 from here never reaches the handler, which is where a REST
    // series is recorded, so these rejections were counted globally only and
    // never against their route. Named and recorded here instead.
    const std::string method_name = crow::method_name(req.method);
    const std::string route = endpoint->urlPath;
    if (auto* rc = RequestContextScope::current()) {
        rc->route_template_storage = route;
        rc->route_template = rc->route_template_storage;
    }
    const auto record_rejection = [&](int status) {
        const auto* rc = RequestContextScope::current();
        const auto started = rc != nullptr ? rc->t0 : std::chrono::steady_clock::now();
        RuntimeMetrics::instance().recordRest(
            method_name, route, status,
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started));
    };

    // auth_kind is the endpoint's configured scheme (enum, never a secret).
    const std::string auth_kind = endpoint->auth.type;

    auto auth_header = req.get_header_value("Authorization");
    if (auth_header.empty()) {
        CROW_LOG_DEBUG << "No Authorization header found";
        // The challenge and the audit name the endpoint's OWN scheme. Both
        // used to say basic whatever the endpoint was configured for, which
        // sent bearer clients looking for a password prompt.
        rejectUnauthorized(res, endpoint->auth.type, /*credentials_sent=*/false,
                           "This endpoint requires authentication; no Authorization header was sent.");
        // Record the outcome for the audit line. Completion itself is NOT done
        // here: Crow runs every middleware's after_handle during the
        // short-circuit unwind (crow/middleware.h:151-155), so
        // RequestContextMiddleware::after_handle still runs for this 401.
        // Completing here as well emitted the line twice.
        if (auto* rc = RequestContextScope::current()) {
            rc->auth_kind = staticAuthKind(endpoint->auth.type);
            rc->principal = "anonymous";
        }
        record_rejection(401);
        res.end();
        flapi::GlobalTelemetry().authEnforced(auth_kind, /*allow=*/false);
        return;
    }

    if (endpoint->auth.type == "basic") {
        ctx.auth_type = "basic";
        ctx.authenticated = authenticateBasic(auth_header, *endpoint, ctx);
    } else if (endpoint->auth.type == "bearer" || endpoint->auth.type == "jwt") {
        // `jwt` is normalised to `bearer` at load; accepted here as well for
        // endpoints constructed in code.
        ctx.auth_type = "bearer";
        ctx.authenticated = authenticateBearer(auth_header, *endpoint, ctx);
    } else if (endpoint->auth.type == "oidc") {
        ctx.auth_type = "oidc";
        ctx.authenticated = authenticateOIDC(auth_header, *endpoint, ctx);
    }

    // An identity with a control character (newline, NUL, ...) or a role
    // containing a comma is refused, not cleaned up: it reaches SQL templates
    // as auth.*, stripping would turn "bob\n" into another principal's name,
    // and a comma would split one role into two. See auth_params.hpp.
    const char* refusal = ctx.authenticated ? IdentityRefusal(ctx.username, ctx.email, ctx.roles) : nullptr;
    if (refusal != nullptr) {
        CROW_LOG_WARNING << "Authentication refused on " << endpoint->getIdentifier()
                         << " (" << ctx.auth_type << "): " << refusal;
        ctx.authenticated = false;
        ctx.username.clear();
        ctx.email.clear();
        ctx.roles.clear();
    }

    if (!ctx.authenticated) {
        CROW_LOG_DEBUG << "Authentication failed";
        // invalid_token only when a token was actually presented; a Basic
        // header sent to a bearer endpoint gets the bare challenge.
        const bool token_sent = auth_header.rfind("Bearer ", 0) == 0;
        rejectUnauthorized(res, endpoint->auth.type, token_sent,
                           "The credentials sent were not accepted.");
        // As above: record only. after_handle still runs for this response.
        if (auto* rc = RequestContextScope::current()) {
            rc->auth_kind = staticAuthKind(endpoint->auth.type);
            rc->principal = ctx.username.empty() ? "anonymous" : ctx.username;
        }
        record_rejection(401);
        res.end();
    } else {
        CROW_LOG_DEBUG << "Authentication successful for user: " << ctx.username;
        if (auto* rc = RequestContextScope::current()) {
            rc->auth_kind = staticAuthKind(endpoint->auth.type);
            rc->principal = ctx.username;
        }
        // Authorisation, after authentication: `auth.roles` was documented
        // and never parsed nor checked, so a principal lacking every listed
        // role got 200. Any one listed role suffices. 403, not 401: the
        // credentials are valid, retrying with them cannot help.
        if (!hasRequiredRole(endpoint->auth.roles, ctx.roles)) {
            CROW_LOG_INFO << "Authorization denied for user '" << ctx.username << "' on "
                          << endpoint->getIdentifier() << ": requires one of the endpoint's roles";
            res.code = 403;
            res.set_header("Content-Type", "application/json");
            crow::json::wvalue body;
            body["error"] = "Forbidden";
            body["message"] = "The authenticated principal does not hold any role this endpoint requires.";
            res.body = body.dump();
            record_rejection(403);
            res.end();
            flapi::GlobalTelemetry().authEnforced(auth_kind, /*allow=*/false);
            return;
        }
    }
    flapi::GlobalTelemetry().authEnforced(auth_kind, ctx.authenticated);
}

bool AuthMiddleware::hasRequiredRole(const std::vector<std::string>& required,
                                     const std::vector<std::string>& held) {
    if (required.empty()) {
        return true;
    }
    for (const auto& role : held) {
        if (std::find(required.begin(), required.end(), role) != required.end()) {
            return true;
        }
    }
    return false;
}

bool AuthMiddleware::authenticateBasic(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx) {
    if (auth_header.substr(0, 6) != "Basic ") {
        return false;
    }

    std::string decoded = crow::utility::base64decode(auth_header.substr(6));
    size_t colon_pos = decoded.find(':');
    
    if (colon_pos == std::string::npos) {
        return false;
    }

    std::string username = decoded.substr(0, colon_pos);
    std::string password = decoded.substr(colon_pos + 1);

    // Try inline users first
    if (!endpoint.auth.users.empty()) {
        return authenticateInlineUsers(username, password, endpoint, ctx);
    }
    
    // Try AWS Secrets Manager if configured
    if (endpoint.auth.from_aws_secretmanager) {
        return authenticateAwsSecrets(username, password, endpoint, ctx);
    }

    return false;
}

bool AuthMiddleware::authenticateInlineUsers(const std::string& username, const std::string& password,
                                          const EndpointConfig& endpoint, context& ctx) {
    bool known = false;
    for (const auto& user : endpoint.auth.users) {
        if (user.username == username) {
            known = true;
            if (verifyCredentials(username, password, user.password)) {
                ctx.username = username;
                ctx.roles = user.roles;
                return true;
            }
        }
    }
    if (known) {
        return false;
    }
    // Unknown user: the same work as a wrong password, so timing does not tell
    // which usernames exist.
    verifyCredentials(username, password,
                      PasswordHasher::dummyHashLike(endpoint.auth.users.empty() ? std::string()
                                                                                : endpoint.auth.users.front().password));
    return false;
}

bool AuthMiddleware::authenticateAwsSecrets(const std::string& username, const std::string& password,
                                         const EndpointConfig& endpoint, context& ctx) {
    if (!endpoint.auth.from_aws_secretmanager || !aws_helper) {
        return false;
    }
    try {
        const auto& secret_name = endpoint.auth.from_aws_secretmanager->secret_name;
        const auto user = aws_helper->findUser(secret_name, username);
        if (!user) {
            // As for inline users: an unknown name costs a wrong password.
            verifyCredentials(username, password, aws_helper->dummyHashFor(secret_name));
            return false;
        }
        if (!verifyCredentials(username, password, user->password)) {
            return false;
        }
        ctx.username = username;
        ctx.roles.insert(ctx.roles.end(), user->roles.begin(), user->roles.end());
        return true;
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error authenticating against AWS Secrets Manager users: " << e.what();
        return false;
    }
}

std::string AuthMiddleware::md5Hash(const std::string& input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    // Create a message digest context
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create MD5 context");
    }

    try {
        // Initialize the digest
        if (!EVP_DigestInit_ex(ctx, EVP_md5(), nullptr)) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("Failed to initialize MD5 digest");
        }

        // Add data to be hashed
        if (!EVP_DigestUpdate(ctx, input.c_str(), input.length())) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("Failed to update MD5 digest");
        }

        // Finalize the digest
        if (!EVP_DigestFinal_ex(ctx, digest, &digest_len)) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("Failed to finalize MD5 digest");
        }

        // Clean up
        EVP_MD_CTX_free(ctx);

        // Convert to hex string
        std::stringstream ss;
        for (unsigned int i = 0; i < digest_len; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
        }
        return ss.str();
    }
    catch (...) {
        EVP_MD_CTX_free(ctx);
        throw;
    }
}

bool AuthMiddleware::verifyPassword(const std::string& provided_password, const std::string& stored_password) {
    // W1.1: delegate to PasswordHasher so all supported formats
    // (PBKDF2-SHA256 modern, MD5 deprecated, plaintext deprecated) are
    // handled in one place. The Wave 0 startup auditor surfaces
    // deprecation warnings; the runtime keeps accepting legacy hashes so
    // existing configs do not break on upgrade.
    return PasswordHasher{}.verify(provided_password, stored_password);
}

bool AuthMiddleware::verifyCredentials(const std::string& username, const std::string& provided_password,
                                       const std::string& stored_password) {
    // Same verdict as verifyPassword, with successful PBKDF2 checks remembered
    // and concurrent derivations capped (see PasswordVerificationCache).
    return PasswordVerificationCache::instance().verifyCredentials(username, provided_password,
                                                                   stored_password);
}

bool AuthMiddleware::authenticateBearer(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx) {
    if (auth_header.substr(0, 7) != "Bearer ") {
        return false;
    }

    // An empty HMAC key is not a weak secret, it is no secret: anyone can sign
    // a token with the empty key and it verifies. This is reachable by
    // accident rather than by misconfiguration in the usual sense -
    // `jwt-secret: '{{env.API_JWT_SECRET}}'` resolves to "" when the variable
    // is unset, silently, and the endpoint then accepts a token claiming any
    // subject and any roles.
    //
    // Measured before this guard: a token forged with an empty key and
    // roles ["admin"] was accepted with 200 and returned the protected rows.
    //
    // Fail closed. The startup auditor also reports this, but the runtime
    // must not depend on anyone having read the warning.
    if (endpoint.auth.jwt_secret.empty()) {
        CROW_LOG_ERROR << "Refusing bearer authentication for "
                       << endpoint.getIdentifier()
                       << ": jwt-secret is empty, so any token would verify. "
                          "Check that the environment variable it interpolates is set.";
        return false;
    }

    BearerVerifierConfig verifier;
    verifier.secret = endpoint.auth.jwt_secret;
    verifier.issuer = endpoint.auth.jwt_issuer;
    verifier.audiences = endpoint.auth.jwt_audiences;
    verifier.require_exp = endpoint.auth.jwt_require_exp;
    std::string reason;
    auto identity = VerifyBearerToken(auth_header.substr(7), verifier, &reason);
    if (!identity) {
        CROW_LOG_DEBUG << "JWT verification failed: " << reason;
        return false;
    }
    ctx.username = std::move(identity->username);
    ctx.email = std::move(identity->email);
    ctx.roles = std::move(identity->roles);
    return true;
}

std::shared_ptr<OIDCAuthHandler> AuthMiddleware::getOIDCHandler(const OIDCConfig& oidc_config) {
    // Make a mutable copy to apply presets
    OIDCConfig config = oidc_config;

    // Apply provider presets if specified
    if (!config.provider_type.empty() && config.provider_type != "generic") {
        bool preset_applied = OIDCProviderPresets::applyPreset(config);
        if (preset_applied) {
            CROW_LOG_DEBUG << "Applied OIDC preset for provider: " << config.provider_type;
        }
    }

    // Validate provider configuration
    std::string validation_error = OIDCProviderPresets::validateProviderConfig(config);
    if (!validation_error.empty()) {
        CROW_LOG_ERROR << "OIDC configuration error: " << validation_error;
        // Note: In production, should throw or return nullopt, but for now log and continue
        // The handler will fail during token validation with clearer errors
    }

    auto key = config.issuer_url + ":" + config.client_id;

    auto it = oidc_handlers.find(key);
    if (it != oidc_handlers.end()) {
        return it->second;
    }

    // Convert OIDCConfig to OIDCAuthHandler::Config
    OIDCAuthHandler::Config handler_config;
    handler_config.issuer_url = config.issuer_url;
    handler_config.client_id = config.client_id;
    handler_config.client_secret = config.client_secret;
    handler_config.allowed_audiences = config.allowed_audiences;
    handler_config.verify_expiration = config.verify_expiration;
    handler_config.clock_skew_seconds = config.clock_skew_seconds;
    handler_config.username_claim = config.username_claim;
    handler_config.email_claim = config.email_claim;
    handler_config.roles_claim = config.roles_claim;
    handler_config.groups_claim = config.groups_claim;
    handler_config.role_claim_path = config.role_claim_path;
    handler_config.enable_client_credentials = config.enable_client_credentials;
    handler_config.enable_refresh_tokens = config.enable_refresh_tokens;
    handler_config.scopes = config.scopes;
    handler_config.jwks_cache_hours = config.jwks_cache_hours;

    // Create new handler for this issuer
    auto handler = std::make_shared<OIDCAuthHandler>(handler_config);
    oidc_handlers[key] = handler;
    return handler;
}

bool AuthMiddleware::authenticateOIDC(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx) {
    if (auth_header.substr(0, 7) != "Bearer ") {
        CROW_LOG_DEBUG << "OIDC: Authorization header doesn't start with 'Bearer '";
        return false;
    }

    // Ensure endpoint has OIDC configuration
    if (!endpoint.auth.oidc) {
        CROW_LOG_WARNING << "OIDC authentication requested but endpoint has no OIDC config";
        return false;
    }

    // Get or create OIDC handler
    auto oidc_handler = getOIDCHandler(*endpoint.auth.oidc);

    // Extract and validate token
    std::string token = auth_header.substr(7);
    auto claims = oidc_handler->validateToken(token);

    if (!claims) {
        CROW_LOG_DEBUG << "OIDC token validation failed";
        return false;
    }

    // Set authentication context
    ctx.username = claims->username;
    ctx.email = claims->email;
    ctx.roles = claims->roles;
    ctx.authenticated = true;

    CROW_LOG_DEBUG << "OIDC authentication successful for user: " << ctx.username;
    return true;
}

void AuthMiddleware::after_handle(crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/) {
    // No action needed after handling the request
}

} // namespace flapi
