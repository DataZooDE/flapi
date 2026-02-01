#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/secretsmanager/SecretsManagerClient.h>
#include <aws/secretsmanager/model/GetSecretValueRequest.h>
#include <aws/secretsmanager/model/GetSecretValueResult.h>
#include <crow/http_request.h>
#include <crow/http_response.h>
#include <crow/utility.h>
#include <algorithm>
#include <cstring>
#include <jwt-cpp/jwt.h>
#include <openssl/evp.h>
#include <sstream>
#include <iomanip>

#include "duckdb/main/secret/secret_manager.hpp"

#include "auth_middleware.hpp"
#include "database_manager.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types/blob.hpp"
#include "oidc_provider_presets.hpp"

namespace flapi 
{

AwsHelper::AwsHelper(std::shared_ptr<DatabaseManager> db_manager) 
    : db_manager(db_manager)
{ }

void AwsHelper::refreshSecretJson(const std::string& secret_name, const std::string& secret_table) {
    auto secret_json = getSecretJson(secret_name);
    persistSecretJson(secret_table, secret_json);
}

std::string AwsHelper::getSecretJson(const std::string& secret_name) 
{
    std::string duck_secret_id = ConfigManager::secretNameToSecretId(secret_name);
    CROW_LOG_DEBUG << "Retrieving secret '" + secret_name + "' -> '" + duck_secret_id + "' from AWS Secrets Manager";

    auto aws_auth_params = tryGetS3AuthParams(duck_secret_id);
    if (!aws_auth_params) {
        throw std::runtime_error("No AWS auth params found for secret '" + secret_name + "', " +
                                 "please use create a duckdb secret with the same name '" + duck_secret_id + "' and type 'S3'");
    }

    Aws::SDKOptions options;
    Aws::InitAPI(options);
    
    Aws::Auth::AWSCredentials credentials(aws_auth_params->access_key, 
                                          aws_auth_params->secret_key, 
                                          aws_auth_params->session_token);
    
    Aws::Client::ClientConfiguration clientConfig;
    clientConfig.region = aws_auth_params->region;
    
    Aws::SecretsManager::SecretsManagerClient secretsClient(credentials, nullptr, clientConfig);
    Aws::SecretsManager::Model::GetSecretValueRequest request;
    request.SetSecretId(secret_name);

    auto response = secretsClient.GetSecretValue(request);
    if (!response.IsSuccess()) {
        Aws::ShutdownAPI(options);
        throw std::runtime_error("Error retrieving secret '" + secret_name + "': " + response.GetError().GetMessage());
    }

    auto secret_json = response.GetResult().GetSecretString();
    CROW_LOG_DEBUG << "Successfully retrieved secret '" + secret_name + "': *****[" + std::to_string(secret_json.size()) + "]";

    Aws::ShutdownAPI(options);
    return secret_json;
}

void AwsHelper::persistSecretJson(const std::string& secret_table, const std::string& secret_json) {
    db_manager->refreshSecretsTable(secret_table, secret_json);
}

std::optional<AwsAuthParams> AwsHelper::tryGetS3AuthParams(const std::string& secret_name) 
{
    auto [secret_manager, transaction] = db_manager->getSecretManagerAndTransaction();
    auto secret_entry = secret_manager.GetSecretByName(transaction, secret_name);
    if (!secret_entry) {
        return std::nullopt;
    }

    auto kv_secret = dynamic_cast<const duckdb::KeyValueSecret *>(secret_entry->secret.get());
    if (!kv_secret) {
        return std::nullopt;
    }

    AwsAuthParams auth_params;
    auth_params.access_key = kv_secret->TryGetValue("key_id", false).ToString();
    auth_params.secret_key = kv_secret->TryGetValue("secret", false).ToString();
    auth_params.session_token = kv_secret->TryGetValue("session_token", false).ToString();
    auth_params.region = kv_secret->TryGetValue("region", false).ToString();

    return auth_params;
}

// ------------------------------------------------------------------------------------------------
void AuthMiddleware::initialize(std::shared_ptr<ConfigManager> config_manager) {
    this->config_manager = config_manager;
    this->db_manager = DatabaseManager::getInstance();
    this->aws_helper = std::make_shared<AwsHelper>(db_manager);

    initializeAwsSecretsManager();
}

void AuthMiddleware::initializeAwsSecretsManager() {
    for (const auto& endpoint : config_manager->getEndpoints()) {
        if (!endpoint.auth.from_aws_secretmanager || !endpoint.auth.enabled) {
            continue;
        }

        CROW_LOG_DEBUG << "Initializing AWS Secrets Manager for endpoint: " << endpoint.urlPath;

        auto aws_secretmanager_config = *endpoint.auth.from_aws_secretmanager;

        auto secret_name = aws_secretmanager_config.secret_name;
        if (secret_name.empty()) {
            throw std::runtime_error("AWS Secrets Manager secret name in endpoint " + endpoint.urlPath + " is not set");
        }

        auto secret_table = aws_secretmanager_config.secret_table;
        if (secret_table.empty()) {
            throw std::runtime_error("AWS Secrets Manager table in endpoint " + endpoint.urlPath + " is not set");
        }

        auto init_sql = aws_secretmanager_config.init;
        if (!init_sql.empty()) {
            CROW_LOG_DEBUG << "Executing init statement for AWS Secrets Manager for endpoint: " << endpoint.urlPath;
            db_manager->executeInitStatement(init_sql);
        }

        aws_helper->refreshSecretJson(secret_name, secret_table);
    }
}

void AuthMiddleware::before_handle(crow::request& req, crow::response& res, context& ctx) {
    if (!config_manager) return;

    // Skip if response already completed (e.g., by rate limit middleware)
    if (res.is_completed()) return;

    const auto* endpoint = config_manager->getEndpointForPathAndMethod(
        req.url,
        crow::method_name(req.method)
    );
    if (!endpoint || !endpoint->auth.enabled) {
        return;
    }

    CROW_LOG_DEBUG << "Auth enabled for endpoint: " << req.url;

    auto auth_header = req.get_header_value("Authorization");
    if (auth_header.empty()) {
        CROW_LOG_DEBUG << "No Authorization header found";
        res.code = 401;
        res.set_header("WWW-Authenticate", "Basic realm=\"flAPI\"");
        res.end();
        return;
    }

    if (endpoint->auth.type == "basic") {
        ctx.authenticated = authenticateBasic(auth_header, *endpoint, ctx);
    } else if (endpoint->auth.type == "bearer") {
        ctx.authenticated = authenticateBearer(auth_header, *endpoint, ctx);
    } else if (endpoint->auth.type == "oidc") {
        ctx.authenticated = authenticateOIDC(auth_header, *endpoint, ctx);
    }

    if (!ctx.authenticated) {
        CROW_LOG_DEBUG << "Authentication failed";
        res.code = 401;
        res.end();
    } else {
        CROW_LOG_DEBUG << "Authentication successful for user: " << ctx.username;
    }
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
    for (const auto& user : endpoint.auth.users) {
        if (user.username == username && verifyPassword(password, user.password)) {
            ctx.username = username;
            ctx.roles = user.roles;
            return true;
        }
    }
    return false;
}

bool AuthMiddleware::authenticateAwsSecrets(const std::string& username, const std::string& password,
                                         const EndpointConfig& endpoint, context& ctx) {
    if (!endpoint.auth.from_aws_secretmanager || !db_manager) {
        return false;
    }

    try {
        const auto& aws_config = *endpoint.auth.from_aws_secretmanager;
        auto result = db_manager->findUserInSecretsTable(aws_config.secret_table, username);
        if (!result) {
            return false;
        }

        auto [stored_password, roles] = result.value();
        if (verifyPassword(password, stored_password)) 
        {
            ctx.username = username;
            for (const auto& role : roles) {
                ctx.roles.push_back(role);
            }
            
            return true;
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error authenticating against AWS Secrets: " << e.what();
    }

    return false;
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
    // Check if the stored password is an MD5 hash
    if (stored_password.length() == 32 && std::all_of(stored_password.begin(), stored_password.end(),
            [](char c) { return std::isxdigit(c); })) {
        return md5Hash(provided_password) == stored_password;
    }
    
    return provided_password == stored_password;
}

bool AuthMiddleware::authenticateBearer(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx) {
    if (auth_header.substr(0, 7) != "Bearer ") {
        return false;
    }

    std::string token = auth_header.substr(7);

    try {
        auto decoded = jwt::decode(token);

        auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{endpoint.auth.jwt_secret})
            .with_issuer(endpoint.auth.jwt_issuer);

        verifier.verify(decoded);

        // Extract claims
        ctx.username = decoded.get_payload_claim("sub").as_string();
        auto roles = decoded.get_payload_claim("roles").as_array();
        for (auto& role : roles) {
            ctx.roles.push_back(role.get<std::string>());
        }

        return true;
    } catch (const std::exception& e) {
        CROW_LOG_DEBUG << "JWT verification failed: " << e.what();
        return false;
    }
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
    ctx.roles = claims->roles;
    ctx.authenticated = true;

    CROW_LOG_DEBUG << "OIDC authentication successful for user: " << ctx.username;
    return true;
}

void AuthMiddleware::after_handle(crow::request& /*req*/, crow::response& /*res*/, context& /*ctx*/) {
    // No action needed after handling the request
}

} // namespace flapi
