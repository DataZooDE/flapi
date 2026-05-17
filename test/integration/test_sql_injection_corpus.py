"""End-to-end SQL-injection corpus for W3.1 prepared statements (#25).

Boots flapi with two endpoints exercising the prepared-statement path:

- /lookup-int    — `id` validated as int → bound via duckdb_bind_int64
- /lookup-string — `name` validated as string → bound via duckdb_bind_varchar

Each endpoint runs against an in-memory table seeded with rows that have
no surprising values. The contract under test:

1. **Numeric injection payloads are rejected** by the integer validator
   (400 Bad Request) — they never reach SQL.
2. **String injection payloads pass validation** (they are valid strings)
   but are **bound as data**, so the underlying query cannot smuggle
   `UNION SELECT`, `OR 1=1`, comment, or quote-escape constructs into the
   parsed SQL — every payload returns *zero rows*, not the whole table.

Marked `standalone_server` so the conftest autouse fixture does not also
spin up the shared api_configuration server. Skips cleanly if the local
DuckDB extension cache is incompatible with the bundled DuckDB submodule.
"""

import os
import socket
import subprocess
import tempfile
import time
from typing import Iterator, List

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
            f"project-name: sql-injection-test\n"
            f"project-description: SQL injection corpus E2E\n"
            f"http-port: {port}\n"
            f"template:\n"
            f"  path: ./sqls\n"
            f"connections:\n"
            f"  inmem:\n"
            f"    properties:\n"
            f"      database: ':memory:'\n"
            f"duckdb:\n"
            f"  access_mode: READ_WRITE\n"
            f"  threads: 1\n"
        )

    # Endpoint 1: typed-int param → bound as int64
    with open(os.path.join(sqls, "lookup_int.yaml"), "w") as f:
        f.write("""
url-path: /lookup-int
method: GET
template-source: lookup_int.sql
connection: [inmem]
request:
  - field-name: id
    field-in: query
    field-type: int
    required: true
    validators:
      - type: int
        min: 1
        max: 100000
        preventSqlInjection: false
""")
    with open(os.path.join(sqls, "lookup_int.sql"), "w") as f:
        f.write(
            "SELECT * FROM (VALUES (1,'alice'),(2,'bob'),(3,'carol')) AS t(id,name) "
            "WHERE id = {{ params.id }}\n"
        )

    # Endpoint 2: typed-string param → bound as varchar
    # preventSqlInjection: false on purpose — we want to prove the bind
    # layer itself defangs payloads, not the heuristic regex above it.
    with open(os.path.join(sqls, "lookup_string.yaml"), "w") as f:
        f.write("""
url-path: /lookup-string
method: GET
template-source: lookup_string.sql
connection: [inmem]
request:
  - field-name: name
    field-in: query
    field-type: string
    required: true
    validators:
      - type: string
        min-length: 1
        max-length: 200
        preventSqlInjection: false
""")
    with open(os.path.join(sqls, "lookup_string.sql"), "w") as f:
        # The string field is double-brace (typed-scalar bindable) — the
        # rewriter replaces this site with `?` and binds the value via
        # duckdb_bind_varchar. The single quotes around it are dropped
        # because they would otherwise become part of the literal.
        f.write(
            "SELECT * FROM (VALUES (1,'alice'),(2,'bob'),(3,'carol')) AS t(id,name) "
            "WHERE name = {{ params.name }}\n"
        )

    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture
def injection_server() -> Iterator[str]:
    binary = _flapi_binary()
    port = _free_port()
    with tempfile.TemporaryDirectory(prefix="flapi_inj_") as tmpdir:
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


# -- payload corpora ------------------------------------------------------

# Numeric payloads. Every one of these must be rejected by the int
# validator (400) — they should not even reach the SQL layer, but if
# the validator regressed, the bind layer must error rather than execute.
NUMERIC_PAYLOADS = [
    "1 OR 1=1",
    "1; DROP TABLE t",
    "1 UNION SELECT 1,'evil'",
    "1/**/OR/**/1=1",
    "1' OR '1'='1",
    "'1' OR '1'='1'--",
    "0xdeadbeef",
    "1e3",                # scientific form — bind layer rejects too
    "1.5",                # float in int field
    "-2147483649",        # below int32 (still ok if int64) — accept iff bind allows
    "abc",
    "",
    "   ",
    "1 AND SLEEP(5)",
    "1) UNION SELECT NULL--",
    " ;",
    "/*comment*/1",
]

# String payloads that PASS the string validator (they are technically
# valid strings) but must NOT alter the SQL. With prepared statements,
# every one of these should match zero rows in the seed table because
# the value, taken as a literal, is not one of {alice, bob, carol}.
STRING_PAYLOADS_NO_MATCH = [
    "alice' OR '1'='1",
    "alice'; DROP TABLE t--",
    "alice' UNION SELECT 1,'evil'--",
    "'; DROP TABLE t; --",
    "alice'--",
    "alice'/*",
    "%' OR 1=1 --",
    "alice' OR ''='",
    "\\' OR 1=1",
    "\";DROP TABLE t;",
    "'" * 50,
    # NUL-byte truncation lives in the HTTP/URL decoder, not the bind
    # layer. Crow may stop at the NUL and forward just "alice", which
    # is a legitimate row match — not an injection. The bind layer's
    # NUL-preservation is covered by the C++ unit test for
    # QueryExecutor::executeWithBindings.
    "ALICE",                       # case mismatch
    "alice\\",
    "carol' UNION SELECT password FROM users--",
    "x' OR 'a'='a' OR 'x'='",
    "Robert');DROP TABLE Students;--",  # xkcd 327
]

# String payloads that legitimately match a seed row. These prove the
# bind layer does not over-defang — exact-string lookups still work.
STRING_PAYLOADS_MATCH = [
    ("alice", 1),
    ("bob", 2),
    ("carol", 3),
]


@pytest.mark.standalone_server
class TestSqlInjectionCorpus:
    """End-to-end coverage of W3.1 PR B prepared statements."""

    @pytest.mark.parametrize("payload", NUMERIC_PAYLOADS)
    def test_numeric_payloads_are_rejected_or_yield_zero_rows(self, injection_server, payload):
        # The int validator should reject all of these with 400. If a
        # regression ever lets one through, the bind layer must reject it
        # before any SQL parses (we'd see a 500 with "Bind conversion
        # failed"). What MUST NOT happen is a 200 with table-leaking rows.
        r = requests.get(
            f"{injection_server}/lookup-int",
            params={"id": payload},
            timeout=10,
        )
        if r.status_code == 200:
            # A 200 must produce zero rows — the payload is interpreted
            # as a bound int and won't match anything in the seed table.
            body = r.json()
            rows = body if isinstance(body, list) else body.get("data", body.get("rows", []))
            assert isinstance(rows, list), f"unexpected response shape for {payload!r}: {body}"
            assert rows == [], f"numeric payload {payload!r} leaked rows: {rows}"
        else:
            # Anything 4xx is acceptable rejection; 5xx means something
            # crashed and is a bug.
            assert 400 <= r.status_code < 500, (
                f"numeric payload {payload!r} produced server error: "
                f"{r.status_code} {r.text}"
            )

    @pytest.mark.parametrize("payload", STRING_PAYLOADS_NO_MATCH)
    def test_string_injection_payloads_match_zero_rows(self, injection_server, payload):
        # Every payload is a syntactically valid string (validator passes)
        # but no payload is exactly equal to a seed row's `name`. With
        # prepared statements the value is bound as data, so the result
        # must be an empty rowset — never the full table (which would
        # indicate the injection succeeded).
        r = requests.get(
            f"{injection_server}/lookup-string",
            params={"name": payload},
            timeout=10,
        )
        assert r.status_code == 200, (
            f"payload {payload!r} unexpectedly errored: "
            f"{r.status_code} {r.text}"
        )
        body = r.json()
        rows = body if isinstance(body, list) else body.get("data", body.get("rows", []))
        assert isinstance(rows, list), f"unexpected response shape for {payload!r}: {body}"
        # CRITICAL: an injection that succeeded would return all 3 rows
        # (because `OR 1=1` makes the WHERE always-true). Prepared
        # statements make that impossible — the payload is just data.
        assert rows == [], (
            f"INJECTION LEAK: payload {payload!r} returned rows {rows}; "
            "expected empty set because no seed row has that literal name"
        )

    @pytest.mark.parametrize("payload, expected_id", STRING_PAYLOADS_MATCH)
    def test_string_legitimate_values_still_match(self, injection_server, payload, expected_id):
        # Negative-control: verify the prepared path doesn't over-defang
        # legitimate lookups. Exact string match must still succeed.
        r = requests.get(
            f"{injection_server}/lookup-string",
            params={"name": payload},
            timeout=10,
        )
        assert r.status_code == 200, r.text
        body = r.json()
        rows = body if isinstance(body, list) else body.get("data", body.get("rows", []))
        assert len(rows) == 1, f"expected one row for {payload!r}, got: {rows}"
        assert rows[0]["id"] == expected_id

    def test_integer_lookup_with_valid_value_returns_the_row(self, injection_server):
        r = requests.get(
            f"{injection_server}/lookup-int",
            params={"id": "2"},
            timeout=10,
        )
        assert r.status_code == 200, r.text
        body = r.json()
        rows = body if isinstance(body, list) else body.get("data", body.get("rows", []))
        assert len(rows) == 1
        assert rows[0]["id"] == 2
        assert rows[0]["name"] == "bob"
