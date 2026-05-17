"""End-to-end SQL-injection corpus for the WRITE path (#25 follow-up).

The read-path corpus (`test_sql_injection_corpus.py`) proves the prepared-
statement bind defangs injection on GET endpoints. This file proves the
same for POST endpoints — the write path also runs through the
PreparedTemplateRewriter and `duckdb_bind_*`, including across multi-
statement INSERT … ; SELECT … RETURNING templates.

Endpoints:

- `POST /widgets/`     — single-statement INSERT with `id` (int) and
                          `name` (string) bound via prepared placeholders.
- `POST /widgets-ret/` — multi-statement: INSERT then SELECT RETURNING.
                          Verifies the binding plan is correctly sliced
                          across two prepared statements.

For each endpoint, fire injection payloads through the body and assert:

1. The injection never executes — the seed table either stays empty or
   only ever contains the literal value (a bound parameter), never the
   side-effect of an OR-injection (extra rows) or a DROP (table gone).
2. Legitimate inserts still work and round-trip correctly.

Marked `standalone_server` so the conftest autouse fixture does not also
spin up the shared api_configuration server.
"""

import os
import socket
import subprocess
import tempfile
import time
from typing import Any, Iterator, List

import pytest
import requests


def _repo_root() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _flapi_binary() -> str:
    candidates: List[str] = []
    for build_type in ("release", "debug"):
        path = os.path.join(_repo_root(), "build", build_type, "flapi")
        if os.path.exists(path):
            candidates.append(path)
    if not candidates:
        pytest.skip("flapi binary not found in build/release or build/debug")
    candidates.sort(key=os.path.getmtime, reverse=True)
    return candidates[0]


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _write_config(dirpath: str, port: int) -> str:
    sqls = os.path.join(dirpath, "sqls")
    os.makedirs(sqls)

    with open(os.path.join(dirpath, "flapi.yaml"), "w") as f:
        f.write(
            f"project-name: sql-injection-write-test\n"
            f"project-description: Write-path SQL injection corpus\n"
            f"http-port: {port}\n"
            f"template:\n"
            f"  path: ./sqls\n"
            f"connections:\n"
            f"  inmem:\n"
            f"    init: |\n"
            f"      CREATE TABLE widgets (id BIGINT, name VARCHAR);\n"
            f"    properties:\n"
            f"      database: ':memory:'\n"
            f"duckdb:\n"
            f"  access_mode: READ_WRITE\n"
            f"  threads: 1\n"
        )

    # Single-statement INSERT — bindings: id (int64), name (varchar).
    with open(os.path.join(sqls, "widget_create.yaml"), "w") as f:
        f.write("""
url-path: /widgets/
method: POST
template-source: widget_create.sql
connection: [inmem]
operation:
  type: insert
  transaction: false
  validate-before-write: true
request:
  - field-name: id
    field-in: body
    field-type: int
    required: true
    validators:
      - type: int
        min: 1
        max: 1000000
        preventSqlInjection: false
  - field-name: name
    field-in: body
    field-type: string
    required: true
    validators:
      - type: string
        min-length: 1
        max-length: 200
        preventSqlInjection: false
""")
    with open(os.path.join(sqls, "widget_create.sql"), "w") as f:
        f.write("INSERT INTO widgets VALUES ({{ params.id }}, {{ params.name }})\n")

    # Multi-statement INSERT … ; SELECT … — exercises bindings spread
    # across two prepared statements.
    with open(os.path.join(sqls, "widget_create_ret.yaml"), "w") as f:
        f.write("""
url-path: /widgets-ret/
method: POST
template-source: widget_create_ret.sql
connection: [inmem]
operation:
  type: insert
  transaction: false
  validate-before-write: true
  returns-data: true
request:
  - field-name: id
    field-in: body
    field-type: int
    required: true
    validators:
      - type: int
        min: 1
        max: 1000000
        preventSqlInjection: false
  - field-name: name
    field-in: body
    field-type: string
    required: true
    validators:
      - type: string
        min-length: 1
        max-length: 200
        preventSqlInjection: false
""")
    with open(os.path.join(sqls, "widget_create_ret.sql"), "w") as f:
        f.write(
            "INSERT INTO widgets VALUES ({{ params.id }}, {{ params.name }});\n"
            "SELECT id, name FROM widgets WHERE id = {{ params.id }} AND name = {{ params.name }}\n"
        )

    # Helper to inspect the table.
    with open(os.path.join(sqls, "widget_list.yaml"), "w") as f:
        f.write("""
url-path: /widgets/
method: GET
template-source: widget_list.sql
connection: [inmem]
""")
    with open(os.path.join(sqls, "widget_list.sql"), "w") as f:
        f.write("SELECT id, name FROM widgets ORDER BY id\n")

    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture(scope="module")
def write_server() -> Iterator[str]:
    binary = _flapi_binary()
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_inj_wr_") as tmpdir:
        config_path = _write_config(tmpdir, port)
        log_path = os.path.join(tmpdir, "server.log")
        log_file = open(log_path, "w")
        proc = subprocess.Popen(
            [binary, "-c", config_path, "--no-telemetry"],
            cwd=tmpdir,
            stdout=log_file,
            stderr=subprocess.STDOUT,
        )
        try:
            base_url = f"http://127.0.0.1:{port}"
            deadline = time.time() + 30
            up = False
            while time.time() < deadline:
                if proc.poll() is not None:
                    break
                try:
                    r = requests.get(f"{base_url}/", timeout=1)
                    if r.status_code < 500:
                        up = True
                        break
                except requests.exceptions.RequestException:
                    time.sleep(0.5)
            if not up:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                log_file.close()
                with open(log_path) as f:
                    log_text = f.read()
                if "core_functions_duckdb_cpp_init" in log_text and "unique_ptr that is NULL" in log_text:
                    pytest.skip(
                        "flapi could not boot: local DuckDB extension cache is "
                        "incompatible with the in-tree DuckDB submodule. CI exercises this path."
                    )
                raise RuntimeError(f"flapi failed to start. Log:\n{log_text}")
            yield base_url
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            log_file.close()


def _rows(body: Any) -> list:
    if isinstance(body, list):
        return body
    if isinstance(body, dict):
        return body.get("data", body.get("rows", []))
    return []


def _list_widgets(base_url: str) -> list:
    r = requests.get(f"{base_url}/widgets/", timeout=10)
    assert r.status_code == 200, r.text
    return _rows(r.json())


# Injection payloads for the `name` body field. The string validator
# passes all of them; the prepared bind takes the value as a literal.
# `id` is held constant at a known integer so we can verify exactly one
# row landed in the table with the bound-as-data name.
NAME_INJECTION_PAYLOADS = [
    "evil'); DROP TABLE widgets; --",
    "'; DROP TABLE widgets; --",
    "x' OR '1'='1",
    "x'; DELETE FROM widgets; --",
    "x' UNION SELECT 99, 'hacked' --",
    "'); INSERT INTO widgets VALUES (777, 'hacked'); --",
    "x'/*",
    "x'\\",
    "Robert');DROP TABLE widgets;--",   # xkcd 327
]


# Numeric injection on the int field. Strict-parsed validator rejects
# these at 400; if a regression lets one through, the bind layer would
# error too. What MUST NOT happen is a 200 with an unexpected row landing
# in the table (e.g. id=99 from a UNION SELECT).
ID_INJECTION_PAYLOADS = [
    "1; DROP TABLE widgets",
    "1 UNION SELECT 99",
    "1) ; DELETE FROM widgets;--",
    "abc",
    "1.5",
    "1' OR '1'='1",
]


@pytest.mark.standalone_server
class TestSqlInjectionWriteCorpus:
    """End-to-end coverage of W3.1 PR B prepared statements on write paths."""

    @pytest.mark.parametrize("payload", NAME_INJECTION_PAYLOADS)
    def test_name_injection_payload_inserts_as_literal_only(self, write_server, payload):
        before = _list_widgets(write_server)
        unique_id = 1_000 + hash(payload) % 1_000_000  # avoid PK collisions across parametrize
        r = requests.post(
            f"{write_server}/widgets/",
            json={"id": unique_id, "name": payload},
            timeout=10,
        )
        # The string validator passes, the bind is the defense. A 201 is
        # expected — the payload is a legitimate (if hostile) string value.
        assert r.status_code in (200, 201), f"unexpected status {r.status_code}: {r.text}"

        after = _list_widgets(write_server)
        # Exactly one new row must have appeared with the literal name.
        # An injection that smuggled "; DROP TABLE widgets;" would have
        # dropped the table — meaning the GET would now error or return
        # an empty list. An "UNION SELECT 99,'hacked'" smuggle would
        # produce additional rows with the smuggled values.
        new_rows = [row for row in after if row not in before]
        assert len(new_rows) == 1, (
            f"INJECTION LEAK: payload {payload!r} produced {len(new_rows)} new rows "
            f"(before={before}, after={after})"
        )
        inserted = new_rows[0]
        assert inserted["id"] == unique_id, inserted
        assert inserted["name"] == payload, (
            f"name mismatch: stored {inserted['name']!r}, expected literal {payload!r}"
        )

    @pytest.mark.parametrize("payload", ID_INJECTION_PAYLOADS)
    def test_id_injection_payload_is_rejected(self, write_server, payload):
        before = _list_widgets(write_server)
        r = requests.post(
            f"{write_server}/widgets/",
            json={"id": payload, "name": "x"},
            timeout=10,
        )
        # The strict int validator rejects malformed numerics at 400;
        # if it regressed, the bind layer would also error at 400.
        # The only thing that MUST NOT happen is a 200 followed by a
        # surprise row in the table.
        after = _list_widgets(write_server)
        if r.status_code in (200, 201):
            new_rows = [row for row in after if row not in before]
            assert len(new_rows) == 0, (
                f"INJECTION LEAK: payload {payload!r} on id field produced new rows: {new_rows}"
            )
        else:
            assert 400 <= r.status_code < 500, r.text
            assert after == before, (
                f"Rejected request still side-effected the table: "
                f"before={before} after={after}"
            )

    def test_legit_insert_round_trips(self, write_server):
        before = _list_widgets(write_server)
        r = requests.post(
            f"{write_server}/widgets/",
            json={"id": 500_000, "name": "totally normal"},
            timeout=10,
        )
        assert r.status_code in (200, 201), r.text
        after = _list_widgets(write_server)
        assert {"id": 500_000, "name": "totally normal"} in after
        assert len(after) == len(before) + 1

    @pytest.mark.parametrize("payload", NAME_INJECTION_PAYLOADS[:3])
    def test_multistatement_returning_binds_correctly(self, write_server, payload):
        # Multi-statement template: INSERT then SELECT, both referencing
        # `params.name` and `params.id`. The binding plan is distributed
        # across two prepared statements. Both must bind the same literal
        # value and the SELECT must return the exact row we just inserted.
        unique_id = 800_000 + (hash(payload) % 100_000)
        r = requests.post(
            f"{write_server}/widgets-ret/",
            json={"id": unique_id, "name": payload},
            timeout=10,
        )
        assert r.status_code in (200, 201), f"unexpected {r.status_code}: {r.text}"
        body = r.json()
        returned = body.get("data") if isinstance(body, dict) else None
        assert returned, f"expected returned data, got: {body}"
        # The SELECT should have echoed back exactly one row, with the
        # literal payload as `name` (proving the bind didn't smuggle SQL).
        assert isinstance(returned, list) and len(returned) == 1, returned
        assert returned[0]["id"] == unique_id
        assert returned[0]["name"] == payload
