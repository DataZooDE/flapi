"""Docs must not show configuration flAPI does not read (#158, #163).

- `${NAME}` is not environment substitution; only `{{env.NAME}}` is.
- TLS is the `enforce-https` block; a top-level `https:` block is never read.
"""

import glob
import os
import re

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FENCE = re.compile(r"```ya?ml\n(.*?)```", re.S)


def _docs():
    paths = [os.path.join(REPO, "AGENTS.md"), os.path.join(REPO, "README.md")]
    paths += glob.glob(os.path.join(REPO, "docs", "**", "*.md"), recursive=True)
    paths = [p for p in paths if os.sep + "archive" + os.sep not in p and os.path.exists(p)]
    return paths


def _yaml_blocks():
    for path in _docs():
        with open(path) as f:
            text = f.read()
        for m in FENCE.finditer(text):
            yield os.path.relpath(path, REPO), m.group(1)


def test_yaml_examples_do_not_use_dollar_brace_variables():
    assert sum(1 for _ in _yaml_blocks()) > 20, "the scan found almost no YAML blocks"
    bad = []
    for path, block in _yaml_blocks():
        for line in block.splitlines():
            if re.search(r"\$\{[A-Za-z_][A-Za-z0-9_]*\}", line) and not line.lstrip().startswith("#"):
                bad.append(f"{path}: {line.strip()}")
    assert not bad, "`${NAME}` is not substituted by flAPI; use {{env.NAME}}:\n" + "\n".join(bad)


def test_yaml_examples_do_not_use_a_top_level_https_block():
    bad = [path for path, block in _yaml_blocks() if re.search(r"^https:\s*$", block, re.M)]
    assert not bad, f"a top-level `https:` block is never read; use enforce-https: {bad}"
