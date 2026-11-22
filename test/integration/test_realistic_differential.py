"""Differential correctness on a realistic project (#241): current build vs the reference build,
and concurrent vs serial.

Project and request matrix: realistic_project.py (parquet + CSV + DuckDB file + DuckLake cache,
validators, path parameters, sections, joins, aggregates, nested types, unicode, large text, auth,
MCP tools). Every request is sent

1. to the CURRENT binary and the REFERENCE binary (default build/release-1.5/flapi, DuckDB 1.5.5,
   code before the #241 performance work; FLAPI_REFERENCE_BINARY overrides; skipped when absent).
   JSON / CSV bodies and the stable headers must be byte-for-byte equal; Arrow bodies must decode to
   equal tables (the IPC bytes are not defined to be stable). Every difference is classified against
   EXPECTED below - an expected DuckDB 2.0 / new-feature difference, with the reason - and any other
   difference fails the test with the full list;
2. serially, twice, to the current binary (the responses must be deterministic), then by 48 parallel
   keep-alive clients, each request by 8 of them, every client in its own random order, while the
   DuckLake cache is refreshed underneath: every response must equal the serial one exactly (races in the streaming
   ResultSink, the shared pools or the telemetry aggregation show up as a different body).

Runs in well under two minutes on 8 cores.
"""
import concurrent.futures as cf
import io
import json
import os
import random
import shutil
import tempfile
import threading
import time

import pytest
import requests

import realistic_project as rp

pytestmark = [pytest.mark.standalone_server, pytest.mark.concurrency]

CLIENTS = int(os.environ.get("FLAPI_DIFF_CLIENTS", "48"))
COPIES = int(os.environ.get("FLAPI_DIFF_COPIES", "8"))      # concurrent senders per request

# (predicate on request, predicate on the difference) -> judgement. Each entry is a difference that
# is NOT a regression. Keep the reason next to it.
EXPECTED = [
    # Nested object keys (STRUCT fields, MAP keys) come out in crow::json's unordered_map order in
    # BOTH builds - never the declared order (see test_nested_keys_keep_their_declared_order). The old
    # path deep-copied every row three times, the streaming sink (6627194) moves it, so the hash order
    # comes out differently: same JSON value, different text. Not a value regression; it does break
    # 6627194's "byte-identical" claim for any multi-field STRUCT or MAP.
    ("nested STRUCT/MAP key order: undefined in both builds, flipped by 6627194 (move instead of copy)",
     lambda req: True, lambda d: d["kind"] == "key_order"),
    # `total-count: false` (8c06da4) is new: the reference does not know the key and still counts.
    ("total-count: false is new (8c06da4); the reference ignores the key",
     lambda req: req.path == "/documents" and req.kind == "rest",
     lambda d: d["kind"] in ("total_count", "header:x-total-count")),
    ("total-count: false is new (8c06da4); the reference ignores the key",
     lambda req: req.path == "/documents" and req.kind in ("csv", "arrow"),
     lambda d: d["kind"] == "header:x-total-count"),
]


@pytest.fixture(scope="module")
def data_dir():
    d = tempfile.mkdtemp(prefix="flapi_real_data_")
    rp.generate_data(d)
    yield d
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture(scope="module")
def shim():
    d = tempfile.mkdtemp(prefix="flapi_real_shim_")
    yield rp.build_shim(d)
    shutil.rmtree(d, ignore_errors=True)


@pytest.fixture(scope="module")
def current(data_dir, shim):
    # FLAPI_DIFF_NODELAY=1: preload the TCP_NODELAY shim here too (to time a pre-fc47cf3 binary fairly)
    nodelay = os.environ.get("FLAPI_DIFF_NODELAY") == "1"
    s = rp.Server(rp.current_binary(), data_dir, shim=shim if nodelay else None, nodelay=nodelay)
    yield s
    code = s.stop()
    log = s.output()
    s.cleanup()
    assert code == 0, f"current binary exited {code} on SIGTERM:\n{log[-3000:]}"


@pytest.fixture(scope="module")
def reference(data_dir, shim):
    ref = rp.reference_binary()
    if not ref.exists():
        pytest.skip(f"no reference binary at {ref} (set FLAPI_REFERENCE_BINARY)")
    s = rp.Server(ref, data_dir, shim=shim, nodelay=True)
    yield s
    s.stop()
    s.cleanup()


@pytest.fixture(scope="module")
def matrix(current):
    try:
        totals = rp.read_totals(current.base)
    except AssertionError as e:
        raise AssertionError(f"{e}\n{current.output()[-3000:]}")
    return rp.build_matrix(totals), totals


# ── comparison ────────────────────────────────────────────────────────────────────────────────────

def canon(v):
    """The JSON value with object keys sorted, recursively: equal canon = equal value."""
    if isinstance(v, dict):
        return {k: canon(v[k]) for k in sorted(v)}
    if isinstance(v, list):
        return [canon(x) for x in v]
    return v


def first_divergence(a, b, path="$", order=True):
    """Where two decoded JSON values first differ; with order=False key order is ignored."""
    if isinstance(a, dict) and isinstance(b, dict):
        if sorted(a) != sorted(b):
            return f"{path}: keys {sorted(a)} vs {sorted(b)}", "keys"
        if order and list(a) != list(b):
            return f"{path}: key order {list(a)} vs {list(b)}", "key_order"
        for k in a:
            r = first_divergence(a[k], b[k], f"{path}.{k}", order)
            if r:
                return r
        return None
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return f"{path}: {len(a)} vs {len(b)} items", "length"
        for i, (x, y) in enumerate(zip(a, b)):
            r = first_divergence(x, y, f"{path}[{i}]", order)
            if r:
                return r
        return None
    if a != b or type(a) is not type(b):
        return f"{path}: {a!r:.160} vs {b!r:.160}", "value"
    return None


def decode_csv(body):
    """CSV rows, every cell decoded. flAPI writes each value as JSON text (strings quoted and
    JSON-escaped, nested values as JSON), so a data line is a JSON array without its brackets; it
    is not RFC 4180 CSV (a nested value's commas are not quoted), so csv.reader cannot split it."""
    rows = []
    for line in body.decode("utf-8").split("\n"):
        try:
            rows.append(json.loads("[" + line + "]"))
        except ValueError:
            rows.append(line)            # the header line
    return rows


def decode(req, resp, body):
    if req.kind == "csv" and resp.headers.get("Content-Type", "").startswith("text/csv"):
        return decode_csv(body)
    v = json.loads(body)
    if req.kind == "mcp" and isinstance(v, dict) and "result" in v:
        # the tool text is itself JSON; compare inside it
        try:
            v = [json.loads(c["text"]) if c.get("type") == "text" else c for c in v["result"]["content"]]
        except (KeyError, TypeError, ValueError):
            pass
    return v


def diff(req, cur, ref):
    """List of {kind, detail} between two responses; [] when identical."""
    out = []
    cs, ch, cb = rp.fingerprint(cur)
    rs, rh, rb = rp.fingerprint(ref)
    if cs != rs:
        out.append({"kind": "status", "detail": f"{cs} vs {rs}: {cb[:200]!r} / {rb[:200]!r}"})
    for k in sorted(set(ch) | set(rh)):
        if ch.get(k) != rh.get(k):
            out.append({"kind": f"header:{k}", "detail": f"{ch.get(k)!r} vs {rh.get(k)!r}"})
    if cb == rb:
        return out
    if "arrow" in cur.headers.get("Content-Type", "") and "arrow" in ref.headers.get("Content-Type", ""):
        ct, rt = rp.arrow_table(cb), rp.arrow_table(rb)
        if ct.schema != rt.schema:
            out.append({"kind": "arrow_schema", "detail": f"{ct.schema} vs {rt.schema}"})
        elif not ct.equals(rt):
            out.append({"kind": "arrow_data", "detail": f"{ct.slice(0, 2).to_pylist()} vs {rt.slice(0, 2).to_pylist()}"})
        return out          # equal tables: the IPC bytes are not stable (padding), the data is
    try:
        cj, rj = decode(req, cur, cb), decode(req, ref, rb)
    except ValueError as e:
        return out + [{"kind": "undecodable", "detail": f"{e}: {cb[:200]!r} vs {rb[:200]!r}"}]
    if isinstance(cj, dict) and isinstance(rj, dict) and set(cj) ^ set(rj) == {"total_count"}:
        out.append({"kind": "total_count", "detail": "present in one only"})
        cj.pop("total_count", None)
        rj.pop("total_count", None)
    d = first_divergence(cj, rj, order=False)            # a different VALUE first
    if d:
        return out + [{"kind": d[1], "detail": d[0]}]
    d = first_divergence(cj, rj, order=True)             # then only the key order
    if d:
        return out + [{"kind": d[1], "detail": d[0]}]
    if not out:
        out.append({"kind": "text", "detail": f"same value, different text: {cb[:160]!r} vs {rb[:160]!r}"})
    return out


def judge(req, d):
    for reason, on_req, on_diff in EXPECTED:
        if on_req(req) and on_diff(d):
            return reason
    return None


def test_totals_agree_with_the_reference(current, reference, matrix):
    _, totals = matrix
    assert rp.read_totals(reference.base) == totals


def test_every_response_matches_the_reference(current, reference, matrix):
    reqs, _ = matrix
    cs, rs = requests.Session(), requests.Session()
    unexplained, explained = [], {}
    for req in reqs:
        cur, ref = req.send(cs, current.base), req.send(rs, reference.base)
        for d in diff(req, cur, ref):
            why = judge(req, d)
            if why:
                explained.setdefault(why, []).append(req.name)
            else:
                unexplained.append((req.name, d["kind"], d["detail"]))
    print(f"\n{len(reqs)} requests compared; expected differences:")
    for why, names in explained.items():
        print(f"  {len(names):4d} x {why}")
    groups = {}
    for name, kind, detail in unexplained:
        groups.setdefault((name.split("?")[0], kind), []).append((name, detail))
    report = [f"{len(v):4d} x [{k[1]}] {k[0]}   e.g. {v[0][0]}\n         {v[0][1][:400]}"
              for k, v in sorted(groups.items())]
    out = os.environ.get("FLAPI_DIFF_REPORT")
    if out:
        with open(out, "w") as f:
            json.dump([{"request": n, "kind": k, "detail": d} for n, k, d in unexplained], f, indent=1)
    assert not unexplained, (f"{len(unexplained)} unexplained differences current vs reference, "
                             f"by endpoint and kind:\n" + "\n".join(report))


def test_concurrent_responses_equal_serial_responses(current, matrix):
    reqs, _ = matrix
    s = requests.Session()
    serial = [rp.digest(r.send(s, current.base)) for r in reqs]
    again = [rp.digest(r.send(s, current.base)) for r in reqs]
    # A response that differs between two serial runs is a bug of its own (reported below); the
    # concurrency comparison runs on the deterministic rest so that one bug cannot hide another.
    flaky = {i for i in range(len(reqs)) if serial[i] != again[i]}
    server_errors = [r.name for r in reqs if r.send(s, current.base).status_code >= 500]
    assert not server_errors, f"5xx in the serial run: {server_errors}"

    stop = threading.Event()
    refreshes = []

    def refresher():
        rs = requests.Session()
        while not stop.is_set():
            r = rs.post(f"{current.base}/api/v1/_config/endpoints/-sales-daily/cache/refresh",
                        headers=rp.CS_AUTH, timeout=120)
            refreshes.append(r.status_code)
            time.sleep(0.2)

    def client(seed):
        sess = requests.Session()           # one keep-alive connection per client
        # every request is sent by COPIES different clients, all at the same time as the rest
        order = [i for i in range(len(reqs)) if (i + seed) % CLIENTS < COPIES]
        random.Random(seed).shuffle(order)
        bad = []
        for i in order:
            if i in flaky:
                continue
            r = reqs[i].send(sess, current.base)
            if r.status_code >= 500 or rp.digest(r) != serial[i]:
                bad.append(f"{reqs[i].name}: {r.status_code} {r.content[:200]!r}")
        return bad

    t = threading.Thread(target=refresher, daemon=True)
    t.start()
    started = time.time()
    try:
        with cf.ThreadPoolExecutor(CLIENTS) as pool:
            results = list(pool.map(client, range(CLIENTS)))
    finally:
        stop.set()
        t.join(timeout=120)
    took = time.time() - started
    bad = [b for r in results for b in r]
    sent = COPIES * (len(reqs) - len(flaky))
    print(f"\n{CLIENTS} keep-alive clients, {sent} requests ({COPIES} per matrix entry) in {took:.1f}s "
          f"({sent / took:.0f} req/s), {len(refreshes)} cache refreshes meanwhile")
    assert refreshes and set(refreshes) == {200}
    assert not bad, f"{len(bad)} concurrent responses differ from the serial ones:\n" + "\n".join(bad[:30])
    assert not flaky, f"serial responses are not deterministic: {[reqs[i].name for i in sorted(flaky)][:20]}"


@pytest.mark.xfail(strict=True, reason="pre-existing in both builds: nested objects are written in crow::json "
                                       "unordered_map order, not the declared STRUCT / MAP order")
def test_nested_keys_keep_their_declared_order(current):
    """STRUCT fields and MAP entries in the order DuckDB holds them (the declared order)."""
    row = requests.get(f"{current.base}/types", params={"limit": 1, "offset": 4}, timeout=30).json()["data"][0]
    assert list(row["address"]) == ["street", "zip", "geo"], row["address"]
    assert list(row["address"]["geo"]) == ["lat", "lon"], row["address"]
    assert [list(e) for e in row["list_of_struct"]] == [["k", "v"], ["k", "v"]], row["list_of_struct"]
    assert list(row["map_of_list"]) == ["x", "y"], row["map_of_list"]
    assert list(row["attrs"]) == ["k0", "k1"], row["attrs"]


def mini_server(tmp_path, sql):
    """A one-endpoint project on the current binary, for cases that would take the shared server down."""
    (tmp_path / "sqls").mkdir()
    (tmp_path / "flapi.yaml").write_text("project-name: mini\ntemplate:\n  path: ./sqls\nconnections:\n"
                                         "  c:\n    properties: {}\nrate_limit:\n  enabled: false\n")
    (tmp_path / "sqls" / "q.yaml").write_text("url-path: /q\nmethod: GET\ntemplate-source: q.sql\n"
                                              "connection: [c]\nwith-pagination: false\n")
    (tmp_path / "sqls" / "q.sql").write_text(sql)
    from test_duckdb_upgrade import Run
    return Run(rp.current_binary(), str(tmp_path))


@pytest.mark.xfail(strict=True, reason="pre-existing in both builds: convertVectorDateToJson / TimeToJson / "
                                       "IntervalToJson ignore the validity mask, so NULL renders as the zero value "
                                       "(and, under DuckDB 2.0, as whatever bytes the NULL slot holds)")
def test_null_dates_times_and_intervals_are_null(tmp_path):
    run = mini_server(tmp_path, "SELECT NULL::DATE AS d, NULL::TIME AS t, NULL::INTERVAL AS i, "
                                "NULL::TIMESTAMP AS ts\n")
    try:
        row = requests.get(f"{run.base}/q", timeout=30).json()[0]
    finally:
        run.stop()
    assert row == {"d": None, "t": None, "i": None, "ts": None}, row


@pytest.mark.xfail(strict=True, reason="pre-existing in both builds (seen on the 1.5.5 reference too, far more "
                                       "often under DuckDB 2.0): a NULL DATE renders as the bytes in its slot")
def test_null_dates_render_deterministically(current):
    """Every 11th customer has a NULL birth_date in the parquet file. Whatever a NULL renders as, it
    must render the same way every time. convertVectorDateToJson ignores the validity mask and
    formats the NULL slot's bytes: 1970-01-01 when DuckDB zeroed them, a random date
    ('1509947-12-16', '-4943794-02-21', ...) when it did not - far more often under DuckDB 2.0."""
    seen = set()
    for _ in range(8):
        rows = requests.get(f"{current.base}/birthdays", timeout=30).json()
        seen |= {r["birth_date"] for r in rows if r["id"] % 11 == 0}
    assert seen == {None}, f"NULL birth_date rendered as {sorted(map(str, seen))[:8]}"


@pytest.mark.xfail(strict=True, reason="pre-existing in both builds: the Arrow serializer has no INTERVAL "
                                       "mapping ('Arrow array view set failed', 500)")
def test_arrow_interval(tmp_path):
    run = mini_server(tmp_path, "SELECT INTERVAL 3 DAY AS i\n")
    try:
        r = requests.get(f"{run.base}/q", params={"format": "arrow"}, timeout=30)
    finally:
        run.stop()
    assert r.status_code == 200, r.text


@pytest.mark.xfail(strict=True, reason="pre-existing in both builds: convertVectorEnumToJson reads the ENUM "
                                       "index as idx_t (8 bytes) whatever the physical width; the server segfaults")
def test_an_enum_column_does_not_crash_the_server(tmp_path):
    run = mini_server(tmp_path, "SELECT range AS id, CAST((['new', 'paid', 'shipped'])[range % 3 + 1] "
                                "AS ENUM('new', 'paid', 'shipped')) AS e FROM range(8)\n")
    try:
        try:
            r = requests.get(f"{run.base}/q", timeout=30)
        except requests.ConnectionError:
            pytest.fail(f"server died: exit {run.proc.wait(timeout=10)}")
        assert [x["e"] for x in r.json()] == ["new", "paid", "shipped"] * 2 + ["new", "paid"]
    finally:
        run.stop()
