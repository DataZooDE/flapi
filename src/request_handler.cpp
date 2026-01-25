#include "request_handler.hpp"
#include "json_utils.hpp"
#include "content_negotiation.hpp"
#include "arrow_serializer.hpp"

#include <iostream>
#include <map>
#include <fstream>
#include "crow/json.h"
#include <sstream>
#include <algorithm>

namespace flapi {

RequestHandler::RequestHandler(std::shared_ptr<DatabaseManager> db_manager, std::shared_ptr<ConfigManager> config_manager)
    : db_manager(db_manager), config_manager(config_manager), validator(std::make_shared<RequestValidator>()) 
{
    defaultParams["offset"] = "0";
    defaultParams["limit"] = "100";
}

void RequestHandler::handleRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams) {
    // Skip if response already completed (e.g., by rate limit or auth middleware)
    if (res.is_completed()) {
        CROW_LOG_DEBUG << "Skipping request handler - response already completed";
        return;
    }

    CROW_LOG_DEBUG << "Handling request ["<< crow::method_name(req.method) << "]: " << endpoint.urlPath;

    switch (req.method) {
        case crow::HTTPMethod::Get:
            handleGetRequest(req, res, endpoint, pathParams);
            break;
        case crow::HTTPMethod::Post:
        case crow::HTTPMethod::Put:
        case crow::HTTPMethod::Patch:
            handleWriteRequest(req, res, endpoint, pathParams);
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

void RequestHandler::handleWriteRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams) {
    try {
        // Extract parameters from body, path, query (body takes precedence)
        auto params = combineWriteParameters(req, pathParams, endpoint);
        
        // Validate parameters
        auto validationErrors = validator->validateRequestParameters(endpoint.request_fields, params);
        
        // For write operations, enforce stricter validation if configured
        if (endpoint.operation.validate_before_write) {
            // Enforce all required fields
            for (const auto& field : endpoint.request_fields) {
                if (field.required && params.find(field.fieldName) == params.end()) {
                    validationErrors.push_back({field.fieldName, "Required field is missing"});
                }
            }
            
            // Reject unknown parameters when validate-before-write is enabled
            // This helps catch typos and incorrect parameter names (e.g., ProductName vs product_name)
            auto unknownParamErrors = validator->validateRequestFields(endpoint.request_fields, params);
            validationErrors.insert(validationErrors.end(), 
                                    unknownParamErrors.begin(), 
                                    unknownParamErrors.end());
        }
        
        // If there are validation errors, return a 400 Bad Request response
        if (!validationErrors.empty()) {
            crow::json::wvalue errorResponse;
            std::vector<crow::json::wvalue> errorMessages;
            for (const auto& error : validationErrors) {
                crow::json::wvalue errorJson;
                errorJson["field"] = error.fieldName;
                errorJson["message"] = error.errorMessage;
                errorMessages.push_back(std::move(errorJson));
            }
            errorResponse["errors"] = std::move(errorMessages);
            
            res.code = 400;
            res.set_header("Content-Type", "application/json");
            res.write(errorResponse.dump());
            res.end();
            return;
        }
        
        // Execute write operation (with or without transaction)
        WriteResult writeResult;
        if (endpoint.operation.transaction) {
            writeResult = db_manager->executeWriteInTransaction(endpoint, params);
        } else {
            writeResult = db_manager->executeWrite(endpoint, params);
        }
        
        // Prepare response
        res.set_header("Content-Type", "application/json");
        
        // Determine HTTP status code based on method
        if (req.method == crow::HTTPMethod::Post) {
            res.code = 201;  // Created
        } else {
            res.code = 200;  // OK for PUT/PATCH
        }
        
        // Handle cache operations after successful write
        handleCacheAfterWrite(endpoint, writeResult);
        
        // Build response body
        crow::json::wvalue response;
        response["rows_affected"] = static_cast<int64_t>(writeResult.rows_affected);
        
        // Include returned data if available and configured
        if (writeResult.returned_data.has_value() && endpoint.operation.returns_data) {
            response["data"] = std::move(*writeResult.returned_data);
        } else if (writeResult.returned_data.has_value()) {
            // Include returned data even if not explicitly configured (helpful for debugging)
            response["data"] = std::move(*writeResult.returned_data);
        }
        
        res.write(response.dump());
        res.end();
        return;
        
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error handling write request: " << e.what();
        res.code = 500;
        res.body = std::string("Internal Server Error: ") + e.what();
        res.end();
        return;
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
        auto params = combineParameters(req, defaultParams, pathParams, endpoint);
        
        // First validate the known parameters
        auto validationErrors = validator->validateRequestParameters(endpoint.request_fields, params);
        
        // If strict parameter checking is enabled, validate for unknown parameters
        if (endpoint.request_fields_validation) {
            auto unknownParamErrors = validator->validateRequestFields(endpoint.request_fields, params);
            validationErrors.insert(validationErrors.end(), 
                                    unknownParamErrors.begin(), 
                                    unknownParamErrors.end());
        }
        
        // If there are validation errors, return a 400 Bad Request response
        if (!validationErrors.empty()) {
            crow::json::wvalue errorResponse;
            std::vector<crow::json::wvalue> errorMessages;
            for (const auto& error : validationErrors) {
                crow::json::wvalue errorJson;
                errorJson["field"] = error.fieldName;
                errorJson["message"] = error.errorMessage;
                errorMessages.push_back(std::move(errorJson));
            }
            errorResponse["errors"] = std::move(errorMessages);
            
            res.code = 400;
            res.set_header("Content-Type", "application/json");
            res.write(errorResponse.dump());
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

        // Content negotiation - determine response format
        std::string acceptHeader = req.get_header_value("Accept");
        // Get format from query parameter using Crow's url_params
        // req.url only contains the path, query params are in url_params
        std::string formatParam;
        char* formatValue = req.url_params.get("format");
        if (formatValue != nullptr) {
            formatParam = formatValue;
        }

        // Configure endpoint format support
        ResponseFormatConfig formatConfig;
        formatConfig.formats = {"json", "csv"};  // Default formats
        formatConfig.defaultFormat = "json";
        formatConfig.arrowEnabled = true;  // Enable Arrow globally
        if (formatConfig.arrowEnabled) {
            formatConfig.formats.push_back("arrow");
        }

        auto negotiation = negotiateContentType(acceptHeader, formatParam, formatConfig);

        // Handle Arrow format
        if (negotiation.format == ResponseFormat::ARROW_STREAM) {
            auto executor = db_manager->executeQueryRaw(endpoint, params);

            ArrowSerializerConfig arrowConfig;
            arrowConfig.codec = negotiation.codec;  // Use negotiated codec (lz4, zstd, or empty)

            auto arrowResult = serializeToArrowIPC(executor->result, arrowConfig);

            if (!arrowResult.success) {
                res.code = 500;
                res.set_header("Content-Type", "application/json");
                crow::json::wvalue errorResponse;
                errorResponse["error"] = "Arrow serialization failed";
                errorResponse["message"] = arrowResult.errorMessage;
                res.write(errorResponse.dump());
                res.end();
                return;
            }

            // Disable Crow's GZIP compression for Arrow responses
            // Arrow has its own compression (LZ4/ZSTD) and GZIP compression
            // would corrupt the IPC stream format
            res.compressed = false;

            res.set_header("Content-Type", "application/vnd.apache.arrow.stream");
            res.set_header("X-Arrow-Rows", std::to_string(arrowResult.rowCount));
            res.set_header("X-Arrow-Batches", std::to_string(arrowResult.batchCount));
            res.set_header("X-Arrow-Bytes", std::to_string(arrowResult.bytesWritten));
            if (!arrowConfig.codec.empty()) {
                res.set_header("X-Arrow-Codec", arrowConfig.codec);
            }

            // Set Content-Length explicitly for binary Arrow data
            // Required since we disabled compression - lets client know exact response size
            // Note: We do NOT erase Transfer-Encoding; let Crow manage framing
            res.set_header("Content-Length", std::to_string(arrowResult.data.size()));

            // Set body directly - more efficient than write() as it avoids extra string copy
            res.body.assign(
                reinterpret_cast<const char*>(arrowResult.data.data()),
                arrowResult.data.size()
            );
            res.end();
            return;
        }

        // Execute query for JSON/CSV (existing path)
        QueryResult queryResult = db_manager->executeQuery(endpoint, params, endpoint.with_pagination);

        // Handle CSV format
        if (negotiation.format == ResponseFormat::CSV) {
            std::string csvData = convertToCSV(queryResult);
            res.set_header("Content-Type", "text/csv");
            res.write(csvData);
        } else {
            // JSON format (default)
            res.set_header("Content-Type", "application/json");

            if (endpoint.with_pagination) {
                crow::json::wvalue response;
                response["data"] = std::move(queryResult.data);
                response["next"] = createNextUrl(req, queryResult);
                response["total_count"] = queryResult.total_count;

                res.write(response.dump());
            } else {
                res.write(queryResult.data.dump());
            }
        }

        res.add_header("X-Total-Count", std::to_string(queryResult.total_count));
        res.add_header("X-Offset", std::to_string(offset));
        res.add_header("X-Limit", std::to_string(limit));
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

bool RequestHandler::isCacheDetailsRequest(const crow::request& req, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams) {
    return req.url.find("/cache") != std::string::npos;
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

std::map<std::string, std::string> RequestHandler::combineParameters(const crow::request& req, const std::map<std::string, std::string>& defaultParams, const std::map<std::string, std::string>& pathParams, const EndpointConfig& endpoint) {
    std::map<std::string, std::string> params = defaultParams;

    for (const auto& key : pathParams) {
        params[key.first] = key.second;
    }

    for (const auto& field : endpoint.request_fields) {
        if (!field.defaultValue.empty() && params.find(field.fieldName) == params.end()) {
            params[field.fieldName] = field.defaultValue;
        }
    }

    for (const auto& key : req.url_params.keys()) {
        params[key] = req.url_params.get(key);
    }
    return params;
}

std::map<std::string, std::string> RequestHandler::combineWriteParameters(const crow::request& req,
                                                                          const std::map<std::string, std::string>& pathParams,
                                                                          const EndpointConfig& endpoint) {
    std::map<std::string, std::string> params = defaultParams;

    // Add path parameters first (lowest precedence for body params)
    for (const auto& [key, value] : pathParams) {
        params[key] = value;
    }

    // Extract parameters from JSON body (highest precedence for write operations)
    if (req.method == crow::HTTPMethod::Post || 
        req.method == crow::HTTPMethod::Put || 
        req.method == crow::HTTPMethod::Patch) {
        
        if (!req.body.empty()) {
            try {
                auto bodyJson = crow::json::load(req.body);
                if (bodyJson) {
                    // Helper function to convert JSON value to string
                    auto jsonValueToString = [&bodyJson, &req](const std::string& key) -> std::string {
                        if (!bodyJson.has(key)) {
                            return "";
                        }
                        auto jsonVal = bodyJson[key];
                        std::string value;
                        if (jsonVal.t() == crow::json::type::String) {
                            value = jsonVal.s();
                        } else if (jsonVal.t() == crow::json::type::Number) {
                            // Try integer first, then double
                            try {
                                value = std::to_string(jsonVal.i());
                            } catch (...) {
                                value = std::to_string(jsonVal.d());
                            }
                        } else if (jsonVal.t() == crow::json::type::True) {
                            value = "true";
                        } else if (jsonVal.t() == crow::json::type::False) {
                            value = "false";
                        } else if (jsonVal.t() == crow::json::type::Null) {
                            value = "";
                        } else if (jsonVal.t() == crow::json::type::Object || jsonVal.t() == crow::json::type::List) {
                            // For objects and arrays, extract JSON string from the original body
                            try {
                                auto parsedBody = crow::json::load(req.body);
                                if (parsedBody && parsedBody.has(key)) {
                                    // Create a wvalue with just this field and dump it
                                    crow::json::wvalue tempObj;
                                    auto fieldRval = parsedBody[key];
                                    if (fieldRval.t() == crow::json::type::Object) {
                                        for (const auto& k : fieldRval.keys()) {
                                            std::string keyStr = k;
                                            auto v = fieldRval[keyStr];
                                            if (v.t() == crow::json::type::String) {
                                                tempObj[keyStr] = v.s();
                                            } else if (v.t() == crow::json::type::Number) {
                                                try {
                                                    tempObj[keyStr] = v.i();
                                                } catch (...) {
                                                    tempObj[keyStr] = v.d();
                                                }
                                            } else if (v.t() == crow::json::type::True) {
                                                tempObj[keyStr] = true;
                                            } else if (v.t() == crow::json::type::False) {
                                                tempObj[keyStr] = false;
                                            } else {
                                                tempObj[keyStr] = nullptr;
                                            }
                                        }
                                    } else if (fieldRval.t() == crow::json::type::List) {
                                        std::vector<crow::json::wvalue> arr;
                                        for (size_t i = 0; i < fieldRval.size(); i++) {
                                            auto v = fieldRval[i];
                                            if (v.t() == crow::json::type::String) {
                                                arr.push_back(crow::json::wvalue(v.s()));
                                            } else if (v.t() == crow::json::type::Number) {
                                                try {
                                                    arr.push_back(crow::json::wvalue(v.i()));
                                                } catch (...) {
                                                    arr.push_back(crow::json::wvalue(v.d()));
                                                }
                                            } else if (v.t() == crow::json::type::True) {
                                                arr.push_back(crow::json::wvalue(true));
                                            } else if (v.t() == crow::json::type::False) {
                                                arr.push_back(crow::json::wvalue(false));
                                            } else {
                                                arr.push_back(crow::json::wvalue(nullptr));
                                            }
                                        }
                                        tempObj = crow::json::wvalue(arr);
                                    }
                                    value = tempObj.dump();
                                } else {
                                    value = "{}";
                                }
                            } catch (...) {
                                value = "{}";  // Fallback for complex types
                            }
                        } else {
                            // Fallback: try to get string representation
                            value = jsonVal.s();
                        }
                        return value;
                    };
                    
                    // FIRST PASS: Extract ALL fields from JSON body (including unknown ones)
                    // This allows validation to detect unknown parameters later
                    for (const auto& key : bodyJson.keys()) {
                        std::string fieldName = key;
                        std::string value = jsonValueToString(fieldName);
                        if (!value.empty() || bodyJson[fieldName].t() == crow::json::type::Null) {
                            params[fieldName] = value;
                        }
                    }
                }
            } catch (const std::exception& e) {
                CROW_LOG_WARNING << "Failed to parse JSON body: " << e.what();
                // Continue with other parameter sources
            }
        }
    }

    // Apply endpoint defaults for defined fields before query params
    // This ensures configured defaults take precedence over query parameters
    for (const auto& field : endpoint.request_fields) {
        if (!field.defaultValue.empty() && params.find(field.fieldName) == params.end()) {
            params[field.fieldName] = field.defaultValue;
        }
    }

    // Add query parameters (for backward compatibility and flexibility)
    // Only adds parameters not already set by body or endpoint defaults
    for (const auto& key : req.url_params.keys()) {
        if (params.find(key) == params.end()) {
            params[key] = req.url_params.get(key);
        }
    }

    return params;
}

void RequestHandler::handleCacheAfterWrite(const EndpointConfig& endpoint, const WriteResult& writeResult) {
    // Only process if cache is enabled for this endpoint
    if (!db_manager->isCacheEnabled(endpoint)) {
        return;
    }
    
    // Invalidate cache if configured
    if (endpoint.cache.invalidate_on_write) {
        db_manager->invalidateCache(endpoint);
        CROW_LOG_DEBUG << "Cache invalidated after write operation for endpoint: " << endpoint.getIdentifier();
    }
    
    // Refresh cache if configured
    if (endpoint.cache.refresh_on_write) {
        // Note: Cache refresh typically involves running the cache sync
        // For now, we'll trigger a manual refresh via the cache manager
        // This may need to be enhanced based on the actual cache refresh mechanism
        auto cache_manager = db_manager->getCacheManager();
        if (cache_manager) {
            // The actual refresh mechanism would depend on CacheManager's API
            // For now, we'll log that refresh was requested
            CROW_LOG_DEBUG << "Cache refresh requested after write operation for endpoint: " << endpoint.getIdentifier();
            // In a full implementation, we would call a refresh method here
            // cache_manager->refreshCacheForEndpoint(endpoint);
        }
    }
    
    // If neither option is set, let the heartbeat handle refresh naturally
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

} // namespace flapi