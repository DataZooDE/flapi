#pragma once

#include <crow.h>
#include "crow/middlewares/cors.h"
#include "crow/compression.h"

#include "auth_middleware.hpp"
#include "cors_middleware.hpp"
#include "rate_limit_middleware.hpp"
#include "request_context_middleware.hpp"

namespace flapi {

// The one and only spelling of flAPI's Crow application type.
//
// NEVER spell `crow::App<...>` out anywhere else. Every mention of the
// middleware list must be this alias, because `crow::App<A,B,C>` and
// `crow::App<A,B,C,D>` are two *distinct, valid* types: adding a middleware
// to one spelling and not another compiles, and silently produces a second
// middleware tuple that is default-constructed and never configured. The
// symptom is a middleware that mysteriously does nothing on some routes.
// This header exists so the alias can be shared by api_server.hpp,
// mcp_route_handlers.hpp and open_api_doc_generator.hpp without the include
// cycle that reaching for api_server.hpp would create.
// Guarded in CI by scripts/check_crow_app_alias.sh.
//
// Middleware order matters: `after_handle` runs in reverse order, so
// `FlapiCorsMiddleware` is declared BEFORE `crow::CORSHandler` to get the LAST word:
// Crow's handler writes its wildcard defaults first, then ours overwrites the origin
// for an allowed Origin and REMOVES them for a disallowed one. (It used to run
// before Crow's handler, which could not undo the `Access-Control-Allow-Origin: *`
// it then added - so a non-allowlisted origin was still granted access, #189.)
// RequestContextMiddleware is FIRST, deliberately: Crow runs before_handle in
// declaration order, so only first position brackets rate limiting and auth and
// therefore sees the 401/403/429 rejections an operator most often asks about.
using FlapiApp = crow::App<RequestContextMiddleware, FlapiCorsMiddleware, crow::CORSHandler, RateLimitMiddleware, AuthMiddleware>;

}  // namespace flapi
