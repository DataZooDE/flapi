"""Shared helpers for the OpenTelemetry integration tests.

The primary harness is the FILE exporter plus a JSONL parse, not a collector,
for three reasons: it is deterministic (no ports, no retry backoff, no background
thread leaking into the next test), it needs no Python protobuf dependency, and
it exercises a shipped product feature - the air-gapped deployment topology.

The shared `flapi_server` fixture hardcodes DATAZOO_DISABLE_TELEMETRY=1 and a
fixed config, so every tracing test spawns its own server.
"""
import json
import os
import signal
import socket
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from typing import Any, Dict, Iterator, List, Optional

import pytest
import requests


def repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def flapi_binary() -> str:
    build_type = os.getenv("FLAPI_BUILD_TYPE", "release")
    for candidate in (
        os.path.join(repo_root(), "build", build_type, "flapi"),
        os.path.join(repo_root(), "build", "release", "flapi"),
        os.path.join(repo_root(), "build", "debug", "flapi"),
    ):
        if os.path.exists(candidate):
            return candidate
    pytest.skip("flapi binary not found")


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


@dataclass
class Span:
    name: str
    trace_id: str
    span_id: str
    parent_span_id: str
    kind: int
    attributes: Dict[str, Any] = field(default_factory=dict)
    status: Dict[str, Any] = field(default_factory=dict)

    @property
    def is_server(self) -> bool:
        return self.kind == 2   # SPAN_KIND_SERVER


def _attr_value(value: Dict[str, Any]) -> Any:
    # OTLP/JSON wraps every attribute value in a type tag.
    for key in ("stringValue", "boolValue"):
        if key in value:
            return value[key]
    for key in ("intValue", "doubleValue"):
        if key in value:
            raw = value[key]
            return int(raw) if key == "intValue" else float(raw)
    if "arrayValue" in value:
        return [_attr_value(v) for v in value["arrayValue"].get("values", [])]
    return None


def spans_from_file(path: str) -> List[Span]:
    """Flatten an OTLP-JSON trace file into a list of spans."""
    if not os.path.exists(path):
        return []
    out: List[Span] = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                doc = json.loads(line)
            except json.JSONDecodeError:
                continue
            for resource_span in doc.get("resourceSpans", []):
                for scope_span in resource_span.get("scopeSpans", []):
                    for raw in scope_span.get("spans", []):
                        attrs = {
                            a["key"]: _attr_value(a.get("value", {}))
                            for a in raw.get("attributes", [])
                        }
                        out.append(Span(
                            name=raw.get("name", ""),
                            trace_id=raw.get("traceId", ""),
                            span_id=raw.get("spanId", ""),
                            parent_span_id=raw.get("parentSpanId", ""),
                            kind=raw.get("kind", 0),
                            attributes=attrs,
                            status=raw.get("status", {}),
                        ))
    return out


def raw_text(path: str) -> str:
    """The whole file as text, for the no-leak assertion.

    Substring-searching the raw bytes is strictly stronger than inspecting a
    parsed attribute list: it also covers span names, event names, status
    messages and resource attributes that nobody remembered to enumerate.
    """
    if not os.path.exists(path):
        return ""
    with open(path, errors="replace") as f:
        return f.read()


def find_span(spans: List[Span], *, name: Optional[str] = None,
              attrs: Optional[Dict[str, Any]] = None) -> Optional[Span]:
    for s in spans:
        if name is not None and s.name != name:
            continue
        if attrs and any(s.attributes.get(k) != v for k, v in attrs.items()):
            continue
        return s
    return None


def find_spans(spans: List[Span], *, attrs: Optional[Dict[str, Any]] = None) -> List[Span]:
    return [s for s in spans
            if not attrs or all(s.attributes.get(k) == v for k, v in attrs.items())]


def children_of(spans: List[Span], parent: Span) -> List[Span]:
    return [s for s in spans if s.parent_span_id == parent.span_id]


class TracedServer:
    """A flapi server with tracing enabled and the file exporter configured."""

    def __init__(self, extra_config: str = "", endpoints: Optional[Dict[str, str]] = None,
                 tracing_block: Optional[str] = None):
        self.tmp = tempfile.mkdtemp(prefix="flapi_otel_")
        self.traces_path = os.path.join(self.tmp, "traces.jsonl")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        self._write_config(extra_config, endpoints or {}, tracing_block)
        self.proc: Optional[subprocess.Popen] = None

    def _write_config(self, extra_config: str, endpoints: Dict[str, str],
                      tracing_block: Optional[str]) -> None:
        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)

        tracing = tracing_block if tracing_block is not None else (
            "tracing:\n"
            "  enabled: true\n"
            "  exporter: otlp_file\n"
            "  capture: metadata\n"
            f"  file:\n    path: {self.traces_path}\n"
            "  flush:\n    mode: on_response\n"
            "  sample:\n    type: always_on\n"
        )

        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                f"project-name: otel-test\n"
                f"project-description: OpenTelemetry integration test\n"
                f"http-port: {self.port}\n"
                f"template:\n  path: ./sqls\n"
                f"connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                f"mcp:\n  enabled: true\n"
                f"{tracing}"
                f"{extra_config}"
            )

        if not endpoints:
            endpoints = {
                "open.yaml": ("url-path: /open\nmethod: GET\n"
                              "template-source: open.sql\nconnection: [inmem]\n"),
                "open.sql": "SELECT 1 AS n\n",
            }
        for name, content in endpoints.items():
            with open(os.path.join(sqls, name), "w") as f:
                f.write(content)

    def start(self) -> "TracedServer":
        with open(self.log_path, "w") as log:
            self.proc = subprocess.Popen(
                [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
                 "-p", str(self.port), "--log-level", "warning"],
                stdout=log, stderr=subprocess.STDOUT, cwd=self.tmp,
                env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
                preexec_fn=os.setsid,
            )
        deadline = time.time() + 60
        while time.time() < deadline:
            if self.proc.poll() is not None:
                pytest.fail(f"server exited during startup:\n{self.log()}")
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=1).status_code == 200:
                    return self
            except requests.RequestException:
                time.sleep(0.3)
        pytest.fail(f"server never became live:\n{self.log()}")

    def stop(self) -> None:
        if self.proc is None:
            return
        try:
            os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
            self.proc.wait(timeout=10)
        except Exception:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except Exception:
                pass

    def log(self) -> str:
        try:
            with open(self.log_path, errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def spans(self) -> List[Span]:
        return spans_from_file(self.traces_path)

    def raw_traces(self) -> str:
        return raw_text(self.traces_path)


def traced_server(**kwargs) -> Iterator[TracedServer]:
    server = TracedServer(**kwargs).start()
    try:
        yield server
    finally:
        server.stop()
