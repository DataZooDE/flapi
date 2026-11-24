#pragma once

#include <crow/http_request.h>
#include <crow/http_response.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "config_manager.hpp"
#include "database_manager.hpp"
#include "oidc_auth_handler.hpp"


namespace flapi {

struct AwsAuthParams {
    std::string access_key;
    std::string secret_key;
    std::string session_token;
    std::string region;
};

// One entry of a Secrets-Manager user list: {"auth":[{"username","password","roles"}]}.
struct SecretUser {
    std::string username;
    std::string password;   // as stored: plaintext, MD5 or $pbkdf2-sha256$...
    std::vector<std::string> roles;
};

// Basic-auth users fetched from AWS Secrets Manager (`auth.from-aws-secretmanager`).
//
// The list is kept here, in memory, keyed by secret-name - never in the main
// DuckDB, where every SQL template could read it (#254). Each secret is fetched
// once, however many endpoints (or the top-level `auth:`) name it. The AWS
// credentials go straight to the SDK: configured secret-id/secret-key, else the
// SDK's default credential chain; only a legacy `init:` involves DuckDB, and
// then a private instance of its own.
class AwsHelper {
public:
    explicit AwsHelper(std::shared_ptr<DatabaseManager> db_manager);

    // Fetch the secret's user list unless it is already loaded. Throws on failure.
    void loadUsers(const AuthFromSecretManagerConfig& config);
    bool hasUsers(const std::string& secret_name) const;
    // The first user of that name in the secret's list.
    std::optional<SecretUser> findUser(const std::string& secret_name, const std::string& username) const;

    // The secret's SecretString, fetched from Secrets Manager.
    std::string getSecretJson(const AuthFromSecretManagerConfig& config);

    // Parse {"auth":[{"username":..,"password":..,"roles":[..]}]}. Throws, without
    // quoting the document, when it is not that shape.
    static std::vector<SecretUser> parseUserList(const std::string& secret_json);

    using EnvLookup = std::function<std::optional<std::string>(const std::string&)>;
    // AWS_ENDPOINT_URL_SECRETS_MANAGER unless AWS_IGNORE_CONFIGURED_ENDPOINT_URLS is
    // true; empty: AWS's endpoint. AWS_ENDPOINT_URL is deliberately NOT used.
    static std::string resolveEndpointOverride(const EnvLookup& env);
    // Throws for a plain-http override whose host is not loopback, unless
    // FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT is set.
    static void checkEndpointOverride(const std::string& url, const EnvLookup& env);
    static bool isLoopbackHost(const std::string& url);
    // `configured`, else AWS_REGION, else AWS_DEFAULT_REGION, else the AWS profile's
    // region; throws when none is set.
    static std::string resolveRegion(const std::string& configured, const EnvLookup& env,
                                     const std::string& profile_region = "");
    // An AWS error message fit for a log: first line, key ids and long encoded
    // runs masked, at most 300 characters.
    static std::string sanitiseAwsErrorMessage(const std::string& message);
    // A stored hash shaped like this secret's passwords that nothing matches;
    // checked for an unknown username (PasswordHasher::dummyHashLike).
    std::string dummyHashFor(const std::string& secret_name) const;

private:
    // Legacy `init:`: run it in a private DuckDB and read the S3 secret it
    // creates; else the main database's secret of that name (deprecated).
    std::optional<AwsAuthParams> credentialsFromLegacyInit(const AuthFromSecretManagerConfig& config);

private:
    std::shared_ptr<DatabaseManager> _db_manager;
    mutable std::mutex _mutex;
    std::unordered_map<std::string, std::vector<SecretUser>> _users;
    std::unordered_map<std::string, std::string> _dummy_hashes;
};


class AuthMiddleware {
public:
    struct context {
        bool authenticated = false;
        std::string username;
        std::string email;       // populated for OIDC auth, empty otherwise
        std::string auth_type;   // "basic", "bearer", or "oidc"
        std::vector<std::string> roles;
    };

    void initialize(std::shared_ptr<ConfigManager> config_manager);
    void before_handle(crow::request& req, crow::response& res, context& ctx);
    void after_handle(crow::request& req, crow::response& res, context& ctx);

private:
    std::shared_ptr<ConfigManager> config_manager;
    std::shared_ptr<DatabaseManager> db_manager;
    std::shared_ptr<AwsHelper> aws_helper;
    std::unordered_map<std::string, std::shared_ptr<OIDCAuthHandler>> oidc_handlers;  // Cache per issuer

    // Initalize AWS secrets
    void initializeAwsSecretsManager();
    // Drop the user-list tables older versions left in the main database.
    void dropLegacyUserTables(const AuthFromSecretManagerConfig& config);

    // Get or create OIDC handler for given config
    std::shared_ptr<OIDCAuthHandler> getOIDCHandler(const OIDCConfig& config);

    bool authenticateBasic(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx);
    bool authenticateBearer(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx);
    bool authenticateOIDC(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx);

    // Authentication backend methods
    bool authenticateInlineUsers(const std::string& username, const std::string& password,
                                 const EndpointConfig& endpoint, context& ctx);
    bool authenticateAwsSecrets(const std::string& username, const std::string& password,
                                const EndpointConfig& endpoint, context& ctx);
    
   
    

public:
    static std::string md5Hash(const std::string& input);
    static bool verifyPassword(const std::string& provided_password, const std::string& stored_password);
    // verifyPassword for a Basic-auth request: cached and concurrency-capped.
    static bool verifyCredentials(const std::string& username, const std::string& provided_password,
                                  const std::string& stored_password);
    // True when `required` is empty or `held` contains at least one of them.
    static bool hasRequiredRole(const std::vector<std::string>& required,
                                const std::vector<std::string>& held);
    // The `WWW-Authenticate` challenge of a 401 for the endpoint's configured
    // scheme: `Basic realm="flAPI"` for basic, `Bearer realm="flAPI"` for
    // bearer and oidc, with `error="invalid_token"` when a token was sent and
    // refused (RFC 6750 §3).
    static std::string challengeFor(const std::string& auth_type, bool credentials_sent);
};

} // namespace flapi