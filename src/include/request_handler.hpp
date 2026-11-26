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
    void handleRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams, const std::map<std::string, std::string>& authParams = {});

    // Cache maintenance after a successful write (REST and MCP):
    //   invalidate-on-write -> refresh synchronously, before the response;
    //   refresh-on-write    -> refresh asynchronously, the response does not wait.
    static void handleCacheAfterWrite(const std::shared_ptr<DatabaseManager>& db_manager,
                                      const EndpointConfig& endpoint);

    // A write's response body, `{"rows_affected":N,"data":[...]}`, with the
    // RETURNING rows' keys in column order and doubles in shortest form (a
    // wvalue dump scrambles the keys). `data` is present when rows came back,
    // and as [] when `always_include_data` is set and none did.
    static std::string formatWriteResultJson(const WriteResult& result, bool always_include_data);
    // `offset` / `limit` in `params` that are not a plain non-negative integer,
    // one error each ("limit must be a non-negative integer"). REST and MCP.
    // The rows this request returned (a read) or changed (a write), for the
    // audit line's row_count and the span's db.response.returned_rows. A no-op
    // when no request context is ambient.
    static void RecordRowCount(std::int64_t rows) noexcept;
    static std::vector<ValidationError> PaginationErrors(const std::map<std::string, std::string>& params);

    // Fills `field-in: header` fields from the request headers (case-insensitive).
    static void applyHeaderFields(const crow::request& req, const EndpointConfig& endpoint,
                                  std::map<std::string, std::string>& params);

private:
    std::map<std::string, std::string> defaultParams;

    void handleGetRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams, const std::map<std::string, std::string>& authParams);
    void handleWriteRequest(const crow::request& req, crow::response& res, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams, const std::map<std::string, std::string>& authParams);

    bool isCacheDetailsRequest(const crow::request& req, const EndpointConfig& endpoint, const std::map<std::string, std::string>& pathParams);

    std::map<std::string, std::string> combineParameters(const crow::request& req, 
                                                         const std::map<std::string, std::string>& defaultParams, 
                                                         const std::map<std::string, std::string>& pathParams,
                                                         const EndpointConfig& endpoint);
    std::map<std::string, std::string> combineWriteParameters(const crow::request& req,
                                                              const std::map<std::string, std::string>& pathParams,
                                                              const EndpointConfig& endpoint);
    QueryResult executeQuery(const EndpointConfig& endpoint, const std::map<std::string, std::string>& params);
    bool validateRequestParameters(const std::vector<RequestFieldConfig>& requestFields, const std::map<std::string, std::string>& params);
    void parsePaginationParams(std::map<std::string, std::string>& params);
    std::string convertToCSV(const QueryResult& queryResult);
    static std::string escapeCSV(const std::string& str);
    // One row (plus the header before the first) as convertToCSV writes it.
    static void appendCSVRow(const crow::json::wvalue& row, const std::vector<std::string>& columns,
                             bool first_row, std::string& out);
    // The `next` link: the path, the new window and the client's own query
    // string as sent, percent-encoded, without `__auth_*` keys and names the
    // endpoint reads from a header, the body or the path. Never a server-side
    // value: a field's `default:` stays out of the URL.
    static std::string createNextUrl(const crow::request& req, const QueryResult& queryResult,
                                     const EndpointConfig& endpoint);

    

    std::shared_ptr<DatabaseManager> db_manager;
    std::shared_ptr<ConfigManager> config_manager;
    std::shared_ptr<RequestValidator> validator;
};

} // namespace flapi