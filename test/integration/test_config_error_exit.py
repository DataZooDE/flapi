"""A configuration error must exit cleanly, not abort (#126).

`initializeConfig` was not inside a try/catch, so any exception it threw -
an invalid flapi.yaml, a missing template directory, a malformed endpoint -
reached std::terminate. flAPI binds that to a handler which logs the message
and then calls std::abort().

The diagnostic was printed, which was fine. The abort was not: a plain typo
raised SIGABRT, dumped core, and exited 134. In a container that is reported
as a crash, indistinguishable from a real fault, and on a host with core dumps
enabled every restart attempt wrote one - of a ~77 MB binary.
"""
import os
import subprocess
import tempfile

import pytest

from otel_helpers import flapi_binary

pytestmark = pytest.mark.standalone_server

# 128 + SIGABRT(6). A shell reports an abort this way, and it is what the
# old binary returned.
ABORT_EXIT_CODE = 134


def run_with_config(contents: str):
    tmp = tempfile.mkdtemp(prefix="flapi_badcfg_")
    cfg = os.path.join(tmp, "flapi.yaml")
    with open(cfg, "w") as f:
        f.write(contents)
    return subprocess.run(
        [flapi_binary(), "-c", cfg, "-p", "0", "--log-level", "warning"],
        cwd=tmp, capture_output=True, text=True, timeout=120,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})


class TestConfigErrorExit:
    def test_malformed_yaml_exits_one_not_aborts(self):
        r = run_with_config("project-name: x\nthis is not: [valid\n")
        assert r.returncode == 1, (
            f"expected a clean exit, got {r.returncode} "
            f"({'abort' if r.returncode == ABORT_EXIT_CODE else 'other'})")
        assert r.returncode != ABORT_EXIT_CODE

    def test_the_error_says_what_is_wrong(self):
        # An operator must be able to act on it without a debugger.
        r = run_with_config("project-name: x\nthis is not: [valid\n")
        combined = r.stdout + r.stderr
        assert "Configuration error" in combined, combined[-800:]
        assert "cannot start until the configuration is valid" in combined

    def test_a_missing_config_file_also_exits_cleanly(self):
        tmp = tempfile.mkdtemp(prefix="flapi_nocfg_")
        r = subprocess.run(
            [flapi_binary(), "-c", os.path.join(tmp, "absent.yaml"),
             "-p", "0", "--log-level", "warning"],
            cwd=tmp, capture_output=True, text=True, timeout=120,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
        assert r.returncode != ABORT_EXIT_CODE
        assert r.returncode != 0


MAIN_CONFIG = """\
project-name: validate-all
template:
  path: ./sqls
connections:
  inmem:
    properties:
      database: ':memory:'
"""


def make_project(files):
    """files: {relative name under sqls/: content}. Returns the project dir."""
    tmp = tempfile.mkdtemp(prefix="flapi_valall_")
    os.makedirs(os.path.join(tmp, "sqls"))
    with open(os.path.join(tmp, "flapi.yaml"), "w") as f:
        f.write(MAIN_CONFIG)
    for name, content in files.items():
        with open(os.path.join(tmp, "sqls", name), "w") as f:
            f.write(content)
    return tmp


def endpoint_yaml(path, sql, fields=("id",), method="GET", field_in="query"):
    req = "".join(
        f"  - field-name: {n}\n    field-in: {field_in}\n    required: false\n" for n in fields)
    return (f"url-path: {path}\nmethod: {method}\nrequest:\n{req}"
            f"template-source: {sql}\nconnection: [inmem]\n")


def validate(project):
    r = subprocess.run(
        [flapi_binary(), "-c", os.path.join(project, "flapi.yaml"), "--validate-config",
         "--log-level", "error"],
        cwd=project, capture_output=True, text=True, timeout=120,
        env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"})
    return r, r.stdout + r.stderr


class TestValidateConfigReportsEverything:
    """#215: --validate-config stopped at the first invalid template file and
    never checked `{{ params.x }}` names against the declared request fields."""

    def test_reports_all_invalid_files_not_just_the_first(self):
        project = make_project({
            "a.yaml": endpoint_yaml("/a", "a.sql"),
            "a.sql": "SELECT {{#params.id}}1{{/params.zz}}\n",          # crossed sections
            "b.yaml": endpoint_yaml("/b", "b.sql"),
            "b.sql": "SELECT {{ params.nope }} AS n\n",                  # undeclared param
            "c.yaml": endpoint_yaml("/c", "c.sql"),
            "c.sql": "SELECT {{{ params.id }}} AS n\n",                  # fine
        })
        r, out = validate(project)
        assert r.returncode == 1, out
        assert "a.sql" in out, f"first bad template not reported:\n{out}"
        assert "params.nope" in out, f"undeclared param not reported:\n{out}"
        assert "b.sql" in out or "/b" in out, out

    def test_two_syntax_errors_are_both_listed(self):
        project = make_project({
            "a.yaml": endpoint_yaml("/a", "a.sql"),
            "a.sql": "SELECT {{#params.id}}1\n",
            "b.yaml": endpoint_yaml("/b", "b.sql"),
            "b.sql": "SELECT 1 {{/params.id}}\n",
        })
        r, out = validate(project)
        assert r.returncode == 1, out
        assert "a.sql" in out and "b.sql" in out, out

    def test_undeclared_param_alone_fails_validation(self):
        project = make_project({
            "b.yaml": endpoint_yaml("/b", "b.sql"),
            "b.sql": "SELECT {{ params.nope }} AS n\n",
        })
        r, out = validate(project)
        assert r.returncode == 1, out
        assert "params.nope" in out, out
        assert "b.sql" in out or "/b" in out, out

    def test_valid_templates_are_not_flagged(self):
        # Every shape that must NOT be reported: path/header/body fields,
        # section-scoped names, conn/env/cache/context, comments, the always-set
        # limit/offset, and a name declared only by the url-path.
        project = make_project({
            "ok.yaml": (
                "url-path: /ok/:tenant\nmethod: POST\nrequest:\n"
                "  - field-name: tenant\n    field-in: path\n"
                "  - field-name: X-Org\n    field-in: header\n    required: false\n"
                "  - field-name: items\n    field-in: body\n    required: false\n"
                "  - field-name: q\n    field-in: query\n    required: false\n"
                "template-source: ok.sql\nconnection: [inmem]\n"),
            "ok.sql": (
                "SELECT '{{{ params.tenant }}}' AS t, '{{{ params.X-Org }}}' AS o\n"
                "{{#params.items}}, {{name}}{{/params.items}}\n"
                "{{#params.q}}-- q is {{{ params.q }}}{{/params.q}}\n"
                "{{^params.q}}-- no q{{/params.q}}\n"
                "{{! params.commented_out }}\n"
                "FROM '{{{ conn.database }}}' {{{ env.HOME }}} {{{ cache.table }}}\n"
                "{{{ context.conn.path }}} {{{ context.auth.username }}}\n"
                "LIMIT {{ params.limit }} OFFSET {{ params.offset }}\n"),
        })
        r, out = validate(project)
        assert "params." not in out.replace("params.nope", ""), (
            f"a valid template was flagged:\n{out}")
        assert r.returncode == 0, out
