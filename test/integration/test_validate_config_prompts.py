"""`flapi --validate-config` must accept MCP prompts (#156).

A prompt holds its template INLINE and needs no template-source file or database
connection. The loader knew that; the validator held every file under the
templates directory to the REST endpoint rule, so flAPI's own shipped example
(`examples/flapi.yaml`, which includes `customers-mcp-prompt.yaml`) failed the very
command operators are told to run in CI:

    x Endpoint: customer_data_analysis
      ERROR: template-source cannot be empty
"""

import os
import subprocess
import tempfile

import pytest

from otel_helpers import flapi_binary

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _write(base, rel, text):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    return path


def _validate(files):
    tmp = tempfile.mkdtemp(prefix="flapi_valprompt_")
    for rel, text in files.items():
        _write(tmp, rel, text)
    cfg = _write(tmp, "flapi.yaml",
                 "project-name: p\nproject-description: prompt validation\n"
                 "template:\n  path: ./sqls\n"
                 "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
    r = subprocess.run([flapi_binary(), "-c", cfg, "--validate-config"],
                       capture_output=True, text=True, cwd=tmp, timeout=60)
    return r.returncode, r.stdout + r.stderr


PROMPT = ("mcp-prompt:\n  name: greet\n  description: A greeting prompt\n"
          "  template: |\n    Hello {{name}}, welcome.\n")


class TestPromptsValidate:

    def test_a_prompt_is_a_valid_file(self):
        code, out = _validate({"sqls/prompt.yaml": PROMPT})
        assert code == 0, (
            "--validate-config rejected an MCP prompt; it holds its template inline\n"
            + out[-2000:])
        assert "template-source cannot be empty" not in out, out[-2000:]

    def test_a_prompt_next_to_a_rest_endpoint(self):
        code, out = _validate({
            "sqls/prompt.yaml": PROMPT,
            "sqls/ep.yaml": ("url-path: /ep\nmethod: GET\ntemplate-source: ep.sql\n"
                             "connection: [inmem]\n"),
            "sqls/ep.sql": "SELECT 1 AS n\n"})
        assert code == 0, out[-2000:]

    def test_an_empty_prompt_is_still_rejected(self):
        code, out = _validate({"sqls/prompt.yaml":
                               "mcp-prompt:\n  name: empty\n  description: nothing to say\n"})
        assert code != 0, "a prompt with no template was accepted\n" + out[-2000:]
        assert "template" in out, out[-2000:]

    def test_a_rest_endpoint_still_needs_a_template_source(self):
        # The fence: only prompts are exempt from the template-source rule.
        code, out = _validate({"sqls/ep.yaml":
                               "url-path: /ep\nmethod: GET\nconnection: [inmem]\n"})
        assert code != 0, out
        assert "template-source" in out, out[-2000:]


class TestShippedExamplesValidate:

    # flapi-test.yaml is NOT here: it fails --validate-config too, but for a
    # different and legitimate reason - two SAP endpoints reference a connection the
    # test config deliberately excludes. Filed separately; add it back when fixed.
    @pytest.mark.parametrize("name", ["flapi.yaml", "flapi-bigquery-procedure.yaml"])
    def test_the_example_configs_pass_their_own_validator(self, name):
        # flapi.yaml is the one that failed: it loads
        # sqls/customers/customers-mcp-prompt.yaml.
        examples = os.path.join(REPO, "examples")
        r = subprocess.run([flapi_binary(), "-c", os.path.join(examples, name),
                            "--validate-config"],
                           capture_output=True, text=True, cwd=examples, timeout=120)
        out = r.stdout + r.stderr
        assert r.returncode == 0, f"{name} fails --validate-config\n" + out[-3000:]
