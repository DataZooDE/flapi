#include "request_handler.hpp"
#include "auth_params.hpp"
#include "template_secrets.hpp"
#include "json_utils.hpp"
#include "content_negotiation.hpp"
#include "arrow_serializer.hpp"
#include "query_executor.hpp"
#include "request_context.hpp"

#include <iostream>
#include <map>
#include <fstream>
#include "crow/json.h"
#include <sstream>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

namespace flapi {

namespace {

// Pagination values go into `LIMIT {{params.limit}}` / `OFFSET ...`, so they must be a plain
// non-negative integer - the WHOLE string. std::stoll accepted a numeric prefix, so
// `limit=1;SELECT 1`, `limit=5 ` and `limit=1 OR 1=1` reached the SQL text (#191).
bool ParsePlainNonNegativeInt(const std::string& text, int64_t& out) {
    if (text.empty() || text.size() > 15) {
        return false;
    }
    int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

// DuckLake's answers to time travel (`AT (VERSION => n)` / `AT (TIMESTAMP => t)`)
// to a snapshot that never existed or that retention has expired.
bool isSnapshotNotFoundMessage(const std::string& message) {
    return message.find("No snapshot found at version") != std::string::npos ||
           message.find("No snapshot found at timestamp") != std::string::npos ||
           message.find("does not exist at version") != std::string::npos ||
           message.find("does not exist at timestamp") != std::string::npos;
}

// `AT (VERSION => n)` for a snapshot from before the cache's schema existed:
// DuckDB names the schema, not the version - "Table with name "sales.t" does
// not exist because schema "sales" does not exist" - so it was a 500. A ready
// cache's schema exists NOW (readiness gates the request), so on a cached
// endpoint this is the past being asked for.
bool isSchemaBeforeSnapshotMessage(const std::string& message, const EndpointConfig& endpoint) {
    const std::string schema = endpoint.cache.schema.empty() ? "main" : endpoint.cache.schema;
    return message.find("does not exist because schema \"" + schema + "\" does not exist") != std::string::npos;
}

void writeInternalError(crow::response& res) {
    crow::json::wvalue body;
    body["error"] = "Internal Server Error";
    body["message"] = "The request failed on the server. The details are in the server log "
                      "under this request id.";
    if (const auto* rc = RequestContextScope::current()) {
        body["request_id"] = std::string(rc->requestIdView());
    }
    res.code = 500;
    res.set_header("Content-Type", "application/json");
    res.body = body.dump();
    res.end();
}

}  // namespace

RequestHandler::RequestHandler(std::shared_ptr<DatabaseManager> db_manager, std::shared_ptr<ConfigManager> config_manager)
    : db_manager(db_manager), config_manager(config_manager), validator(std::make_shared<RequestValidator>()) 
{
    defaultParams["offset"] = "0";
    defaultParams["limit"] = "100";
}

void RequestHandler::handleRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams, const std::map<std::string, std::string>& authParams) {
    // Skip if response already completed (e.g., by rate limit or auth middleware)
    if (res.is_completed()) {
        CROW_LOG_DEBUG << "Skipping request handler - response already completed";
        return;
    }

    CROW_LOG_DEBUG << "Handling request ["<< crow::method_name(req.method) << "]: " << endpoint.urlPath;

    if (auto cache_manager = db_manager->getCacheManager()) {
        if (auto readiness = cache_manager->readinessBlock(config_manager, endpoint)) {
            auto block_response = CacheManager::readinessBlockResponse(*readiness);
            res.code = block_response.code;
            for (const auto& header : block_response.headers) {
                res.set_header(header.first, header.second);
            }
            res.write(block_response.body);
            res.end();
            return;
        }
    }

    switch (req.method) {
        case crow::HTTPMethod::Get:
            handleGetRequest(req, res, endpoint, pathParams, authParams);
            break;
        case crow::HTTPMethod::Post:
        case crow::HTTPMethod::Put:
        case crow::HTTPMethod::Patch:
        case crow::HTTPMethod::Delete:
            // `operation.type: read` on a POST (or PUT/PATCH/DELETE) is a read
            // that takes its input as a body (#271): validators, cache serving
            // and the pagination envelope, as MCP and /doc.yaml already treat
            // it. These methods default to write, so only an explicit
            // `type: read` gets here.
            if (endpoint.operation.type == OperationConfig::Read) {
                handleGetRequest(req, res, endpoint, pathParams, authParams);
                break;
            }
            // DELETE runs its template like every other write. It used to skip
            // the template entirely: a cached endpoint got its cache refreshed
            // ("Cache invalidated"), anything else got 405, and the row the
            // caller asked to delete was never touched.
            handleWriteRequest(req, res, endpoint, pathParams, authParams);
            break;
        default:
            res.code = 405;
            res.body = "Method not allowed";
            res.end();
            break;
    }
}

void RequestHandler::handleWriteRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams, const std::map<std::string, std::string>& authParams) {
    // Declared out here so the catch block can scrub the error with the
    // secrets this request's template could interpolate.
    std::map<std::string, std::string> params;
    try {
        // Extract parameters from body, path, query (body takes precedence)
        params = combineWriteParameters(req, pathParams, endpoint);
        // Inject auth context as reserved params for template rendering
        for (const auto& [k, v] : authParams) {
            params[k] = v;
        }

        // Validate parameters
        auto validationErrors = validator->validateRequestParameters(endpoint.request_fields, params);
        
        // For write operations, enforce stricter validation if configured
        if (endpoint.operation.validate_before_write) {
            // Required fields are already enforced by validateRequestParameters;
            // adding them again here listed every missing field twice.
            
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
        handleCacheAfterWrite(db_manager, endpoint);
        
        res.write(formatWriteResultJson(writeResult, endpoint.operation.returns_data));
        res.end();
        return;
        
    } catch (const BadRequestError& e) {
        CROW_LOG_WARNING << "Bad request (write): " << e.what();
        crow::json::wvalue body;
        body["error"] = std::string(e.what());
        res.code = 400;
        res.set_header("Content-Type", "application/json");
        res.write(body.dump());
        res.end();
        return;
    } catch (const std::exception& e) {
        // Lock contention is transient, not a fault in the request. flAPI has
        // already retried with backoff; if the lock outlived that budget the
        // honest answer is "try again", which is 503 + Retry-After. Returning
        // 500 told clients their request was wrong and discouraged the retry
        // that would have worked.
        if (isLockContentionMessage(e.what())) {
            CROW_LOG_WARNING << "write contended on a locked backend: " << e.what();
            res.code = 503;
            res.set_header("Retry-After", "1");
            res.set_header("Content-Type", "application/json");
            crow::json::wvalue body;
            body["error"] = "The database is busy; retry shortly.";
            res.write(body.dump());
            res.end();
            return;
        }
        // Scrubbed BEFORE it is logged, not just before it is returned.
        // A DuckDB error quotes the failing statement, and logs are shipped,
        // indexed and read by people who are not entitled to the
        // credentials a template interpolated.
        const auto secrets = collectTemplateSecrets(config_manager.get(), endpoint, params);
        const std::string public_detail =
            publicErrorMessage("Error handling write request", e.what(), secrets);
        CROW_LOG_ERROR << public_detail;
        // The body carries NO part of the failure text. A database error
        // quotes the statement that failed, which IS the rendered template,
        // and names absolute file paths ("No files found that match the
        // pattern /srv/app/data/x.parquet"). Secret scrubbing (above, for the
        // log) removes credentials but not the SQL or the paths, which map the
        // deployment for anyone who can trigger an error. The detail stays in
        // the server log, which carries the same request id as this body.
        writeInternalError(res);
        return;
    }
}

void RequestHandler::handleGetRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams, const std::map<std::string, std::string>& authParams) {
    // Declared out here so the catch block can scrub the error with the
    // secrets this request's template could interpolate.
    std::map<std::string, std::string> params;
    try {
        // A read over POST (`operation.type: read`, #271) takes its parameters
        // from the JSON body as a write does; combineParameters reads only the
        // query string, and `field-in: body` fields may not come from there.
        const bool body_read = req.method == crow::HTTPMethod::Post || req.method == crow::HTTPMethod::Put ||
                               req.method == crow::HTTPMethod::Patch || req.method == crow::HTTPMethod::Delete;
        params = body_read ? combineWriteParameters(req, pathParams, endpoint)
                           : combineParameters(req, defaultParams, pathParams, endpoint);
        // Inject auth context as reserved params for template rendering
        for (const auto& [k, v] : authParams) {
            params[k] = v;
        }
        
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
        if ((params.count("offset") && !ParsePlainNonNegativeInt(params["offset"], offset)) ||
            (params.count("limit") && !ParsePlainNonNegativeInt(params["limit"], limit))) {
            res.code = 400;
            res.body = "Invalid pagination parameters: offset and limit must be non-negative integers";
            res.end();
            return;
        }
        // The template renders these, so hand it the normalised numbers, not the raw text.
        params["offset"] = std::to_string(offset);
        params["limit"] = std::to_string(limit);

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
            if (!negotiation.codec.empty()) {
                res.code = 406;
                res.set_header("Content-Type", "application/json");
                crow::json::wvalue errorResponse;
                errorResponse["error"] = "Arrow compression not supported";
                errorResponse["codec"] = negotiation.codec;
                res.write(errorResponse.dump());
                res.end();
                return;
            }

            // Only the client's own ?limit= / ?offset= page an Arrow stream (already validated by
            // parsePaginationParams); the JSON default page of 100 never truncates it.
            std::optional<int64_t> arrow_limit;
            std::optional<int64_t> arrow_offset;
            if (req.url_params.get("limit") != nullptr) {
                arrow_limit = std::stoll(params["limit"]);
            }
            if (req.url_params.get("offset") != nullptr) {
                arrow_offset = std::stoll(params["offset"]);
            }
            auto executor = db_manager->executeQueryRaw(endpoint, params, arrow_limit, arrow_offset);

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

        // Rows are written straight into the response text as they are converted: a result is
        // never held as a JSON tree and copies of it (a 9 MB response peaked at 721 MB that way).
        const bool csv = negotiation.format == ResponseFormat::CSV;
        std::string rows_text = csv ? std::string() : std::string("[");
        std::vector<std::string> row_columns;
        bool first_row = true;
        ResultSink sink;
        sink.reset = [&]() {
            rows_text = csv ? std::string() : std::string("[");
            row_columns.clear();
            first_row = true;
        };
        sink.begin = [&](const std::vector<std::string>& columns) { row_columns = columns; };
        if (!csv) {
            sink.json_rows = &rows_text;   // rows go from the vectors straight into the text
        }
        sink.row = [&](const crow::json::wvalue& row) {
            if (csv) {
                appendCSVRow(row, row_columns, first_row, rows_text);
            } else {
                if (!first_row) {
                    rows_text += ',';
                }
                QueryResult::appendRow(row, row_columns, rows_text);
            }
            first_row = false;
        };
        QueryResult queryResult = db_manager->executeQuery(
            endpoint, params, endpoint.with_pagination,
            endpoint.total_count ? DatabaseManager::TotalCount::Exact : DatabaseManager::TotalCount::Skip,
            &sink);
        if (!csv) {
            rows_text += ']';
        }
        // limit=0 asks for the count alone. Its "next page" starts at the same
        // offset, so `next` pointed at the request itself and a client
        // following it looped forever.
        if (limit == 0) {
            queryResult.next.clear();
        }

        // Handle CSV format
        if (csv) {
            res.set_header("Content-Type", "text/csv");
            res.body = std::move(rows_text);
        } else {
            // JSON format (default)
            res.set_header("Content-Type", "application/json");

            // Assembled by hand around dumpData(): rows keep their SELECT
            // column order and doubles their shortest form, which a
            // crow::json::wvalue (an unordered map, 21-digit doubles) loses.
            if (endpoint.with_pagination && !endpoint.total_count) {
                // `total-count: false`: no COUNT ran, so there is no total to report.
                res.body = "{\"data\":" + rows_text +
                           ",\"next\":" + crow::json::wvalue(createNextUrl(req, queryResult)).dump() + "}";
            } else if (endpoint.with_pagination) {
                std::string body;
                const std::string next = crow::json::wvalue(createNextUrl(req, queryResult)).dump();
                body.reserve(rows_text.size() + next.size() + 48);
                body += "{\"data\":";
                body += rows_text;
                body += ",\"next\":";
                body += next;
                body += ",\"total_count\":";
                body += std::to_string(queryResult.total_count);
                body += '}';
                res.body = std::move(body);
            } else {
                res.body = std::move(rows_text);
            }
        }

        if (endpoint.total_count) {
            res.add_header("X-Total-Count", std::to_string(queryResult.total_count));
        }
        res.add_header("X-Offset", std::to_string(offset));
        res.add_header("X-Limit", std::to_string(limit));
        res.add_header("X-Next", createNextUrl(req, queryResult));
        res.end();
        return;
    } catch (const BadRequestError& e) {
        CROW_LOG_WARNING << "Bad request (read): " << e.what();
        crow::json::wvalue body;
        body["error"] = std::string(e.what());
        res.code = 400;
        res.set_header("Content-Type", "application/json");
        res.write(body.dump());
        res.end();
        return;
    } catch (const std::exception& e) {
        // Reads are collateral damage in single-writer contention - a GET
        // failing because an unrelated POST held a lock is the least defensible
        // outcome of the lot. Same treatment as the write path.
        if (isLockContentionMessage(e.what())) {
            CROW_LOG_WARNING << "read contended on a locked backend: " << e.what();
            res.code = 503;
            res.set_header("Retry-After", "1");
            res.set_header("Content-Type", "application/json");
            crow::json::wvalue body;
            body["error"] = "The database is busy; retry shortly.";
            res.write(body.dump());
            res.end();
            return;
        }
        // DuckLake 1.1 (DuckDB 2.0) refuses `AT (TIMESTAMP => t)` with t in the future, where the
        // 1.0 extension answered the latest snapshot. The caller's value is the problem: a 400.
        if (endpoint.cache.enabled &&
            std::string(e.what()).find("Cannot time travel to a future timestamp") != std::string::npos) {
            CROW_LOG_INFO << "time travel to a future timestamp: " << e.what();
            res.code = 400;
            res.set_header("Content-Type", "application/json");
            crow::json::wvalue body;
            body["error"] = "snapshot_in_future";
            body["message"] = "The requested time-travel timestamp is in the future; ask for the current data "
                              "without a timestamp, or for a time at or before now";
            res.write(body.dump());
            res.end();
            return;
        }
        // Time travel on a cached endpoint to a snapshot that does not exist (or
        // that retention expired) is the caller asking for something absent, not
        // a server fault: it was a 500.
        if (endpoint.cache.enabled &&
            (isSnapshotNotFoundMessage(e.what()) || isSchemaBeforeSnapshotMessage(e.what(), endpoint))) {
            CROW_LOG_INFO << "time travel to an unknown snapshot: " << e.what();
            res.code = 404;
            res.set_header("Content-Type", "application/json");
            crow::json::wvalue body;
            body["error"] = "snapshot_not_found";
            body["message"] = "The requested cache snapshot does not exist; it may have been expired by the "
                              "retention policy";
            res.write(body.dump());
            res.end();
            return;
        }
        // Scrubbed BEFORE it is logged, not just before it is returned.
        // A DuckDB error quotes the failing statement, and logs are shipped,
        // indexed and read by people who are not entitled to the
        // credentials a template interpolated.
        const auto secrets = collectTemplateSecrets(config_manager.get(), endpoint, params);
        const std::string public_detail =
            publicErrorMessage("Error handling request", e.what(), secrets);
        CROW_LOG_ERROR << public_detail;
        // The body carries NO part of the failure text. A database error
        // quotes the statement that failed, which IS the rendered template,
        // and names absolute file paths ("No files found that match the
        // pattern /srv/app/data/x.parquet"). Secret scrubbing (above, for the
        // log) removes credentials but not the SQL or the paths, which map the
        // deployment for anyone who can trigger an error. The detail stays in
        // the server log, which carries the same request id as this body.
        writeInternalError(res);
        return;
    }
}

bool RequestHandler::isCacheDetailsRequest(const crow::request& req, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams) {
    return req.url.find("/cache") != std::string::npos;
}

void RequestHandler::parsePaginationParams(std::map<std::string, std::string>& params) {
    int64_t offset = 0;
    int64_t limit = 100;
    if ((params.count("offset") && !ParsePlainNonNegativeInt(params["offset"], offset)) ||
        (params.count("limit") && !ParsePlainNonNegativeInt(params["limit"], limit))) {
        throw std::runtime_error("Invalid pagination parameters");
    }
    // Add parsed values back to params
    params["offset"] = std::to_string(offset);
    params["limit"] = std::to_string(limit);
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

// The `__auth_*` rule lives in auth_params.hpp - see the comment there for
// what a caller could do before it existed.

namespace {

// `field-in` says where a value may come from. A declared field is read ONLY
// from that source (#192): a query parameter must not be able to replace a
// path value, nor satisfy a body field, nor the body a query field. Names that
// no field declares keep the old any-source behaviour - validation is what
// reports an unknown parameter. A field whose `field-in` is none of the four
// known sources (empty, `cookie`) is not restricted either.
bool mayComeFrom(const EndpointConfig& endpoint, const std::string& name, const char* source) {
    for (const auto& field : endpoint.request_fields) {
        if (field.fieldName != name) {
            continue;
        }
        const std::string& declared = field.fieldIn;
        if (declared == "path" || declared == "query" || declared == "body" || declared == "header") {
            return declared == source;
        }
        return true;
    }
    return true;
}

} // namespace

std::map<std::string, std::string> RequestHandler::combineParameters(const crow::request& req, const std::map<std::string, std::string>& defaultParams, const std::map<std::string, std::string>& pathParams, const EndpointConfig& endpoint) {
    std::map<std::string, std::string> params = defaultParams;

    // A repeated query parameter (?x=1&x=2) used to take the first value
    // silently - a client sending two filters got one. Ambiguous input is a 400.
    std::set<std::string> seen_query_keys;
    for (const auto& key : req.url_params.keys()) {
        if (!seen_query_keys.insert(key).second) {
            throw BadRequestError("Duplicate query parameter: " + key);
        }
    }
    for (const auto& key : seen_query_keys) {
        if (isReservedAuthKey(key)) {
            continue;   // server-injected identity is not client input
        }
        if (pathParams.find(key) != pathParams.end() || !mayComeFrom(endpoint, key, "query")) {
            continue;   // a route parameter is path-only; a body/header/path field is not a query value
        }
        params[key] = req.url_params.get(key);
    }

    // Path values go in after the query string and win over it.
    for (const auto& key : pathParams) {
        if (isReservedAuthKey(key.first)) {
            continue;
        }
        params[key.first] = key.second;
    }

    for (const auto& field : endpoint.request_fields) {
        if (!field.defaultValue.empty() && params.find(field.fieldName) == params.end()) {
            params[field.fieldName] = field.defaultValue;
        }
    }
    applyHeaderFields(req, endpoint, params);
    return params;
}

void RequestHandler::applyHeaderFields(const crow::request& req, const EndpointConfig& endpoint,
                                       std::map<std::string, std::string>& params) {
    // `field-in: header` fields come from the request HEADERS. They were read
    // from the query string instead, so a declared header was never seen and
    // a same-named query parameter stood in for it. A header field is now
    // satisfied only by the header (or its configured default): a query
    // parameter must not be able to impersonate e.g. a gateway-set tenant id.
    // Crow's header map is case-insensitive, as HTTP header names are.
    for (const auto& field : endpoint.request_fields) {
        if (field.fieldIn != "header" || isReservedAuthKey(field.fieldName)) {
            continue;
        }
        const std::string value = req.get_header_value(field.fieldName);
        if (!value.empty()) {
            params[field.fieldName] = value;
        } else if (!field.defaultValue.empty()) {
            params[field.fieldName] = field.defaultValue;
        } else {
            params.erase(field.fieldName);
        }
    }
}

std::map<std::string, std::string> RequestHandler::combineWriteParameters(const crow::request& req,
                                                                          const std::map<std::string, std::string>& pathParams,
                                                                          const EndpointConfig& endpoint) {
    std::map<std::string, std::string> params = defaultParams;

    // Add path parameters first (lowest precedence for body params)
    for (const auto& [key, value] : pathParams) {
        if (isReservedAuthKey(key)) {
            continue;
        }
        params[key] = value;
    }

    // Extract parameters from JSON body (highest precedence for write operations).
    // A DELETE body is optional; most clients send the key in the path or query.
    if (req.method == crow::HTTPMethod::Post ||
        req.method == crow::HTTPMethod::Put ||
        req.method == crow::HTTPMethod::Patch ||
        req.method == crow::HTTPMethod::Delete) {
        
        if (!req.body.empty()) {
            try {
                auto bodyJson = crow::json::load(req.body);
                if (bodyJson) {
                    // A nested number, keeping its fraction: i() truncates 9.99 to 9.
                    auto jsonNumber = [](const crow::json::rvalue& v) -> crow::json::wvalue {
                        switch (v.nt()) {
                            case crow::json::num_type::Floating_point:
                            case crow::json::num_type::Double_precision_floating_point:
                                return crow::json::wvalue(v.d());
                            case crow::json::num_type::Unsigned_integer:
                                return crow::json::wvalue(v.u());
                            default:
                                return crow::json::wvalue(v.i());
                        }
                    };
                    // Helper function to convert JSON value to string
                    auto jsonValueToString = [&bodyJson, &req, &jsonNumber](const std::string& key) -> std::string {
                        if (!bodyJson.has(key)) {
                            return "";
                        }
                        auto jsonVal = bodyJson[key];
                        std::string value;
                        if (jsonVal.t() == crow::json::type::String) {
                            value = jsonVal.s();
                        } else if (jsonVal.t() == crow::json::type::Number) {
                            // The number's own text, exactly as sent. i() lexical-casts and
                            // stops at the '.', so 9.99 became "9" - and 301.7 then passed an
                            // `int` validator as 301. The validators decide what is acceptable.
                            value = static_cast<std::string>(jsonVal);
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
                                                tempObj[keyStr] = jsonNumber(v);
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
                                                arr.push_back(jsonNumber(v));
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
                                    value = QueryResult::dumpJson(tempObj);   // shortest doubles
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
                        if (isReservedAuthKey(fieldName)) {
                            continue;   // identity comes from the middleware, not the body
                        }
                        if (pathParams.find(fieldName) != pathParams.end() || !mayComeFrom(endpoint, fieldName, "body")) {
                            continue;   // a path value is not overridable; a query/header field is not a body value
                        }
                        std::string value = jsonValueToString(fieldName);
                        params[fieldName] = value;
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
        if (isReservedAuthKey(key)) {
            continue;
        }
        if (pathParams.find(key) != pathParams.end() || !mayComeFrom(endpoint, key, "query")) {
            continue;
        }
        if (params.find(key) == params.end()) {
            params[key] = req.url_params.get(key);
        }
    }

    applyHeaderFields(req, endpoint, params);
    return params;
}

namespace {

// One background worker for refresh-on-write. A write must not wait for the
// refresh (that is what invalidate-on-write is for), but a detached thread per
// write could still be running DuckDB while statics are torn down at exit, and
// a burst of writes would start a refresh each. A single worker, joined on
// destruction, with refreshes coalesced per endpoint, avoids both.
class AsyncCacheRefresher {
public:
    static AsyncCacheRefresher& instance() {
        static AsyncCacheRefresher refresher;
        return refresher;
    }

    void schedule(std::shared_ptr<DatabaseManager> db_manager, const EndpointConfig& endpoint) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_stopping) {
                return;
            }
            std::string key = endpoint.getIdentifier();
            if (!_queued.insert(key).second) {
                return;   // already pending; that refresh will see this write too
            }
            _jobs.push_back(Job{std::move(db_manager), endpoint, std::move(key)});
            if (!_worker.joinable()) {
                _worker = std::thread([this] { run(); });
            }
        }
        _cv.notify_one();
    }

    ~AsyncCacheRefresher() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stopping = true;
            _jobs.clear();
        }
        _cv.notify_all();
        if (_worker.joinable()) {
            _worker.join();
        }
    }

private:
    struct Job {
        std::shared_ptr<DatabaseManager> db_manager;
        EndpointConfig endpoint;
        std::string key;
    };

    void run() {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _cv.wait(lock, [this] { return _stopping || !_jobs.empty(); });
                if (_stopping) {
                    return;
                }
                job = std::move(_jobs.front());
                _jobs.pop_front();
                _queued.erase(job.key);
            }
            try {
                if (job.db_manager->invalidateCache(job.endpoint)) {
                    CROW_LOG_DEBUG << "Cache refreshed after write (async) for endpoint: " << job.key;
                } else {
                    CROW_LOG_WARNING << "Asynchronous cache refresh after write failed for endpoint: " << job.key;
                }
            } catch (const std::exception& e) {
                CROW_LOG_WARNING << "Asynchronous cache refresh after write failed for endpoint "
                                 << job.key << ": " << e.what();
            }
        }
    }

    std::mutex _mutex;
    std::condition_variable _cv;
    std::deque<Job> _jobs;
    std::set<std::string> _queued;
    std::thread _worker;
    bool _stopping = false;
};

}  // namespace

std::string RequestHandler::formatWriteResultJson(const WriteResult& result, bool always_include_data) {
    std::string out = "{\"rows_affected\":" + std::to_string(result.rows_affected);
    if (result.returned_data.has_value()) {
        // Included even if not explicitly configured (helpful for debugging).
        out += ",\"data\":";
        out += QueryResult::dumpJson(*result.returned_data,
                                     result.returned_columns.empty() ? nullptr : &result.returned_columns);
    } else if (always_include_data) {
        // A RETURNING that matched no row is still an answer: an empty list,
        // not a missing key a client has to special-case.
        out += ",\"data\":[]";
    }
    out += '}';
    return out;
}

void RequestHandler::handleCacheAfterWrite(const std::shared_ptr<DatabaseManager>& db_manager,
                                           const EndpointConfig& endpoint) {
    if (!db_manager || !db_manager->isCacheEnabled(endpoint)) {
        return;
    }

    // invalidate-on-write: refresh NOW, before the response, so a read issued
    // after this write's response sees the write.
    if (endpoint.cache.invalidate_on_write) {
        db_manager->invalidateCache(endpoint);
        CROW_LOG_DEBUG << "Cache invalidated after write operation for endpoint: " << endpoint.getIdentifier();
        return;   // already fresh; a second, asynchronous refresh would be redundant
    }

    // refresh-on-write: refresh in the background; the response does not
    // wait. This was a no-op that only logged.
    if (endpoint.cache.refresh_on_write) {
        AsyncCacheRefresher::instance().schedule(db_manager, endpoint);
        CROW_LOG_DEBUG << "Cache refresh scheduled after write operation for endpoint: " << endpoint.getIdentifier();
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

    // Columns in SELECT order when the result knows them; the row object
    // itself is an unordered map.
    std::vector<std::string> columns = queryResult.columns;
    if (columns.empty()) {
        columns = queryResult.data[0].keys();
    }
    bool first = true;
    for (const auto& column : columns) {
        if (!first) csv << ",";
        csv << escapeCSV(column);
        first = false;
    }
    csv << "\n";
    for (size_t i = 0; i < queryResult.data.size(); i++) {
        const auto& row = queryResult.data[static_cast<unsigned>(i)];
        first = true;
        for (const auto& column : columns) {
            if (!first) csv << ",";
            csv << QueryResult::dumpJson(row[column]);
            first = false;
        }
        csv << "\n";
    }

    return csv.str();
}

void RequestHandler::appendCSVRow(const crow::json::wvalue& row, const std::vector<std::string>& columns,
                                  bool first_row, std::string& out) {
    // The same text convertToCSV writes: a header before the first row, then one line per row.
    std::vector<std::string> keys = columns.empty() ? row.keys() : columns;
    if (first_row) {
        bool first = true;
        for (const auto& column : keys) {
            if (!first) out += ',';
            out += escapeCSV(column);
            first = false;
        }
        out += '\n';
    }
    bool first = true;
    for (const auto& column : keys) {
        if (!first) out += ',';
        out += QueryResult::dumpJson(row[column]);
        first = false;
    }
    out += '\n';
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
