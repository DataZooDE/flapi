"""Path parameters are percent-decoded after the route matched (#220).

The route is matched on the RAW path (so `%2F` stays inside one segment), then each
captured value is decoded once, before validation:

 * `/customers/%37` reaches an `int` validator as `7`, not `%37`;
 * `/names/John%20Doe` reaches the template as `John Doe`;
 * `%2F` decodes to `/` inside the single parameter (it does not add a segment);
 * `+` is a literal plus in a path (it is not a space, unlike a query string);
 * a malformed escape (`%zz`, a truncated `%4`) or an encoded NUL (`%00`) is a 400;
 * decoding happens once: `%252F` arrives as the text `%2F`.

Requests are sent with http.client so the client does not normalise the path first
(`requests` unquotes `%37` itself).
"""

import http.client
import json
import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server


def _w(base, rel, text):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def _endpoint(route, field, vtype, sql):
    return (f"url-path: {route}\nmethod: GET\ntemplate-source: SRC.sql\nconnection: [inmem]\n"
            f"request:\n  - field-name: {field}\n    field-in: path\n    validators:\n      - type: {vtype}\n",
            sql)


@pytest.fixture(scope="module")
def server():
    tmp = tempfile.mkdtemp(prefix="flapi_pathdec_")
    port = free_port()
    _w(tmp, "flapi.yaml",
       "project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
       "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
    specs = {
        "cust": _endpoint("/customers/:id", "id", "int", "SELECT {{ params.id }} AS id\n"),
        "names": _endpoint("/names/:name", "name", "string", "SELECT {{ params.name }} AS name\n"),
    }
    for name, (yaml_text, sql) in specs.items():
        _w(tmp, f"sqls/{name}.yaml", yaml_text.replace("SRC", name))
        _w(tmp, f"sqls/{name}.sql", sql)
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "-p", str(port), "--log-level", "warning"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=tmp,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    for _ in range(300):
        try:
            if requests.get(f"http://127.0.0.1:{port}/health", timeout=2).status_code == 200:
                break
        except requests.RequestException:
            pass
        time.sleep(0.2)
    yield port
    proc.terminate()
    proc.wait(timeout=30)


def raw_get(port, path):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    try:
        conn.request("GET", path)
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", "replace")
        return resp.status, body
    finally:
        conn.close()


def first_row(body):
    return json.loads(body)["data"][0]


class TestPercentEncodedValuesAreDecoded:

    def test_an_encoded_digit_reaches_the_int_validator_decoded(self, server):
        status, body = raw_get(server, "/customers/%37")
        assert status == 200, body
        assert first_row(body)["id"] == 7

    def test_a_plain_int_still_works(self, server):
        status, body = raw_get(server, "/customers/42")
        assert status == 200, body
        assert first_row(body)["id"] == 42

    def test_an_encoded_space_arrives_as_a_space(self, server):
        status, body = raw_get(server, "/names/John%20Doe")
        assert status == 200, body
        assert first_row(body)["name"] == "John Doe"

    def test_an_encoded_slash_stays_in_one_segment_and_decodes(self, server):
        status, body = raw_get(server, "/names/a%2Fb")
        assert status == 200, body
        assert first_row(body)["name"] == "a/b"

    def test_a_literal_slash_is_still_a_new_segment(self, server):
        status, _ = raw_get(server, "/names/a/b")
        assert status == 404

    def test_utf8_escapes_decode_to_the_character(self, server):
        status, body = raw_get(server, "/names/caf%C3%A9")
        assert status == 200, body
        assert first_row(body)["name"] == "café"

    def test_plus_is_a_plus_in_a_path(self, server):
        status, body = raw_get(server, "/names/a+b")
        assert status == 200, body
        assert first_row(body)["name"] == "a+b"

    def test_decoding_happens_once(self, server):
        status, body = raw_get(server, "/names/a%252Fb")
        assert status == 200, body
        assert first_row(body)["name"] == "a%2Fb"


class TestMalformedEscapesAreRejected:

    @pytest.mark.parametrize("path", [
        "/names/%zz", "/names/%4", "/names/ab%", "/names/%g1", "/names/a%00b", "/customers/%00",
    ])
    def test_a_bad_escape_or_encoded_nul_is_400(self, server, path):
        status, body = raw_get(server, path)
        assert status == 400, f"{path} -> {status} {body[:150]}"

    def test_a_decoded_value_is_still_validated(self, server):
        status, body = raw_get(server, "/customers/%31%20OR%20true")   # "1 OR true"
        assert status == 400, body
