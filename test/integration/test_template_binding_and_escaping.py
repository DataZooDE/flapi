"""Template binding sites and SQL escaping (#277), against the real binary.

* (a) a `{{ params.x }}` binds only where SQL takes a value: an `enum` in
  `ORDER BY` and a `DATE '{{ d }}'` stay text instead of a `?` DuckDB refuses,
  and `c = '{{ params.name }}'` binds as a whole literal, as the triple form does.
* (b) `{{ }}` doubles single quotes instead of HTML-escaping: a section-scoped
  `c = '{{ params.name }}'` matches O'Brien and `{{ conn.path }}` is a real path,
  while the Swagger UI page keeps HTML escaping.
* (c) `--validate-config` warns about free text pasted where it cannot bind.
* (d) `template/expand?validate_only` reports only variables that are really
  undefined, by their own names, and answers 200 even when `valid` is false.
"""
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary

pytestmark = pytest.mark.standalone_server

TOKEN = "binding-escaping-token"
AUTH = {"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"}

CONFIG = """\
project-name: "esc <b>bold</b> & 'q'"
project-description: template binding and escaping
template:
  path: ./sqls
connections:
  people:
    init: |
      CREATE OR REPLACE TABLE people AS
      SELECT * FROM (VALUES (1, 'O''Brien', DATE '2024-01-02'),
                            (2, 'Smith', DATE '2024-03-04'),
                            (3, 'A/B', DATE '2024-05-06')) t(id, name, born);
  files:
    properties:
      path: ./data/two.csv
rate_limit:
  enabled: false
"""


def endpoint(path, sql_file, fields="", connection="people"):
    return (f"url-path: {path}\nmethod: GET\nwith-pagination: false\n"
            f"template-source: {sql_file}\nconnection: [{connection}]\n"
            + (f"request:\n{fields}" if fields else ""))


NAME = """\
  - field-name: name
    field-in: query
    validators:
      - type: string
        max-length: 50
"""
SORT = """\
  - field-name: sort
    field-in: query
    default: id
    validators:
      - type: enum
        allowedValues: [id, name]
"""
DATE = """\
  - field-name: d
    field-in: query
    required: true
    validators:
      - type: date
"""

FILES = {
    # (a) binding sites
    "sorted.yaml": endpoint("/sorted", "sorted.sql", SORT),
    "sorted.sql": "SELECT id, name FROM people ORDER BY {{ params.sort }}",
    "sorted-list.yaml": endpoint("/sorted-list", "sorted-list.sql", SORT),
    "sorted-list.sql": "SELECT id, name FROM people ORDER BY born IS NULL, {{ params.sort }}",
    "born.yaml": endpoint("/born", "born.sql", DATE),
    "born.sql": "SELECT id FROM people WHERE born = DATE '{{ params.d }}'",
    "exact.yaml": endpoint("/exact", "exact.sql", NAME),
    "exact.sql": "SELECT id FROM people WHERE name = '{{ params.name }}' ORDER BY id",
    "like.yaml": endpoint("/like", "like.sql", NAME),
    "like.sql": "SELECT id FROM people WHERE name LIKE '%{{ params.name }}%' ORDER BY id",
    # (b) SQL escaping
    "section.yaml": endpoint("/section", "section.sql", NAME),
    "section.sql": ("SELECT id FROM people WHERE 1=1\n"
                    "{{#params.name}}  AND name = '{{ params.name }}'\n{{/params.name}}ORDER BY id"),
    "path.yaml": endpoint("/path", "path.sql", connection="files"),
    "path.sql": "SELECT count(*) AS n FROM read_csv('{{ conn.path }}')",
    # (d) validate_only
    "vars.yaml": endpoint("/vars", "vars.sql", NAME, connection="files"),
    "vars.sql": ("SELECT count(*) AS n FROM read_csv('{{{ conn.path }}}') WHERE 1=1 {{! a comment }}\n"
                 "{{#params.name}}  AND b = '{{ params.name }}'\n{{/params.name}}"
                 "{{^params.name}}  AND b <> ''\n{{/params.name}}"),
}


def _port():
    for port in range(31500, 31600):
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", port))
            except OSError:
                continue
            return port
    pytest.skip("no free port in 31500-31599")


class Server:
    def __init__(self, root):
        self.port = _port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(root, "server.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", "flapi.yaml", "-p", str(self.port), "--log-level", "info",
             "--config-service", "--config-service-token", TOKEN],
            cwd=root, stdout=self.log, stderr=subprocess.STDOUT, preexec_fn=os.setsid,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        deadline = time.time() + 90
        while time.time() < deadline:
            if self.proc.poll() is not None:
                pytest.fail("server exited:\n" + self.output()[-4000:])
            try:
                if requests.get(f"{self.base}/health/live", timeout=1).status_code == 200:
                    return
            except requests.RequestException:
                time.sleep(0.3)
        pytest.fail("server never became live:\n" + self.output()[-4000:])

    def output(self):
        self.log.flush()
        with open(self.log_path) as f:
            return f.read()

    def stop(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=10)
        self.log.close()


def write_project(root):
    os.makedirs(os.path.join(root, "sqls"), exist_ok=True)
    os.makedirs(os.path.join(root, "data"), exist_ok=True)
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(CONFIG)
    with open(os.path.join(root, "data", "two.csv"), "w") as f:
        f.write("a,b\n1,x\n2,y\n")
    for name, body in FILES.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(body)


@pytest.fixture(scope="module")
def server():
    root = tempfile.mkdtemp(prefix="flapi_bindesc_")
    write_project(root)
    s = Server(root)
    yield s
    s.stop()
    shutil.rmtree(root, ignore_errors=True)


def get(server, path, **params):
    r = requests.get(f"{server.base}{path}", params=params, timeout=30)
    assert r.status_code == 200, f"{path} {params} -> {r.status_code} {r.text[:300]}\n" + server.output()[-2000:]
    return r.json()


# ------------------------------------------------------------------ (a) binding sites

def test_an_enum_in_order_by_is_a_name_not_a_placeholder(server):
    assert [r["name"] for r in get(server, "/sorted", sort="name")] == ["A/B", "O'Brien", "Smith"]
    assert [r["id"] for r in get(server, "/sorted")] == [1, 2, 3]


def test_an_enum_after_a_comma_in_order_by_is_a_name_too(server):
    assert [r["name"] for r in get(server, "/sorted-list", sort="name")] == ["A/B", "O'Brien", "Smith"]


def test_a_date_literal_with_double_braces_is_not_a_quoted_question_mark(server):
    assert get(server, "/born", d="2024-01-02") == [{"id": 1}]


def test_a_quoted_double_brace_value_binds_and_matches_an_apostrophe(server):
    assert get(server, "/exact", name="O'Brien") == [{"id": 1}]
    assert get(server, "/exact", name="Smith") == [{"id": 2}]


def test_a_double_brace_like_pattern_binds(server):
    assert get(server, "/like", name="Brien") == [{"id": 1}]
    assert get(server, "/like", name="A/") == [{"id": 3}]


def test_the_quoted_double_brace_value_is_a_bound_parameter(server):
    r = requests.post(f"{server.base}/api/v1/_config/endpoints/-exact/template/expand",
                      headers=AUTH, json={"parameters": {"name": "O'Brien"}}, timeout=30)
    assert r.status_code == 200, r.text
    body = r.json()
    assert "name = ?" in body["expanded"], body
    assert [b["value"] for b in body["bindings"]] == ["O'Brien"], body


# ------------------------------------------------------------------ (b) SQL escaping

def test_a_section_scoped_quoted_double_brace_matches_an_apostrophe(server):
    assert get(server, "/section", name="O'Brien") == [{"id": 1}]
    assert get(server, "/section", name="A/B") == [{"id": 3}]
    assert [r["id"] for r in get(server, "/section")] == [1, 2, 3]


def test_a_quote_cannot_leave_the_section_scoped_literal(server):
    r = requests.get(f"{server.base}/section", params={"name": "x' OR 'a'='a"}, timeout=30)
    assert r.status_code in (200, 400), r.text
    if r.status_code == 200:
        assert r.json() == [], r.text


def test_the_section_scoped_value_renders_quote_doubled(server):
    r = requests.post(f"{server.base}/api/v1/_config/endpoints/-section/template/expand",
                      headers=AUTH, json={"parameters": {"name": "O'Brien"}}, timeout=30)
    assert r.status_code == 200, r.text
    assert "name = 'O''Brien'" in r.json()["expanded"], r.text


def test_a_double_brace_connection_path_is_a_real_path(server):
    assert get(server, "/path") == [{"n": 2}]


def test_the_swagger_page_still_html_escapes(server):
    r = requests.get(f"{server.base}/doc", timeout=30)
    assert r.status_code == 200, r.text[:300]
    assert "esc &lt;b&gt;bold&lt;&#x2F;b&gt; &amp; &#39;q&#39;" in r.text, r.text[:800]
    assert "<b>bold</b>" not in r.text


# ------------------------------------------------------------------ (c) lint

LINT_FILES = {
    "risky.yaml": endpoint("/risky", "risky.sql", NAME),
    "risky.sql": ("SELECT id FROM people WHERE 1=1\n"
                  "{{#params.name}}  AND name = '{{{ params.name }}}'\n{{/params.name}}ORDER BY id"),
    "safe.yaml": endpoint("/safe", "safe.sql", NAME),
    "safe.sql": ("SELECT id FROM people WHERE 1=1\n"
                 "{{#params.name}}  AND name = '{{ params.name }}'\n{{/params.name}}ORDER BY id"),
}


def test_validate_config_warns_about_free_text_pasted_where_it_cannot_bind(tmp_path):
    root = str(tmp_path)
    write_project(root)
    for name, body in LINT_FILES.items():
        (tmp_path / "sqls" / name).write_text(body)
    proc = subprocess.run([flapi_binary(), "-c", "flapi.yaml", "--validate-config"], cwd=root,
                          capture_output=True, text=True, timeout=120,
                          env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    out = proc.stdout + proc.stderr
    risky = [l for l in out.splitlines() if "free-text field 'name'" in l]
    assert risky, out[-3000:]
    assert all("risky.sql:2:" in l for l in risky), risky
    assert any("write '{{ params.name }}'" in l for l in risky), risky
    assert not any("safe.sql" in l for l in risky), risky
    # The (a)/(b) project itself is clean: every free-text site there binds or is quote-doubled.
    assert not any(f"{n}.sql" in l for l in risky for n in ("exact", "like", "section", "sorted", "born")), risky



# ------------------------------------------------------------------ (d) validate_only

def validate(server, slug, body):
    r = requests.post(f"{server.base}/api/v1/_config/endpoints/{slug}/template/expand?validate_only=1",
                      headers=AUTH, json=body, timeout=30)
    assert r.status_code == 200, r.text
    return r.json()


def undefined(result):
    return sorted(w["variable"] for w in result["warnings"] if w["type"] == "undefined_variable")


def test_validate_only_reports_no_false_undefined_variables(server):
    for params in ({}, {"name": "x"}):
        result = validate(server, "-vars", {"parameters": params})
        assert result["valid"] is True, result
        assert undefined(result) == [], result


def test_validate_only_names_what_is_really_undefined(server):
    result = validate(server, "-vars", {"parameters": {"name": "x"},
                                         "template": "SELECT '{{ params.nope }}', '{{{ conn.nothere }}}' AS b"})
    assert undefined(result) == ["conn.nothere", "params.nope"], result


def test_validate_only_answers_200_when_the_sql_is_invalid(server):
    result = validate(server, "-vars", {"parameters": {}, "template": "SELEC 1"})
    assert result["valid"] is False, result
