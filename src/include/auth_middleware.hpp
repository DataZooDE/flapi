#pragma once

#include <crow/http_request.h>
#include <crow/http_response.h>
#include <memory>
#include <string>

#include "config_manager.hpp"
#include "database_manager.hpp"


namespace flapi {

struct AwsAuthParams {
    std::string access_key;
    std::string secret_key;
    std::string session_token;
    std::string region;
};

class AwsHelper {
public:
    AwsHelper(std::shared_ptr<DatabaseManager> db_manager);
    
    void refreshSecretJson(const std::string& secret_name, const std::string& secret_table);
    std::string getSecretJson(const std::string& secret_name);
    void persistSecretJson(const std::string& secret_table, const std::string& secret_json);

private:

    std::optional<AwsAuthParams> tryGetS3AuthParams(const std::string& secret_name);

    std::shared_ptr<DatabaseManager> db_manager;
};


class AuthMiddleware {
public:
    struct context {
        bool authenticated = false;
        std::string username;
        std::vector<std::string> roles;
    };

    void initialize(std::shared_ptr<ConfigManager> config_manager);
    void before_handle(crow::request& req, crow::response& res, context& ctx);
    void after_handle(crow::request& req, crow::response& res, context& ctx);

private:
    std::shared_ptr<ConfigManager> config_manager;
    std::shared_ptr<DatabaseManager> db_manager;
    std::shared_ptr<AwsHelper> aws_helper;

    // Initalize AWS secrets
    void initializeAwsSecretsManager();

    bool authenticateBasic(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx);
    bool authenticateBearer(const std::string& auth_header, const EndpointConfig& endpoint, context& ctx);
    
    // Authentication backend methods
    bool authenticateInlineUsers(const std::string& username, const std::string& password,
                                 const EndpointConfig& endpoint, context& ctx);
    bool authenticateAwsSecrets(const std::string& username, const std::string& password,
                                const EndpointConfig& endpoint, context& ctx);
    
   
    

public:
    static std::string md5Hash(const std::string& input);
    static bool verifyPassword(const std::string& provided_password, const std::string& stored_password);
};

} // namespace flapi