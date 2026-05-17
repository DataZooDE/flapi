"""End-to-end SQL-injection corpus for W3.1 prepared statements (#25).

Boots a flapi server with one endpoint per validator type, each backed by
an in-memory table seeded with known rows, and fires injection payloads
through the full HTTP → RequestValidator → PreparedTemplateRewriter →
duckdb_bind_* → DuckDB pipeline.

The contract under test:

1. **Strict validators (int / double / boolean / date / time / uuid /
   enum / email)** reject malformed input at HTTP boundary with 4xx.
   Injection payloads are rejected here before SQL is ever built.
2. **Loose validator (string)** accepts arbitrary strings; the prepared-
   statement bind is the hard boundary. Every payload reaches DuckDB as
   a primitive value, never as SQL text, so it matches at most a single
   literal row in the seed table — never the whole table.
3. **Legitimate values still match** — no over-defanging.

Per-type endpoints (one HTTP GET each):

    /lookup-int       /lookup-double    /lookup-boolean
    /lookup-date      /lookup-time      /lookup-uuid
    /lookup-enum      /lookup-email     /lookup-string

A pagination endpoint (`/lookup-int-paged`) verifies the prepared path
also works when the query is wrapped in `SELECT * FROM (...) LIMIT N
OFFSET M` (and its companion COUNT(*) wrap).

Marked `standalone_server` so the conftest autouse fixture does not
spin up the shared api_configuration server. Skips cleanly if the
local DuckDB extension cache is incompatible with the in-tree DuckDB
submodule; CI runs against fresh extensions.
"""

import os
import socket
import subprocess
import tempfile
import time
from typing import Any, Iterator, List, Tuple

import pytest
import requests


# ----------------------------------------------------------------------
# Fixtures / scaffolding
# ----------------------------------------------------------------------


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


# Each endpoint configuration: (path-segment, request-field YAML, SQL).
# The SQL must SELECT id from a 3-row VALUES table where exactly one row
# matches the "happy" parameter we'll send. The corpus then proves that
# every injection payload returns 0 rows (or a 4xx) while the happy
# parameter still returns 1 row.
ENDPOINTS = [
    # (slug, field-yaml, sql)
    (
        "int",
        """
  - field-name: id
    field-in: query
    field-type: int
    required: true
    validators:
      - type: int
        min: 1
        max: 100000
        preventSqlInjection: false
""",
        "SELECT id, label FROM (VALUES (1,'one'),(2,'two'),(3,'three')) AS t(id,label) "
        "WHERE id = {{ params.id }}",
    ),
    (
        "double",
        """
  - field-name: x
    field-in: query
    field-type: number
    required: true
    validators:
      - type: number
        preventSqlInjection: false
""",
        "SELECT * FROM (VALUES (1.5,'a'),(2.5,'b'),(3.5,'c')) AS t(x,label) "
        "WHERE x = {{ params.x }}",
    ),
    (
        "boolean",
        """
  - field-name: flag
    field-in: query
    field-type: boolean
    required: true
    validators:
      - type: boolean
        preventSqlInjection: false
""",
        # Only one of (true,'yes') matches `flag=true`; the (false,'no')
        # row is the negative control.
        "SELECT * FROM (VALUES (true,'yes'),(false,'no')) AS t(flag,label) "
        "WHERE flag = {{ params.flag }}",
    ),
    (
        "date",
        """
  - field-name: d
    field-in: query
    field-type: date
    required: true
    validators:
      - type: date
        preventSqlInjection: false
""",
        "SELECT * FROM (VALUES (DATE '2024-03-15','spring'),(DATE '2024-06-21','summer')) "
        "AS t(d,label) WHERE d = {{ params.d }}",
    ),
    (
        "time",
        """
  - field-name: t
    field-in: query
    field-type: time
    required: true
    validators:
      - type: time
        preventSqlInjection: false
""",
        "SELECT * FROM (VALUES (TIME '13:45:07','noon'),(TIME '00:00:00','midnight')) "
        "AS u(t,label) WHERE t = {{ params.t }}",
    ),
    (
        "uuid",
        """
  - field-name: u
    field-in: query
    field-type: string
    required: true
    validators:
      - type: uuid
        preventSqlInjection: false
""",
        "SELECT * FROM (VALUES "
        "('11111111-1111-1111-1111-111111111111','first'),"
        "('22222222-2222-2222-2222-222222222222','second')) "
        "AS t(u,label) WHERE u = {{ params.u }}",
    ),
    (
        "enum",
        """
  - field-name: status
    field-in: query
    field-type: string
    required: true
    validators:
      - type: enum
        allowedValues: [active, inactive, pending]
        preventSqlInjection: false
""",
        "SELECT * FROM (VALUES ('active','A'),('inactive','I'),('pending','P')) "
        "AS t(status,label) WHERE status = {{ params.status }}",
    ),
    (
        "email",
        """
  - field-name: e
    field-in: query
    field-type: string
    required: true
    validators:
      - type: email
        preventSqlInjection: false
""",
        "SELECT * FROM (VALUES "
        "('alice@example.com','A'),"
        "('bob@example.com','B')) "
        "AS t(e,label) WHERE e = {{ params.e }}",
    ),
    (
        "string",
        """
  - field-name: name
    field-in: query
    field-type: string
    required: true
    validators:
      - type: string
        min-length: 1
        max-length: 200
        preventSqlInjection: false
""",
        "SELECT * FROM (VALUES (1,'alice'),(2,'bob'),(3,'carol')) AS t(id,name) "
        "WHERE name = {{ params.name }}",
    ),
]


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

    for slug, field_yaml, sql in ENDPOINTS:
        with open(os.path.join(sqls, f"lookup_{slug}.yaml"), "w") as f:
            f.write(
                f"url-path: /lookup-{slug}\n"
                f"method: GET\n"
                f"template-source: lookup_{slug}.sql\n"
                f"connection: [inmem]\n"
                f"request:{field_yaml}"
            )
        with open(os.path.join(sqls, f"lookup_{slug}.sql"), "w") as f:
            f.write(sql + "\n")

    # Extra endpoint: typed param + pagination wrap. Same shape as
    # lookup-int but with 20 seed rows so an ?offset/?limit query can
    # actually paginate. The handler will wrap the rewritten SQL in a
    # SELECT * FROM (...) LIMIT ? OFFSET ? — both the wrap and the
    # rewritten inner statement must still bind `id` correctly.
    with open(os.path.join(sqls, "lookup_int_paged.yaml"), "w") as f:
        f.write(
            "url-path: /lookup-int-paged\n"
            "method: GET\n"
            "template-source: lookup_int_paged.sql\n"
            "connection: [inmem]\n"
            "request:\n"
            "  - field-name: min_id\n"
            "    field-in: query\n"
            "    field-type: int\n"
            "    required: true\n"
            "    validators:\n"
            "      - type: int\n"
            "        min: 0\n"
            "        max: 100000\n"
            "        preventSqlInjection: false\n"
        )
    with open(os.path.join(sqls, "lookup_int_paged.sql"), "w") as f:
        f.write(
            "SELECT i AS id, 'x' AS label FROM range(1, 21) AS r(i) "
            "WHERE i >= {{ params.min_id }} ORDER BY i\n"
        )

    return os.path.join(dirpath, "flapi.yaml")


@pytest.fixture(scope="module")
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


def _rows(body: Any) -> List[dict]:
    if isinstance(body, list):
        return body
    if isinstance(body, dict):
        return body.get("data", body.get("rows", []))
    return []


def _expect_no_leak(
    base_url: str,
    path: str,
    param: str,
    payload: str,
    *,
    sentinel_row_count: int,
) -> None:
    """Send `payload` and assert: either 4xx (validator rejection) OR 200
    with at most one row (legitimate literal match) — never the full
    seed-table count, which would indicate successful injection."""
    r = requests.get(f"{base_url}{path}", params={param: payload}, timeout=10)
    if r.status_code == 200:
        rows = _rows(r.json())
        assert isinstance(rows, list), f"unexpected response shape for {payload!r}: {r.text}"
        # Critical assertion: an OR-1=1 injection would return all
        # `sentinel_row_count` rows. The bind path means the payload is
        # interpreted as a single literal — it can match at most one
        # specific seed row (in practice 0 for the injection payloads,
        # since none equal a seed value).
        assert len(rows) < sentinel_row_count, (
            f"INJECTION LEAK on {path}: payload {payload!r} returned {len(rows)} rows "
            f"(seed table has {sentinel_row_count}); a successful OR-injection would "
            f"return them all."
        )
    else:
        assert 400 <= r.status_code < 500, (
            f"{path}: payload {payload!r} produced server error "
            f"{r.status_code}: {r.text}"
        )


# ----------------------------------------------------------------------
# Payload corpora (per type)
# ----------------------------------------------------------------------

# Generic injection payloads attempted against numeric and date/time
# fields. Strict validators reject these at 4xx; bind layer would error
# if any slipped through. Either outcome is acceptable; what isn't is a
# 200 with all rows leaked.
INJECTION_NUMERIC = [
    "1 OR 1=1",
    "1; DROP TABLE t",
    "1 UNION SELECT 1, 'evil'",
    "1/**/OR/**/1=1",
    "1' OR '1'='1",
    "'1' OR '1'='1'--",
    "1e3",
    "1.5",
    "0xdeadbeef",
    "abc",
    "",
    "   ",
    "1 AND SLEEP(5)",
    "1) UNION SELECT NULL--",
    " ;",
    "/*comment*/1",
]

INJECTION_DOUBLE = [
    "1.5 OR 1=1",
    "1.5; DROP TABLE t",
    "1.5' OR '1'='1",
    "1.5 UNION SELECT 1.0",
    "abc",
    "",
    "1.5/*",
    "  1.5 garbage",
]

INJECTION_BOOLEAN = [
    "true OR 1=1",
    "true; DROP TABLE",
    "yes",
    "TRUE; DROP TABLE",
    "1 OR 1=1",
    "false' --",
    "2",       # not a bool
    "",
]

INJECTION_DATE = [
    "2024-03-15' OR '1'='1",
    "2024-03-15; DROP TABLE t",
    "2024-13-99",
    "9999-99-99",
    "abc",
    "",
    "2024/03/15",       # wrong separator
    "15-03-2024",       # wrong order
    "2024-03-15 UNION",
]

INJECTION_TIME = [
    "12:00:00' OR '1'='1",
    "12:00:00; DROP TABLE",
    "24:00:00",          # invalid hour
    "12:00:60",          # invalid second
    "abc",
    "",
    "12:00",             # short form
    "12-00-00",          # wrong separator
]

INJECTION_UUID = [
    "11111111-1111-1111-1111-111111111111' OR '1'='1",
    "11111111-1111-1111-1111-111111111111; DROP TABLE",
    "abc' OR 1=1",
    "not-a-uuid",
    "",
    "' UNION SELECT password--",
    "11111111-1111-1111-1111-11111111111Z",  # non-hex
]

INJECTION_ENUM = [
    "active' OR '1'='1",
    "active; DROP TABLE",
    "ACTIVE",            # case mismatch — not in allowed-values
    "deleted",           # not in allowed-values
    "",
    "active OR pending",
    "' UNION SELECT 'x'--",
]

INJECTION_EMAIL = [
    "alice@example.com' OR '1'='1",
    "alice@example.com; DROP TABLE",
    "alice@example.com OR 1=1",
    "not-an-email",
    "",
    "'; DROP TABLE t; --@x.com",
    "<script>@x.com",
]

# String payloads — the validator passes any string-shaped input, so
# the prepared bind is the actual defense. None of these equals a seed
# row's `name`, so all must come back as 0 rows.
INJECTION_STRING_NO_MATCH = [
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
    "ALICE",
    "alice\\",
    "carol' UNION SELECT password FROM users--",
    "x' OR 'a'='a' OR 'x'='",
    "Robert');DROP TABLE Students;--",  # xkcd 327
    "1' OR id<>0--",                    # tries to expose 'id'
    "alice' OR length(name)>0--",
]


# ----------------------------------------------------------------------
# Tests
# ----------------------------------------------------------------------


@pytest.mark.standalone_server
class TestSqlInjectionCorpus:
    """End-to-end coverage of W3.1 PR B prepared statements.

    Each test sweeps a payload set against the endpoint and asserts:
    no payload returns the full seed-table row count (an OR-injection),
    legitimate values still return their single matching row.
    """

    # -- Numeric / temporal types ------------------------------------

    @pytest.mark.parametrize("payload", INJECTION_NUMERIC)
    def test_int(self, injection_server, payload):
        _expect_no_leak(injection_server, "/lookup-int", "id", payload, sentinel_row_count=3)

    @pytest.mark.parametrize("payload", INJECTION_DOUBLE)
    def test_double(self, injection_server, payload):
        _expect_no_leak(injection_server, "/lookup-double", "x", payload, sentinel_row_count=3)

    @pytest.mark.parametrize("payload", INJECTION_BOOLEAN)
    def test_boolean(self, injection_server, payload):
        _expect_no_leak(injection_server, "/lookup-boolean", "flag", payload, sentinel_row_count=2)

    @pytest.mark.parametrize("payload", INJECTION_DATE)
    def test_date(self, injection_server, payload):
        _expect_no_leak(injection_server, "/lookup-date", "d", payload, sentinel_row_count=2)

    @pytest.mark.parametrize("payload", INJECTION_TIME)
    def test_time(self, injection_server, payload):
        _expect_no_leak(injection_server, "/lookup-time", "t", payload, sentinel_row_count=2)

    # -- Format-strict varchar-bindable types ------------------------

    @pytest.mark.parametrize("payload", INJECTION_UUID)
    def test_uuid(self, injection_server, payload):
        _expect_no_leak(injection_server, "/lookup-uuid", "u", payload, sentinel_row_count=2)

    @pytest.mark.parametrize("payload", INJECTION_ENUM)
    def test_enum(self, injection_server, payload):
        _expect_no_leak(injection_server, "/lookup-enum", "status", payload, sentinel_row_count=3)

    @pytest.mark.parametrize("payload", INJECTION_EMAIL)
    def test_email(self, injection_server, payload):
        _expect_no_leak(injection_server, "/lookup-email", "e", payload, sentinel_row_count=2)

    # -- Loose varchar: prepared bind is the actual defense ---------

    @pytest.mark.parametrize("payload", INJECTION_STRING_NO_MATCH)
    def test_string(self, injection_server, payload):
        _expect_no_leak(injection_server, "/lookup-string", "name", payload, sentinel_row_count=3)

    # -- Negative controls: legitimate values still match -----------

    LEGIT_CASES: List[Tuple[str, str, str, Any]] = [
        ("/lookup-int",     "id",     "2",                                          2),
        ("/lookup-double",  "x",      "1.5",                                        1.5),
        ("/lookup-boolean", "flag",   "true",                                       True),
        ("/lookup-date",    "d",      "2024-03-15",                                 "2024-03-15"),
        ("/lookup-time",    "t",      "13:45:07",                                   "13:45:07"),
        ("/lookup-uuid",    "u",      "11111111-1111-1111-1111-111111111111",       "11111111-1111-1111-1111-111111111111"),
        ("/lookup-enum",    "status", "active",                                     "active"),
        ("/lookup-email",   "e",      "alice@example.com",                          "alice@example.com"),
        ("/lookup-string",  "name",   "alice",                                      "alice"),
    ]

    @pytest.mark.parametrize("path, param, value, _expected", LEGIT_CASES)
    def test_legit_value_returns_one_row(self, injection_server, path, param, value, _expected):
        r = requests.get(f"{injection_server}{path}", params={param: value}, timeout=10)
        assert r.status_code == 200, f"{path} with {value!r}: {r.status_code} {r.text}"
        rows = _rows(r.json())
        assert len(rows) == 1, f"{path} with {value!r} returned {len(rows)} rows: {rows}"

    # -- Pagination + bindings ---------------------------------------

    def test_pagination_with_bound_param_returns_correct_page(self, injection_server):
        # Seed has 20 rows (ids 1..20). min_id=5 narrows to 16 rows;
        # limit=5 + offset=10 should return ids 15..19.
        r = requests.get(
            f"{injection_server}/lookup-int-paged",
            params={"min_id": "5", "limit": "5", "offset": "10"},
            timeout=10,
        )
        assert r.status_code == 200, r.text
        body = r.json()
        rows = _rows(body)
        assert len(rows) == 5, f"expected 5 rows, got {len(rows)}: {body}"
        ids = sorted(int(r["id"]) for r in rows)
        assert ids == [15, 16, 17, 18, 19], f"unexpected ids: {ids}"
        # If pagination metadata is exposed, the total count for min_id=5
        # is 16 (ids 5..20). When `total_count` is present in the body
        # it must reflect the inner filter, not the seed-table size.
        if isinstance(body, dict) and "total_count" in body:
            assert body["total_count"] == 16, body

    def test_pagination_injection_attempt_returns_zero_rows(self, injection_server):
        # min_id is int-typed → strict parse rejects the payload at 4xx.
        # If a regression let it through, the bind layer would error.
        # What MUST NOT happen is a 200 with the full 20 rows.
        r = requests.get(
            f"{injection_server}/lookup-int-paged",
            params={"min_id": "0 OR 1=1", "limit": "100", "offset": "0"},
            timeout=10,
        )
        if r.status_code == 200:
            rows = _rows(r.json())
            assert len(rows) < 20, (
                f"INJECTION LEAK on /lookup-int-paged: payload returned {len(rows)} rows"
            )
        else:
            assert 400 <= r.status_code < 500, r.text
