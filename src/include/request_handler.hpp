#pragma once

#include <crow.h>
#include <map>
#include <string>
#include "config_manager.hpp"
#include "database_manager.hpp"
#include "sql_template_processor.hpp"
#include "request_validator.hpp"

namespace flapi {

class RequestHandler {
public:
    RequestHandler(std::shared_ptr<DatabaseManager> db_manager, std::shared_ptr<ConfigManager> config_manager);
    void handleRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams);

private:
    std::map<std::string, std::string> defaultParams;

    void handleDeleteRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams);
    void handleGetRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams);

    std::map<std::string, std::string> combineParameters(
        const crow::request& req, 
        const std::map<std::string, std::string>& defaultParams, 
        const std::map<std::string, std::string>& pathParams,
        const EndpointConfig& endpoint);
    QueryResult executeQuery(const EndpointConfig& endpoint, const std::map<std::string, std::string>& params);
    bool validateRequestParameters(const std::vector<RequestFieldConfig>& requestFields, const std::map<std::string, std::string>& params);
    void parsePaginationParams(std::map<std::string, std::string>& params);
    std::string convertToCSV(const QueryResult& queryResult);
    void csvHeaderFromExampleObject(const crow::json::wvalue& obj, std::ostringstream& csv);
    void serializeObjectToCsvLine(const crow::json::wvalue& obj, std::ostringstream& csv);
    std::string escapeCSV(const std::string& str);
    std::string createNextUrl(const crow::request& req, const QueryResult& queryResult);

    std::shared_ptr<DatabaseManager> db_manager;
    std::shared_ptr<ConfigManager> config_manager;
    std::shared_ptr<RequestValidator> validator;
};

} // namespace flapi