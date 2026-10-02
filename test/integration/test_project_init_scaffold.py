"""The `flapii project init` scaffold must be a config flAPI actually reads (#159).

It generated `description:` and a top-level `template-source:` (neither is read:
the keys are `project-description` and `template: {path}`), `${VAR}` references
(not substituted) and a rate-limit block with dead keys, so the first config a
new user got silently did nothing. This extracts the real scaffold from the CLI
source and runs the real binary over it, so it cannot drift again.
"""

import os
import re
import subprocess
import tempfile

import yaml

from otel_helpers import flapi_binary

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TEMPLATES = os.path.join(REPO, "cli", "src", "commands", "project", "templates.ts")


def _scaffold():
    """{relative path: content} from the PROJECT_TEMPLATES object literal."""
    with open(TEMPLATES) as f:
        src = f.read()
    body = src[src.index("export const PROJECT_TEMPLATES"):src.index("export function getTemplateFilePaths")]
    out = {}
    for m in re.finditer(r"'([^']+)': `(.*?)`,?\n", body, re.S):
        out[m.group(1)] = m.group(2).replace("\\${", "${")
    return out


def _write_project():
    files = _scaffold()
    assert "flapi.yaml" in files and "sqls/sample.yaml" in files, sorted(files)
    tmp = tempfile.mkdtemp(prefix="flapi_scaffold_")
    for rel, content in files.items():
        path = os.path.join(tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(content)
    return tmp


def test_the_scaffold_passes_the_validator_and_loads_its_endpoint():
    tmp = _write_project()
    r = subprocess.run([flapi_binary(), "-c", os.path.join(tmp, "flapi.yaml"), "--validate-config"],
                       capture_output=True, text=True, cwd=tmp, timeout=60)
    out = r.stdout + r.stderr
    assert r.returncode == 0, out[-2500:]
    assert "Unknown top-level" not in out, "the scaffold carries a key flAPI never reads\n" + out[-2500:]
    # The templates directory was actually loaded - the sample endpoint exists.
    assert "Parsed 1 endpoint(s)" in out, out[-2500:]


def test_the_scaffold_does_not_use_unsubstituted_dollar_brace_variables():
    for rel, content in _scaffold().items():
        uncommented = [l for l in content.splitlines() if not l.lstrip().startswith("#")]
        assert not any(re.search(r"\$\{[A-Za-z_]", l) for l in uncommented), (
            f"{rel} uses ${{VAR}}, which flAPI does not substitute; use {{{{env.VAR}}}}")


def test_the_rate_limit_template_uses_keys_flapi_reads():
    content = _scaffold()["common/rate-limit.yaml"]
    block = yaml.safe_load(content)["rate-limit"]
    assert set(block) <= {"enabled", "max", "interval", "key"}, (
        f"rate-limit keys flAPI does not read: {sorted(set(block) - {'enabled', 'max', 'interval', 'key'})}")
