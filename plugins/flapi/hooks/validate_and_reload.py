#!/usr/bin/env python3
"""flAPI plugin hook: validate config after Claude edits it, hot-reload, render.

PostToolUse on Edit / Write / MultiEdit. For an edited *.yaml / *.yml / *.sql
file that belongs to a flAPI project (a flapi.yaml in it or an ancestor dir):

1. If `flapi` is on PATH (or FLAPI_BIN is set), run
   `flapi -c <flapi.yaml> --validate-config`. On failure, the output goes back
   to Claude (exit 2 on PostToolUse feeds stderr to the model; the edit itself
   has already happened, so nothing is blocked). SECURITY WARNINGS it prints
   are passed on even when validation passes.
2. flapi.yaml itself is never hot-reloaded: the hook says to restart the
   server. A file outside template.path that is not an endpoint YAML (e.g. an
   {{include}} fragment elsewhere) is validated only.
3. If a config-service server is reachable (FLAPI_URL, default
   http://localhost:8080, + FLAPI_CONFIG_SERVICE_TOKEN):
   - an endpoint YAML is reloaded BY FILE
     (POST /api/v1/_config/endpoints/reload-file), which also loads a NEW file
     and addresses exactly the endpoint(s) the file declares, even when
     another file shares the url-path (GET + POST /orders);
   - a .sql file is looked up with POST .../endpoints/by-template (template-source
     and cache template-file), and each declaring YAML is reloaded by file;
   - the affected templates are then rendered: the serving template with
     ?validate_only (render + EXPLAIN), a cache template-file with
     ?target=cache; render and SQL errors are reported.
   A rejected token (HTTP 401/403) is reported; no server is a silent no-op.

Every other precondition that is not met (no flapi binary, no project, no
token) is a silent no-op: the hook must never get in the way of editing.
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
import urllib.parse
import urllib.request
from pathlib import Path

EXTENSIONS = {".yaml", ".yml", ".sql"}
TIMEOUT_S = 20
ENDPOINT_KEYS = re.compile(r"^(url-path|mcp-tool|mcp-resource|mcp-prompt):", re.MULTILINE)


class TokenRejected(Exception):
    """The config service answered 401/403: the token is missing or wrong."""


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


def template_dir(cfg: Path) -> Path:
    """template.path of flapi.yaml (default ./sqls), without a YAML library."""
    try:
        text = cfg.read_text(encoding="utf-8")
    except OSError:
        text = ""
    match = re.search(r"^template:\s*\n((?:[ \t]+.*\n?)*)", text, re.MULTILINE)
    value = "sqls"
    if match:
        inner = re.search(r"^[ \t]+path:\s*['\"]?([^'\"#\n]+?)['\"]?\s*(#.*)?$", match.group(1), re.MULTILINE)
        if inner:
            value = inner.group(1).strip()
    path = Path(value)
    return (path if path.is_absolute() else cfg.parent / path).resolve()


def is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root)
        return True
    except ValueError:
        return False


def path_to_slug(url_path: str) -> str:
    """Mirror of cli/shared/src/lib/url.ts pathToSlug."""
    out = []
    for ch in url_path:
        out.append({"/": "-", "-": "~1", "~": "~0"}.get(ch, ch))
    return "".join(out) or "empty"


def key_to_url(key: str, suffix: str) -> str:
    """Config-service URL for an endpoint key ("GET /orders" or an MCP name)."""
    method, _, path = key.partition(" ")
    if path.startswith("/"):
        slug = urllib.parse.quote(path_to_slug(path), safe=":~")
        return f"/api/v1/_config/endpoints/{slug}/{suffix}", f"method={method}"
    return f"/api/v1/_config/endpoints/{urllib.parse.quote(key, safe='')}/{suffix}", ""


def validate(flapi: str, cfg: Path) -> tuple[bool, str, list[str]]:
    """(passed, failure detail, security warning lines)."""
    try:
        proc = subprocess.run(
            [flapi, "-c", str(cfg), "--validate-config", "--no-telemetry"],
            capture_output=True, text=True, timeout=TIMEOUT_S, cwd=str(cfg.parent),
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return True, f"(flapi validation skipped: {exc})", []
    output = (proc.stdout + "\n" + proc.stderr).strip()
    security: list[str] = []
    in_security = False
    for line in output.splitlines():
        if line.startswith("SECURITY WARNINGS"):
            in_security = True
            continue
        if in_security:
            if line.startswith("["):
                security.append(line.strip())
            elif line.strip() and not set(line.strip()) <= {"="}:
                in_security = False
    # Only what explains the failure: INFO lines bury it.
    relevant = [line for line in output.splitlines()
                if re.search(r"ERROR|WARN|✗|[Ee]rror|Missing|Invalid|\[[A-Z_]+\]", line)]
    return proc.returncode == 0, "\n".join(relevant[-40:]) or output[-2000:], security


def request(method: str, url: str, token: str, body: dict | None = None, ok_codes=(200,)):
    """JSON answer for an ok status; HTTPError otherwise; TokenRejected on 401/403."""
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("X-Config-Token", token)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read().decode() or "{}"
            return resp.status, (json.loads(raw) if raw.strip().startswith(("{", "[")) else {})
    except urllib.error.HTTPError as exc:
        if exc.code in (401, 403):
            raise TokenRejected(f"HTTP {exc.code}") from exc
        if exc.code in ok_codes:
            raw = exc.read().decode(errors="replace") or "{}"
            try:
                return exc.code, json.loads(raw)
            except ValueError:
                return exc.code, {"error": raw}
        raise


def error_text(exc: urllib.error.HTTPError) -> str:
    raw = exc.read().decode(errors="replace")
    try:
        body = json.loads(raw)
        return str(body.get("error") or body.get("reason") or body.get("message") or raw)[:400]
    except ValueError:
        return raw[:400]


def reload_file(base: str, token: str, yaml_file: Path) -> tuple[list[str], str | None]:
    """(endpoint keys reloaded, error or None). A non-endpoint YAML is ([], None)."""
    try:
        status, body = request("POST", f"{base}/api/v1/_config/endpoints/reload-file", token,
                               {"file": str(yaml_file.resolve())}, ok_codes=(200, 201, 400, 409))
    except urllib.error.HTTPError as exc:
        return [], f"{yaml_file.name} (HTTP {exc.code}: {error_text(exc)})"
    if body.get("success"):
        return [str(k) for k in body.get("endpoints", [])], None
    message = str(body.get("error", ""))
    if "is not an endpoint file" in message:
        return [], None   # a shared fragment in template.path: nothing to reload
    return [], f"{yaml_file.name} (HTTP {status}: {message[:400]})"


def render(base: str, token: str, key: str, cache: bool) -> str | None:
    """Render one endpoint's template (serving: render + EXPLAIN; cache: render). Error or None."""
    path, method_query = key_to_url(key, "template/expand")
    query = "&".join(q for q in (method_query, "target=cache" if cache else "validate_only=1") if q)
    what = f"{key} ({'cache template-file' if cache else 'template'})"
    try:
        status, body = request("POST", f"{base}{path}?{query}", token, {"parameters": {}}, ok_codes=(200, 400))
    except urllib.error.HTTPError as exc:
        return f"{what}: HTTP {exc.code}: {error_text(exc)}"
    if status != 200:
        return f"{what}: {body.get('error') or body}"
    if not cache and body.get("valid") is False:
        errors = "; ".join(str(e.get("message", e)) for e in body.get("errors", []))
        return f"{what}: {errors}"
    if cache and body.get("watermark_dropped_reason"):
        return f"{what}: warning: {body['watermark_dropped_reason']}"
    return None


def has_cache_template(yaml_file: Path) -> bool:
    try:
        return "template-file:" in yaml_file.read_text(encoding="utf-8")
    except OSError:
        return False


def emit(message: str) -> None:
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PostToolUse",
                                             "additionalContext": message}}))


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

    notes: list[str] = []
    flapi = os.environ.get("FLAPI_BIN") or shutil.which("flapi")
    if flapi:
        ok, output, security = validate(flapi, cfg)
        if not ok:
            print(f"flapi --validate-config failed for {cfg} after editing {edited.name}:\n{output}",
                  file=sys.stderr)
            return 2
        if security:
            notes.append("flapi --validate-config SECURITY WARNINGS:\n" + "\n".join(security))

    # flapi.yaml is read once at startup: there is nothing to hot-reload.
    if edited.resolve() == cfg.resolve():
        notes.append(f"flapi: {edited.name} changed - restart the server to apply it "
                     "(the main configuration is not hot-reloaded).")
        emit("\n".join(notes))
        return 0

    templates = template_dir(cfg)
    try:
        text = edited.read_text(encoding="utf-8") if edited.suffix != ".sql" else ""
    except OSError:
        text = ""
    is_endpoint_yaml = edited.suffix != ".sql" and bool(ENDPOINT_KEYS.search(text))
    if not is_within(edited, templates):
        if is_endpoint_yaml:
            notes.append(f"flapi: {edited.name} is outside template.path ({templates}); "
                         "the server only loads endpoint YAMLs from there.")
        if notes:
            emit("\n".join(notes))
        return 0

    token = os.environ.get("FLAPI_CONFIG_SERVICE_TOKEN")
    base = (os.environ.get("FLAPI_URL") or os.environ.get("FLAPI_BASE_URL") or "http://localhost:8080").rstrip("/")
    if not token:
        if notes:
            emit("\n".join(notes))
        return 0

    reloaded: list[str] = []
    failed: list[str] = []
    to_render: list[tuple[str, bool]] = []
    try:
        if edited.suffix == ".sql":
            _, found = request("POST", f"{base}/api/v1/_config/endpoints/by-template", token,
                               {"template_path": str(edited.resolve())})
            files: dict[str, set[str]] = {}
            for ep in found.get("endpoints", []):
                roles = ep.get("roles") or [ep.get("role", "template-source")]
                if ep.get("config_file_path"):
                    files.setdefault(ep["config_file_path"], set()).update(roles)
            for yaml_file, roles in sorted(files.items()):
                keys, error = reload_file(base, token, Path(yaml_file))
                if error:
                    failed.append(error)
                for key in keys:
                    reloaded.append(key)
                    to_render.extend((key, role == "cache.template-file") for role in sorted(roles))
        else:
            keys, error = reload_file(base, token, edited)
            if error:
                failed.append(error)
            for key in keys:
                reloaded.append(key)
                to_render.append((key, False))
                if has_cache_template(edited):
                    to_render.append((key, True))
        render_errors = [e for e in (render(base, token, k, c) for k, c in to_render) if e]
    except TokenRejected as exc:
        print(f"flapi: the config service at {base} rejected FLAPI_CONFIG_SERVICE_TOKEN ({exc}); "
              f"{edited.name} was NOT reloaded. Set it to the server's --config-service-token.",
              file=sys.stderr)
        return 2
    except (urllib.error.URLError, OSError, ValueError):
        # no server running: nothing to reload
        if notes:
            emit("\n".join(notes))
        return 0

    if failed or render_errors:
        lines = []
        if failed:
            lines.append("flapi hot-reload failed for: " + "; ".join(failed))
        if render_errors:
            lines.append("flapi: reloaded, but rendering failed:\n  " + "\n  ".join(render_errors))
        if notes:
            lines.extend(notes)
        print("\n".join(lines), file=sys.stderr)
        return 2
    if reloaded:
        notes.insert(0, f"flapi: validated, hot-reloaded and rendered {', '.join(reloaded)} on {base}.")
    elif edited.suffix == ".sql":
        notes.insert(0, f"flapi: config valid; no loaded endpoint uses {edited.name}.")
    elif is_endpoint_yaml:
        notes.insert(0, f"flapi: config valid; {edited.name} declares no endpoint the server loaded.")
    if notes:
        emit("\n".join(notes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
