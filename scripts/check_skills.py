#!/usr/bin/env python3
"""Verify the flAPI Claude Code plugin against the real code and binaries.

    make check-skills
    python3 scripts/check_skills.py [--flapi build/release/flapi] [--flapii flapii] [-v]

Skills are only useful if they are right, and they rot silently: a renamed key
or CLI flag keeps "working" in prose. This checker fails the build when the
plugin under plugins/flapi/ says something the code does not do.

Checks
------
1. Manifests: marketplace.json / plugin.json parse, names agree, every skill
   directory has a SKILL.md whose frontmatter `name` matches its directory.
2. Version: plugin.json `version`, every SKILL.md frontmatter `version` and its
   `**flAPI:** v<version>` banner equal the newest released heading in
   CHANGELOG.md (`## v<version> ...`). Drift fails.
3. Config keys: every mapping key in every ```yaml block must be READ by the
   parser somewhere in src/ (a subscript `["key"]`, `safeGet(...,"key")`,
   `has("key")`, or a key list). Children of user-named maps (connection
   names, `properties`, JSON-schema `properties`, ...) are skipped.
4. Forbidden facts: deprecated keys and known-wrong forms
   (scripts/skill_snippets/forbidden.txt) may appear only where the text
   says they are gone ("deprecated", "no effect", "ignored", ...). This and
   checks 5, 6 and 8 also cover Readme.md, TELEMETRY.md and docs/ (not
   docs/archive, docs/blog, docs/promotion).
8. Links: every relative link and #anchor in the plugin and the docs resolves
   (anchors as GitHub generates them from headings).
9. Quick Start: the Readme's `<!-- flapi: quickstart ... -->` blocks run in a
   clean directory against the built binaries, as a newcomer would run them.
5. Template variables: every `{{cache.X}}` names a variable the template
   processor actually sets (src/sql_template_processor.cpp).
6. CLI: every `flapii ...` and `flapi ...` invocation in a code block or inline
   code names a real (sub)command, and every `--option` it uses exists on that
   command - learned from the binaries' own --help output.
7. Executable examples: tagged snippets are assembled into throwaway
   projects and run through the real server.

Snippet tags (first line of a fenced block)
-------------------------------------------
    ```yaml
    # flapi: config group=orders            -> flapi.yaml of group "orders"
    # flapi: endpoint group=orders file=orders.yaml [allow=MCP_UNAUTHENTICATED_TOOLS]
    #   (inside an endpoint) # flapi: check GET /orders?limit=2 -> 200
    ```sql
    -- flapi: template group=orders file=orders.sql

A group without its own config uses scripts/skill_snippets/fixture/flapi.yaml
(connections `customers` -> ./data/customers.parquet, `northwind` -> the
northwind sqlite attached as `nw`, DuckLake enabled). `./data` is linked to
examples/data, so a group's own config can use the same relative paths. Every group must pass `flapi --validate-config` with no
errors and no warnings; a group with `check` lines is then started with the
config service on and each check is requested.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

try:
    import yaml  # PyYAML
except ImportError:  # pragma: no cover - CI installs it
    yaml = None

REPO = Path(__file__).resolve().parent.parent
PLUGIN = REPO / "plugins" / "flapi"
SUPPORT = REPO / "scripts" / "skill_snippets"
FENCE = re.compile(r"^```(\w+)?[^\n]*\n(.*?)^```", re.S | re.M)
TAG = re.compile(r"^\s*(?:#|--)\s*flapi:\s*(\w+)(.*)$")

# Keys whose CHILDREN are names the user chooses, not config keys.
USER_NAMED_CHILDREN = {
    "connections",      # connection names (their children ARE checked)
    "methods",          # mcp.auth.methods: JSON-RPC method names
    "properties",       # connection properties / JSON-schema properties
    "env", "headers", "arguments", "params", "defaults",
    "examples", "example", "secrets-map", "variables", "presets",
}
# Keys whose whole subtree is user data (JSON schema bodies, free-form maps).
# `duckdb:` passes any other key straight to DuckDB as a setting (threads,
# max_memory, ...), so its children are DuckDB's namespace, not flAPI's.
OPAQUE_SUBTREES = {"duckdb", "input_schema", "inputSchema", "input-schema", "output_schema",
                   "outputSchema", "output-schema", "schema-properties", "items", "annotations"}


@dataclass
class Snippet:
    path: Path
    line: int
    lang: str
    body: str
    tag: str | None = None
    attrs: dict = field(default_factory=dict)


@dataclass
class Report:
    failures: list = field(default_factory=list)
    passes: int = 0
    verbose: bool = False

    def fail(self, where: str, msg: str):
        self.failures.append(f"FAIL {where}: {msg}")

    def ok(self, where: str, msg: str = ""):
        self.passes += 1
        if self.verbose:
            print(f"ok   {where} {msg}")


def rel(p: Path) -> str:
    try:
        return str(p.relative_to(REPO))
    except ValueError:
        return str(p)


# --------------------------------------------------------------------------- inputs

def plugin_markdown() -> list[Path]:
    files = sorted((PLUGIN / "skills").glob("*/**/*.md")) + sorted((PLUGIN / "commands").glob("*.md"))
    readme = PLUGIN / "README.md"
    return ([readme] if readme.exists() else []) + files


def doc_markdown() -> list[Path]:
    """The user-facing docs outside the plugin: the Readme and the maintained docs/ tree.

    docs/archive/ is unmaintained by design, docs/blog/ and docs/promotion/ are dated
    writing that describes the release they shipped with.
    """
    skip = ("archive", "blog", "promotion")
    files = [REPO / "Readme.md", REPO / "TELEMETRY.md"]
    for f in sorted((REPO / "docs").rglob("*.md")):
        if f.relative_to(REPO / "docs").parts[0] not in skip:
            files.append(f)
    return [f for f in files if f.exists()]


def extract_snippets(path: Path) -> list[Snippet]:
    text = path.read_text(encoding="utf-8")
    out = []
    for m in FENCE.finditer(text):
        lang = (m.group(1) or "").lower()
        body = m.group(2)
        line = text.count("\n", 0, m.start()) + 1
        snip = Snippet(path, line, lang, body)
        first = body.splitlines()[0] if body.strip() else ""
        t = TAG.match(first)
        if t:
            snip.tag = t.group(1)
            for kv in shlex.split(t.group(2)):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    snip.attrs[k] = v
        out.append(snip)
    return out


def frontmatter(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    if not text.startswith("---\n"):
        return {}
    end = text.find("\n---", 4)
    if end < 0 or yaml is None:
        return {}
    return yaml.safe_load(text[4:end]) or {}


def released_version() -> str | None:
    for line in (REPO / "CHANGELOG.md").read_text(encoding="utf-8").splitlines():
        m = re.match(r"^## v(\d+\.\d+\.\d+)\b", line)
        if m:
            return m.group(1)
    return None


def parser_keys() -> set[str]:
    """Every key the C++ code reads from YAML/JSON config."""
    keys: set[str] = set()
    patterns = [
        r'\[\s*"([A-Za-z][A-Za-z0-9_-]*)"\s*\]',
        r'safeGet<[^>]+>\s*\([^,]+,\s*"([A-Za-z][A-Za-z0-9_-]*)"',
        r'\.has\(\s*"([A-Za-z][A-Za-z0-9_-]*)"\s*\)',
        r'\bkey\s*==\s*"([A-Za-z][A-Za-z0-9_-]*)"',
        r'(?:firstExistingKey|getFirst\w*|readAny\w*)\([^{]*\{([^}]*)\}',
        r'KnownTopLevelKeys\(\)\s*\{(.*?)return keys;',
    ]
    for src in list((REPO / "src").rglob("*.cpp")) + list((REPO / "src").rglob("*.hpp")):
        text = src.read_text(encoding="utf-8", errors="replace")
        for pat in patterns:
            for m in re.finditer(pat, text, re.S):
                chunk = m.group(1)
                if '"' in chunk:
                    keys.update(re.findall(r'"([A-Za-z][A-Za-z0-9_-]*)"', chunk))
                else:
                    keys.add(chunk)
    return keys


def cache_template_vars() -> set[str]:
    text = (REPO / "src" / "sql_template_processor.cpp").read_text(encoding="utf-8")
    return set(re.findall(r'ctx\["cache"\]\["([A-Za-z]+)"\]', text))


def forbidden_terms() -> list[tuple[str, str]]:
    items = []
    f = SUPPORT / "forbidden.txt"
    if f.exists():
        for line in f.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#"):
                term, _, why = line.partition("\t")
                items.append((term.strip(), why.strip()))
    return items


# --------------------------------------------------------------------------- checks

def check_manifests(rep: Report) -> str | None:
    try:
        market = json.loads((REPO / ".claude-plugin" / "marketplace.json").read_text())
        plugin = json.loads((PLUGIN / ".claude-plugin" / "plugin.json").read_text())
    except (OSError, json.JSONDecodeError) as exc:
        rep.fail("manifests", str(exc))
        return None
    entry = next((p for p in market.get("plugins", []) if p.get("name") == plugin.get("name")), None)
    if entry is None:
        rep.fail("marketplace.json", f"no plugin entry named {plugin.get('name')!r}")
    elif (REPO / entry.get("source", "")).resolve() != PLUGIN.resolve():
        rep.fail("marketplace.json", f"source {entry.get('source')} is not plugins/flapi")
    else:
        rep.ok("manifests")
    for mcp in [PLUGIN / ".mcp.json", PLUGIN / "hooks" / "hooks.json"]:
        if mcp.exists():
            try:
                json.loads(mcp.read_text())
                rep.ok(rel(mcp))
            except json.JSONDecodeError as exc:
                rep.fail(rel(mcp), str(exc))
    return plugin.get("version")


def check_versions(rep: Report, plugin_version: str | None):
    expected = released_version()
    if not expected:
        rep.fail("CHANGELOG.md", "no '## vX.Y.Z' release heading found")
        return
    if plugin_version != expected:
        rep.fail("plugin.json", f"version {plugin_version} != latest release v{expected} in CHANGELOG.md")
    for skill_dir in sorted((PLUGIN / "skills").iterdir()):
        skill = skill_dir / "SKILL.md"
        if not skill.exists():
            rep.fail(rel(skill_dir), "missing SKILL.md")
            continue
        fm = frontmatter(skill)
        if fm.get("name") != skill_dir.name:
            rep.fail(rel(skill), f"frontmatter name {fm.get('name')!r} != directory {skill_dir.name!r}")
        if not fm.get("description"):
            rep.fail(rel(skill), "frontmatter description missing")
        if str(fm.get("version")) != expected:
            rep.fail(rel(skill), f"frontmatter version {fm.get('version')} != v{expected}")
        if f"**flAPI:** v{expected}" not in skill.read_text(encoding="utf-8"):
            rep.fail(rel(skill), f"banner '**flAPI:** v{expected}' missing")
        rep.ok(rel(skill), "frontmatter")


def walk_keys(node, path=()):
    if isinstance(node, dict):
        for k, v in node.items():
            if not isinstance(k, str):
                continue
            yield path, k
            if k in OPAQUE_SUBTREES:
                continue
            if k in USER_NAMED_CHILDREN and isinstance(v, dict):
                for child in v.values():
                    yield from walk_keys(child, path + (k, "*"))
                continue
            yield from walk_keys(v, path + (k,))
    elif isinstance(node, list):
        for item in node:
            yield from walk_keys(item, path)


def check_yaml_keys(rep: Report, snippets: list[Snippet], known: set[str]):
    for s in snippets:
        if s.lang not in ("yaml", "yml"):
            continue
        where = f"{rel(s.path)}:{s.line}"
        # Mustache sections are not YAML; blank them before parsing.
        body = re.sub(r"\{\{[^}]*\}\}\}?", "x", s.body)
        try:
            docs = list(yaml.safe_load_all(body))
        except yaml.YAMLError as exc:
            rep.fail(where, f"yaml does not parse: {str(exc).splitlines()[0]}")
            continue
        bad = sorted({k for doc in docs for _, k in walk_keys(doc) if k not in known})
        if bad:
            rep.fail(where, f"keys no parser reads: {', '.join(bad)}")
        else:
            rep.ok(where, "yaml keys")


# A forbidden term may be named where the text says it is gone (sentences wrap,
# so the next line counts too).
GONE = ("deprecated", "no effect", "ignored", "removed", "dropped", "existed",
        "earlier version", "does not exist", "drops")


def check_forbidden(rep: Report, files: list[Path]):
    terms = forbidden_terms()
    for f in files:
        lines = f.read_text(encoding="utf-8").splitlines()
        for n, line in enumerate(lines, 1):
            context = (line + " " + (lines[n] if n < len(lines) else "")).lower()
            for term, why in terms:
                if term in line and not any(w in context for w in GONE):
                    rep.fail(f"{rel(f)}:{n}", f"'{term}' ({why})")


def check_template_vars(rep: Report, files: list[Path], allowed: set[str]):
    pat = re.compile(r"\{\{\{?\s*[#^/]?\s*cache\.([A-Za-z]+)")
    for f in files:
        lines = f.read_text(encoding="utf-8").splitlines()
        for n, line in enumerate(lines, 1):
            context = (line + " " + (lines[n] if n < len(lines) else "")).lower()
            for var in pat.findall(line):
                if var not in allowed and not any(w in context for w in GONE):
                    rep.fail(f"{rel(f)}:{n}", f"{{{{cache.{var}}}}} is not set by the template processor "
                                              f"(known: {', '.join(sorted(allowed))})")


def github_slug(heading: str, seen: dict[str, int]) -> str:
    """The anchor GitHub generates for a Markdown heading."""
    # Inline HTML goes, except inside code spans (`--to <dir>` keeps "dir").
    parts = heading.split("`")
    text = "".join(p if i % 2 else re.sub(r"<[^>]+>", "", p) for i, p in enumerate(parts))
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)   # links and images keep their text
    text = text.strip().lower()
    slug = re.sub(r"[^\w\- ]", "", text, flags=re.UNICODE).replace(" ", "-")
    n = seen.get(slug, 0)
    seen[slug] = n + 1
    return slug if n == 0 else f"{slug}-{n}"


def anchors_of(path: Path, cache: dict[Path, set[str]]) -> set[str]:
    if path not in cache:
        text = path.read_text(encoding="utf-8")
        text = FENCE.sub("", text)  # a `# comment` in a code block is not a heading
        seen: dict[str, int] = {}
        found = {github_slug(m.group(1), seen) for m in re.finditer(r"^#{1,6}\s+(.*?)\s*#*\s*$", text, re.M)}
        found |= set(re.findall(r"""<a\s+(?:name|id)=["']([^"']+)["']""", text))
        cache[path] = found
    return cache[path]


LINK = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)|<img[^>]+src=\"([^\"]+)\"|<a[^>]+href=\"([^\"]+)\"")


def check_links(rep: Report, files: list[Path]):
    """Every relative link resolves to a file, and every #anchor to a heading in it."""
    cache: dict[Path, set[str]] = {}
    for f in files:
        text = FENCE.sub(lambda m: "\n" * m.group(0).count("\n"), f.read_text(encoding="utf-8"))
        for n, line in enumerate(text.splitlines(), 1):
            for m in LINK.finditer(line):
                target = next(g for g in m.groups() if g)
                if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I):   # http:, mailto:, ...
                    continue
                path_part, _, anchor = target.partition("#")
                dest = f if not path_part else (f.parent / path_part).resolve()
                where = f"{rel(f)}:{n}"
                if not dest.exists():
                    rep.fail(where, f"link target {target} does not exist")
                    continue
                if anchor and dest.suffix == ".md" and anchor.lower() not in anchors_of(dest, cache):
                    rep.fail(where, f"anchor #{anchor} is not a heading in {rel(dest)}")
                    continue
                rep.ok(where, f"link {target}")


# ------------------------------------------------------------------ Readme Quick Start

QUICKSTART = re.compile(r"^<!--\s*flapi:\s*quickstart\s+(\w+)([^>]*)-->\s*\n```\w*\n(.*?)^```", re.S | re.M)


def run_quickstart(rep: Report, flapi: Path, flapii: list[str]):
    """Run the Readme Quick Start as a newcomer would, against the built binaries.

    Blocks are marked with an HTML comment on the line before the fence:
      <!-- flapi: quickstart run -->              run with bash, must exit 0
      <!-- flapi: quickstart serve -->            started in the background; the server must come up
      <!-- flapi: quickstart try expect="x" -->   run once the server is up; must exit 0 and print x
    `uvx --from flapi-io <cmd>` resolves to the binaries under test, and port 8080 to a free port
    (FLAPI_PORT for the server, a text substitution in the blocks).
    """
    readme = REPO / "Readme.md"
    blocks = [(m.group(1), dict(re.findall(r'(\w+)="([^"]*)"', m.group(2))), m.group(3),
               readme.read_text(encoding="utf-8").count("\n", 0, m.start()) + 1)
              for m in QUICKSTART.finditer(readme.read_text(encoding="utf-8"))]
    if not any(k == "serve" for k, *_ in blocks) or not any(k == "try" for k, *_ in blocks):
        rep.fail("Readme.md", "no runnable Quick Start (<!-- flapi: quickstart serve/try --> blocks)")
        return
    port = free_port()
    shim = (
        "set -euo pipefail\n"
        f"FLAPI_BIN={shlex.quote(str(flapi))}\n"
        f"FLAPII_BIN=({' '.join(shlex.quote(a) for a in flapii)})\n"
        'uvx() { [ "${1:-}" = --from ] && shift 2; local cmd=$1; shift\n'
        '  case "$cmd" in flapi) "$FLAPI_BIN" "$@";; flapii) "${FLAPII_BIN[@]}" "$@";;\n'
        '  *) echo "quickstart: unexpected uvx command $cmd" >&2; return 2;; esac; }\n'
    )
    # FLAPI_PORT moves a server the Readme starts on its default port.
    env = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1", "FLAPI_NO_TELEMETRY": "1", "NO_COLOR": "1",
           "FLAPI_PORT": str(port)}
    with tempfile.TemporaryDirectory(prefix="flapi-quickstart-") as tmp:
        server = None
        log = Path(tmp) / "server.log"
        try:
            for kind, attrs, body, line in blocks:
                where = f"Readme.md:{line} (quickstart {kind})"
                script = shim + body.replace("8080", str(port))
                if kind == "serve":
                    server = subprocess.Popen(["bash", "-c", script], cwd=tmp, env=env,
                                              stdout=log.open("w"), stderr=subprocess.STDOUT,
                                              start_new_session=True)
                    deadline = time.time() + 90
                    up = False
                    while time.time() < deadline and server.poll() is None:
                        try:
                            with urllib.request.urlopen(f"http://127.0.0.1:{port}/health/live", timeout=2):
                                up = True
                                break
                        except (urllib.error.URLError, OSError):
                            time.sleep(0.3)
                    if not up:
                        rep.fail(where, "server did not come up:\n" + log.read_text(errors="replace")[-1500:])
                        return
                    rep.ok(where, "server up")
                    continue
                proc = subprocess.run(["bash", "-c", script], cwd=tmp, env=env, capture_output=True,
                                      text=True, timeout=300)
                out = proc.stdout + proc.stderr
                if proc.returncode != 0:
                    rep.fail(where, f"exit {proc.returncode}:\n{out[-1500:]}")
                    return
                if "expect" in attrs and attrs["expect"] not in out:
                    rep.fail(where, f"output lacks {attrs['expect']!r}:\n{out[-1500:]}")
                    return
                rep.ok(where)
        finally:
            if server is not None:
                try:
                    os.killpg(server.pid, signal.SIGTERM)
                    server.wait(timeout=15)
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    os.killpg(server.pid, signal.SIGKILL)


# ------------------------------------------------------------------ CLI help model

class HelpModel:
    """Command tree learned from `<bin> [path] --help`."""

    def __init__(self, argv: list[str], style: str):
        self.argv = argv
        self.style = style  # "commander" | "argparse"
        self.cache: dict[tuple, tuple[set, set]] = {}

    def help(self, path: tuple) -> tuple[set, set]:
        if path in self.cache:
            return self.cache[path]
        try:
            out = subprocess.run(self.argv + list(path) + ["--help"], capture_output=True,
                                 text=True, timeout=30, env={**os.environ, "NO_COLOR": "1",
                                                             "DATAZOO_DISABLE_TELEMETRY": "1"})
            text = out.stdout + out.stderr
        except (OSError, subprocess.TimeoutExpired):
            text = ""
        subs, opts = set(), set(re.findall(r"(--[a-z][a-z0-9-]*)", text))
        section = None
        for line in text.splitlines():
            head = line.strip().lower().rstrip(":")
            if head in ("commands", "subcommands"):
                section = "cmd"
                continue
            if head.endswith("arguments") or head == "options":
                section = None
                continue
            if section == "cmd":
                m = re.match(r"^  ([a-z][a-z0-9-]*)\b", line)
                if m and m.group(1) != "help":
                    subs.add(m.group(1))
        self.cache[path] = (subs, opts)
        return subs, opts

    def check(self, tokens: list[str]) -> str | None:
        path: tuple = ()
        i = 0
        # Leading global options (`flapii -o table templates ...`, `flapi -c x.yaml pack`).
        root_opts = self.help(())[1]
        while i < len(tokens) and tokens[i].startswith("-"):
            name = tokens[i].split("=", 1)[0]
            if name.startswith("--") and name not in root_opts and name not in ("--help", "--version"):
                return f"option {name} not accepted by '<root>'"
            i += 1
            # an option value follows unless the option is a flag or uses =
            if "=" not in tokens[i - 1] and i < len(tokens) and not tokens[i].startswith("-") \
                    and tokens[i] not in self.help(())[0]:
                i += 1
        while i < len(tokens):
            tok = tokens[i]
            subs, _ = self.help(path)
            if tok.startswith("-") or tok in ("|", "&&", ";", ">", "\\"):
                break
            if tok in subs:
                path = path + (tok,)
                i += 1
                continue
            if subs and not path:
                return f"unknown command '{tok}' (known: {', '.join(sorted(subs))})"
            if subs and path and not self._is_leaf(path):
                return f"unknown subcommand '{' '.join(path)} {tok}' (known: {', '.join(sorted(subs))})"
            break
        # Global options (e.g. flapii --output) are accepted after any command.
        opts = self.help(path)[1] | self.help(())[1]
        for tok in tokens[i:]:
            if tok in ("|", "&&", ";", ">"):
                break
            if tok.startswith("--"):
                name = tok.split("=", 1)[0]
                if name not in opts and name not in ("--help", "--version"):
                    return f"option {name} not accepted by '{' '.join(path) or '<root>'}'"
        return None

    def _is_leaf(self, path: tuple) -> bool:
        return not self.help(path)[0]


def code_only(text: str) -> str:
    """Keep fenced blocks and inline `code`; blank the prose ("flapi holds the lock" is no command)."""
    out, fenced = [], False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            out.append("")
        elif fenced:
            out.append(line)
        else:
            out.append(" ".join(f"`{m}`" for m in re.findall(r"`([^`]+)`", line)))
    return "\n".join(out)


def cli_invocations(files: list[Path], binary: str, prose_ok: bool = True) -> list[tuple[Path, int, list[str]]]:
    found = []
    pat = re.compile(rf"(?:^|[\s`$(]){re.escape(binary)}\s+([^`\n]*)")
    if not prose_ok:
        # In docs a command starts a line or a code span (after an optional prompt or
        # `&&`); "a running flapi holds the lock" inside a quoted message is no call.
        pat = re.compile(rf"(?:^\s*(?:[$>]\s+)?|`|&&\s*|\|\s*|\$\()(?:\./)?{re.escape(binary)}\s+([^`\n]*)")
    for f in files:
        text = f.read_text(encoding="utf-8")
        if not prose_ok:
            text = code_only(text)
        for n, line in enumerate(text.splitlines(), 1):
            for m in pat.finditer(line):
                rest = m.group(1).split("#", 1)[0].strip()
                if not rest or rest.startswith(("is ", "binary", "server", "itself")):
                    continue
                try:
                    tokens = shlex.split(rest)
                except ValueError:
                    tokens = rest.split()
                # The command ends at the first shell operator or redirection.
                cut = []
                for t in tokens:
                    if re.match(r"^(\|\|?|&&?|;|\d?>>?.*|<.*|\)+)$", t):
                        break
                    stripped = t.rstrip(";)")
                    if stripped:
                        cut.append(stripped)
                    if t != stripped:
                        break
                tokens = cut
                # `claude mcp add ... flapi http://...` names a server, it is not a flapi call
                if re.search(r"\bmcp\s+add\b[^`\n]*$", line[:m.start() + 1]):
                    continue
                if tokens and re.match(r"^[a-z-]", tokens[0]):
                    found.append((f, n, tokens))
    return found


def check_cli(rep: Report, files: list[Path], name: str, model: HelpModel | None, prose_ok: bool = True):
    if model is None:
        return
    for f, n, tokens in cli_invocations(files, name, prose_ok):
        problem = model.check(tokens)
        where = f"{rel(f)}:{n}"
        if problem:
            rep.fail(where, f"`{name} {' '.join(tokens)}`: {problem}")
        else:
            rep.ok(where, f"{name} {' '.join(tokens[:3])}")


# ----------------------------------------------------------- executable examples

def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run_groups(rep: Report, snippets: list[Snippet], flapi: Path):
    groups: dict[str, list[Snippet]] = {}
    for s in snippets:
        if s.tag in ("config", "endpoint", "template", "file"):
            g = s.attrs.get("group")
            if not g:
                rep.fail(f"{rel(s.path)}:{s.line}", f"`flapi: {s.tag}` needs group=")
                continue
            skill = s.path.relative_to(PLUGIN).parts[1] if s.path.is_relative_to(PLUGIN / "skills") else s.path.parent.name
            groups.setdefault(f"{skill}/{g}", []).append(s)
    for name, members in sorted(groups.items()):
        run_group(rep, name, members, flapi)


def strip_tag(body: str) -> str:
    lines = body.splitlines(keepends=True)
    return "".join(lines[1:]) if lines and TAG.match(lines[0]) else body


def run_group(rep: Report, name: str, members: list[Snippet], flapi: Path):
    where = f"group {name} ({rel(members[0].path)}:{members[0].line})"
    with tempfile.TemporaryDirectory(prefix="flapi-skill-") as tmp:
        root = Path(tmp)
        shutil.copytree(SUPPORT / "fixture", root, dirs_exist_ok=True)
        # Examples use the natural relative paths ./data/customers.parquet and
        # ./data/northwind.sqlite; link instead of copying 25 MB per group.
        (root / "data").symlink_to(REPO / "examples" / "data", target_is_directory=True)
        (root / "sqls").mkdir(exist_ok=True)
        checks = []
        for s in members:
            body = strip_tag(s.body)
            if s.tag == "config":
                (root / "flapi.yaml").write_text(body)
                continue
            target = s.attrs.get("file")
            if not target:
                rep.fail(f"{rel(s.path)}:{s.line}", f"`flapi: {s.tag}` needs file=")
                continue
            base = root if s.tag == "file" else root / "sqls"
            dest = base / target
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(body)
            for line in body.splitlines():
                t = TAG.match(line)
                if t and t.group(1) == "check":
                    checks.append(t.group(2).strip())
        env = {**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1", "FLAPI_NO_TELEMETRY": "1"}
        proc = subprocess.run([str(flapi), "-c", "flapi.yaml", "--validate-config"], cwd=root,
                              capture_output=True, text=True, timeout=120, env=env)
        out = proc.stdout + proc.stderr
        # SECURITY WARNINGS goes to stderr as a banner, not a log line.
        # A group may accept specific auditor codes it demonstrates on purpose
        # (`allow=MCP_UNAUTHENTICATED_TOOLS` on any of its tags), never others.
        allowed = {c for m in members for c in m.attrs.get("allow", "").split(",") if c}
        security_codes = set(re.findall(r"^\[([A-Z_]+)\]", out, re.M))
        unexpected_security = security_codes - allowed
        log_warned = re.search(r"(?im)^\s*(⚠|warning)|\[WARNING", out)
        warned = log_warned or unexpected_security
        if proc.returncode != 0 or warned:
            rep.fail(where, "flapi --validate-config: " + " | ".join(
                l.strip() for l in out.splitlines()
                if re.search(r"(?i)error|warn|✗|⚠", l) or re.match(r"^\[[A-Z_]+\]", l))[:1500])
            return
        rep.ok(where, "validate-config")
        if checks:
            run_checks(rep, where, root, flapi, checks, env)


def run_checks(rep: Report, where: str, root: Path, flapi: Path, checks: list[str], env: dict):
    port = free_port()
    log = (root / "server.log").open("w")
    server = subprocess.Popen([str(flapi), "-c", "flapi.yaml", "-p", str(port), "--config-service",
                               "--config-service-token", "skill-check-token"],
                              cwd=root, stdout=log, stderr=subprocess.STDOUT, env=env,
                              start_new_session=True)
    base = f"http://127.0.0.1:{port}"
    try:
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(f"{base}/health/live", timeout=2):
                    break
            except (urllib.error.URLError, OSError):
                if server.poll() is not None:
                    break
                time.sleep(0.3)
        # caches warm asynchronously; give /health a chance to report ready
        for _ in range(100):
            try:
                with urllib.request.urlopen(f"{base}/health", timeout=2) as r:
                    if json.loads(r.read() or b"{}").get("status") in ("ready", "ok", "healthy"):
                        break
            except (urllib.error.URLError, OSError, ValueError):
                pass
            time.sleep(0.3)
        for check in checks:
            m = re.match(r"^(GET|POST|PUT|PATCH|DELETE)\s+(\S+)(?:\s+body=(\{.*\}))?\s*->\s*(\d{3})$", check)
            if not m:
                rep.fail(where, f"malformed check: {check!r}")
                continue
            method, url, body, expect = m.groups()
            req = urllib.request.Request(base + url, method=method,
                                         data=body.encode() if body else None,
                                         headers={"Content-Type": "application/json"} if body else {})
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    code, text = r.status, r.read().decode(errors="replace")
            except urllib.error.HTTPError as exc:
                code, text = exc.code, exc.read().decode(errors="replace")
            except (urllib.error.URLError, OSError) as exc:
                code, text = -1, str(exc)
            if str(code) != expect:
                log.flush()
                tail = (root / "server.log").read_text(errors="replace")[-800:]
                rep.fail(where, f"{method} {url} -> {code}, expected {expect}: {text[:300]}\n--- server log tail ---\n{tail}")
            else:
                rep.ok(where, f"{method} {url} -> {code}")
    finally:
        try:
            os.killpg(server.pid, signal.SIGTERM)
            server.wait(timeout=15)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            os.killpg(server.pid, signal.SIGKILL)
        log.close()


# ---------------------------------------------------------------------- main

def resolve_flapii(arg: str | None) -> list[str] | None:
    if arg:
        return shlex.split(arg)
    if os.environ.get("FLAPII_BIN"):
        return shlex.split(os.environ["FLAPII_BIN"])
    if shutil.which("flapii"):
        return ["flapii"]
    dist = REPO / "cli" / "dist" / "index.js"
    if dist.exists() and shutil.which("node"):
        return ["node", str(dist)]
    return None


def set_version(version: str) -> int:
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        print(f"--set-version: {version!r} is not X.Y.Z (no leading v)", file=sys.stderr)
        return 2
    manifest = PLUGIN / ".claude-plugin" / "plugin.json"
    data = json.loads(manifest.read_text())
    data["version"] = version
    manifest.write_text(json.dumps(data, indent=2) + "\n")
    for skill in sorted((PLUGIN / "skills").glob("*/SKILL.md")):
        text = skill.read_text(encoding="utf-8")
        text = re.sub(r"(?m)^version: .*$", f"version: {version}", text, count=1)
        text = re.sub(r"\*\*flAPI:\*\* v[0-9.]+", f"**flAPI:** v{version}", text)
        skill.write_text(text, encoding="utf-8")
    print(f"stamped plugin version {version}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--flapi", default=str(REPO / "build" / "release" / "flapi"))
    ap.add_argument("--flapii", default=None, help="command for the flapii CLI (default: PATH or cli/dist)")
    ap.add_argument("--static-only", action="store_true", help="skip everything that needs a binary")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--set-version", metavar="X.Y.Z",
                    help="release prep: stamp plugin.json, SKILL.md versions and banners, then exit")
    args = ap.parse_args()

    if args.set_version:
        return set_version(args.set_version)

    if yaml is None:
        print("check_skills: PyYAML is required (pip install pyyaml)", file=sys.stderr)
        return 2

    rep = Report(verbose=args.verbose)
    files = plugin_markdown()
    snippets = [s for f in files for s in extract_snippets(f)]

    version = check_manifests(rep)
    check_versions(rep, version)
    check_yaml_keys(rep, snippets, parser_keys())
    check_forbidden(rep, files)
    check_template_vars(rep, files, cache_template_vars())

    # The Readme and docs/ teach the same facts, so they get the same checks,
    # minus the runnable groups (their examples are not tagged).
    docs = doc_markdown()
    check_forbidden(rep, docs)
    check_template_vars(rep, docs, cache_template_vars())
    check_links(rep, docs + files)

    if not args.static_only:
        # Resolved now: groups run with cwd set to a temp project directory.
        flapi = Path(args.flapi).resolve()
        if not flapi.exists():
            rep.fail("flapi", f"{flapi} not found; build it (make release) or pass --static-only")
        else:
            flapi_help = HelpModel([str(flapi)], "argparse")
            check_cli(rep, files, "flapi", flapi_help)
            check_cli(rep, docs, "flapi", flapi_help, prose_ok=False)
            run_groups(rep, snippets, flapi)
        flapii = resolve_flapii(args.flapii)
        if flapii is None:
            rep.fail("flapii", "CLI not found (PATH, FLAPII_BIN, or cli/dist after `npm run build`)")
        else:
            flapii_help = HelpModel(flapii, "commander")
            check_cli(rep, files, "flapii", flapii_help)
            check_cli(rep, docs, "flapii", flapii_help, prose_ok=False)
            if flapi.exists():
                run_quickstart(rep, flapi, flapii)

    # Claude Code's own validator, when the CLI is installed (it is not in CI).
    if not args.static_only and shutil.which("claude"):
        for target in (PLUGIN, REPO):
            proc = subprocess.run(["claude", "plugin", "validate", str(target)], capture_output=True,
                                  text=True, timeout=120)
            if proc.returncode != 0 or "Validation passed" not in proc.stdout + proc.stderr:
                rep.fail(f"claude plugin validate {rel(target) or '.'}", (proc.stdout + proc.stderr)[-1500:])
            else:
                rep.ok(f"claude plugin validate {rel(target) or '.'}")

    for line in rep.failures:
        print(line)
    print(f"check_skills: {rep.passes} passed, {len(rep.failures)} failed "
          f"({len(files)} plugin files, {len(docs)} doc files, {len(snippets)} snippets)")
    return 1 if rep.failures else 0


if __name__ == "__main__":
    sys.exit(main())
