#!/usr/bin/env bash
# Guard for the invariant documented on APIServer::warmEndpoint (#141):
# nothing in flapi may synthesise a crow::request and feed it to the router.
#
# A default-constructed crow::request has `io_service == nullptr` and
# `middleware_context == nullptr`. crow's app.handle_full() is the only way to
# push such a request through routing and middleware, and the heartbeat did
# exactly that - which cost the same null dereference twice in one release,
# once in the #120 handler offload and once in handleDynamicRequest() after the
# first fix routed synthesised requests inline to it. Both sites were then
# guarded on `req.middleware_context != nullptr`; both guards are gone, and the
# router now assumes - correctly - that every request it sees came off a socket.
#
# That assumption holds only while there is no synthetic caller. Background work
# that needs to drive an endpoint calls RequestHandler::handleRequest directly
# (see APIServer::warmEndpoint), or the cache layer directly (see
# HeartbeatWorker::performDuckLakeScheduledTasks).
set -euo pipefail
cd "$(dirname "$0")/.."

# `(.|->)handle_full(` is the call; the sed drops `file:line:` and then any hit
# whose line is a // or * comment, so the explanations above and in
# api_server.cpp do not trip their own guard.
offenders=$(grep -rnE '(\.|->)handle_full[[:space:]]*\(' src/ \
                 --include='*.hpp' --include='*.cpp' \
            | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)' || true)

if [ -n "$offenders" ]; then
    echo "ERROR: app.handle_full() reintroduces a synthetic request into the router:" >&2
    echo "$offenders" >&2
    echo >&2
    echo "The router assumes every request came off a socket - req.io_service and" >&2
    echo "req.middleware_context are dereferenced unguarded. Call" >&2
    echo "RequestHandler::handleRequest directly instead; see" >&2
    echo "APIServer::warmEndpoint in src/api_server.cpp." >&2
    exit 1
fi
echo "OK: no synthetic requests are routed through crow (handle_full absent from src/)."
