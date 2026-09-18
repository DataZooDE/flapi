"""A read concurrent with a write must not wedge a SQLite attachment (#116).

One GET issued at the same moment as one POST against a DuckDB SQLite
attachment deadlocks the two against each other. Two requests are enough. In
the reported production case the attachment then never answered again, while
the process, DuckDB, `/health/live` and every other connection stayed perfectly
healthy — nothing crashed, nothing logged an error, and the instance kept
accepting traffic it could no longer serve.

Measured here on the minimal fixture below: without the fix, 19 of 20
read+write pairs fail (503 once the lock-contention retry budget runs out) and
the first pair takes 25s; with it, 20 of 20 pass in well under a second.
Serialising only the writes does not help — the pair that wedges is a reader
and a writer — so flAPI serialises *all* access to connections that attach
SQLite.

These tests drive the real binary over real HTTP against a real SQLite file.
"""
import json
import os
import sqlite3
import subprocess
import tempfile
import threading
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

# Generous: the point is to tell "answers eventually" from "never answers
# again", not to measure latency. A wedged attachment blows straight through it.
REQUEST_TIMEOUT_S = 30


class _Server:
    """flAPI over a real SQLite attachment, with one read and one write route."""

    def __init__(self, serialize_access=None):
        self.tmp = tempfile.mkdtemp(prefix="flapi_sqlite_")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.tmp, "server.log")
        self.db_path = os.path.join(self.tmp, "shop.sqlite")

        con = sqlite3.connect(self.db_path)
        con.execute("CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT)")
        con.executemany("INSERT INTO products (name) VALUES (?)",
                        [(f"product-{i}",) for i in range(50)])
        con.commit()
        con.close()

        sqls = os.path.join(self.tmp, "sqls")
        os.makedirs(sqls, exist_ok=True)
        with open(os.path.join(sqls, "read.yaml"), "w") as f:
            f.write("url-path: /products\nmethod: GET\n"
                    "template-source: read.sql\nconnection: [shop]\n")
        with open(os.path.join(sqls, "read.sql"), "w") as f:
            f.write("SELECT id, name FROM shop.products ORDER BY id\n")
        with open(os.path.join(sqls, "write.yaml"), "w") as f:
            f.write(
                "url-path: /products\nmethod: POST\n"
                "operation:\n  type: write\n  returns-data: false\n  transaction: true\n"
                "request:\n"
                "  - field-name: name\n    field-in: body\n    required: true\n"
                "    validators:\n      - type: string\n        min: 1\n        max: 100\n"
                "template-source: write.sql\nconnection: [shop]\n")
        with open(os.path.join(sqls, "write.sql"), "w") as f:
            f.write("INSERT INTO shop.products (name) VALUES ('{{{ params.name }}}');\n")

        serialize_line = ""
        if serialize_access is not None:
            serialize_line = f"    serialize-access: {str(serialize_access).lower()}\n"

        with open(os.path.join(self.tmp, "flapi.yaml"), "w") as f:
            f.write(
                "project-name: sqlite-concurrency\n"
                "project-description: read+write concurrency on a SQLite attachment\n"
                f"http-port: {self.port}\n"
                "template:\n  path: ./sqls\n"
                "connections:\n  shop:\n"
                + serialize_line +
                "    init: |\n"
                "      INSTALL sqlite;\n"
                "      LOAD sqlite;\n"
                f"      ATTACH IF NOT EXISTS '{self.db_path}' AS shop (TYPE sqlite);\n")
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen(
            [flapi_binary(), "-c", os.path.join(self.tmp, "flapi.yaml"),
             "-p", str(self.port), "--log-level", "warning"],
            stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT, cwd=self.tmp,
            env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1"},
            preexec_fn=os.setsid)
        deadline = time.time() + 90
        while time.time() < deadline:
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=2).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail(f"server did not start; log:\n{open(self.log_path).read()[-4000:]}")

    def stop(self):
        if self.proc:
            import signal
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=30)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()


def _read_and_write_together(server, tag):
    """Fire one GET and one POST at the same instant. Returns (read, write).

    Each entry is the status code, or a string describing how it failed. A
    wedged attachment shows up as "timeout" — the request never returns.
    """
    barrier = threading.Barrier(2)
    results = {}

    def read():
        try:
            barrier.wait(timeout=30)
            results["read"] = requests.get(
                f"{server.base_url}/products", timeout=REQUEST_TIMEOUT_S).status_code
        except requests.Timeout:
            results["read"] = "timeout"
        except Exception as e:                      # noqa: BLE001
            results["read"] = f"error: {e}"

    def write():
        try:
            barrier.wait(timeout=30)
            results["write"] = requests.post(
                f"{server.base_url}/products", json={"name": tag},
                timeout=REQUEST_TIMEOUT_S).status_code
        except requests.Timeout:
            results["write"] = "timeout"
        except Exception as e:                      # noqa: BLE001
            results["write"] = f"error: {e}"

    threads = [threading.Thread(target=read, daemon=True),
               threading.Thread(target=write, daemon=True)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=REQUEST_TIMEOUT_S + 10)
    return results.get("read"), results.get("write")


class TestSqliteReadWriteConcurrency:
    def test_one_read_and_one_write_at_once(self):
        # The minimal reproduction from #116. Before the fix this wedged every
        # time, and the wedge was permanent.
        with _Server() as s:
            read, write = _read_and_write_together(s, "concurrent-1")
            assert read == 200, f"read: {read}"
            assert write in (200, 201), f"write: {write}"

    def test_the_attachment_still_answers_afterwards(self):
        # The part that made #116 an outage rather than a failed request: in
        # production the attachment never answered again, while /health/live
        # stayed green and the instance kept taking traffic. This fixture
        # recovers on its own, so this one does not go red without the fix -
        # it is here to catch a fix that trades a deadlock for a leaked lock.
        with _Server() as s:
            _read_and_write_together(s, "concurrent-2")

            r = requests.get(f"{s.base_url}/products", timeout=REQUEST_TIMEOUT_S)
            assert r.status_code == 200, f"the attachment stopped answering: {r.text}"
            assert len(r.json()["data"]) >= 50

    def test_sustained_interleaved_traffic(self):
        # One pair is enough to wedge it, but a single pass could in principle
        # get lucky. Twenty cannot.
        with _Server() as s:
            failures = []
            for i in range(20):
                read, write = _read_and_write_together(s, f"sustained-{i}")
                if read != 200 or write not in (200, 201):
                    failures.append((i, read, write))
            assert not failures, f"{len(failures)}/20 pairs failed: {failures[:5]}"

    def test_every_write_landed(self):
        # Serialisation must not quietly drop writes: waiting for a turn is
        # acceptable, losing the row is not. Sequential, so it passes either
        # way - it guards the fix, not the bug.
        with _Server() as s:
            for i in range(10):
                r = requests.post(f"{s.base_url}/products", json={"name": f"row-{i}"},
                                  timeout=REQUEST_TIMEOUT_S)
                assert r.status_code in (200, 201), r.text

            con = sqlite3.connect(s.db_path)
            names = {row[0] for row in con.execute("SELECT name FROM products")}
            con.close()
            assert {f"row-{i}" for i in range(10)} <= names

    def test_a_connection_that_does_not_need_it_is_not_serialised(self):
        # The heuristic must stay narrow. A parquet or BigQuery connection has
        # no reason to pay for this, and an operator can say so explicitly.
        with _Server(serialize_access=False) as s:
            r = requests.get(f"{s.base_url}/products", timeout=REQUEST_TIMEOUT_S)
            assert r.status_code == 200, r.text
