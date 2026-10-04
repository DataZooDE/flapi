"""Request-validator hardening (#192).

Found by the crew review of the server, reproduced against a real binary:
 * the SQL keyword heuristic was bypassed with whitespace: a value such as
   x'<newline>OR 2>1<newline>OR n = 'x passed it and, spliced into
   `WHERE n = '{{{ params.name }}}'`, returned every row;
 * `number` / `float` / `double` / `boolean` were classified as bindable (which skips
   the heuristic) but had NO type validation, so `0 OR true` passed for a
   `{{{ params.amount }}}` site;
 * request-supplied `cacheTable` / `cacheSchema` / `cacheCatalog` overrode the
   server-owned cache metadata a template renders.
"""

import os
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port


def _w(base, rel, text):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def _endpoint(name, field, vtype, sql):
    return (f"url-path: /{name}\nmethod: GET\ntemplate-source: {name}.sql\nconnection: [inmem]\n"
            f"request:\n  - field-name: {field}\n    field-in: query\n    validators:\n      - type: {vtype}\n",
            sql)


@pytest.fixture(scope="module")
def server():
    tmp = tempfile.mkdtemp(prefix="flapi_valhard_")
    port = free_port()
    _w(tmp, "flapi.yaml",
       "project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
       "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
       "ducklake:\n  enabled: true\n  alias: cache\n  metadata-path: ./cache.ducklake\n  data-path: ./cache\n")
    rows = "(VALUES ('alice', 1, true), ('bob', 2, false), ('O''Brien', 3, true)) t(n, a, b)"
    specs = {
        "s": _endpoint("s", "name", "string", f"SELECT * FROM {rows} WHERE n = '{{{{{{params.name}}}}}}'\n"),
        "sb": _endpoint("sb", "name", "string", f"SELECT * FROM {rows} WHERE n = {{{{ params.name }}}}\n"),
        "num": _endpoint("num", "amount", "number", f"SELECT * FROM {rows} WHERE a > {{{{{{params.amount}}}}}}\n"),
        "flt": _endpoint("flt", "amount", "float", f"SELECT * FROM {rows} WHERE a > {{{{{{params.amount}}}}}}\n"),
        "dbl": _endpoint("dbl", "amount", "double", f"SELECT * FROM {rows} WHERE a > {{{{{{params.amount}}}}}}\n"),
        "bool": _endpoint("bool", "flag", "boolean", f"SELECT * FROM {rows} WHERE b = {{{{{{params.flag}}}}}}\n"),
        "bl2": _endpoint("bl2", "flag", "bool", f"SELECT * FROM {rows} WHERE b = {{{{{{params.flag}}}}}}\n"),
        "integer": _endpoint("integer", "amount", "integer", f"SELECT * FROM {rows} WHERE a > {{{{{{params.amount}}}}}}\n"),
    }
    for name, (yaml_text, sql) in specs.items():
        _w(tmp, f"sqls/{name}.yaml", yaml_text)
        _w(tmp, f"sqls/{name}.sql", sql)
    for t in ("one", "two"):
        _w(tmp, f"sqls/{t}.yaml",
           f"url-path: /{t}\nmethod: GET\ntemplate-source: {t}.sql\nconnection: [inmem]\n"
           f"cache:\n  enabled: true\n  table: {t}_cache\n  schedule: 1h\n  template-file: {t}_populate.sql\n")
        _w(tmp, f"sqls/{t}.sql", "SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}}\n")
        _w(tmp, f"sqls/{t}_populate.sql",
           f"CREATE OR REPLACE TABLE {{{{{{cache.catalog}}}}}}.{{{{{{cache.schema}}}}}}.{{{{{{cache.table}}}}}} AS SELECT '{t}' AS which\n")
    proc = subprocess.Popen(
        [flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "-p", str(port), "--log-level", "warning"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=tmp,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    base = f"http://127.0.0.1:{port}"
    for _ in range(300):
        try:
            h = requests.get(f"{base}/health", timeout=2)
            if h.status_code == 200 and h.json().get("caches", {}).get("total", 0) >= 2:
                break
        except requests.RequestException:
            pass
        time.sleep(0.2)
    yield base
    proc.terminate()
    proc.wait(timeout=30)


def _get(base, path, **params):
    return requests.get(f"{base}{path}", params=params, timeout=10)


INJECTIONS = [
    "x'\n\nOR 2>1\n\nOR n =   'x",        # newlines (the reported bypass)
    "x'\tOR\t2>1\tOR\tn\t=\t'x",           # tabs
    "x'\x0bOR\x0b2>1\x0bOR\x0bn='x",       # vertical tab
    "x'\x0cOR\x0c2>1\x0cOR n='x",          # form feed
    "x' OR n LIKE '%",                     # LIKE, no digits
    "x'||(SELECT 'a')||'",                 # concatenation
    "x' ) OR ( 'a'='a",                    # parentheses
    "x'  or  true  or  n  =  'x",          # extra spaces, boolean literal
    "x'\r\nOR\r\n1<2\r\nOR n='x",          # CRLF
    "x' UNION ALL SELECT 'a",              # keyword, mixed whitespace handled by the old list too
]


class TestTheStringHeuristicSurvivesWhitespaceTricks:

    @pytest.mark.parametrize("payload", INJECTIONS, ids=[repr(p)[:28] for p in INJECTIONS])
    def test_an_injection_is_rejected_and_returns_no_rows(self, server, payload):
        r = _get(server, "/s", name=payload)
        assert r.status_code == 400, f"{payload!r} -> {r.status_code} {r.text[:150]}"

    @pytest.mark.parametrize("name,expected_rows", [("alice", 1), ("bob", 1), ("nobody", 0)])
    def test_ordinary_values_still_work(self, server, name, expected_rows):
        r = _get(server, "/s", name=name)
        assert r.status_code == 200, f"{name!r} -> {r.status_code} {r.text[:150]}"
        assert len(r.json()["data"]) == expected_rows

    @pytest.mark.parametrize("name,expected_rows", [("O'Brien", 1), ("it's", 0), ("Bread and Butter", 0), ("Black or White", 0)])
    def test_names_with_apostrophes_and_everyday_words_work_where_the_value_is_bound(self, server, name, expected_rows):
        # `{{ params.name }}` is a prepared-statement parameter: an apostrophe is just a
        # character there, and the heuristic must not turn it into a 400.
        r = _get(server, "/sb", name=name)
        assert r.status_code == 200, f"{name!r} -> {r.status_code} {r.text[:150]}"
        assert len(r.json()["data"]) == expected_rows


class TestNumericAndBooleanTypesAreValidated:

    @pytest.mark.parametrize("path", ["/num", "/flt", "/dbl", "/integer"])
    @pytest.mark.parametrize("bad", ["0 OR true", "1; SELECT 1", "abc", "1 2", "", "1e", "--1", "0x10", "NaN-1"])
    def test_a_non_number_is_rejected(self, server, path, bad):
        r = _get(server, path, amount=bad)
        assert r.status_code == 400, f"{path} amount={bad!r} -> {r.status_code} {r.text[:150]}"

    @pytest.mark.parametrize("path,good", [("/num", "1"), ("/num", "-1"), ("/num", "1.5"), ("/num", "1e0"),
                                           ("/flt", "0.5"), ("/dbl", ".5"), ("/integer", "2")])
    def test_a_real_number_is_accepted(self, server, path, good):
        r = _get(server, path, amount=good)
        assert r.status_code == 200, f"{path} amount={good!r} -> {r.status_code} {r.text[:150]}"

    @pytest.mark.parametrize("path", ["/bool", "/bl2"])
    @pytest.mark.parametrize("bad", ["true OR 1=1", "yes", "2", "true;", "t", ""])
    def test_a_non_boolean_is_rejected(self, server, path, bad):
        r = _get(server, path, flag=bad)
        assert r.status_code == 400, f"{path} flag={bad!r} -> {r.status_code} {r.text[:150]}"

    @pytest.mark.parametrize("path", ["/bool", "/bl2"])
    @pytest.mark.parametrize("good", ["true", "false", "TRUE", "False", "1", "0"])
    def test_a_real_boolean_is_accepted(self, server, path, good):
        r = _get(server, path, flag=good)
        assert r.status_code == 200, f"{path} flag={good!r} -> {r.status_code} {r.text[:150]}"


class TestCacheMetadataIsServerOwned:

    def test_a_request_cannot_redirect_a_cached_endpoint_to_another_table(self, server):
        assert _get(server, "/one").json()["data"][0]["which"] == "one"
        assert _get(server, "/two").json()["data"][0]["which"] == "two"
        for key, value in (("cacheTable", "two_cache"), ("cacheSchema", "audit"), ("cacheCatalog", "memory")):
            r = _get(server, "/one", **{key: value})
            body = r.json() if r.status_code == 200 else {}
            assert r.status_code == 200 and body["data"][0]["which"] == "one", (
                f"{key}={value} changed what /one reads: {r.status_code} {r.text[:150]}")
