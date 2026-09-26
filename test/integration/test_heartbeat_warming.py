"""The heartbeat warms an endpoint without pretending to be an HTTP request (#141).

`APIServer::requestForEndpoint()` used to synthesise a bare `crow::request` and
call `app.handle_full()`, i.e. it entered the router and the middleware chain
with `req.io_service == nullptr` and `req.middleware_context == nullptr`. That
cost the same null dereference twice in one release - once in the #120 handler
offload, once in `handleDynamicRequest()` after the first fix routed such
requests inline to it - and was patched both times with a
`req.middleware_context != nullptr` guard.

`APIServer::warmEndpoint()` replaces it and calls `RequestHandler::handleRequest`
directly, so both guards are gone. This file is the coverage that had never
existed: `docs/CONFIG_REFERENCE.md` §2.10 said "**Tests:** *None*", and nothing
under test/ referenced `requestForEndpoint` except the crash regression in
test_handler_offload.py.

Four things are asserted here:

  1. the heartbeat still exercises the FULL serving path - connection, template
     render, query - for an endpoint with no cache at all. Warming, not
     refreshing: that is the behaviour the design deliberately keeps.
  2. `heartbeat.params` actually reach the template. This one is RED before
     #141: the params were parsed by `ConfigManager::parseEndpointHeartbeat()`
     and then never read, because `performHeartbeat()` called
     `requestForEndpoint(endpoint)` with its defaulted empty map.
  3. the process survives repeated heartbeat warm-ups (the original crash class).
  4. a heartbeat-backed cache is populated and served.
"""
import os
import signal
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

MARKER_VALUE = "hb-marker-4711"
# Only ever produced by rendering the template with the param present, so
# finding this in the log is positive evidence the value reached mustache. A
# quoted literal rather than a trailing `--` comment, so nothing appended after
# the template could be commented out.
RENDERED_MARKER = "'hb:" + MARKER_VALUE + "'"
# The inverted-section branch. Renders to invalid SQL on purpose, so when the
# param does NOT arrive the failure is loud - a DuckDB binder error quoting this
# identifier - instead of a silently-warmed-with-nothing success.
MISSING_MARKER = "hb_param_was_not_passed"

# `{{^...}}` renders only when the param is absent. Neither branch depends on
# anything but `params.hb_marker`, which comes from `heartbeat.params` alone -
# no HTTP request ever carries it.
WARM_SQL = (
    "{{^params.hb_marker}}SELECT " + MISSING_MARKER + "{{/params.hb_marker}}\n"
    "{{#params.hb_marker}}SELECT 1 AS n, 'hb:{{{ params.hb_marker }}}' AS hb"
    "{{/params.hb_marker}}\n"
)


class _Server:
    """A flapi with a 1s heartbeat over one uncached and one cached endpoint.

    Ephemeral port, temp config tree, no shared state with any other test.
    """

    def __init__(self, log_level="debug"):
        self.log_level = log_level
        self.tmp = tempfile.mkdtemp(prefix="flapi_heartbeat_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        self.proc = None

        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        os.makedirs(os.path.join(self.tmp, "data"), exist_ok=True)

        # NOT cached: the point of keeping the heartbeat a warm-up rather than a
        # refreshCache() call is that this endpoint gets exercised too.
        #
        # `hb_marker` is deliberately NOT declared under `request:` - it is not
        # client input. Undeclared params are accepted (request-fields-validation
        # defaults to false) and, being undeclared, the prepared-statement
        # rewriter leaves the value interpolated, so it appears verbatim in the
        # "Processed query" debug line these assertions read.
        with open(os.path.join(sqls, "warm.yaml"), "w") as f:
            f.write("url-path: /warm\nmethod: GET\n"
                    "template-source: warm.sql\nconnection: [inmem]\n"
                    "heartbeat:\n"
                    "  enabled: true\n"
                    "  params:\n"
                    f"    hb_marker: {MARKER_VALUE}\n")
        with open(os.path.join(sqls, "warm.sql"), "w") as f:
            f.write(WARM_SQL)

        with open(os.path.join(sqls, "cached.yaml"), "w") as f:
            f.write("url-path: /cached\nmethod: GET\n"
                    "template-source: cached.sql\nconnection: [inmem]\n"
                    "cache:\n"
                    "  enabled: true\n"
                    "  table: hb_cache\n"
                    "  schema: main\n"
                    "  schedule: 1s\n"
                    "heartbeat:\n  enabled: true\n")
        with open(os.path.join(sqls, "cached.sql"), "w") as f:
            f.write("SELECT 42 AS n\n")

        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: heartbeat-warming\n"
                "project-description: the heartbeat warms endpoints without routing\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                "duckdb:\n  access_mode: READ_WRITE\n"
                "ducklake:\n  enabled: true\n  alias: cache\n"
                f"  metadata-path: {os.path.join(self.tmp, 'meta.ducklake')}\n"
                f"  data-path: {os.path.join(self.tmp, 'data')}\n"
                "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                # worker-interval is an INTEGER number of seconds, not a duration.
                "heartbeat:\n  enabled: true\n  worker-interval: 1\n")

    def start(self):
        env = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"}
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", self.log_level],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT,
            cwd=self.tmp, env=env, preexec_fn=os.setsid)
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=2).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail(f"server did not start:\n{self.log()[-3000:]}")

    def stop(self):
        if self.proc:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=30)

    def log(self):
        try:
            return open(self.log_path).read()
        except OSError:
            return ""

    def wait_for_log(self, needle, timeout=30):
        """Poll the log until `needle` appears. Returns the log either way."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            log = self.log()
            if needle in log:
                return log
            time.sleep(0.5)
        return self.log()

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


class TestHeartbeatWarmsTheFullPipeline:
    def test_the_heartbeat_reaches_the_request_handler_for_an_uncached_endpoint(self):
        # The design decision this locks in: a heartbeat-enabled endpoint with NO
        # cache must still have its connection, template and query exercised.
        # "Handling request [GET]: /warm" is emitted by
        # RequestHandler::handleRequest and by nothing else, so it is direct
        # evidence the warm-up reached the handler - and, since /warm has no
        # cache, evidence that the heartbeat is not merely a refreshCache() call.
        with _Server() as s:
            log = s.wait_for_log("Handling request [GET]: /warm", timeout=30)
            assert "Performing heartbeat for endpoint /warm" in log, (
                "the heartbeat worker never ticked for /warm; everything below "
                "would assert nothing\n" + log[-3000:])
            assert "Handling request [GET]: /warm" in log, (
                "the heartbeat fired but never reached RequestHandler - the "
                "warm-up no longer exercises the serving path\n" + log[-3000:])

    def test_heartbeat_params_reach_the_template(self):
        # RED before #141. `heartbeat.params` was parsed and never read:
        # performHeartbeat() called requestForEndpoint(endpoint) with its
        # defaulted empty map, so `params.hb_marker` was absent, the inverted
        # section rendered `SELECT hb_param_was_not_passed`, and DuckDB failed to
        # bind it.
        with _Server() as s:
            log = s.wait_for_log(RENDERED_MARKER, timeout=30)
            assert "Performing heartbeat for endpoint /warm" in log, (
                "the heartbeat worker never ticked for /warm\n" + log[-3000:])
            assert RENDERED_MARKER in log, (
                "heartbeat.params never reached the template: the rendered "
                f"query does not contain {RENDERED_MARKER!r}\n" + log[-4000:])
            assert MISSING_MARKER not in log, (
                "the template took its param-absent branch, so heartbeat.params "
                "were dropped on the way to the renderer\n" + log[-4000:])

    def test_a_heartbeat_warm_up_does_not_kill_the_server(self):
        # The original crash class, kept: without a direct path the synthesised
        # request went through the router, dereferenced a null io_service in the
        # offload Completer (or a null middleware_context in
        # handleDynamicRequest) and took the process with it. The offload is on
        # by default here, exactly as it is in production.
        with _Server(log_level="warning") as s:
            deadline = time.time() + 15
            last = None
            while time.time() < deadline:
                try:
                    last = requests.get(f"{s.base_url}/health/live", timeout=3)
                except requests.RequestException as exc:
                    last = exc
                time.sleep(1)
            assert isinstance(last, requests.Response) and last.status_code == 200, (
                "the server stopped answering while the heartbeat ran\n"
                + s.log()[-3000:])
            assert s.proc.poll() is None, (
                "the process exited during heartbeat warm-ups\n" + s.log()[-3000:])

    def test_a_heartbeat_backed_cache_is_refreshed_and_still_served(self):
        # The cached half of the same design decision: warming a cached endpoint
        # populates it, so a real caller is served from the materialised table.
        with _Server(log_level="warning") as s:
            time.sleep(6)   # several worker intervals
            r = requests.get(f"{s.base_url}/cached", timeout=30)
            assert r.status_code == 200, r.text[:500] + "\n" + s.log()[-3000:]
            rows = r.json()["data"]
            assert rows and rows[0]["n"] == 42, (
                f"cache served nothing usable: {rows!r}\n" + s.log()[-3000:])
