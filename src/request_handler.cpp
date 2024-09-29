#include "request_handler.hpp"
#include <iostream>
#include <map>
#include <fstream>
#include "crow/json.h"
#include <sstream>
#include <algorithm>

namespace flapi {

RequestHandler::RequestHandler(std::shared_ptr<DatabaseManager> db_manager, std::shared_ptr<ConfigManager> config_manager)
    : db_manager(db_manager), config_manager(config_manager) 
{
    defaultParams["offset"] = "0";
    defaultParams["limit"] = "100";
}

void RequestHandler::handleRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams) {
    CROW_LOG_DEBUG << "Handling request ["<< crow::method_name(req.method) << "]: " << endpoint.urlPath;

    switch (req.method) {
        case crow::HTTPMethod::Get:
            handleGetRequest(req, res, endpoint, pathParams);
            break;
        case crow::HTTPMethod::Delete:
            handleDeleteRequest(req, res, endpoint, pathParams);
            break;
        default:
            res.code = 405;
            res.body = "Method not allowed";
            res.end();
            break;
    }
}

void RequestHandler::handleDeleteRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams) {
    if (db_manager->isCacheEnabled(endpoint)) {
        auto ret = db_manager->invalidateCache(endpoint);
        if (ret) {
            res.code = 200;
            res.body = "Cache invalidated";
            res.end();
        }
        else {
            res.code = 500;
            res.body = "Internal Server Error";
            res.end();
        }
    }
    else {
        res.code = 405;
        res.body = "Method not allowed";
        res.end();
    }
}

void RequestHandler::handleGetRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams) {
    try {
        auto params = combineParameters(req, defaultParams, pathParams);
        if (!validateRequestParameters(endpoint.requestFields, params)) {
            res.code = 400;
            res.body = "Invalid request parameters";
            res.end();
            return;
        }

        // Parse pagination parameters
        int64_t offset = 0;
        int64_t limit = 100;
        try {
            if (params.count("offset")) offset = std::stoll(params["offset"]);
            if (params.count("limit")) limit = std::stoll(params["limit"]);
        } catch (const std::exception& e) {
            res.code = 400;
            res.body = "Invalid pagination parameters";
            res.end();
            return;
        }

        QueryResult queryResult = db_manager->executeQuery(endpoint, params);

        // Determine the response format
        std::string acceptHeader = req.get_header_value("Accept");
        bool returnCSV = (acceptHeader.find("text/csv") != std::string::npos) ||
                      (req.url.find(".csv") != std::string::npos);

        if (returnCSV) {
            std::string csvData = convertToCSV(queryResult);
            res.set_header("Content-Type", "text/csv");
            res.write(csvData);
        } else {
            // Prepare the JSON response
            crow::json::wvalue response;
            response["data"] = std::move(queryResult.data);
            response["next"] = createNextUrl(req, queryResult);
            response["total_count"] = queryResult.total_count;

            res.set_header("Content-Type", "application/json");
            res.write(response.dump());
        }

        res.add_header("X-Total-Count", std::to_string(queryResult.total_count));
        res.add_header("X-Next", createNextUrl(req, queryResult));
        res.end();
        return;
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error handling request: " << e.what();
        res.code = 500;
        res.body = std::string("Internal Server Error: ") + e.what();
        res.end();
        return;
    }
}

void RequestHandler::parsePaginationParams(std::map<std::string, std::string>& params) {
    int64_t offset = 0;
    int64_t limit = 100;
    try {
        if (params.count("offset")) offset = std::stoll(params["offset"]);
        if (params.count("limit")) limit = std::stoll(params["limit"]);
        
        // Add parsed values back to params
        params["offset"] = std::to_string(offset);
        params["limit"] = std::to_string(limit);
    } catch (const std::exception& e) {
        throw std::runtime_error("Invalid pagination parameters");
    }
}

std::string RequestHandler::createNextUrl(const crow::request& req, const QueryResult& queryResult) 
{
    if (queryResult.next.empty()) {
        return "";
    }

    std::string baseUrl = req.url.substr(0, req.url.find('?'));
    if (baseUrl.empty()) 
        baseUrl = req.url;
    return baseUrl + queryResult.next;
}

std::map<std::string, std::string> RequestHandler::combineParameters(const crow::request& req, const std::map<std::string, std::string>& defaultParams, const std::map<std::string, std::string>& pathParams) {
    std::map<std::string, std::string> params = defaultParams;

    for (const auto& key : pathParams) {
        params[key.first] = key.second;
    }

    for (const auto& key : req.url_params.keys()) {
        params[key] = req.url_params.get(key);
    }
    return params;
}

QueryResult RequestHandler::executeQuery(const EndpointConfig& endpoint, const std::map<std::string, std::string>& orig_params) {
    std::map<std::string, std::string> params = orig_params;
    return db_manager->executeQuery(endpoint, params);
}

std::string RequestHandler::convertToCSV(const QueryResult& queryResult) 
{
    std::ostringstream csv;

    if (queryResult.data.size() == 0) {
        return csv.str();
    }

    csvHeaderFromExampleObject(queryResult.data[0], csv);
    for (size_t i = 0; i < queryResult.data.size(); i++) {
        serializeObjectToCsvLine(queryResult.data[i], csv);
    }

    return csv.str();
}

void RequestHandler::csvHeaderFromExampleObject(const crow::json::wvalue& obj, std::ostringstream& csv) {
    bool first = true;
    for (const auto& key : obj.keys()) {
        if (!first) csv << ",";
        csv << escapeCSV(key);
        first = false;
    }
    csv << "\n";
}

void RequestHandler::serializeObjectToCsvLine(const crow::json::wvalue& obj, std::ostringstream& csv) {     
    bool first = true;
    for (const auto& key : obj.keys()) {
        if (!first) csv << ",";
        csv << obj[key].dump();
        first = false;
    }
    csv << "\n";
}

std::string RequestHandler::escapeCSV(const std::string& str) {
    if (str.find_first_of(",\"\n") == std::string::npos) {
        return str;
    }

    std::ostringstream result;
    result << '"';
    for (char c : str) {
        if (c == '"') {
            result << "\"\"";
        } else {
            result << c;
        }
    }
    result << '"';
    return result.str();
}

bool RequestHandler::validateRequestParameters(const std::vector<RequestFieldConfig>& requestFields, const std::map<std::string, std::string>& params) {
    for (const auto& field : requestFields) {
        auto it = params.find(field.fieldName);
        if (it == params.end()) {
            if (field.required) {
                CROW_LOG_ERROR << "Missing required parameter: " << field.fieldName;
                return false;
            }
        } else {
            if (field.required && it->second.empty()) {
                CROW_LOG_ERROR << "Empty required parameter: " << field.fieldName;
                return false;
            }
        }
    }
    return true;
}

} // namespace flapi