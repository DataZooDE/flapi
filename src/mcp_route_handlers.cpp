#include "mcp_route_handlers.hpp"
#include "json_utils.hpp"
#include "arrow_metrics.hpp"
#include "mcp_authorization_policy.hpp"
#include "mcp_schema_builder.hpp"
#include "mcp_header_validation.hpp"
#include <iostream>
#include <sstream>
#include <optional>

namespace flapi {

// ========== Helper function implementations ==========

std::string MCPRouteHandlers::formatJsonRpcError(int code, const std::string& message) {
    // Escape quotes in message for JSON safety
    std::string escaped_message;
    escaped_message.reserve(message.size());
    for (char c : message) {
        if (c == '"') {
            escaped_message += "\\\"";
        } else if (c == '\\') {
            escaped_message += "\\\\";
        } else if (c == '\n') {
            escaped_message += "\\n";
        } else if (c == '\r') {
            escaped_message += "\\r";
        } else if (c == '\t') {
            escaped_message += "\\t";
        } else {
            escaped_message += c;
        }
    }
    return "{\"code\":" + std::to_string(code) + ",\"message\":\"" + escaped_message + "\"}";
}

MCPResponse MCPRouteHandlers::initResponse(const MCPRequest& request) {
    MCPResponse response;
    response.id = request.id;
    return response;
}

bool MCPRouteHandlers::extractRequiredStringParam(const crow::json::wvalue& params,
                                                   const std::string& param_name,
                                                   std::string& out_value,
                                                   MCPResponse& response) const {
    // Check if parameter exists
    if (params.count(param_name) == 0) {
        response.error = formatJsonRpcError(-32602, "Invalid params: missing " + param_name);
        return false;
    }

    auto param_value = params[param_name];

    // Check for null
    if (JsonUtils::isNull(param_value)) {
        response.error = formatJsonRpcError(-32602, "Invalid params: " + param_name + " cannot be null");
        return false;
    }

    // Extract string value
    if (JsonUtils::isString(param_value)) {
        out_value = JsonUtils::extractString(param_value);
    } else {
        out_value = param_value.dump();
    }

    return true;
}

std::optional<std::string> MCPRouteHandlers::authorizeMCPEntity(
    const crow::request& http_req,
    const std::optional<std::vector<std::string>>& allowed_roles,
    const std::string& entity_label) const
{
    const bool mcp_auth_enabled = config_manager_ && config_manager_->getMCPConfig().auth.enabled;

    // Demo mode (auth disabled): the startup auditor already warns; keep the
    // open-by-default behaviour so first-run experiences are unaffected.
    if (!mcp_auth_enabled) {
        return std::nullopt;
    }

    std::vector<std::string> user_roles;
    if (auth_handler_) {
        auto auth_context = auth_handler_->authenticate(http_req);
        if (auth_context) {
            user_roles = auth_context->roles;
        }
    }

    MCPAuthorizationPolicy policy;
    auto decision = policy.authorizeRoles(allowed_roles, entity_label, user_roles, mcp_auth_enabled);
    if (decision.allowed) {
        return std::nullopt;
    }
    return decision.reason;
}

bool MCPRouteHandlers::applyPagination(const MCPRequest& request, size_t total,
                                       size_t& out_offset, size_t& out_count,
                                       std::string& out_next_cursor, MCPResponse& response) const {
    out_offset = 0;
    out_count = total;
    out_next_cursor.clear();

    const int page_size = config_manager_->getMCPConfig().page_size;
    const uint64_t gen = entity_generation_.load(std::memory_order_relaxed);

    // Decode an incoming cursor if present. The cursor is base64 of
    // {"offset":N,"gen":G}; a mismatched generation or malformed cursor is a
    // client error (-32602) so it cannot page over a list that changed.
    size_t offset = 0;
    auto params = crow::json::load(request.params.dump());
    if (params && params.has("cursor") && params["cursor"].t() == crow::json::type::String) {
        std::string decoded = crow::utility::base64decode(params["cursor"].s());
        auto cur = crow::json::load(decoded);
        if (!cur || !cur.has("offset") || !cur.has("gen")) {
            response.error = formatJsonRpcError(-32602, "Invalid pagination cursor");
            return false;
        }
        if (static_cast<uint64_t>(cur["gen"].u()) != gen) {
            response.error = formatJsonRpcError(-32602,
                "Pagination cursor is stale (the list changed); restart from the first page");
            return false;
        }
        offset = static_cast<size_t>(cur["offset"].u());
    }

    if (offset > total) {
        offset = total;
    }
    out_offset = offset;

    // page_size <= 0 disables pagination: one page with everything, no cursor.
    if (page_size <= 0) {
        out_count = total - offset;
        return true;
    }

    const size_t remaining = total - offset;
    out_count = std::min(remaining, static_cast<size_t>(page_size));
    const size_t next_offset = offset + out_count;
    if (next_offset < total) {
        crow::json::wvalue cur;
        cur["offset"] = static_cast<uint64_t>(next_offset);
        cur["gen"] = static_cast<uint64_t>(gen);
        out_next_cursor = crow::utility::base64encode(cur.dump(), cur.dump().size());
    }
    return true;
}

std::optional<std::string> MCPRouteHandlers::validateMirroredHeaders(
    const crow::request& http_req, const MCPRequest& request) const {
    // MCP-Protocol-Version must be present and equal the _meta version.
    std::string proto = http_req.get_header_value("MCP-Protocol-Version");
    if (proto.empty()) {
        return std::string("Missing required MCP-Protocol-Version header");
    }
    if (!mcp::headerMatches(proto, request.meta_protocol_version)) {
        return std::string("MCP-Protocol-Version header does not match _meta protocolVersion");
    }

    // Mcp-Method must be present and equal the JSON-RPC method.
    std::string method_hdr = http_req.get_header_value("Mcp-Method");
    if (method_hdr.empty()) {
        return std::string("Missing required Mcp-Method header");
    }
    if (!mcp::headerMatches(method_hdr, request.method)) {
        return std::string("Mcp-Method header does not match the request method");
    }

    // Mcp-Name mirrors the primary target for name/uri-bearing methods.
    std::string expected_name;
    bool name_required = false;
    auto params = crow::json::load(request.params.dump());
    if (request.method == "tools/call" || request.method == "prompts/get") {
        name_required = true;
        if (params && params.has("name") && params["name"].t() == crow::json::type::String) {
            expected_name = params["name"].s();
        }
    } else if (request.method == "resources/read") {
        name_required = true;
        if (params && params.has("uri") && params["uri"].t() == crow::json::type::String) {
            expected_name = params["uri"].s();
        }
    }
    if (name_required) {
        std::string name_hdr = http_req.get_header_value("Mcp-Name");
        if (name_hdr.empty()) {
            return std::string("Missing required Mcp-Name header");
        }
        if (!mcp::headerMatches(name_hdr, expected_name)) {
            return std::string("Mcp-Name header does not match the request target");
        }
    }

    return std::nullopt;
}

std::string MCPRouteHandlers::buildResourceMetadataUrl(const crow::request& http_req) const {
    // Prefer an explicit canonical URI; else derive the origin from forwarded/
    // Host headers so the URL is correct behind a reverse proxy.
    const auto& auth = config_manager_->getMCPConfig().auth;
    std::string origin;
    if (!auth.canonical_resource_uri.empty()) {
        origin = auth.canonical_resource_uri;
        // Strip any path so we can append the well-known path cleanly.
        auto scheme_end = origin.find("://");
        auto path_start = origin.find('/', scheme_end == std::string::npos ? 0 : scheme_end + 3);
        if (path_start != std::string::npos) {
            origin = origin.substr(0, path_start);
        }
    } else {
        std::string scheme = http_req.get_header_value("X-Forwarded-Proto");
        if (scheme.empty()) {
            scheme = "http";
        }
        std::string host = http_req.get_header_value("X-Forwarded-Host");
        if (host.empty()) {
            host = http_req.get_header_value("Host");
        }
        if (host.empty()) {
            host = "localhost:" + std::to_string(port_);
        }
        origin = scheme + "://" + host;
    }
    return origin + "/.well-known/oauth-protected-resource";
}

std::string MCPRouteHandlers::buildWwwAuthenticate(const crow::request& http_req, bool insufficient_scope) const {
    std::string value = "Bearer";
    const auto& auth = config_manager_->getMCPConfig().auth;
    // Point clients at the metadata document only when there is an OAuth/OIDC
    // authorization server to discover.
    if (auth.type == "oidc" && auth.oidc.has_value()) {
        value += " resource_metadata=\"" + buildResourceMetadataUrl(http_req) + "\"";
    }
    if (insufficient_scope) {
        value += (value == "Bearer" ? " " : ", ");
        value += "error=\"insufficient_scope\"";
    }
    return value;
}

// ========== End helper functions ==========

MCPRouteHandlers::MCPRouteHandlers(std::shared_ptr<ConfigManager> config_manager,
                                 std::shared_ptr<DatabaseManager> db_manager,
                                 std::shared_ptr<MCPSessionManager> session_manager,
                                 std::shared_ptr<MCPClientCapabilitiesDetector> capabilities_detector,
                                 std::unique_ptr<ConfigToolAdapter> config_tool_adapter,
                                 int port)
    : config_manager_(config_manager), db_manager_(db_manager),
      session_manager_(session_manager), capabilities_detector_(capabilities_detector),
      config_tool_adapter_(std::move(config_tool_adapter)),
      port_(port)
{
    CROW_LOG_INFO << "MCPRouteHandlers constructor called - initializing MCP server components";
    // Initialize tool handler
    try {
        tool_handler_ = std::make_unique<MCPToolHandler>(db_manager, config_manager);
        CROW_LOG_DEBUG << "MCPToolHandler initialized successfully";
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Failed to initialize MCPToolHandler: " << e.what();
        tool_handler_ = nullptr;
    }

    // Initialize the Tasks worker pool (MCP 2026-07-28 Tasks extension). When a
    // database manager is available, back the task store with a DuckDB table so
    // tasks survive a restart (durable only when duckdb.db_path is file-backed).
    {
        const auto& mcp_cfg = config_manager->getMCPConfig();
        MCPTaskManager::SqlExec sql_exec = nullptr;
        if (db_manager) {
            auto dbm = db_manager;
            sql_exec = [dbm](const std::string& sql)
                -> std::vector<std::map<std::string, std::string>> {
                // executeQuery(string) returns rows only in `.data` (a JSON
                // array of objects); convert them to column->string maps.
                std::vector<std::map<std::string, std::string>> out;
                // with_pagination=false: never wrap DDL/DML (CREATE/INSERT/UPDATE)
                // or the recovery SELECT in a pagination subquery.
                auto qr = dbm->executeQuery(sql, {}, /*with_pagination=*/false);
                auto rows = crow::json::load(qr.data.dump());
                if (rows && rows.t() == crow::json::type::List) {
                    for (const auto& row : rows) {
                        if (row.t() != crow::json::type::Object) {
                            continue;
                        }
                        std::map<std::string, std::string> m;
                        for (const auto& key : row.keys()) {
                            const auto& v = row[key];
                            m[key] = (v.t() == crow::json::type::String)
                                ? std::string(v.s())
                                : crow::json::wvalue(v).dump();
                        }
                        out.push_back(std::move(m));
                    }
                }
                return out;
            };
        }
        task_manager_ = std::make_unique<MCPTaskManager>(
            static_cast<size_t>(mcp_cfg.tasks_workers > 0 ? mcp_cfg.tasks_workers : 1),
            static_cast<size_t>(mcp_cfg.tasks_queue_depth > 0 ? mcp_cfg.tasks_queue_depth : 32),
            std::move(sql_exec));
    }

    // Initialize MCP auth handler
    try {
        auth_handler_ = std::make_unique<MCPAuthHandler>(config_manager);
        CROW_LOG_DEBUG << "MCPAuthHandler initialized successfully";
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Failed to initialize MCPAuthHandler: " << e.what();
        auth_handler_ = nullptr;
    }

    // Initialize server info with default values (no separate MCP config needed)
    server_info_.name = "flapi-mcp-server";
    server_info_.version = "0.3.0";
    server_info_.protocol_version = "2025-11-25";

    // Initialize server capabilities with all available capabilities
    capabilities_.tools = {"tools", "resources"};
    capabilities_.resources = {"resources"};

    // Discover MCP entities from unified configuration
    // Note: This might fail if endpoints are not yet loaded. We'll retry later if needed.
    try {
        discoverMCPEntitiesImpl();
        CROW_LOG_DEBUG << "MCP entities discovered successfully: " << tool_definitions_.size() << " tools, " << resource_definitions_.size() << " resources";
    } catch (const std::exception& e) {
        CROW_LOG_WARNING << "Failed to discover MCP entities during construction: " << e.what();
        CROW_LOG_DEBUG << "Will retry MCP entity discovery when routes are registered";
    }

    CROW_LOG_INFO << "MCP Route Handlers initialized with " << tool_definitions_.size() << " tools and "
                  << resource_definitions_.size() << " resources";
    CROW_LOG_INFO << "flAPI MCP server ready at: http://localhost:" << (port_ > 0 ? std::to_string(port_) : "8080") << "/mcp/jsonrpc";
    CROW_LOG_INFO << "Transport type: Streamable HTTP, URL ready to paste into MCP inspector tool";
}

void MCPRouteHandlers::registerRoutes(crow::App<crow::CORSHandler, FlapiCorsMiddleware, RateLimitMiddleware, AuthMiddleware>& app, int port) {
    port_ = port; // Update port if provided

    CROW_LOG_INFO << "Registering MCP routes with application...";
    CROW_LOG_INFO << "MCP Route Handlers registerRoutes called - tools: " << tool_definitions_.size() << ", resources: " << resource_definitions_.size();

    // MCP JSON-RPC endpoint (main protocol endpoint)
    CROW_ROUTE(app, "/mcp/jsonrpc")
        .methods("POST"_method)
        ([this](const crow::request& req) -> crow::response {
            try {
                CROW_LOG_DEBUG << "MCP JSON-RPC route handler called";

                // Extract session ID from request (if present)
                auto session_id = extractSessionIdFromRequest(req);

                // Parse and validate the request EARLY to determine if it's initialize
                auto mcp_request = parseMCPRequest(req);
                if (!mcp_request) {
                    CROW_LOG_ERROR << "Failed to parse MCP request";
                    return createJsonRpcErrorResponse("", -32700, "Parse error: Invalid JSON", session_id);
                }

                CROW_LOG_DEBUG << "MCP request: method=" << mcp_request->method << ", id=" << mcp_request->id;

                // JSON-RPC notification: a request object with no `id` member.
                // The receiver MUST NOT return a response (previously flAPI
                // replied with a spurious -32601 and id:null). Acknowledge at the
                // transport level with 202 and no body. notifications/initialized
                // and notifications/cancelled are no-ops for this server.
                if (!mcp_request->id_present) {
                    CROW_LOG_DEBUG << "MCP notification (no id), method=" << mcp_request->method
                                   << " — acknowledged without a response body";
                    return crow::response(202);
                }

                // MCP 2026-07-28 modern-era preamble validation. Legacy requests
                // (no _meta protocolVersion) skip this entirely and keep the
                // initialize+session path.
                if (mcp_request->modern_era) {
                    // Unknown/unsupported requested version → -32022 with the
                    // supported list, HTTP 400.
                    bool supported = false;
                    for (const auto* v : flapi::mcp::constants::MCP_SUPPORTED_VERSIONS) {
                        if (mcp_request->meta_protocol_version == v) {
                            supported = true;
                            break;
                        }
                    }
                    if (!supported) {
                        crow::json::wvalue data;
                        crow::json::wvalue sv = crow::json::wvalue::list();
                        size_t i = 0;
                        for (const auto* v : flapi::mcp::constants::MCP_SUPPORTED_VERSIONS) {
                            sv[i++] = std::string(v);
                        }
                        data["supported"] = std::move(sv);
                        MCPResponse mcp_response;
                        mcp_response.id = mcp_request->id;
                        crow::json::wvalue err;
                        err["code"] = flapi::mcp::constants::UNSUPPORTED_PROTOCOL_VERSION;
                        err["message"] = "Unsupported protocol version: " + mcp_request->meta_protocol_version;
                        err["data"] = std::move(data);
                        mcp_response.error = err.dump();
                        mcp_response.http_status = 400;
                        return createJsonRpcResponse(*mcp_request, mcp_response, std::nullopt);
                    }
                    // clientCapabilities is required in the modern preamble.
                    if (!mcp_request->meta_has_client_capabilities) {
                        MCPResponse mcp_response;
                        mcp_response.id = mcp_request->id;
                        mcp_response.error = formatJsonRpcError(
                            -32602, "Missing required _meta clientCapabilities");
                        mcp_response.http_status = 400;
                        return createJsonRpcResponse(*mcp_request, mcp_response, std::nullopt);
                    }

                    // Mirrored-header validation. The modern transport requires
                    // MCP-Protocol-Version, Mcp-Method and (for name/uri-bearing
                    // methods) Mcp-Name to mirror the body so an edge proxy can
                    // route without parsing it. A mismatch or a missing required
                    // header is -32020 HeaderMismatch / HTTP 400.
                    if (auto mismatch = validateMirroredHeaders(req, *mcp_request)) {
                        MCPResponse mcp_response;
                        mcp_response.id = mcp_request->id;
                        crow::json::wvalue err;
                        err["code"] = flapi::mcp::constants::HEADER_MISMATCH;
                        err["message"] = *mismatch;
                        mcp_response.error = err.dump();
                        mcp_response.http_status = 400;
                        return createJsonRpcResponse(*mcp_request, mcp_response, std::nullopt);
                    }
                }

                // Layer 1 (protocol) authorization.
                //
                // SECURITY: authentication and method authorization are derived
                // from the HTTP request on EVERY call and are independent of any
                // session header. A prior version only ran authorizeMethod inside
                // the "session header present" branch, so a client that simply
                // omitted `Mcp-Session-Id` bypassed the check entirely and could
                // reach resources/read, tools/call, etc. unauthenticated. Sessions
                // are now a legacy echo only — never an authorization carrier.
                std::optional<MCPSession::AuthContext> auth_context;
                if (auth_handler_) {
                    auth_context = auth_handler_->authenticate(req);

                    // server/discover is the modern entry point (the analogue of
                    // initialize) and must be publicly reachable so a client can
                    // discover how to authenticate.
                    if (mcp_request->method == "initialize" || mcp_request->method == "server/discover") {
                        if (mcp_request->method == "initialize"
                            && auth_handler_->methodRequiresAuth("initialize") && !auth_context) {
                            CROW_LOG_WARNING << "MCP initialize: authentication required but failed";
                            MCPResponse mcp_response;
                            mcp_response.id = mcp_request->id;
                            mcp_response.error = "{\"code\":-32000,\"message\":\"Authentication required for initialize\"}";
                            mcp_response.http_status = 401;
                            mcp_response.www_authenticate = buildWwwAuthenticate(req, /*insufficient_scope=*/false);
                            return createJsonRpcResponse(*mcp_request, mcp_response, session_id);
                        }
                    } else if (!auth_handler_->authorizeMethod(mcp_request->method, auth_context)) {
                        CROW_LOG_WARNING << "MCP method " << mcp_request->method << " requires authentication";
                        MCPResponse mcp_response;
                        mcp_response.id = mcp_request->id;
                        mcp_response.error = "{\"code\":-32000,\"message\":\"Authentication required for method: " +
                                           mcp_request->method + "\"}";
                        // RFC 9728 / RFC 6750: an authentication challenge is HTTP
                        // 401 with a WWW-Authenticate header so an OAuth client can
                        // discover the authorization server and start its flow.
                        mcp_response.http_status = 401;
                        mcp_response.www_authenticate = buildWwwAuthenticate(req, /*insufficient_scope=*/false);
                        return createJsonRpcResponse(*mcp_request, mcp_response, session_id);
                    }
                }

                // Session lifecycle (legacy shim only): mint a session for legacy
                // clients that expect an `Mcp-Session-Id` back, and keep an
                // existing one's activity fresh. The session no longer gates
                // authorization — that happened above, unconditionally. The
                // modern (2026-07-28) path is stateless: it never mints or echoes
                // a session and ignores any inbound Mcp-Session-Id.
                if (mcp_request->modern_era) {
                    session_id = std::nullopt;
                } else if (session_manager_) {
                    if (!session_id) {
                        session_id = session_manager_->createSession("", auth_context);
                        CROW_LOG_INFO << "Created new session: " << session_id.value();
                    } else {
                        CROW_LOG_DEBUG << "Session ID extracted from request: " << session_id.value();
                        session_manager_->updateSessionActivity(session_id.value());
                    }
                }

                // Dispatch the request to appropriate handler (passing HTTP request for auth access)
                MCPResponse mcp_response = dispatchMCPRequest(*mcp_request, req);

                // Create and return JSON-RPC response with session header
                return createJsonRpcResponse(*mcp_request, mcp_response, session_id);
            } catch (const std::exception& e) {
                CROW_LOG_ERROR << "Error handling MCP request: " << e.what();
                return createJsonRpcErrorResponse("", -32603, "Internal JSON-RPC error: " + std::string(e.what()), std::nullopt);
            }
        });

    // RFC 9728 OAuth 2.0 Protected Resource Metadata. Lets a standard OAuth
    // client (Claude, VS Code, Goose, ...) discover which authorization server
    // guards this MCP endpoint and begin the browser OAuth flow, instead of
    // needing a bearer token handed over out of band. Only meaningful when an
    // OIDC authorization server is configured.
    CROW_ROUTE(app, "/.well-known/oauth-protected-resource")
        .methods("GET"_method)
        ([this](const crow::request& req) -> crow::response {
            const auto& auth = config_manager_->getMCPConfig().auth;
            if (!(auth.type == "oidc" && auth.oidc.has_value())) {
                // No discoverable authorization server; nothing to advertise.
                return crow::response(404);
            }
            crow::json::wvalue doc;
            doc["resource"] = auth.canonical_resource_uri.empty()
                ? (std::string(req.get_header_value("X-Forwarded-Proto").empty() ? "http" : req.get_header_value("X-Forwarded-Proto"))
                   + "://"
                   + (req.get_header_value("Host").empty() ? ("localhost:" + std::to_string(port_)) : req.get_header_value("Host"))
                   + "/mcp/jsonrpc")
                : auth.canonical_resource_uri;
            crow::json::wvalue servers = crow::json::wvalue::list();
            servers[0] = auth.oidc->issuer_url;
            doc["authorization_servers"] = std::move(servers);
            crow::json::wvalue methods = crow::json::wvalue::list();
            methods[0] = "header";
            doc["bearer_methods_supported"] = std::move(methods);
            if (!auth.scopes_supported.empty()) {
                crow::json::wvalue scopes = crow::json::wvalue::list();
                for (size_t i = 0; i < auth.scopes_supported.size(); ++i) {
                    scopes[i] = auth.scopes_supported[i];
                }
                doc["scopes_supported"] = std::move(scopes);
            }
            auto resp = crow::response(200, doc.dump());
            resp.set_header("Content-Type", "application/json");
            return resp;
        });

    // Health check endpoint
    CROW_ROUTE(app, "/mcp/health")
        .methods("GET"_method)
        ([this]() -> crow::response {
            crow::json::wvalue health;
            health["status"] = "healthy";
            health["server"] = server_info_.name;
            health["version"] = server_info_.version;
            health["protocol_version"] = server_info_.protocol_version;
            auto tool_defs = getToolDefinitionsFromConfig();
            auto resource_defs = getResourceDefinitionsFromConfig();
            health["mcp_available"] = true;
            health["tools_available"] = (tool_defs.size() > 0);
            health["resources_available"] = (resource_defs.size() > 0);
            health["tools_count"] = static_cast<int>(tool_defs.size());
            health["resources_count"] = static_cast<int>(resource_defs.size());

            // Arrow IPC status
            auto& arrowMetrics = ArrowMetrics::instance();
            health["arrow_available"] = true;
            health["arrow_active_streams"] = arrowMetrics.gauges.activeStreams.load();
            health["arrow_total_requests"] = arrowMetrics.counters.totalRequests.load();

            return crow::response(200, health);
        });

    // GET on the MCP endpoint would be the legacy SSE stream, which flAPI does
    // not implement; the 2026-07-28 transport also removed it. Respond 405.
    CROW_ROUTE(app, "/mcp/jsonrpc")
        .methods("GET"_method)
        ([]() -> crow::response {
            crow::response resp(405);
            resp.set_header("Allow", "POST, DELETE");
            return resp;
        });

    // MCP session cleanup endpoint (DELETE request to close session)
    CROW_ROUTE(app, "/mcp/jsonrpc")
        .methods("DELETE"_method)
        ([this](const crow::request& req) -> crow::response {
            try {
                CROW_LOG_DEBUG << "MCP session cleanup endpoint called";

                // Extract session ID from request
                auto session_id = extractSessionIdFromRequest(req);
                if (!session_id) {
                    CROW_LOG_WARNING << "DELETE /mcp/jsonrpc called without session ID header";
                    crow::json::wvalue error_response;
                    error_response["jsonrpc"] = "2.0";
                    error_response["id"] = nullptr;
                    error_response["error"]["code"] = -32000;
                    error_response["error"]["message"] = "Missing Mcp-Session-Id header for session cleanup";
                    return crow::response(400, error_response.dump());
                }

                CROW_LOG_INFO << "Cleaning up session: " << session_id.value();

                // Notify session manager to cleanup the session
                if (session_manager_) {
                    session_manager_->removeSession(session_id.value());
                    CROW_LOG_DEBUG << "Session removed by session manager: " << session_id.value();
                }

                // Return success response
                crow::json::wvalue success_response;
                success_response["jsonrpc"] = "2.0";
                success_response["id"] = nullptr;
                success_response["result"]["session_id"] = session_id.value();
                success_response["result"]["status"] = "closed";

                auto response = crow::response(200, success_response.dump());
                response.set_header("Content-Type", "application/json");
                addSessionHeaderToResponse(response, session_id);
                return response;
            } catch (const std::exception& e) {
                CROW_LOG_ERROR << "Error handling MCP session cleanup: " << e.what();
                crow::json::wvalue error_response;
                error_response["jsonrpc"] = "2.0";
                error_response["id"] = nullptr;
                error_response["error"]["code"] = -32603;
                error_response["error"]["message"] = std::string("Internal error during session cleanup: ") + e.what();
                return crow::response(500, error_response.dump());
            }
        });

    // Try to refresh MCP entities now that configuration should be loaded
    refreshMCPEntities();

    CROW_LOG_INFO << "MCP routes registered with application";
}

void MCPRouteHandlers::cacheOutputSchemaFromRows(const std::string& tool_name,
                                                const std::string& rows_json) const {
    {
        std::lock_guard<std::mutex> lock(output_schema_mu_);
        if (output_schema_cache_.count(tool_name)) {
            return;  // already learned
        }
    }
    auto rows = crow::json::load(rows_json);
    if (!rows || rows.t() != crow::json::type::List || rows.size() == 0) {
        return;
    }
    auto first = rows[0];
    if (first.t() != crow::json::type::Object) {
        return;
    }

    // Infer a JSON Schema for one row from the first row's value types, then the
    // full result shape as {rows: array<row>, row_count: integer}.
    crow::json::wvalue row_props = crow::json::wvalue::object();
    for (const auto& key : first.keys()) {
        crow::json::wvalue col;
        switch (first[key].t()) {
            case crow::json::type::Number: {
                // Distinguish integer from fractional where possible.
                double d = first[key].d();
                col["type"] = (d == static_cast<double>(static_cast<int64_t>(d))) ? "integer" : "number";
                break;
            }
            case crow::json::type::True:
            case crow::json::type::False:  col["type"] = "boolean"; break;
            case crow::json::type::String: col["type"] = "string"; break;
            case crow::json::type::List:   col["type"] = "array"; break;
            case crow::json::type::Object: col["type"] = "object"; break;
            default: break;  // null/unknown: leave type unset
        }
        row_props[key] = std::move(col);
    }

    crow::json::wvalue schema;
    schema["type"] = "object";
    schema["properties"]["rows"]["type"] = "array";
    schema["properties"]["rows"]["items"]["type"] = "object";
    schema["properties"]["rows"]["items"]["properties"] = std::move(row_props);
    schema["properties"]["row_count"]["type"] = "integer";

    std::lock_guard<std::mutex> lock(output_schema_mu_);
    output_schema_cache_.emplace(tool_name, schema.dump());
}

std::string MCPRouteHandlers::getCachedOutputSchema(const std::string& tool_name) const {
    std::lock_guard<std::mutex> lock(output_schema_mu_);
    auto it = output_schema_cache_.find(tool_name);
    return it == output_schema_cache_.end() ? std::string() : it->second;
}

void MCPRouteHandlers::refreshMCPEntities() {
    try {
        discoverMCPEntitiesImpl();
        {
            std::lock_guard<std::mutex> lock(output_schema_mu_);
            output_schema_cache_.clear();  // schemas may change with the config
        }
        // Invalidate outstanding pagination cursors: a cursor minted against the
        // previous list must not silently page over a changed one.
        entity_generation_.fetch_add(1, std::memory_order_relaxed);
        CROW_LOG_INFO << "MCP entities refreshed: " << tool_definitions_.size() << " tools, " << resource_definitions_.size() << " resources";
    } catch (const std::exception& e) {
        CROW_LOG_WARNING << "Failed to refresh MCP entities: " << e.what();
        CROW_LOG_DEBUG << "Continuing with existing MCP definitions";
    }
}

// Request parsing and validation methods
std::optional<MCPRequest> MCPRouteHandlers::parseMCPRequest(const crow::request& req) const {
    // Parse JSON request
    auto json_request = crow::json::load(req.body);
    if (!json_request) {
        CROW_LOG_ERROR << "Invalid JSON in MCP request: " << req.body;
        return std::nullopt;
    }

    // Extract fields from JSON
    MCPRequest mcp_request = extractRequestFields(json_request);

    // Validate the request
    if (!validateMCPRequest(mcp_request)) {
        return std::nullopt;
    }

    CROW_LOG_DEBUG << "parseMCPRequest returning method: '" << mcp_request.method << "'";

    return mcp_request;
}

MCPRequest MCPRouteHandlers::extractRequestFields(const crow::json::wvalue& json_request) const {
    MCPRequest mcp_req;

    // Handle params field - copy the JSON value
    if (json_request.count("params") > 0) {
        mcp_req.params = crow::json::wvalue(json_request["params"]);
    } else {
        mcp_req.params = crow::json::wvalue::object();
    }

    // Handle jsonrpc field which should be "2.0"
    auto jsonrpc_value = json_request["jsonrpc"];
    if (JsonUtils::isString(jsonrpc_value)) {
        mcp_req.jsonrpc = JsonUtils::extractString(jsonrpc_value);
    } else if (JsonUtils::isNull(jsonrpc_value)) {
        mcp_req.jsonrpc = "2.0";  // Default to 2.0 for null
    } else {
        // Convert to string using dump()
        mcp_req.jsonrpc = jsonrpc_value.dump();
    }

    // Handle method field which should be a string
    auto method_value = json_request["method"];
    if (JsonUtils::isString(method_value)) {
        mcp_req.method = JsonUtils::extractString(method_value);
    } else if (JsonUtils::isNull(method_value)) {
        mcp_req.method = "";  // Will be caught by validation
    } else {
        // Convert to string using dump()
        mcp_req.method = method_value.dump();
    }

    // Handle id field which can be string, number, or null — and may be absent
    // entirely (a JSON-RPC notification, which must not be answered).
    mcp_req.id_present = json_request.count("id") > 0;
    if (mcp_req.id_present) {
        auto id_value = json_request["id"];
        // Verbatim JSON token, echoed back losslessly.
        mcp_req.id_raw = id_value.dump();
        if (JsonUtils::isString(id_value)) {
            mcp_req.id = JsonUtils::extractString(id_value);
        } else if (JsonUtils::isNumber(id_value)) {
            mcp_req.id = id_value.dump();
        } else if (JsonUtils::isNull(id_value)) {
            mcp_req.id = "";
        } else {
            mcp_req.id = id_value.dump();
        }
    }

    // MCP 2026-07-28 per-request preamble under params._meta. Its presence (the
    // protocolVersion key specifically) selects the modern stateless path.
    if (json_request.count("params") > 0) {
        auto params_rv = crow::json::load(mcp_req.params.dump());
        if (params_rv && params_rv.has("_meta")) {
            auto meta = params_rv["_meta"];
            namespace k = flapi::mcp::constants;
            if (meta.has(k::META_PROTOCOL_VERSION)
                && meta[k::META_PROTOCOL_VERSION].t() == crow::json::type::String) {
                mcp_req.modern_era = true;
                mcp_req.meta_protocol_version = meta[k::META_PROTOCOL_VERSION].s();
            }
            mcp_req.meta_has_client_capabilities = meta.has(k::META_CLIENT_CAPABILITIES);
            if (meta.has(k::META_LOG_LEVEL)
                && meta[k::META_LOG_LEVEL].t() == crow::json::type::String) {
                mcp_req.meta_log_level = meta[k::META_LOG_LEVEL].s();
            }
            // Client-declared extensions live under clientCapabilities.extensions
            // as an object keyed by extension id.
            if (meta.has(k::META_CLIENT_CAPABILITIES)
                && meta[k::META_CLIENT_CAPABILITIES].t() == crow::json::type::Object) {
                auto caps = meta[k::META_CLIENT_CAPABILITIES];
                if (caps.has("extensions") && caps["extensions"].t() == crow::json::type::Object) {
                    for (const auto& key : caps["extensions"].keys()) {
                        mcp_req.meta_extensions.push_back(key);
                    }
                }
            }
        }
    }

    return mcp_req;
}

bool MCPRouteHandlers::validateMCPRequest(const MCPRequest& request) const {
    // Check if method is empty (was null)
    if (request.method.empty()) {
        CROW_LOG_ERROR << "Invalid Request: method cannot be null";
        return false;
    }

    // Basic validation - method should not be empty and should be valid
    if (request.method.empty()) {
        CROW_LOG_ERROR << "Invalid Request: method cannot be empty";
        return false;
    }

    return true;
}

MCPResponse MCPRouteHandlers::dispatchMCPRequest(const MCPRequest& request, const crow::request& http_req) const {
    return handleMessage(request, http_req);
}

std::optional<std::string> MCPRouteHandlers::extractSessionIdFromRequest(const crow::request& req) const {
    // Try to get session ID from Mcp-Session-Id header
    auto session_header = req.get_header_value(flapi::mcp::constants::MCP_SESSION_HEADER);
    if (!session_header.empty()) {
        return session_header;
    }
    return std::nullopt;
}

void MCPRouteHandlers::addSessionHeaderToResponse(crow::response& resp,
                                                   const std::optional<std::string>& session_id) const {
    if (session_id) {
        resp.set_header(flapi::mcp::constants::MCP_SESSION_HEADER, session_id.value());
        CROW_LOG_DEBUG << "Added session header to response: " << session_id.value();
    }
}

crow::response MCPRouteHandlers::createJsonRpcResponse(const MCPRequest& request, const MCPResponse& mcp_response,
                                                       const std::optional<std::string>& session_id) const {
    crow::json::wvalue response_json;
    response_json["jsonrpc"] = "2.0";

    // Echo the id verbatim from the request's raw JSON token so string/number/
    // null are preserved exactly and large integers are not mangled. An absent
    // id (notification) is handled upstream and never reaches here.
    if (request.id_present && !request.id_raw.empty()) {
        response_json["id"] = crow::json::load(request.id_raw);
    } else {
        response_json["id"] = nullptr;
    }

    if (!mcp_response.error.empty()) {
        response_json["error"] = crow::json::load(mcp_response.error);
    } else if (request.modern_era) {
        // MCP 2026-07-28 result envelope: every result carries a resultType and
        // the server identity under _meta; cacheable results additionally carry
        // ttlMs + cacheScope so clients can cache the (config-stable) lists.
        crow::json::wvalue result = crow::json::load(mcp_response.result);
        // A tools/call that returned a task handle is resultType "task"; every
        // other result is "complete".
        auto parsed_for_type = crow::json::load(mcp_response.result);
        const bool is_task = parsed_for_type && parsed_for_type.has("task")
            && request.method == "tools/call";
        result["resultType"] = is_task ? "task" : "complete";
        result["_meta"][flapi::mcp::constants::META_SERVER_INFO]["name"] = server_info_.name;
        result["_meta"][flapi::mcp::constants::META_SERVER_INFO]["version"] = server_info_.version;

        const std::string& m = request.method;
        if (m == "server/discover") {
            result["ttlMs"] = 3600000;      // 1h — discovery changes only on reload
            result["cacheScope"] = "public";
        } else if (m == "tools/list" || m == "prompts/list" || m == "resources/list"
                   || m == "resources/templates/list" || m == "resources/read") {
            result["ttlMs"] = 300000;       // 5m
            // Lists may be role-filtered per caller in future; default private.
            result["cacheScope"] = "private";
        }
        response_json["result"] = std::move(result);
    } else {
        response_json["result"] = crow::json::load(mcp_response.result);
    }

    auto response = crow::response(mcp_response.http_status, response_json.dump());
    response.set_header("Content-Type", "application/json");
    if (!mcp_response.www_authenticate.empty()) {
        response.set_header("WWW-Authenticate", mcp_response.www_authenticate);
    }
    addSessionHeaderToResponse(response, session_id);
    return response;
}

crow::response MCPRouteHandlers::createJsonRpcErrorResponse(const std::string& id, int code, const std::string& message,
                                                             const std::optional<std::string>& session_id) const {
    crow::json::wvalue error_json;
    error_json["code"] = code;
    error_json["message"] = message;

    crow::json::wvalue response_json;
    response_json["jsonrpc"] = "2.0";

    // Transport-level errors (parse/internal) cannot know the request id, so
    // callers pass an empty id and the spec-correct echo is null. A non-empty id
    // here is treated as a plain string (never re-parsed via std::stod).
    if (id.empty()) {
        response_json["id"] = nullptr;
    } else {
        response_json["id"] = id;
    }

    response_json["error"] = std::move(error_json);

    auto response = crow::response(400, response_json.dump());
    response.set_header("Content-Type", "application/json");
    addSessionHeaderToResponse(response, session_id);
    return response;
}

std::vector<crow::json::wvalue> MCPRouteHandlers::getToolDefinitionsFromConfig() const {
    auto tools = getToolDefinitionsImpl();
    CROW_LOG_DEBUG << "getToolDefinitionsFromConfig returning " << tools.size() << " tools";
    return tools;
}

std::vector<crow::json::wvalue> MCPRouteHandlers::getResourceDefinitionsFromConfig() const {
    return getResourceDefinitionsImpl();
}

void MCPRouteHandlers::discoverMCPEntitiesImpl() {
    CROW_LOG_DEBUG << "Starting MCP entity discovery...";
    std::lock_guard<std::mutex> lock(state_mutex_);

    tool_definitions_.clear();
    resource_definitions_.clear();

    const auto& endpoints = config_manager_->getEndpoints();
    CROW_LOG_INFO << "Found " << endpoints.size() << " total endpoints in config manager";

    if (endpoints.empty()) {
        CROW_LOG_WARNING << "No endpoints found in config manager - tool discovery will be empty";
        return;
    }

    for (const auto& endpoint : endpoints) {
        CROW_LOG_DEBUG << "Checking endpoint: REST=" << endpoint.isRESTEndpoint()
                       << ", MCPTool=" << endpoint.isMCPTool()
                       << ", MCPResource=" << endpoint.isMCPResource();

        if (endpoint.isMCPTool()) {
            CROW_LOG_INFO << "Adding MCP tool: " << (endpoint.mcp_tool ? endpoint.mcp_tool->name : "null");
            tool_definitions_.push_back(endpointToMCPToolDefinition(endpoint));
        }
        if (endpoint.isMCPResource()) {
            CROW_LOG_INFO << "Adding MCP resource: " << (endpoint.mcp_resource ? endpoint.mcp_resource->name : "null");
            resource_definitions_.push_back(endpointToMCPResourceDefinition(endpoint));
        }
    }

    CROW_LOG_INFO << "Discovered " << tool_definitions_.size() << " MCP tools and "
                   << resource_definitions_.size() << " MCP resources";
}

std::vector<crow::json::wvalue> MCPRouteHandlers::getToolDefinitionsImpl() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    CROW_LOG_DEBUG << "getToolDefinitionsImpl returning " << tool_definitions_.size() << " tools from member variable";
    return tool_definitions_;
}

std::vector<crow::json::wvalue> MCPRouteHandlers::getResourceDefinitionsImpl() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return resource_definitions_;
}

crow::json::wvalue MCPRouteHandlers::endpointToMCPToolDefinition(const EndpointConfig& endpoint) const {
    crow::json::wvalue tool_def;

    // Basic tool information
    tool_def["name"] = endpoint.mcp_tool->name;
    tool_def["description"] = endpoint.mcp_tool->description;

    // Build a typed input schema from the request-field validators (int/date/
    // uuid/enum/... with min/max/regex) rather than typing every parameter as a
    // bare string. The validator metadata already drives prepared-statement
    // binding; projecting it lets the model call the tool correctly first time.
    tool_def["inputSchema"] = MCPSchemaBuilder::buildInputSchema(endpoint.request_fields);

    return tool_def;
}

crow::json::wvalue MCPRouteHandlers::endpointToMCPResourceDefinition(const EndpointConfig& endpoint) const {
    crow::json::wvalue resource_def;

    resource_def["name"] = endpoint.mcp_resource->name;
    resource_def["description"] = endpoint.mcp_resource->description;
    resource_def["mimeType"] = endpoint.mcp_resource->mime_type;
    resource_def["uri"] = "flapi://" + endpoint.mcp_resource->name;

    return resource_def;
}

// JSON-RPC message handling methods
MCPResponse MCPRouteHandlers::handleMessage(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    response.id = request.id;

    CROW_LOG_DEBUG << "handleMessage called with method: '" << request.method << "'";

    try {
        // Methods removed in 2026-07-28: initialize (replaced by server/discover
        // + per-request _meta), ping and logging/setLevel (log level is per-
        // request _meta now). Reject them on the modern path; keep them on legacy.
        const bool modern = request.modern_era;
        if (modern && (request.method == "initialize" || request.method == "ping"
                       || request.method == "logging/setLevel")) {
            response.error = "{\"code\":-32601,\"message\":\"Method not available on the 2026-07-28 path: "
                             + request.method + "\"}";
        } else if (request.method == "server/discover") {
            response = handleServerDiscoverRequest(request, http_req);
        } else if (request.method == "tasks/get") {
            response = handleTasksGetRequest(request, http_req);
        } else if (request.method == "tasks/cancel") {
            response = handleTasksCancelRequest(request, http_req);
        } else if (request.method == "initialize") {
            response = handleInitializeRequest(request, http_req);
        } else if (request.method == "tools/list") {
            response = handleToolsListRequest(request, http_req);
        } else if (request.method == "tools/call") {
            response = handleToolsCallRequest(request, http_req);
        } else if (request.method == "resources/list") {
            response = handleResourcesListRequest(request, http_req);
        } else if (request.method == "resources/read") {
            response = handleResourcesReadRequest(request, http_req);
        } else if (request.method == "resources/templates/list") {
            response = handleResourcesTemplatesListRequest(request, http_req);
        } else if (request.method == "prompts/list") {
            response = handlePromptsListRequest(request, http_req);
        } else if (request.method == "prompts/get") {
            response = handlePromptsGetRequest(request, http_req);
        } else if (request.method == "logging/setLevel") {
            response = handleLoggingSetLevelRequest(request, http_req);
        } else if (request.method == "completion/complete") {
            response = handleCompletionCompleteRequest(request, http_req);
        } else if (request.method == "ping") {
            response = handlePingRequest(request, http_req);
        } else {
            response.error = "{\"code\":-32601,\"message\":\"Method not found\"}";
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error handling MCP method '" << request.method << "': " << e.what();
        response.error = "{\"code\":-32603,\"message\":\"Internal error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleServerDiscoverRequest(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    response.id = request.id;

    try {
        crow::json::wvalue result;

        crow::json::wvalue versions = crow::json::wvalue::list();
        size_t i = 0;
        for (const auto* v : flapi::mcp::constants::MCP_SUPPORTED_VERSIONS) {
            versions[i++] = std::string(v);
        }
        result["supportedVersions"] = std::move(versions);

        // Honest capabilities: no listChanged transport; advertise the Tasks
        // extension so clients can opt into asynchronous tool execution.
        result["capabilities"]["tools"]["listChanged"] = false;
        result["capabilities"]["resources"]["subscribe"] = false;
        result["capabilities"]["resources"]["listChanged"] = false;
        result["capabilities"]["prompts"]["listChanged"] = false;
        result["capabilities"]["completions"] = crow::json::wvalue::object();
        result["capabilities"]["extensions"][flapi::mcp::constants::EXTENSION_TASKS] =
            crow::json::wvalue::object();

        std::string instructions = config_manager_->loadMCPInstructions();
        if (!instructions.empty()) {
            result["instructions"] = instructions;
        }

        result["_meta"][flapi::mcp::constants::META_SERVER_INFO]["name"] = server_info_.name;
        result["_meta"][flapi::mcp::constants::META_SERVER_INFO]["version"] = server_info_.version;

        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "server/discover error: " + std::string(e.what()));
    }

    return response;
}

bool MCPRouteHandlers::clientSupportsTasks(const MCPRequest& request) {
    for (const auto& ext : request.meta_extensions) {
        if (ext == flapi::mcp::constants::EXTENSION_TASKS) {
            return true;
        }
    }
    return false;
}

crow::json::wvalue MCPRouteHandlers::taskToJson(const MCPTaskManager::Task& task) const {
    crow::json::wvalue t;
    t["taskId"] = task.task_id;
    switch (task.status) {
        case MCPTaskManager::Status::Working:   t["status"] = "working"; break;
        case MCPTaskManager::Status::Completed: t["status"] = "completed"; break;
        case MCPTaskManager::Status::Failed:    t["status"] = "failed"; break;
        case MCPTaskManager::Status::Cancelled: t["status"] = "cancelled"; break;
    }
    t["pollIntervalMs"] = static_cast<int64_t>(task.poll_interval_ms);
    t["ttlMs"] = static_cast<int64_t>(task.ttl_ms);
    return t;
}

MCPResponse MCPRouteHandlers::handleTasksGetRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    try {
        std::string task_id;
        if (!extractRequiredStringParam(request.params, "taskId", task_id, response)) {
            return response;
        }
        std::string principal;
        if (auth_handler_) {
            auto ac = auth_handler_->authenticate(http_req);
            if (ac && !ac->username.empty()) {
                principal = ac->username;
            }
        }
        if (principal.empty()) {
            principal = "anonymous";
        }

        MCPTaskManager::Task task;
        bool found = false;
        if (!task_manager_ || !task_manager_->get(task_id, principal, task, found)) {
            // Not found and not-authorized are both reported as not found so a
            // task id cannot be probed across principals.
            response.error = formatJsonRpcError(-32602, "Task not found: " + task_id);
            return response;
        }

        crow::json::wvalue result;
        result["task"] = taskToJson(task);
        if (task.status == MCPTaskManager::Status::Completed) {
            result["result"] = crow::json::load(task.result_json);
        } else if (task.status == MCPTaskManager::Status::Failed) {
            result["error"] = task.error_message;
        }
        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "tasks/get error: " + std::string(e.what()));
    }
    return response;
}

MCPResponse MCPRouteHandlers::handleTasksCancelRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    try {
        std::string task_id;
        if (!extractRequiredStringParam(request.params, "taskId", task_id, response)) {
            return response;
        }
        std::string principal;
        if (auth_handler_) {
            auto ac = auth_handler_->authenticate(http_req);
            if (ac && !ac->username.empty()) {
                principal = ac->username;
            }
        }
        if (principal.empty()) {
            principal = "anonymous";
        }

        const bool ok = task_manager_ && task_manager_->cancel(task_id, principal);
        if (!ok) {
            response.error = formatJsonRpcError(-32602, "Task not found: " + task_id);
            return response;
        }
        crow::json::wvalue result;
        result["taskId"] = task_id;
        result["status"] = "cancelling";
        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "tasks/cancel error: " + std::string(e.what()));
    }
    return response;
}

MCPResponse MCPRouteHandlers::handleInitializeRequest(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    response.id = request.id;

    // Layer 1: Protocol-level authentication (mcp.auth)
    std::optional<MCPSession::AuthContext> auth_context;
    if (auth_handler_ && auth_handler_->methodRequiresAuth("initialize")) {
        auth_context = auth_handler_->authenticate(http_req);
        if (!auth_context) {
            CROW_LOG_WARNING << "MCP initialize: authentication required but failed";
            response.error = "{\"code\":-32000,\"message\":\"Authentication required for initialize\"}";
            response.http_status = 401;
            response.www_authenticate = buildWwwAuthenticate(http_req, /*insufficient_scope=*/false);
            return response;
        }
        CROW_LOG_INFO << "MCP initialize: authenticated as " << auth_context->username;
    }

    try {
        // Extract client protocol version from request params
        std::string client_version = "2024-11-05";  // Default to oldest supported for compatibility
        if (request.params.count("protocolVersion") > 0) {
            auto version_value = request.params["protocolVersion"];
            if (version_value.t() == crow::json::type::String) {
                std::string version_str = version_value.dump();
                // Remove quotes if present
                if (version_str.length() >= 2 && version_str.front() == '"' && version_str.back() == '"') {
                    client_version = version_str.substr(1, version_str.length() - 2);
                } else {
                    client_version = version_str;
                }
            }
        }

        // Negotiate protocol version - select highest mutually supported version
        // Supported versions: 2024-11-05, 2025-03-26, 2025-06-18, 2025-11-25
        std::string negotiated_version = "2024-11-05";  // Minimum supported

        if (client_version == "2025-11-25" || client_version == "2025-06-18" ||
            client_version == "2025-03-26" || client_version == "2024-11-05") {
            // Client requested a supported version - use the minimum of client and server
            if (client_version == "2025-11-25") {
                negotiated_version = "2025-11-25";
            } else if (client_version == "2025-06-18") {
                negotiated_version = "2025-06-18";
            } else if (client_version == "2025-03-26") {
                negotiated_version = "2025-03-26";
            } else {
                negotiated_version = "2024-11-05";
            }
        } else {
            // Unknown version - try to handle gracefully by using latest server supports
            CROW_LOG_WARNING << "Client requested unknown protocol version: " << client_version
                           << ", using server default: " << server_info_.protocol_version;
            negotiated_version = server_info_.protocol_version;
        }

        CROW_LOG_DEBUG << "Protocol version negotiated - Client: " << client_version
                      << ", Server: " << server_info_.protocol_version
                      << ", Negotiated: " << negotiated_version;

        crow::json::wvalue result;
        result["protocolVersion"] = negotiated_version;
        result["capabilities"] = crow::json::wvalue();
        // listChanged is advertised as false: flAPI has no server→client
        // notification transport (no SSE/GET stream), so it never emits
        // notifications/tools/list_changed et al. Advertising true while never
        // emitting misleads clients into caching-with-invalidation they will
        // never receive. Honest capability = false.
        result["capabilities"]["tools"] = crow::json::wvalue();
        result["capabilities"]["tools"]["listChanged"] = false;
        result["capabilities"]["resources"] = crow::json::wvalue();
        result["capabilities"]["resources"]["subscribe"] = false;
        result["capabilities"]["resources"]["listChanged"] = false;
        result["capabilities"]["prompts"] = crow::json::wvalue();
        result["capabilities"]["prompts"]["listChanged"] = false;
        result["capabilities"]["logging"] = crow::json::wvalue::object();  // NEW in 2025-11-25; must serialize as {} — null fails strict client schema validation (#100)
        result["serverInfo"]["name"] = server_info_.name;
        result["serverInfo"]["version"] = server_info_.version;

        // Add instructions if configured
        std::string instructions = config_manager_->loadMCPInstructions();
        if (!instructions.empty()) {
            result["instructions"] = instructions;
            CROW_LOG_DEBUG << "Included MCP instructions in initialize response ("
                           << instructions.length() << " characters)";
        }

        response.result = result.dump();
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Initialize error: " << e.what();
        response.error = "{\"code\":-32603,\"message\":\"Initialize error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleToolsListRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        auto tool_definitions = getToolDefinitionsFromConfig();
        CROW_LOG_DEBUG << "Tools list request: found " << tool_definitions.size() << " tools";

        size_t offset = 0, count = tool_definitions.size();
        std::string next_cursor;
        if (!applyPagination(request, tool_definitions.size(), offset, count, next_cursor, response)) {
            return response;
        }

        crow::json::wvalue result;
        crow::json::wvalue tools_array = crow::json::wvalue::list();

        for (size_t i = 0; i < count; ++i) {
            auto def = crow::json::load(tool_definitions[offset + i].dump());
            crow::json::wvalue tool_def(def);
            // Attach a learned outputSchema when this tool has been called at
            // least once (endpoint tools only; config tools carry their own).
            if (def && def.has("name") && !def.has("outputSchema")) {
                std::string schema = getCachedOutputSchema(def["name"].s());
                if (!schema.empty()) {
                    tool_def["outputSchema"] = crow::json::load(schema);
                }
            }
            tools_array[i] = std::move(tool_def);
        }

        result["tools"] = std::move(tools_array);
        if (!next_cursor.empty()) {
            result["nextCursor"] = next_cursor;
        }

        response.result = result.dump();
        CROW_LOG_DEBUG << "Tools list response: " << response.result;
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Tools list error: " << e.what();
        response.error = formatJsonRpcError(-32603, "Tools list error: " + std::string(e.what()));
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleToolsCallRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        // Extract tool name from params
        std::string tool_name;
        if (!extractRequiredStringParam(request.params, "name", tool_name, response)) {
            return response;
        }

        // Extract arguments from params
        crow::json::wvalue arguments;
        if (request.params.count("arguments") > 0) {
            arguments = crow::json::wvalue(request.params["arguments"]);
        } else {
            arguments = crow::json::wvalue::object();
        }

        CROW_LOG_DEBUG << "Tool call request: " << tool_name;

        // Check if this is a config tool (starts with "flapi_")
        if (tool_name.find("flapi_") == 0) {
            // Config tool - use ConfigToolAdapter
            if (config_tool_adapter_) {
                // Extract auth token from request if present
                std::string auth_token;
                if (http_req.headers.count("Authorization") > 0) {
                    auto auth_header = http_req.get_header_value("Authorization");
                    if (auth_header.find("Bearer ") == 0) {
                        auth_token = auth_header.substr(7);
                    }
                }

                auto config_result = config_tool_adapter_->executeTool(tool_name, arguments, auth_token);

                if (config_result.success) {
                    // Convert result to MCP format using ContentResponse
                    mcp::ContentResponse content_response;
                    content_response.addText(config_result.result);
                    crow::json::wvalue mcp_result = content_response.toJson();
                    response.result = mcp_result.dump();
                } else {
                    // Config-tool execution failure is model-actionable → isError.
                    mcp::ContentResponse content_response;
                    content_response.addText("Tool execution failed: " + config_result.error_message);
                    content_response.setError(true);
                    response.result = content_response.toJson().dump();
                }
            } else {
                response.error = formatJsonRpcError(-32603, "Tool execution failed: Config tools not available");
            }
        } else {
            // Endpoint tool - use MCPToolHandler
            if (tool_handler_) {
                MCPToolCallRequest tool_request;
                tool_request.tool_name = tool_name;
                tool_request.arguments = crow::json::wvalue(arguments);

                // Plumb authenticated caller's identity into the tool request:
                //  - roles for W2.1 per-tool RBAC
                //  - username for W1.3 audit log and W2.5 per-tool rate-limit
                //    principal keying
                if (auth_handler_) {
                    auto auth_context = auth_handler_->authenticate(http_req);
                    if (auth_context) {
                        if (!auth_context->username.empty()) {
                            tool_request.context["auth.username"] = auth_context->username;
                        }
                        if (!auth_context->roles.empty()) {
                            std::string roles_csv;
                            for (size_t i = 0; i < auth_context->roles.size(); ++i) {
                                if (i > 0) {
                                    roles_csv += ",";
                                }
                                roles_csv += auth_context->roles[i];
                            }
                            tool_request.context[MCPToolCallRequest::kRolesContextKey] = roles_csv;
                        }
                    }
                }

                // MCP 2026-07-28 Tasks: run the tool as a durable task when it is
                // configured async (or async-after) AND the client declared the
                // tasks capability. A client that cannot poll never sees a task —
                // it falls through to the synchronous path below.
                const EndpointConfig* ep = nullptr;
                for (const auto& e : config_manager_->getEndpoints()) {
                    if (e.isMCPTool() && e.mcp_tool->name == tool_name) {
                        ep = &e;
                        break;
                    }
                }
                const bool tool_async = ep && ep->mcp_tool
                    && (ep->mcp_tool->async || ep->mcp_tool->async_after_ms > 0);
                if (request.modern_era && clientSupportsTasks(request) && tool_async
                    && task_manager_ && tool_handler_) {
                    const auto& mcp_cfg = config_manager_->getMCPConfig();
                    std::string principal = "anonymous";
                    auto pit = tool_request.context.find("auth.username");
                    if (pit != tool_request.context.end() && !pit->second.empty()) {
                        principal = pit->second;
                    }
                    // The work runs the tool and serializes the same envelope a
                    // synchronous call would (content + structuredContent, or an
                    // isError text block on failure).
                    MCPToolHandler* handler = tool_handler_.get();
                    auto work = [handler, tool_request](const std::atomic<bool>&) -> std::string {
                        auto r = handler->executeTool(tool_request);
                        mcp::ContentResponse cr;
                        if (r.success) {
                            cr.addText(r.result);
                            auto rows = crow::json::load(r.result);
                            if (rows) {
                                crow::json::wvalue sc;
                                sc["rows"] = crow::json::wvalue(rows);
                                if (rows.t() == crow::json::type::List) {
                                    sc["row_count"] = static_cast<int64_t>(rows.size());
                                }
                                cr.setStructuredContent(std::move(sc));
                            }
                        } else {
                            cr.addText(r.error_message);
                            cr.setError(true);
                        }
                        return cr.toJson().dump();
                    };

                    std::string task_id = task_manager_->submit(
                        tool_name, principal,
                        mcp_cfg.tasks_default_ttl_ms, mcp_cfg.tasks_poll_interval_ms, work);
                    if (task_id.empty()) {
                        response.error = formatJsonRpcError(-32603,
                            "Task queue is full; retry shortly");
                        return response;
                    }

                    // async-after: give the work a synchronous grace period; if it
                    // finishes in time, return the result inline, else hand back a
                    // task. Pure `async` returns the task immediately.
                    if (!ep->mcp_tool->async && ep->mcp_tool->async_after_ms > 0) {
                        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(ep->mcp_tool->async_after_ms);
                        while (std::chrono::steady_clock::now() < deadline) {
                            MCPTaskManager::Task t;
                            bool found = false;
                            if (task_manager_->get(task_id, principal, t, found)
                                && t.status == MCPTaskManager::Status::Completed) {
                                response.result = t.result_json;
                                return response;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    }

                    crow::json::wvalue task_result;
                    MCPTaskManager::Task t;
                    bool found = false;
                    task_manager_->get(task_id, principal, t, found);
                    task_result["task"] = taskToJson(t);
                    response.result = task_result.dump();
                    return response;
                }

                auto result = tool_handler_->executeTool(tool_request);

                if (result.success) {
                    // Convert result to MCP format using ContentResponse. The
                    // serialized rows are also attached as structuredContent so
                    // a client gets machine-readable JSON without re-parsing the
                    // text block.
                    mcp::ContentResponse content_response;
                    content_response.addText(result.result);

                    auto parsed_rows = crow::json::load(result.result);
                    if (parsed_rows) {
                        crow::json::wvalue structured;
                        structured["rows"] = crow::json::wvalue(parsed_rows);
                        if (parsed_rows.t() == crow::json::type::List) {
                            structured["row_count"] = static_cast<int64_t>(parsed_rows.size());
                        }
                        content_response.setStructuredContent(std::move(structured));
                    }
                    // Learn this tool's outputSchema from the real result columns
                    // so a subsequent tools/list can advertise it.
                    cacheOutputSchemaFromRows(tool_name, result.result);

                    crow::json::wvalue mcp_result = content_response.toJson();
                    response.result = mcp_result.dump();
                } else if (result.failure_kind == MCPToolExecutionResult::FailureKind::NotFound) {
                    // Unknown tool is a protocol-level error the model cannot fix.
                    response.error = formatJsonRpcError(-32602, result.error_message);
                } else if (result.failure_kind == MCPToolExecutionResult::FailureKind::PermissionDenied) {
                    // RBAC denial: the caller authenticated (Layer 1) but lacks
                    // the tool's role → HTTP 403 insufficient_scope (RFC 6750).
                    response.error = formatJsonRpcError(-32000, result.error_message);
                    response.http_status = 403;
                    response.www_authenticate = buildWwwAuthenticate(http_req, /*insufficient_scope=*/true);
                } else {
                    // Tool-execution failures the model CAN act on (bad
                    // arguments, a SQL/runtime error, a rate limit) are returned
                    // as a normal result with isError:true, per the MCP spec, so
                    // the error text reaches the model instead of an opaque
                    // protocol failure.
                    mcp::ContentResponse content_response;
                    std::string message = result.error_message;
                    auto rl = result.metadata.find("retry_after_seconds");
                    if (rl != result.metadata.end()) {
                        message += " (retry_after_seconds=" + rl->second + ")";
                    }
                    content_response.addText(message);
                    content_response.setError(true);
                    crow::json::wvalue mcp_result = content_response.toJson();
                    response.result = mcp_result.dump();
                }
            } else {
                response.error = formatJsonRpcError(-32601, "Tool handler not available");
            }
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Tool call error: " << e.what();
        response.error = formatJsonRpcError(-32603, "Tool call error: " + std::string(e.what()));
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleResourcesListRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        auto resource_definitions = getResourceDefinitionsFromConfig();

        size_t offset = 0, count = resource_definitions.size();
        std::string next_cursor;
        if (!applyPagination(request, resource_definitions.size(), offset, count, next_cursor, response)) {
            return response;
        }

        crow::json::wvalue result;
        crow::json::wvalue resources_array = crow::json::wvalue::list();

        for (size_t i = 0; i < count; ++i) {
            resources_array[i] = crow::json::wvalue(resource_definitions[offset + i]);
        }

        result["resources"] = std::move(resources_array);
        if (!next_cursor.empty()) {
            result["nextCursor"] = next_cursor;
        }

        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "Resources list error: " + std::string(e.what()));
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleResourcesTemplatesListRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);

    try {
        // Collect resources that declare a uri-template.
        std::vector<crow::json::wvalue> templates;
        for (const auto& endpoint : config_manager_->getEndpoints()) {
            if (endpoint.isMCPResource() && !endpoint.mcp_resource->uri_template.empty()) {
                crow::json::wvalue t;
                t["uriTemplate"] = endpoint.mcp_resource->uri_template;
                t["name"] = endpoint.mcp_resource->name;
                t["description"] = endpoint.mcp_resource->description;
                t["mimeType"] = endpoint.mcp_resource->mime_type;
                templates.push_back(std::move(t));
            }
        }

        size_t offset = 0, count = templates.size();
        std::string next_cursor;
        if (!applyPagination(request, templates.size(), offset, count, next_cursor, response)) {
            return response;
        }

        crow::json::wvalue result;
        crow::json::wvalue arr = crow::json::wvalue::list();
        for (size_t i = 0; i < count; ++i) {
            arr[i] = crow::json::wvalue(templates[offset + i]);
        }
        result["resourceTemplates"] = std::move(arr);
        if (!next_cursor.empty()) {
            result["nextCursor"] = next_cursor;
        }
        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "Resource templates list error: " + std::string(e.what()));
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleResourcesReadRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        // Extract resource URI from params
        std::string resource_uri;
        if (!extractRequiredStringParam(request.params, "uri", resource_uri, response)) {
            return response;
        }
        CROW_LOG_DEBUG << "Resource read request: " << resource_uri;

        // Find the resource configuration by URI (exact or uri-template match).
        std::map<std::string, std::string> bound_params;
        auto resource_config = findResourceByURI(resource_uri, bound_params);
        if (!resource_config) {
            response.error = formatJsonRpcError(-32602, "Resource not found: " + resource_uri);
            return response;
        }

        // Layer-2 per-resource RBAC. Without this, resources/read executed the
        // query with no authorization behind the Layer-1 method check — so an
        // anonymous caller who reached this handler could read any resource's
        // full result. Runs before executing the query.
        if (auto denial = authorizeMCPEntity(
                http_req, resource_config->mcp_resource->allowed_roles,
                "Resource '" + resource_config->mcp_resource->name + "'")) {
            CROW_LOG_WARNING << "MCP resources/read denied for '"
                             << resource_config->mcp_resource->name << "': " << *denial;
            // The caller already passed Layer-1 authentication to reach here, so
            // a role denial is an authorization failure: HTTP 403 with
            // error="insufficient_scope" (RFC 6750).
            response.error = formatJsonRpcError(-32000, "Permission denied: " + *denial);
            response.http_status = 403;
            response.www_authenticate = buildWwwAuthenticate(http_req, /*insufficient_scope=*/true);
            return response;
        }

        CROW_LOG_DEBUG << "Reading resource: " << resource_config->mcp_resource->name;

        // Read the resource content (binding any uri-template path params).
        try {
            crow::json::wvalue result = readResourceContent(*resource_config, bound_params);
            response.result = result.dump();
        } catch (const std::exception& e) {
            response.error = formatJsonRpcError(-32603, "Resource read error: " + std::string(e.what()));
        }
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "Resource read error: " + std::string(e.what()));
    }

    return response;
}

namespace {

// Match a concrete URI against a `flapi://.../{var}/...` template. On success,
// fills `out` with each {var} -> the corresponding path segment and returns
// true. Only simple single-segment {var} expansion is supported (no reserved
// expansion, no query templates). A segment bound to a var must be non-empty
// and contain no '/'.
bool matchUriTemplate(const std::string& tmpl, const std::string& uri,
                      std::map<std::string, std::string>& out) {
    size_t ti = 0, ui = 0;
    std::map<std::string, std::string> bound;
    while (ti < tmpl.size()) {
        if (tmpl[ti] == '{') {
            size_t close = tmpl.find('}', ti);
            if (close == std::string::npos) {
                return false;
            }
            std::string var = tmpl.substr(ti + 1, close - ti - 1);
            // The variable captures up to the next literal char in the template
            // (or end of string).
            char delim = (close + 1 < tmpl.size()) ? tmpl[close + 1] : '\0';
            size_t seg_end = (delim == '\0') ? uri.size() : uri.find(delim, ui);
            if (seg_end == std::string::npos) {
                seg_end = uri.size();
            }
            std::string value = uri.substr(ui, seg_end - ui);
            if (value.empty() || value.find('/') != std::string::npos) {
                return false;
            }
            bound[var] = value;
            ui = seg_end;
            ti = close + 1;
        } else {
            if (ui >= uri.size() || uri[ui] != tmpl[ti]) {
                return false;
            }
            ++ti;
            ++ui;
        }
    }
    if (ui != uri.size()) {
        return false;
    }
    out = std::move(bound);
    return true;
}

} // namespace

// Resource functionality implementation
std::optional<EndpointConfig> MCPRouteHandlers::findResourceByURI(
    const std::string& uri, std::map<std::string, std::string>& bound_params) const {
    bound_params.clear();
    if (uri.find("flapi://") != 0) {
        return std::nullopt;
    }

    const auto& endpoints = config_manager_->getEndpoints();

    // 1) Exact static match: flapi://<name>.
    std::string resource_name = uri.substr(8);
    for (const auto& endpoint : endpoints) {
        if (endpoint.isMCPResource() && endpoint.mcp_resource->uri_template.empty()
            && endpoint.mcp_resource->name == resource_name) {
            return endpoint;
        }
    }

    // 2) Templated match: bind {var} path segments into params.
    for (const auto& endpoint : endpoints) {
        if (endpoint.isMCPResource() && !endpoint.mcp_resource->uri_template.empty()) {
            std::map<std::string, std::string> bound;
            if (matchUriTemplate(endpoint.mcp_resource->uri_template, uri, bound)) {
                bound_params = std::move(bound);
                return endpoint;
            }
        }
    }

    return std::nullopt;
}

crow::json::wvalue MCPRouteHandlers::readResourceContent(
    const EndpointConfig& resource_config, const std::map<std::string, std::string>& params) const {
    crow::json::wvalue result;

    // The reported URI is the template (with bound values substituted) when the
    // resource is parameterised, else the static flapi://<name>.
    std::string reported_uri = "flapi://" + resource_config.mcp_resource->name;
    if (!resource_config.mcp_resource->uri_template.empty()) {
        reported_uri = resource_config.mcp_resource->uri_template;
        for (const auto& [k, v] : params) {
            std::string placeholder = "{" + k + "}";
            size_t pos;
            while ((pos = reported_uri.find(placeholder)) != std::string::npos) {
                reported_uri.replace(pos, placeholder.size(), v);
            }
        }
    }

    // Execute the resource's template to get the content
    try {
        // For MCP resources, we need to execute the SQL template using the database manager
        if (db_manager_) {
            // Execute the query using the same method as tools, binding any
            // params extracted from a URI template.
            std::map<std::string, std::string> query_params = params;
            auto query_result = db_manager_->executeQuery(resource_config, query_params, false);

            // Check if the query result structure has the data we need
            if (query_result.data.size() > 0) {
                // Get the result as a JSON string
                std::string result_text;
                if (resource_config.mcp_resource->mime_type == "application/json" ||
                    resource_config.mcp_resource->mime_type.find("json") != std::string::npos) {
                    result_text = query_result.data.dump();
                } else {
                    // For text/plain, convert JSON to a readable text format
                    result_text = query_result.data.dump(); // For now, use JSON format
                }

                // Convert the result to MCP resource content format
                crow::json::wvalue content_item;
                content_item["uri"] = reported_uri;
                content_item["mimeType"] = resource_config.mcp_resource->mime_type;
                content_item["text"] = result_text;

                crow::json::wvalue contents_array = crow::json::wvalue::list();
                contents_array[0] = std::move(content_item);

                result["contents"] = std::move(contents_array);
            } else {
                throw std::runtime_error("Resource query returned no data");
            }
        } else {
            // Fallback: return a simple resource representation
            crow::json::wvalue content_item;
            content_item["uri"] = reported_uri;
            content_item["mimeType"] = resource_config.mcp_resource->mime_type;
            content_item["text"] = "Resource content for: " + resource_config.mcp_resource->name + " (database not available)";

            crow::json::wvalue contents_array = crow::json::wvalue::list();
            contents_array[0] = std::move(content_item);

            result["contents"] = std::move(contents_array);
        }
    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error reading resource " << resource_config.mcp_resource->name << ": " << e.what();
        throw;
    }

    return result;
}

// Prompt functionality implementation
MCPResponse MCPRouteHandlers::handlePromptsListRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        // Collect all prompt definitions first so pagination has a stable count.
        std::vector<crow::json::wvalue> prompt_defs;
        const auto& endpoints = config_manager_->getEndpoints();
        for (const auto& endpoint : endpoints) {
            if (endpoint.isMCPPrompt()) {
                prompt_defs.push_back(endpointToMCPPromptDefinition(endpoint));
            }
        }

        size_t offset = 0, count = prompt_defs.size();
        std::string next_cursor;
        if (!applyPagination(request, prompt_defs.size(), offset, count, next_cursor, response)) {
            return response;
        }

        crow::json::wvalue result;
        crow::json::wvalue prompts_array = crow::json::wvalue::list();
        for (size_t i = 0; i < count; ++i) {
            prompts_array[i] = crow::json::wvalue(prompt_defs[offset + i]);
        }

        result["prompts"] = std::move(prompts_array);
        if (!next_cursor.empty()) {
            result["nextCursor"] = next_cursor;
        }
        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "Prompts list error: " + std::string(e.what()));
    }

    return response;
}

MCPResponse MCPRouteHandlers::handlePromptsGetRequest(const MCPRequest& request, const crow::request& http_req) const {
    auto response = initResponse(request);
    // NOTE: http_req is available here for authentication when needed

    try {
        // Extract prompt name from parameters
        std::string prompt_name;
        if (!extractRequiredStringParam(request.params, "name", prompt_name, response)) {
            return response;
        }

        CROW_LOG_DEBUG << "Prompt get request: " << prompt_name;

        // Find the prompt configuration by name
        auto prompt_config = findPromptByName(prompt_name);
        if (!prompt_config) {
            response.error = formatJsonRpcError(-32602, "Prompt not found: " + prompt_name);
            return response;
        }

        // Layer-2 per-prompt RBAC (see handleResourcesReadRequest for rationale).
        if (auto denial = authorizeMCPEntity(
                http_req, prompt_config->mcp_prompt->allowed_roles,
                "Prompt '" + prompt_config->mcp_prompt->name + "'")) {
            CROW_LOG_WARNING << "MCP prompts/get denied for '"
                             << prompt_config->mcp_prompt->name << "': " << *denial;
            // The caller already passed Layer-1 authentication to reach here, so
            // a role denial is an authorization failure: HTTP 403 with
            // error="insufficient_scope" (RFC 6750).
            response.error = formatJsonRpcError(-32000, "Permission denied: " + *denial);
            response.http_status = 403;
            response.www_authenticate = buildWwwAuthenticate(http_req, /*insufficient_scope=*/true);
            return response;
        }

        // Get arguments for template processing
        const crow::json::wvalue* arguments_ptr = nullptr;
        if (request.params.count("arguments") && request.params["arguments"].t() != crow::json::type::Null) {
            arguments_ptr = &request.params["arguments"];
        }

        // Process the prompt template
        crow::json::wvalue result = processPromptTemplate(*prompt_config, arguments_ptr);
        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = formatJsonRpcError(-32603, "Prompt get error: " + std::string(e.what()));
    }

    return response;
}

std::optional<EndpointConfig> MCPRouteHandlers::findPromptByName(const std::string& name) const {
    const auto& endpoints = config_manager_->getEndpoints();
    for (const auto& endpoint : endpoints) {
        if (endpoint.isMCPPrompt() && endpoint.mcp_prompt->name == name) {
            return endpoint;
        }
    }
    return std::nullopt;
}

crow::json::wvalue MCPRouteHandlers::processPromptTemplate(const EndpointConfig& prompt_config, const crow::json::wvalue* arguments) const {
    crow::json::wvalue result;

    try {
        std::string template_content = prompt_config.mcp_prompt->template_content;

        // Simple parameter substitution - replace {{param_name}} with argument values
        for (const auto& arg_name : prompt_config.mcp_prompt->arguments) {
            std::string placeholder = "{{" + arg_name + "}}";
            std::string replacement = "";

            if (arguments && arguments->count(arg_name) && (*arguments)[arg_name].t() != crow::json::type::Null) {
                const auto& arg_value = (*arguments)[arg_name];
                if (arg_value.t() == crow::json::type::String) {
                    std::string arg_str = arg_value.dump();
                    if (arg_str.length() >= 2 && arg_str.front() == '"' && arg_str.back() == '"') {
                        replacement = arg_str.substr(1, arg_str.length() - 2);
                    } else {
                        replacement = arg_str;
                    }
                } else {
                    replacement = arg_value.dump();
                }
            }

            size_t pos = 0;
            while ((pos = template_content.find(placeholder, pos)) != std::string::npos) {
                template_content.replace(pos, placeholder.length(), replacement);
                pos += replacement.length();
            }
        }

        // Create the prompt response
        result["description"] = prompt_config.mcp_prompt->description;
        result["messages"] = crow::json::wvalue::list();

        // Create a user message with the processed template
        crow::json::wvalue message;
        message["role"] = "user";
        message["content"]["type"] = "text";
        message["content"]["text"] = template_content;

        result["messages"][0] = std::move(message);

    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "Error processing prompt template " << prompt_config.mcp_prompt->name << ": " << e.what();
        throw;
    }

    return result;
}

crow::json::wvalue MCPRouteHandlers::endpointToMCPPromptDefinition(const EndpointConfig& endpoint) const {
    crow::json::wvalue prompt_def;

    prompt_def["name"] = endpoint.mcp_prompt->name;
    prompt_def["description"] = endpoint.mcp_prompt->description;

    // Add arguments as JSON schema
    prompt_def["arguments"] = crow::json::wvalue::list();
    for (size_t i = 0; i < endpoint.mcp_prompt->arguments.size(); ++i) {
        crow::json::wvalue arg_def;
        arg_def["name"] = endpoint.mcp_prompt->arguments[i];
        arg_def["type"] = "string";  // Default to string for now
        arg_def["description"] = "Parameter " + endpoint.mcp_prompt->arguments[i];
        prompt_def["arguments"][i] = std::move(arg_def);
    }

    return prompt_def;
}

// Ping functionality implementation
MCPResponse MCPRouteHandlers::handlePingRequest(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    // NOTE: http_req is available here for authentication when needed
    response.id = request.id;

    try {
        CROW_LOG_DEBUG << "Ping request received";

        // MCP ping response should be an empty object per specification
        // This complies with standard JSON-RPC ping implementations
        crow::json::wvalue result = crow::json::wvalue::object();
        // Empty object - no additional fields needed for ping

        response.result = result.dump();
    } catch (const std::exception& e) {
        response.error = "{\"code\":-32603,\"message\":\"Ping error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleLoggingSetLevelRequest(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    // NOTE: http_req is available here for authentication when needed
    response.id = request.id;

    try {
        // Extract log level from params
        if (request.params.count("level") == 0) {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: missing 'level' field\"}";
            return response;
        }

        auto level_value = request.params["level"];
        std::string log_level;

        if (JsonUtils::isString(level_value)) {
            log_level = JsonUtils::extractString(level_value);
        } else {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: 'level' must be a string\"}";
            return response;
        }

        // Map MCP log levels to Crow log levels
        // MCP levels: debug, info, notice, warning, error, critical, alert, emergency
        // Crow levels: Debug, Info, Warning, Error (lowercase to avoid Windows macro conflicts)
        crow::LogLevel crow_level = crow::LogLevel::Info;

        if (log_level == "debug") {
            crow_level = crow::LogLevel::Debug;
        } else if (log_level == "info" || log_level == "notice") {
            crow_level = crow::LogLevel::Info;
        } else if (log_level == "warning") {
            crow_level = crow::LogLevel::Warning;
        } else if (log_level == "error" || log_level == "critical" || log_level == "alert" || log_level == "emergency") {
            crow_level = crow::LogLevel::Error;
        } else {
            response.error = "{\"code\":-32602,\"message\":\"Invalid log level: " + log_level + "\"}";
            return response;
        }

        // Set the log level globally
        CROW_LOG_INFO << "Setting log level to: " << log_level;
        crow::logger::setLogLevel(crow_level);

        // Return success response (empty object)
        crow::json::wvalue result = crow::json::wvalue::object();
        response.result = result.dump();

    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "logging/setLevel error: " << e.what();
        response.error = "{\"code\":-32603,\"message\":\"Internal error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

MCPResponse MCPRouteHandlers::handleCompletionCompleteRequest(const MCPRequest& request, const crow::request& http_req) const {
    MCPResponse response;
    // NOTE: http_req is available here for authentication when needed
    response.id = request.id;

    try {
        // Extract parameters from request
        // ref: reference to tool/prompt/resource (e.g., "customer_lookup" or "prompt_name")
        // argument: argument name to complete
        // value: partial value prefix

        if (request.params.count("ref") == 0 || request.params.count("argument") == 0) {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: missing 'ref' or 'argument' field\"}";
            return response;
        }

        // Extract ref (reference to tool/prompt)
        auto ref_value = request.params["ref"];
        std::string ref_str;
        if (JsonUtils::isString(ref_value)) {
            ref_str = JsonUtils::extractString(ref_value);
        } else {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: 'ref' must be a string\"}";
            return response;
        }

        // Extract argument name
        auto arg_value = request.params["argument"];
        std::string argument_name;
        if (JsonUtils::isString(arg_value)) {
            argument_name = JsonUtils::extractString(arg_value);
        } else {
            response.error = "{\"code\":-32602,\"message\":\"Invalid params: 'argument' must be a string\"}";
            return response;
        }

        // Extract optional value (prefix) for filtering
        std::string value_prefix;
        if (request.params.count("value") > 0) {
            auto value_val = request.params["value"];
            if (JsonUtils::isString(value_val)) {
                value_prefix = JsonUtils::extractString(value_val);
            }
        }

        CROW_LOG_DEBUG << "Completion request: ref=" << ref_str << ", argument=" << argument_name
                      << ", prefix=" << value_prefix;

        // Find the tool/prompt by reference name
        std::optional<EndpointConfig> endpoint;
        auto endpoints = config_manager_->getEndpoints();
        for (const auto& ep : endpoints) {
            if ((ep.isMCPTool() && ep.mcp_tool && ep.mcp_tool->name == ref_str) ||
                (ep.isMCPPrompt() && ep.mcp_prompt && ep.mcp_prompt->name == ref_str)) {
                endpoint = ep;
                break;
            }
        }

        if (!endpoint) {
            response.error = "{\"code\":-32602,\"message\":\"Reference not found: " + ref_str + "\"}";
            return response;
        }

        // Find the argument field by name
        const RequestFieldConfig* matching_field = nullptr;
        for (const auto& field : endpoint->request_fields) {
            if (field.fieldName == argument_name) {
                matching_field = &field;
                break;
            }
        }

        if (!matching_field) {
            response.error = "{\"code\":-32602,\"message\":\"Argument not found: " + argument_name + "\"}";
            return response;
        }

        // Build completion suggestions based on validators
        crow::json::wvalue completion;
        crow::json::wvalue values = crow::json::wvalue::list();
        int total_count = 0;
        bool has_more = false;

        // Check for enum validator to provide enum values
        for (const auto& validator : matching_field->validators) {
            if (validator.type == "enum" && validator.allowedValues.size() > 0) {
                // Filter enum values based on prefix
                int added_count = 0;
                for (const auto& enum_val : validator.allowedValues) {
                    if (value_prefix.empty() || enum_val.find(value_prefix) == 0) {
                        if (added_count < 50) {  // Limit results to 50
                            values[added_count] = enum_val;
                            added_count++;
                        } else {
                            has_more = true;
                            break;
                        }
                    }
                    total_count++;
                }
                break;
            }
        }

        // If no enum validator, return empty completion (client can use its own methods)
        completion["values"] = std::move(values);
        completion["total"] = total_count;
        completion["hasMore"] = has_more;

        // The spec wraps the payload under a "completion" key:
        // { "completion": { "values": [...], "total": N, "hasMore": bool } }.
        crow::json::wvalue result;
        result["completion"] = std::move(completion);
        response.result = result.dump();
        CROW_LOG_DEBUG << "Completion result: " << response.result;

    } catch (const std::exception& e) {
        CROW_LOG_ERROR << "completion/complete error: " << e.what();
        response.error = "{\"code\":-32603,\"message\":\"Internal error: " + std::string(e.what()) + "\"}";
    }

    return response;
}

} // namespace flapi

