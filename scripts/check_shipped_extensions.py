#!/usr/bin/env python3
"""Check that a flapi binary loads its default extensions from the extensions/ beside it (#245).

    check_shipped_extensions.py path/to/flapi[.exe] [--extensions DIR]

DIR defaults to the extensions/ next to the binary; pass it when the command is a launcher
(the wheel's `flapi` console script execs site-packages/flapi_io/bin/flapi).

Run by CI on every platform runner right after `flapi extensions install --defaults --dir
<bin>/extensions`, i.e. on exactly the files the release ships. It starts the server with an
empty HOME, FLAPI_EXTENSIONS_OFFLINE=1 and a dead proxy, asks it which extensions are loaded
and from where, and fails unless every one came from the shipped directory and nothing was
written to the user directory. Standard library only (runs on the bare runners).
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

EXPECTED = ["ducklake", "fts", "httpfs", "postgres_scanner", "sqlite_scanner"]


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main() -> int:
    binary = os.path.abspath(sys.argv[1])
    shipped = os.path.join(os.path.dirname(binary), "extensions")
    if len(sys.argv) == 4 and sys.argv[2] == "--extensions":
        shipped = os.path.abspath(sys.argv[3])
    work = tempfile.mkdtemp(prefix="flapi_shipped_check_")
    home = os.path.join(work, "home")
    os.makedirs(home)
    os.makedirs(os.path.join(work, "sqls"))
    port = free_port()
    with open(os.path.join(work, "flapi.yaml"), "w") as f:
        f.write("project-name: shipped-extensions-check\nproject-description: CI check\n"
                "template:\n  path: ./sqls\nduckdb:\n  db_path: ':memory:'\nconnections: {}\n")
    with open(os.path.join(work, "sqls", "exts.yaml"), "w") as f:
        f.write("url-path: /exts\nmethod: GET\nwith-pagination: false\n"
                "template-source: exts.sql\nconnection: []\n")
    with open(os.path.join(work, "sqls", "exts.sql"), "w") as f:
        f.write("SELECT extension_name, install_path FROM duckdb_extensions() "
                "WHERE loaded AND install_mode <> 'STATICALLY_LINKED' ORDER BY extension_name\n")
    env = {k: v for k, v in os.environ.items() if k not in ("FLAPI_EXTENSION_DIRECTORY",)}
    dead = "http://127.0.0.1:9"
    env.update(HOME=home, USERPROFILE=home, FLAPI_EXTENSIONS_OFFLINE="1", DATAZOO_DISABLE_TELEMETRY="1",
               HTTP_PROXY=dead, HTTPS_PROXY=dead, http_proxy=dead, https_proxy=dead)
    log_path = os.path.join(work, "server.log")
    with open(log_path, "w") as log:
        proc = subprocess.Popen([binary, "-c", "flapi.yaml", "-p", str(port)], cwd=work, env=env,
                                stdout=log, stderr=subprocess.STDOUT)
    ok = False
    try:
        rows = None
        deadline = time.time() + 60
        while time.time() < deadline and proc.poll() is None:
            try:
                with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(
                        f"http://127.0.0.1:{port}/exts", timeout=2) as response:
                    body = json.load(response)
                    rows = body["data"] if isinstance(body, dict) else body
                    break
            except OSError:
                time.sleep(0.5)
        if rows is None:
            print("server did not answer", file=sys.stderr)
        else:
            loaded = {r["extension_name"]: r["install_path"] for r in rows}
            print(json.dumps(loaded, indent=2))
            missing = [e for e in EXPECTED if e not in loaded]
            elsewhere = [e for e in EXPECTED if e in loaded and not
                         os.path.normcase(os.path.abspath(loaded[e])).startswith(os.path.normcase(shipped))]
            downloaded = [os.path.join(r, n) for r, _, fs in os.walk(home) for n in fs
                          if n.endswith(".duckdb_extension")]
            for label, items in (("not loaded", missing), ("not loaded from " + shipped, elsewhere),
                                 ("downloaded into HOME", downloaded)):
                if items:
                    print(f"{label}: {items}", file=sys.stderr)
            ok = not (missing or elsewhere or downloaded)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
    if not ok:
        with open(log_path) as f:
            print("=== server log ===\n" + f.read(), file=sys.stderr)
    shutil.rmtree(work, ignore_errors=True)
    print("shipped extensions: OK" if ok else "shipped extensions: FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
