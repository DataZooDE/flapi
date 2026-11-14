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
   - the affected templates are then VALIDATED, never run: the serving
     template with ?validate_only=1 - once with no parameters and once with a
     sample value for every typed request field, so SQL inside
     {{#params.x}} sections is checked too - and a cache template-file with
     ?target=cache&validate_only=1 (rendered with the live watermark, parsed
     per statement, bare-SELECT rule, bound in a rolled-back transaction).
     A parameter used inside `AT (...)` (time travel) gets no sample value:
     an invented version or timestamp does not exist. Parse/bind errors are
     reported (exit 2); a cache whose table is not built yet (seen on the
     no-parameter run) is a note, not a failure.
   Before the token is sent anywhere, the server must be THIS project's: its
   OpenAPI title (GET /doc.yaml, no token) must be this flapi.yaml's
   project-name, and then its template.path this project's. Otherwise the hook
   names the URL it found and stops.
   If flapi.yaml changed since the server started (its mtime is newer than the
   server's uptime) and its ducklake/auth/mcp settings differ from what the
   server reports, the hook says "restart the server first" instead of
   reloading endpoints into a server running the old main configuration.
   A server running from a packed bundle (`bundled` in /api/v1/_config/project)
   cannot take edits from disk: the hook says so instead of reloading.
   A rejected token (HTTP 401/403) is reported; no server is a silent no-op.
4. After a Bash command that deleted an endpoint YAML (rm, git rm, unlink, mv
   away), or an edited YAML that no longer exists, the file is passed to
   reload-file, which unloads what it declared (200 {"status":"removed",
   "removed":[...]}); the removed endpoints are reported. 404 means nothing was
   loaded from it (an older server, which cannot unload, answers 404 too).

FLAPI_CONFIG names the project when the edited file is under its directory OR
its template.path (a writer and a replica sharing ../sqls).

Every other precondition that is not met (no flapi binary, no project, no
token) is a silent no-op: the hook must never get in the way of editing.
Only the standard library is used.
"""
from __future__ import annotations

import datetime
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
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
        # Under the config's directory, or under its template.path even when
        # that lives elsewhere (template: {path: ../shared/sqls}).
        if is_within(path, cfg.parent) or is_within(path, template_dir(cfg)):
            return cfg
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


def key_to_url(key: str, suffix: str) -> tuple[str, str]:
    """Config-service URL for an endpoint key ("GET /orders" or an MCP name)."""
    method, _, path = key.partition(" ")
    tail = f"/{suffix}" if suffix else ""
    if path.startswith("/"):
        slug = urllib.parse.quote(path_to_slug(path), safe=":~")
        return f"/api/v1/_config/endpoints/{slug}{tail}", f"method={method}"
    return f"/api/v1/_config/endpoints/{urllib.parse.quote(key, safe='')}{tail}", ""


def url_path_of(key: str) -> str:
    """What `flapii cache refresh` takes: the url-path, or the MCP name."""
    _, _, path = key.partition(" ")
    return path if path.startswith("/") else key


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


_SAMPLE_STRINGS = ["x", "a", "abc", "test", "A", "ABC", "1", "123", "x1", "a_b", "ALFKI", "2024-01-01",
                   "a@example.com", "00000000-0000-0000-0000-000000000000"]


def _number(value, fallback):
    """A validator bound, unless it is unset or the int-range sentinel."""
    try:
        number = float(value)
    except (TypeError, ValueError):
        return fallback
    return fallback if abs(number) >= 2**31 - 1 else number


def sample_value(field: dict):
    """A value the field's first typed validator accepts, or None (untyped)."""
    for v in field.get("validators") or []:
        kind = str(v.get("type", "")).lower()
        if kind in ("int", "integer"):
            return str(int(_number(v.get("min"), 1)))
        if kind in ("number", "float", "double"):
            return str(_number(v.get("min"), 1.0))
        if kind == "enum":
            values = v.get("allowedValues") or v.get("values") or []
            return str(values[0]) if values else None
        if kind == "date":
            return str(v.get("min") or datetime.date.today().isoformat())
        if kind == "time":
            return str(v.get("min") or "12:00:00")
        if kind in ("boolean", "bool"):
            return "true"
        if kind == "email":
            return "a@example.com"
        if kind == "uuid":
            return "00000000-0000-0000-0000-000000000000"
        if kind == "string":
            min_len = int(_number(v.get("min-length"), 0))
            pattern = v.get("regex") or ""
            for candidate in _SAMPLE_STRINGS:
                value = candidate * max(1, -(-min_len // len(candidate)))
                try:
                    if not pattern or re.fullmatch(pattern, value):
                        return value
                except re.error:
                    break
            return "x" * max(1, min_len)
    return None


_AT_CLAUSE = re.compile(r"\bAT\s*\(((?:[^()]|\([^()]*\))*)\)", re.IGNORECASE)
_PARAM_REF = re.compile(r"params\.([A-Za-z_][A-Za-z0-9_]*)")


def time_travel_params(sql: str) -> set[str]:
    """Names of the params used inside `AT (VERSION => ...)` / `AT (TIMESTAMP => ...)`."""
    names: set[str] = set()
    for clause in _AT_CLAUSE.finditer(sql):
        names.update(_PARAM_REF.findall(clause.group(1)))
    return names


def template_text(templates: Path | None, source: str) -> str:
    if not source:
        return ""
    path = Path(source)
    if not path.is_absolute() and templates is not None:
        path = templates / path
    try:
        return path.read_text(encoding="utf-8")
    except OSError:
        return ""


def sample_params(base: str, token: str, key: str, templates: Path | None = None) -> dict[str, str]:
    """A sample value for each declared typed request field ({} if none or unknown).

    A field used inside AT (...) gets none: a made-up snapshot version or
    timestamp names a snapshot that does not exist, and the resulting error
    says nothing about the edit.
    """
    path, method_query = key_to_url(key, "")
    try:
        _, body = request("GET", f"{base}{path}" + (f"?{method_query}" if method_query else ""), token)
    except urllib.error.HTTPError:
        return {}
    skip = time_travel_params(template_text(templates, str(body.get("template-source") or "")))
    samples = {}
    for field in body.get("request") or []:
        name = field.get("field-name")
        if not name or str(name) in skip:
            continue
        value = sample_value(field)
        if value is not None:
            samples[str(name)] = value
    return samples


def render(base: str, token: str, key: str, cache: bool,
           templates: Path | None = None) -> tuple[list[str], list[str]]:
    """Validate one endpoint's template without running it: (errors, notes)."""
    path, method_query = key_to_url(key, "template/expand")
    query = "&".join(q for q in (method_query, "target=cache" if cache else "", "validate_only=1") if q)
    what = f"{key} ({'cache template-file' if cache else 'template'})"
    param_sets: list[tuple[str, dict]] = [("no params", {})]
    if not cache:
        samples = sample_params(base, token, key, templates)
        if samples:
            param_sets.append(("sample params " + ", ".join(f"{k}={v}" for k, v in sorted(samples.items())),
                               samples))
    errors: list[str] = []
    notes: list[str] = []
    not_built = False
    for label, params in param_sets:
        try:
            status, body = request("POST", f"{base}{path}?{query}", token, {"parameters": params},
                                   ok_codes=(200, 400))
        except urllib.error.HTTPError as exc:
            errors.append(f"{what}: HTTP {exc.code}: {error_text(exc)}")
            break
        if status != 200:
            errors.append(f"{what}: {body.get('error') or body}")
            break
        first_run = label == "no params"
        for warning in body.get("warnings") or []:
            kind = warning.get("type")
            if kind == "cache_not_built":
                # Only the no-parameter run decides "not built"; a sample run
                # failing is about the sample, never evidence of that.
                not_built = not_built or first_run
            elif kind == "watermark_dropped" and first_run:
                notes.append(f"{what}: warning: {warning.get('message')} - the next refresh would load "
                             "the full source")
            elif kind == "not_checked" and first_run:
                notes.append(f"{what}: warning: {warning.get('message')}")
        if body.get("valid") is False:
            problems = []
            for e in body.get("errors") or []:
                message = str(e.get("message", e))
                missing_cache = (re.search(r"Table with name \S+ does not exist", message)
                                 and "cache" in message.lower())
                # An older server reports a cache that is not built as an error.
                if missing_cache and first_run:
                    not_built = True
                    continue
                # The same missing table again on a sample run: already noted.
                if missing_cache and not_built:
                    continue
                problems.append(message)
            if problems:
                errors.append(f"{what} [{label}]: " + "; ".join(problems))
    if not_built:
        notes.append(f"{what}: cache not built yet; run `flapii cache refresh {url_path_of(key)}`")
    if not errors and not cache:
        notes.append(f"{what}: checked with " + " and ".join(label for label, _ in param_sets))
    return errors, notes


def _scalar(value) -> str:
    """A YAML / JSON scalar normalised for comparison."""
    if isinstance(value, bool):
        return "true" if value else "false"
    text = str(value).strip().strip("'\"").strip()
    lowered = text.lower()
    if lowered in ("true", "yes", "on"):
        return "true"
    if lowered in ("false", "no", "off"):
        return "false"
    return text


def project_name(cfg: Path) -> str | None:
    """project-name of flapi.yaml, without a YAML library."""
    try:
        text = cfg.read_text(encoding="utf-8")
    except OSError:
        return None
    match = re.search(r"^project-name:\s*(.+?)\s*(?:#.*)?$", text, re.MULTILINE)
    return _scalar(match.group(1)) if match else None


def top_level_scalars(cfg: Path, sections: tuple[str, ...]) -> dict[str, dict[str, str]]:
    """The scalar keys directly under each named top-level block of flapi.yaml."""
    try:
        lines = cfg.read_text(encoding="utf-8").splitlines()
    except OSError:
        return {}
    out: dict[str, dict[str, str]] = {}
    current: str | None = None
    child_indent: int | None = None
    for line in lines:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())
        if indent == 0:
            match = re.match(r"^([A-Za-z_-]+):\s*(?:#.*)?$", line)
            current = match.group(1) if match and match.group(1) in sections else None
            child_indent = None
            if current:
                out.setdefault(current, {})
            continue
        if current is None:
            continue
        if child_indent is None:
            child_indent = indent
        if indent != child_indent:
            continue
        match = re.match(r"^\s*([A-Za-z_-]+):\s*(.*?)\s*(?:#.*)?$", line)
        if match and match.group(2):
            out[current][match.group(1)] = match.group(2)
    return out


def _get_json_no_token(url: str):
    """(status, body) of a GET that carries NO credential; None if unreadable."""
    try:
        with urllib.request.urlopen(urllib.request.Request(url, method="GET"), timeout=10) as resp:
            return resp.status, resp.read().decode(errors="replace")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode(errors="replace")


def wrong_server(base: str, cfg: Path) -> str | None:
    """Why the server at `base` is not this project's - checked WITHOUT the token.

    The token is the project's config-service secret; a server on the default
    port that belongs to another project must not receive it. GET /doc.yaml
    needs no credential and names the served project (OpenAPI info.title).
    """
    wanted = project_name(cfg)
    if not wanted:
        return None
    status, body = _get_json_no_token(f"{base}/doc.yaml")
    if status != 200:
        return None
    match = re.search(r"^info:\s*\n(?:[ \t]+.*\n)*?[ \t]+title:\s*(.+?)\s*$", body, re.MULTILINE)
    if not match:
        return None
    served = _scalar(match.group(1))
    if served != wanted:
        return (f"flapi: the server at {base} serves project '{served}', not this project's "
                f"'{wanted}' ({cfg}); nothing was sent to it (the config-service token included). "
                "Point FLAPI_URL at this project's server.")
    return None


def main_config_stale(base: str, cfg: Path, project: dict) -> str | None:
    """'restart first' when flapi.yaml changed since the server started and differs from it."""
    try:
        status, body = _get_json_no_token(f"{base}/health")
        uptime = float(json.loads(body).get("uptime_s")) if status in (200, 503) else None
    except (ValueError, TypeError, AttributeError, urllib.error.URLError, OSError):
        uptime = None
    try:
        mtime = cfg.stat().st_mtime
    except OSError:
        return None
    if uptime is not None and mtime <= time.time() - uptime + 1:
        return None   # not touched since the server started
    compared = {"ducklake": ("enabled", "alias", "read-only"), "auth": ("enabled",), "mcp": ("enabled",)}
    in_file = top_level_scalars(cfg, tuple(compared))
    diffs = []
    for section, keys in compared.items():
        served = project.get(section)
        if not isinstance(served, dict):
            continue
        for key in keys:
            if key not in served or key not in in_file.get(section, {}):
                continue
            mine = in_file[section][key]
            if "{{" in mine:
                continue   # interpolated: the server's resolved value cannot be compared
            if _scalar(mine) != _scalar(served[key]):
                diffs.append(f"{section}.{key} is {_scalar(mine)} in {cfg.name}, {_scalar(served[key])} "
                             "on the server")
    if not diffs:
        return None
    return (f"flapi: {cfg.name} changed since the server at {base} started ({'; '.join(diffs)}). "
            "Restart the server first; nothing was reloaded into a server running the old main "
            "configuration.")


def not_this_project(base: str, token: str, templates: Path, cfg: Path | None = None) -> str | None:
    """Why the server cannot take this edit (packed bundle, other project, stale), or None."""
    try:
        _, project = request("GET", f"{base}/api/v1/_config/project", token)
    except urllib.error.HTTPError:
        return None
    if project.get("bundled"):
        return (f"flapi: the server at {base} runs from a packed bundle; edits on disk cannot be "
                "hot-reloaded into it. Repack (`flapi pack`) and redeploy, or run the unpacked project "
                "to develop against.")
    served = str(project.get("template-path") or "")
    if served and Path(served).is_absolute():
        try:
            same = Path(served).resolve() == templates
        except OSError:
            same = True
        if not same:
            return (f"flapi: the server at {base} serves template.path {served}, not this project's "
                    f"{templates}; nothing was reloaded. Point FLAPI_URL at this project's server.")
    if cfg is not None:
        return main_config_stale(base, cfg, project)
    return None


DELETING_COMMANDS = {"rm", "unlink", "trash", "mv"}


def deleted_yaml_files(command: str, cwd: Path) -> list[Path]:
    """YAML files a shell command removed (rm / git rm / unlink / trash / mv away) that are gone now."""
    try:
        lexer = shlex.shlex(command, posix=True, punctuation_chars=";&|")
        lexer.whitespace_split = True
        tokens = list(lexer)
    except ValueError:
        return []
    found: list[Path] = []
    segment: list[str] = []
    for token in tokens + [";"]:
        if token and set(token) <= set(";&|"):
            words = [w for w in segment if "=" not in w or w.startswith(("-", "/", "."))]
            if words and words[0] == "git" and len(words) > 1 and words[1] in ("rm", "mv"):
                words = words[1:]
            if words and Path(words[0]).name in DELETING_COMMANDS:
                operands = [w for w in words[1:] if not w.startswith("-")]
                if Path(words[0]).name == "mv":
                    operands = operands[:-1]   # the destination is not deleted
                for operand in operands:
                    if operand.lower().endswith((".yaml", ".yml")) and not any(c in operand for c in "*?["):
                        path = Path(operand) if Path(operand).is_absolute() else cwd / operand
                        if not path.exists():
                            found.append(path)
            segment = []
        else:
            segment.append(token)
    return found


def unload_deleted(base: str, token: str, yaml_file: Path) -> tuple[str, bool]:
    """(report line, failed) for a deleted endpoint YAML passed to reload-file."""
    try:
        status, body = request("POST", f"{base}/api/v1/_config/endpoints/reload-file", token,
                               {"file": str(yaml_file)}, ok_codes=(200, 400, 404, 409))
    except urllib.error.HTTPError as exc:
        return f"{yaml_file.name}: HTTP {exc.code}: {error_text(exc)}", True
    if status == 200 and (body.get("status") == "removed" or body.get("success")):
        # {"status":"removed","removed":[...]}: the endpoints the file declared.
        keys = [str(k) for k in body.get("removed") or body.get("endpoints") or []]
        if keys:
            return f"flapi: {yaml_file.name} was deleted; the server unloaded {', '.join(keys)}.", False
        return f"flapi: {yaml_file.name} was deleted; the server had nothing loaded from it.", False
    message = str(body.get("error", ""))
    if status == 404:
        # Nothing was loaded from that file. (A server that predates unloading
        # also answers 404 here, and keeps serving the file's endpoints until
        # it restarts.)
        return (f"flapi: {yaml_file.name} was deleted; the server at {base} had nothing loaded from it "
                "(a server too old to unload deleted files keeps serving them until it restarts).", False)
    if "is outside template.path" in message or "is not an endpoint file" in message:
        return "", False
    return f"{yaml_file.name} (HTTP {status}: {message[:400]})", True


def handle_bash(event: dict) -> int:
    """After a shell command: unload endpoint YAMLs it deleted."""
    command = str((event.get("tool_input") or {}).get("command") or "")
    if not re.search(r"\b(rm|unlink|trash|mv)\b", command):
        return 0
    cwd = Path(str(event.get("cwd") or os.getcwd()))
    return handle_deleted(deleted_yaml_files(command, cwd))


def handle_deleted(deleted: list[Path]) -> int:
    """Pass each deleted endpoint YAML to reload-file, which unloads what it declared."""
    token = os.environ.get("FLAPI_CONFIG_SERVICE_TOKEN")
    if not deleted or not token:
        return 0
    base = (os.environ.get("FLAPI_URL") or os.environ.get("FLAPI_BASE_URL") or "http://localhost:8080").rstrip("/")
    lines: list[str] = []
    failed: list[str] = []
    for yaml_file in deleted:
        cfg = find_project_config(yaml_file)
        if cfg is None or yaml_file.resolve() == cfg.resolve():
            continue
        templates = template_dir(cfg)
        if not is_within(yaml_file, templates):
            continue
        try:
            reason = wrong_server(base, cfg) or not_this_project(base, token, templates)
            if reason:
                lines.append(reason)
                continue
            line, is_failure = unload_deleted(base, token, yaml_file.resolve())
        except TokenRejected as exc:
            print(f"flapi: the config service at {base} rejected FLAPI_CONFIG_SERVICE_TOKEN ({exc}); "
                  f"the endpoints of the deleted {yaml_file.name} were NOT unloaded.", file=sys.stderr)
            return 2
        except (urllib.error.URLError, OSError, ValueError):
            return 0   # no server running: nothing to unload
        if line and is_failure:
            failed.append(line)
        elif line:
            lines.append(line)
    if failed:
        print("flapi: unloading deleted endpoint files failed for: " + "; ".join(failed), file=sys.stderr)
        return 2
    if lines:
        emit("\n".join(lines))
    return 0


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
    if event.get("tool_name") == "Bash":
        return handle_bash(event)
    tool_input = event.get("tool_input") or {}
    file_path = tool_input.get("file_path") or tool_input.get("path")
    if not file_path:
        return 0
    edited = Path(file_path)
    if edited.suffix.lower() in (".yaml", ".yml") and not edited.exists():
        # The edited endpoint file is gone: unload what it declared.
        return handle_deleted([edited])
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
        # Token-free identity check first, then the token-carrying ones.
        reason = wrong_server(base, cfg) or not_this_project(base, token, templates, cfg)
        if reason:
            notes.insert(0, reason)
            emit("\n".join(notes))
            return 0
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
        render_errors: list[str] = []
        for key, cache in to_render:
            errors, render_notes = render(base, token, key, cache, templates)
            render_errors.extend(errors)
            notes.extend(render_notes)
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
            lines.append("flapi: reloaded, but template validation failed:\n  " + "\n  ".join(render_errors))
        if notes:
            lines.extend(notes)
        print("\n".join(lines), file=sys.stderr)
        return 2
    if reloaded:
        notes.insert(0, f"flapi: validated, hot-reloaded and checked {', '.join(reloaded)} on {base}.")
    elif edited.suffix == ".sql":
        notes.insert(0, f"flapi: config valid; no loaded endpoint uses {edited.name}.")
    elif is_endpoint_yaml:
        notes.insert(0, f"flapi: config valid; {edited.name} declares no endpoint the server loaded.")
    if notes:
        emit("\n".join(notes))
    return 0


if __name__ == "__main__":
    sys.exit(main())
