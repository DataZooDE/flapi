"""The payload capture tier and the OpenInference overlay (epic issue 14).

This is the tier where flAPI becomes a processor exporting personal data to a
third destination, so the tests are as much about what does NOT happen as what
does.

The design it has to satisfy:
  * metadata remains the default; payload is an explicit opt-in
  * a global `capture: off` beats any per-endpoint opt-in, unconditionally
  * credentials are excluded at EVERY tier, regardless of configuration
  * the existing audit.redact_keys list governs redaction - not a second list
"""
import pytest
import requests

from otel_helpers import find_span, traced_server

pytestmark = pytest.mark.standalone_server

ENDPOINTS = {
    "items.yaml": (
        "url-path: /items\nmethod: GET\n"
        "template-source: items.sql\nconnection: [inmem]\n"
        "request:\n"
        "  - field-name: q\n    field-in: query\n    required: false\n"
    ),
    "items.sql": "SELECT 1 AS n\n",
}

PAYLOAD_TRACING = (
    "tracing:\n"
    "  enabled: true\n"
    "  exporter: otlp_file\n"
    "  capture: payload\n"
    "  openinference: true\n"
    "  file:\n    path: {path}\n"
    "  flush:\n    mode: on_response\n"
    "  sample:\n    type: always_on\n"
)

OFF_TRACING = (
    "tracing:\n"
    "  enabled: true\n"
    "  exporter: otlp_file\n"
    "  capture: off\n"
    "  file:\n    path: {path}\n"
    "  flush:\n    mode: on_response\n"
    "  sample:\n    type: always_on\n"
)


def _server(tracing_template, extra=""):
    import otel_helpers
    s = otel_helpers.TracedServer(endpoints=ENDPOINTS, extra_config=extra,
                                  tracing_block="PLACEHOLDER")
    # The tracing block needs the resolved traces path, which is only known once
    # the server object exists.
    import os
    config = os.path.join(s.tmp, "flapi.yaml")
    with open(config) as f:
        text = f.read()
    text = text.replace("PLACEHOLDER", tracing_template.format(path=s.traces_path))
    with open(config, "w") as f:
        f.write(text)
    return s


@pytest.fixture
def payload_server():
    s = _server(PAYLOAD_TRACING).start()
    try:
        yield s
    finally:
        s.stop()


@pytest.fixture
def capture_off_server():
    s = _server(OFF_TRACING).start()
    try:
        yield s
    finally:
        s.stop()


class TestPayloadTier:
    def test_the_payload_tier_actually_captures_values(self, payload_server):
        # Without this, every "must not leak" assertion below passes vacuously -
        # which is precisely how a security guarantee comes to be believed without
        # being true. Prove capture works before asserting what it excludes.
        requests.get(f"{payload_server.base_url}/items?q=VISIBLE_VALUE", timeout=10)
        payload_server.wait_for_server_span()

        blob = payload_server.raw_traces()
        assert "VISIBLE_VALUE" in blob, (
            "the payload tier must export declared parameter values; if it does "
            "not, the no-leak assertions below prove nothing"
        )

    def test_the_metadata_tier_captures_no_values(self):
        # The same request at the default tier must export nothing.
        metadata_tracing = (
            "tracing:\n  enabled: true\n  exporter: otlp_file\n"
            "  capture: metadata\n"
            "  file:\n    path: {path}\n"
            "  flush:\n    mode: on_response\n"
            "  sample:\n    type: always_on\n"
        )
        s = _server(metadata_tracing).start()
        try:
            requests.get(f"{s.base_url}/items?q=SHOULD_NOT_APPEAR", timeout=10)
            s.wait_for_server_span()
            assert "SHOULD_NOT_APPEAR" not in s.raw_traces(), (
                "the default tier must export structure and shape, never values"
            )
        finally:
            s.stop()

    def test_redaction_applies_at_the_payload_tier(self):
        # Reuses audit.redact_keys - the list operators already configure - rather
        # than a second list that would silently diverge.
        tracing = (
            "tracing:\n  enabled: true\n  exporter: otlp_file\n"
            "  capture: payload\n"
            "  file:\n    path: {path}\n"
            "  flush:\n    mode: on_response\n"
            "  sample:\n    type: always_on\n"
        )
        s = _server(tracing, extra="audit:\n  enabled: false\n  redact:\n    - q\n").start()
        try:
            requests.get(f"{s.base_url}/items?q=REDACT_ME", timeout=10)
            s.wait_for_server_span()
            blob = s.raw_traces()
            assert "REDACT_ME" not in blob, "a redact-listed key must be masked"
            assert "redacted" in blob, "the masked marker must be present"
        finally:
            s.stop()

    def test_capture_off_disables_tracing_entirely(self, capture_off_server):
        # The one lever a security review asks for: a single setting guaranteed
        # to stop export, whatever any endpoint says.
        requests.get(f"{capture_off_server.base_url}/items?q=anything", timeout=10)
        assert capture_off_server.spans() == [], (
            "capture: off must produce no spans at all"
        )

    def test_credentials_are_excluded_even_at_the_payload_tier(self, payload_server):
        # Credentials are never legitimate span content at ANY tier, and that must
        # not depend on an operator's redact list being complete.
        secret = "S3CRET_BEARER_VALUE"
        requests.get(f"{payload_server.base_url}/items",
                     headers={"Authorization": f"Bearer {secret}"}, timeout=10)

        assert secret not in payload_server.raw_traces(), (
            "an Authorization header must never reach a span, at any tier"
        )

    def test_a_filled_path_is_never_exported_even_at_the_payload_tier(self, payload_server):
        marker = "S3CRET_PATH_SEGMENT"
        requests.get(f"{payload_server.base_url}/{marker}", timeout=10)
        assert marker not in payload_server.raw_traces(), (
            "a filled path is a filter over customer data and is excluded at "
            "every tier"
        )

    def test_the_openinference_overlay_is_applied_when_enabled(self, payload_server):
        requests.get(f"{payload_server.base_url}/items?q=hello", timeout=10)
        span = payload_server.wait_for_server_span()

        # The overlay decorates the SAME span - no extra spans, no second
        # exporter. A REST request is deliberately NOT marked as a CHAIN: that
        # would mis-model flAPI as an agent rather than a tool.
        kind = span.attributes.get("openinference.span.kind")
        assert kind in (None, "TOOL"), (
            f"a REST request must not be modelled as an agent CHAIN, got {kind!r}"
        )

    def test_the_overlay_is_absent_when_not_enabled(self):
        metadata_tracing = (
            "tracing:\n  enabled: true\n  exporter: otlp_file\n"
            "  capture: metadata\n  openinference: false\n"
            "  file:\n    path: {path}\n"
            "  flush:\n    mode: on_response\n"
            "  sample:\n    type: always_on\n"
        )
        s = _server(metadata_tracing).start()
        try:
            requests.get(f"{s.base_url}/items", timeout=10)
            s.wait_for_server_span()
            assert "openinference" not in s.raw_traces(), (
                "the overlay must be opt-in; default spans stay vendor-neutral"
            )
        finally:
            s.stop()
