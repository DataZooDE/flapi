#!/usr/bin/env python3
"""flAPI plugin hook: validate config after Claude edits it, then hot-reload.

PostToolUse on Edit / Write / MultiEdit. For an edited *.yaml / *.yml / *.sql
file that belongs to a flAPI project (a flapi.yaml in it or an ancestor dir):

1. If `flapi` is on PATH (or FLAPI_BIN is set), run
   `flapi -c <flapi.yaml> --validate-config`. On failure, the output goes back
   to Claude (exit 2 on PostToolUse feeds stderr to the model; the edit itself
   has already happened, so nothing is blocked).
2. If validation passed and a config-service server is reachable
   (FLAPI_URL, default http://localhost:8080, + FLAPI_CONFIG_SERVICE_TOKEN),
   reload every endpoint that uses the edited file via
   POST /api/v1/_config/endpoints/{slug}/reload.

Every precondition that is not met (no flapi binary, no project, no token, no
server) is a silent no-op: the hook must never get in the way of editing.
Only the standard library is used.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

EXTENSIONS = {".yaml", ".yml", ".sql"}
TIMEOUT_S = 20


def find_project_config(path: Path) -> Path | None:
    """Nearest flapi.yaml at or above the edited file (or FLAPI_CONFIG)."""
    env_cfg = os.environ.get("FLAPI_CONFIG")
    if env_cfg and Path(env_cfg).is_file():
        cfg = Path(env_cfg).resolve()
        try:
            path.resolve().relative_to(cfg.parent)
            return cfg
        except ValueError:
            pass
    for parent in [path.parent, *path.parent.parents]:
        candidate = parent / "flapi.yaml"
        if candidate.is_file():
            return candidate
    return None


def path_to_slug(url_path: str) -> str:
    """Mirror of cli/shared/src/lib/url.ts pathToSlug."""
    out = []
    for ch in url_path:
        out.append({"/": "-", "-": "~1", "~": "~0"}.get(ch, ch))
    return "".join(out) or "_"


def validate(flapi: str, cfg: Path) -> tuple[bool, str]:
    try:
        proc = subprocess.run(
            [flapi, "-c", str(cfg), "--validate-config", "--no-telemetry"],
            capture_output=True, text=True, timeout=TIMEOUT_S, cwd=str(cfg.parent),
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return True, f"(flapi validation skipped: {exc})"
    output = (proc.stdout + proc.stderr).strip()
    # Only what explains the failure: INFO lines bury it.
    relevant = [line for line in output.splitlines()
                if re.search(r"ERROR|WARN|✗|[Ee]rror|Missing|Invalid|\[[A-Z_]+\]", line)]
    return proc.returncode == 0, "\n".join(relevant[-40:]) or output[-2000:]


def request(method: str, url: str, token: str, body: dict | None = None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=5) as resp:
        raw = resp.read().decode() or "{}"
        return json.loads(raw) if raw.strip().startswith(("{", "[")) else {}


def yaml_identifier(edited: Path) -> tuple[str | None, str | None]:
    """(identifier, slug) of an endpoint YAML without a YAML library.

    REST endpoints are addressed by `url-path` (slug: `/` -> `-`); MCP-only
    tools, resources and prompts by the `name:` under their `mcp-*:` block,
    which the config service takes as-is.
    """
    try:
        lines = edited.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None, None
    for line in lines:
        match = re.match(r"^url-path:\s*['\"]?([^'\"#\s]+)", line)
        if match:
            return match.group(1), path_to_slug(match.group(1))
    in_mcp = False
    for line in lines:
        if re.match(r"^mcp-(tool|resource|prompt):", line):
            in_mcp = True
            continue
        if in_mcp:
            if line and not line[0].isspace():
                in_mcp = False
                continue
            match = re.match(r"^\s+name:\s*['\"]?([^'\"#\s]+)", line)
            if match:
                return match.group(1), match.group(1)
    return None, None


def endpoints_for_file(base: str, token: str, edited: Path) -> list[tuple[str, str]]:
    """(identifier, slug) of loaded endpoints that use the edited YAML or SQL template."""
    if edited.suffix == ".sql":
        found = request("POST", f"{base}/api/v1/_config/endpoints/by-template", token,
                        {"template_path": str(edited)})
        out = set()
        for ep in found.get("endpoints", []):
            if ep.get("url_path"):
                out.add((ep["url_path"], path_to_slug(ep["url_path"])))
            elif ep.get("mcp_name"):
                out.add((ep["mcp_name"], ep["mcp_name"]))
        return sorted(out)
    ident, slug = yaml_identifier(edited)
    return [(ident, slug)] if ident else []


def main() -> int:
    try:
        event = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    tool_input = event.get("tool_input") or {}
    file_path = tool_input.get("file_path") or tool_input.get("path")
    if not file_path:
        return 0
    edited = Path(file_path)
    if edited.suffix.lower() not in EXTENSIONS or not edited.exists():
        return 0
    cfg = find_project_config(edited)
    if cfg is None:
        return 0

    flapi = os.environ.get("FLAPI_BIN") or shutil.which("flapi")
    if flapi:
        ok, output = validate(flapi, cfg)
        if not ok:
            print(f"flapi --validate-config failed for {cfg} after editing {edited.name}:\n{output}",
                  file=sys.stderr)
            return 2

    token = os.environ.get("FLAPI_CONFIG_SERVICE_TOKEN")
    if not token:
        return 0
    base = (os.environ.get("FLAPI_URL") or os.environ.get("FLAPI_BASE_URL") or "http://localhost:8080").rstrip("/")
    try:
        targets = endpoints_for_file(base, token, edited)
    except (urllib.error.URLError, OSError, ValueError):
        return 0  # no server running: nothing to reload

    reloaded, failed = [], []
    for url_path, slug in targets:
        try:
            request("POST", f"{base}/api/v1/_config/endpoints/{slug}/reload", token)
            reloaded.append(url_path)
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                continue  # a new endpoint file: reported below as not loaded
            failed.append(f"{url_path} (HTTP {exc.code}: {exc.read().decode(errors='replace')[:300]})")
        except (urllib.error.URLError, OSError) as exc:
            failed.append(f"{url_path} ({exc})")

    if failed:
        print("flapi hot-reload failed for: " + "; ".join(failed), file=sys.stderr)
        return 2
    if reloaded:
        message = f"flapi: validated and hot-reloaded {', '.join(reloaded)} on {base}."
    elif edited.suffix != ".sql":
        message = (f"flapi: config valid. {edited.name} is not a loaded endpoint on {base}; "
                   "a new endpoint file needs `flapii endpoints create` or a server restart.")
    else:
        return 0
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse",
                                             "additionalContext": message}}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
