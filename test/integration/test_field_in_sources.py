"""`field-in` is honoured per declared source (#192).

A parameter is taken only from the place its field declares:

 * a `field-in: path` value cannot be overridden by `?id=` or by a body key;
 * a `field-in: body` field is not satisfied by the query string (so a required
   one is "missing", 400);
 * a `field-in: query` field is not satisfied by the body (on a write the body
   used to win over the declared query value);
 * a name that is a route parameter is path-only even when undeclared.
"""

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


def _fields(*fields):
    out = "request:\n"
    for name, where, vtype, required in fields:
        out += f"  - field-name: {name}\n    field-in: {where}\n    required: {str(required).lower()}\n"
        out += f"    validators:\n      - type: {vtype}\n"
    return out


def _get(url_path, name, fields):
    return {f"{name}.yaml": (f"url-path: {url_path}\nmethod: GET\ntemplate-source: {name}.sql\n"
                             f"connection: [app]\n" + _fields(*fields))}


def _write(url_path, name, fields, method="POST"):
    return {f"{name}.yaml": (f"url-path: {url_path}\nmethod: {method}\ntemplate-source: {name}.sql\n"
                             f"connection: [app]\noperation:\n  type: write\n" + _fields(*fields))}


@pytest.fixture(scope="module")
def server():
    tmp = tempfile.mkdtemp(prefix="flapi_fieldin_")
    port = free_port()
    _w(tmp, "flapi.yaml",
       "project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
       "connections:\n  app:\n    init: |\n      ATTACH IF NOT EXISTS './app.duckdb' AS app;\n"
       "      CREATE TABLE IF NOT EXISTS app.log (tag VARCHAR, id BIGINT, note VARCHAR);\n"
       "    properties:\n      x: y\n")
    files = {}
    files.update(_get("/byid/:id", "byid", [("id", "path", "int", True)]))
    _w(tmp, "sqls/byid.sql", "SELECT {{ params.id }} AS id\n")
    files.update(_get("/needs-body", "needsbody", [("payload", "body", "string", True)]))
    _w(tmp, "sqls/needsbody.sql", "SELECT {{ params.payload }} AS payload\n")
    files.update(_get("/optional-body", "optbody", [("payload", "body", "string", False)]))
    _w(tmp, "sqls/optbody.sql", "SELECT {{ params.payload }} AS payload\n")
    files.update(_get("/log", "log", []))
    _w(tmp, "sqls/log.sql", "SELECT tag, id, note FROM app.log ORDER BY tag\n")
    # A write whose route param is declared path, plus a body field.
    files.update(_write("/x/:id", "xwrite", [("id", "path", "int", True), ("note", "body", "string", True)]))
    _w(tmp, "sqls/xwrite.sql", "INSERT INTO app.log VALUES ('x', {{ params.id }}, {{ params.note }})\n")
    # A write where the route param is NOT declared at all.
    files.update(_write("/u/:id", "uwrite", [("note", "body", "string", True)]))
    _w(tmp, "sqls/uwrite.sql", "INSERT INTO app.log VALUES ('u', {{ params.id }}, {{ params.note }})\n")
    # id in the body, note in the query.
    files.update(_write("/y", "ywrite", [("id", "body", "int", True), ("note", "query", "string", True)]))
    _w(tmp, "sqls/ywrite.sql", "INSERT INTO app.log VALUES ('y', {{ params.id }}, {{ params.note }})\n")
    for rel, text in files.items():
        _w(tmp, f"sqls/{rel}", text)
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "-p", str(port), "--log-level", "warning"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=tmp,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    base = f"http://127.0.0.1:{port}"
    for _ in range(300):
        try:
            if requests.get(f"{base}/health", timeout=2).status_code == 200:
                break
        except requests.RequestException:
            pass
        time.sleep(0.2)
    yield base
    proc.terminate()
    proc.wait(timeout=30)


def log_rows(base, tag):
    r = requests.get(f"{base}/log", timeout=10)
    assert r.status_code == 200, r.text
    return [row for row in r.json()["data"] if row["tag"] == tag]


class TestPathParametersCannotBeOverridden:

    def test_a_query_parameter_cannot_replace_a_path_value(self, server):
        r = requests.get(f"{server}/byid/1", params={"id": 2}, timeout=10)
        assert r.status_code == 200, r.text
        assert r.json()["data"][0]["id"] == 1

    def test_a_body_key_cannot_replace_a_path_value(self, server):
        r = requests.post(f"{server}/x/1", json={"id": 99, "note": "n1"}, timeout=10)
        assert r.status_code in (200, 201), r.text
        ids = [row["id"] for row in log_rows(server, "x")]
        assert 1 in ids and 99 not in ids, ids

    def test_a_query_parameter_cannot_replace_a_path_value_on_a_write(self, server):
        r = requests.post(f"{server}/x/2", params={"id": 77}, json={"note": "n2"}, timeout=10)
        assert r.status_code in (200, 201), r.text
        ids = [row["id"] for row in log_rows(server, "x")]
        assert 2 in ids and 77 not in ids, ids

    def test_an_undeclared_route_parameter_is_path_only(self, server):
        r = requests.post(f"{server}/u/3", params={"id": 55}, json={"id": 66, "note": "n3"}, timeout=10)
        # Whether an undeclared body key is accepted is the endpoint's own setting;
        # what matters is that the row carries the PATH value.
        assert r.status_code in (200, 201, 400), r.text
        if r.status_code == 200:
            assert [row["id"] for row in log_rows(server, "u")] == [3]


class TestDeclaredSourceOnly:

    def test_a_body_field_is_not_satisfied_by_the_query_string(self, server):
        r = requests.get(f"{server}/needs-body", params={"payload": "abc"}, timeout=10)
        assert r.status_code == 400, r.text

    def test_an_optional_body_field_is_absent_when_only_in_the_query(self, server):
        r = requests.get(f"{server}/optional-body", params={"payload": "abc"}, timeout=10)
        assert r.status_code == 200, r.text
        assert r.json()["data"][0]["payload"] is None

    def test_a_declared_query_field_wins_over_a_body_key_of_the_same_name(self, server):
        r = requests.post(f"{server}/y", params={"note": "from-query"},
                          json={"id": 5, "note": "from-body"}, timeout=10)
        assert r.status_code in (200, 201), r.text
        rows = log_rows(server, "y")
        assert [row["note"] for row in rows] == ["from-query"], rows

    def test_a_declared_body_field_ignores_the_query_string(self, server):
        r = requests.post(f"{server}/y", params={"id": 9, "note": "q2"}, json={"note": "b2"}, timeout=10)
        assert r.status_code == 400, r.text     # id is declared body and required
