"""A real OTLP/HTTP collector, for testing the network export path.

Every existing tracing test uses the *file* exporter, which cannot answer the
questions that matter about `otlp_http`: does a span actually leave the process
before the response returns, and what happens when the collector is slow,
hanging or refusing connections. Mocking that away would prove nothing — the
behaviour under test lives in opentelemetry-cpp's exporter and in the network,
not in flAPI's own code.

So this is a genuine HTTP server speaking OTLP/JSON on `/v1/traces`, recording
what arrived and *when*. flAPI is configured with `protocol: http/json` so
neither side needs protobuf.

Modes:
    ok          accept and record immediately
    slow(ms)    accept, but take `ms` before responding
    hang        accept and never respond (until shutdown)
    refuse      return 503 without recording
"""
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List, Optional

from otel_helpers import Span, free_port, _attr_value


class _Handler(BaseHTTPRequestHandler):
    def do_POST(self):  # noqa: N802 - BaseHTTPRequestHandler's interface
        collector: "OtlpCollector" = self.server.collector  # type: ignore[attr-defined]

        # Path-strict, like a real collector. Accepting any path would let a
        # regression in the exporter's URL construction ship green - the fixture
        # would happily record spans posted to the wrong endpoint.
        if self.path.split("?")[0] != "/v1/traces":
            collector._record_bad_path(self.path)
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""

        mode, arg = collector.mode
        if mode == "hang":
            # Never respond. The point is to prove flAPI gives up on its own
            # budget rather than waiting on a third party indefinitely.
            collector._hanging.set()
            while not collector._stopping.is_set():
                time.sleep(0.02)
            return
        if mode == "slow":
            time.sleep(arg / 1000.0)
        if mode == "refuse":
            self.send_response(503)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        collector._record(body)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"{}")

    def log_message(self, *args):  # silence the default stderr spam
        pass


class OtlpCollector:
    """An OTLP/HTTP endpoint that records what flAPI sent it, and when."""

    def __init__(self, mode: str = "ok", slow_ms: int = 0):
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        # The FULL trace URL, which is what tracing.endpoint must be set to.
        # opentelemetry-cpp uses an overridden url verbatim - it does not append
        # /v1/traces - so a base URL posts to "/" and a real collector 404s it.
        # Silently, because the exporter discards its own failure result.
        self.endpoint = f"{self.base_url}/v1/traces"
        self._mode = (mode, slow_ms)
        self._lock = threading.Lock()
        self._spans: List[Span] = []
        self._receipts: List[float] = []
        self._raw: List[str] = []
        self._stopping = threading.Event()
        self._hanging = threading.Event()
        self._bad_paths: List[str] = []
        self._server: Optional[ThreadingHTTPServer] = None
        self._thread: Optional[threading.Thread] = None

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> "OtlpCollector":
        self._server = ThreadingHTTPServer(("127.0.0.1", self.port), _Handler)
        self._server.collector = self  # type: ignore[attr-defined]
        self._server.daemon_threads = True
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stopping.set()
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()

    def __enter__(self) -> "OtlpCollector":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # -- state -------------------------------------------------------------
    @property
    def mode(self):
        return self._mode

    def set_mode(self, mode: str, slow_ms: int = 0) -> None:
        self._mode = (mode, slow_ms)

    def _record(self, body: bytes) -> None:
        now = time.monotonic()
        try:
            doc = json.loads(body.decode("utf-8", errors="replace"))
        except json.JSONDecodeError:
            return
        parsed: List[Span] = []
        for resource_span in doc.get("resourceSpans", []):
            for scope_span in resource_span.get("scopeSpans", []):
                for raw in scope_span.get("spans", []):
                    parsed.append(Span(
                        name=raw.get("name", ""),
                        trace_id=raw.get("traceId", ""),
                        span_id=raw.get("spanId", ""),
                        parent_span_id=raw.get("parentSpanId", ""),
                        kind=raw.get("kind", 0),
                        attributes={a["key"]: _attr_value(a.get("value", {}))
                                    for a in raw.get("attributes", [])},
                        status=raw.get("status", {}),
                    ))
        with self._lock:
            self._spans.extend(parsed)
            self._receipts.append(now)
            self._raw.append(body.decode("utf-8", errors="replace"))

    # -- assertions --------------------------------------------------------
    def spans(self) -> List[Span]:
        with self._lock:
            return list(self._spans)

    def spans_for_trace(self, trace_id: str) -> List[Span]:
        tid = trace_id.lower()
        return [s for s in self.spans() if s.trace_id.lower() == tid]

    def export_count(self) -> int:
        """Number of HTTP exports received.

        This is the number that says whether a request's spans left in ONE round
        trip or one per span - the difference between BatchSpanProcessor with a
        force-flush and SimpleSpanProcessor.
        """
        with self._lock:
            return len(self._receipts)

    def raw(self) -> str:
        with self._lock:
            return "\n".join(self._raw)

    def _record_bad_path(self, path: str) -> None:
        with self._lock:
            self._bad_paths.append(path)

    def bad_paths(self) -> List[str]:
        """Requests that arrived on a path a real collector would 404."""
        with self._lock:
            return list(self._bad_paths)

    def saw_a_hanging_request(self) -> bool:
        return self._hanging.is_set()

    def wait_for_spans(self, predicate, timeout: float = 5.0, message: str = ""):
        deadline = time.time() + timeout
        while time.time() < deadline:
            current = self.spans()
            if predicate(current):
                return current
            time.sleep(0.02)
        raise AssertionError(
            message or f"condition not met within {timeout}s; "
            f"saw {[(s.name, s.kind) for s in self.spans()]}")


def collector_tracing_block(endpoint: str,
                            blocking_timeout_ms: Optional[int] = None,
                            extra: str = "") -> str:
    """A `tracing:` block pointed at a collector, for TracedServer(tracing_block=...).

    `http/json` on purpose: it keeps protobuf out of the test dependencies on
    both sides.
    """
    block = (
        "tracing:\n"
        "  enabled: true\n"
        "  exporter: otlp_http\n"
        f"  endpoint: {endpoint}\n"
        "  protocol: http/json\n"
        "  capture: metadata\n"
        "  sample:\n    type: always_on\n"
        "  flush:\n"
        "    mode: on_response\n"
        "    timeout_ms: 30000\n"          # long, so batch export cannot rescue a test
    )
    if blocking_timeout_ms is not None:
        block += f"    blocking_timeout_ms: {blocking_timeout_ms}\n"
    return block + extra
