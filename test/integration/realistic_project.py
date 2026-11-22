"""A realistic flAPI project for the differential and soak suites (#241).

Not a test module: it builds the project tree, its data, a server launcher that works for any
flapi binary (the current build and a reference one), and the request matrix both suites run.

The project, written fresh into a temp directory for every server:

* connections
    - ``crm``        parquet (customers: unicode names with quotes/backslashes/control chars,
                     DECIMAL balances, LIST tags, MAP attrs, STRUCT address, TIMESTAMPTZ, BLOB,
                     UUID strings, NULLs) and a parquet of large text documents
    - ``sales``      CSV (orders, with quoted commas and newlines in free text)
    - ``warehouse``  a DuckDB database file, built by the connection's ``init`` (products)
    - DuckLake cache ``/sales-daily`` (full refresh from the CSV), refreshed through the config service
* endpoints: typed validators, path parameters, bound (``{{ params.x }}``) and unbound
  (``'{{{ params.x }}}'``) parameters, ``{{#params.x}}`` sections, ORDER BY and no ORDER BY,
  aggregates, joins across connections, every nested type flAPI serialises, an unpaged endpoint,
  a ``total-count: false`` endpoint, basic auth with roles on two endpoints, MCP tools.

Data is generated deterministically in Python (pyarrow + csv) so both binaries read identical bytes.
"""
import csv
import decimal
import hashlib
import os
import pathlib
import random
import shutil
import signal
import subprocess
import tempfile
import time
import uuid as uuidlib
from datetime import date, datetime, timedelta, timezone

import pytest
import requests

from otel_helpers import free_port

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]

N_CUSTOMERS = 2_400
N_ORDERS = 24_000
N_PRODUCTS = 310
N_DOCS = 160
TOKEN = "realistic-token-241"
CS_AUTH = {"Authorization": f"Bearer {TOKEN}"}
ADMIN = ("admin", "s3cret-Admin")
ANALYST = ("analyst", "an4lyst")
VIEWER = ("viewer", "v1ewer")

NAMES = ["Zoë", "Øyvind", "李雷", "Ελένη", "Søren 🚀", "محمد", "José́", "O'Brien", '"Quoted" Inc.',
         "back\\slash", "tab\there", "line\nbreak", "ctrl\x01\x1f", "sep arator", "Ünïcödé ß",
         "Ναυσικά", "Ярослав", "한국어", "plain"]
COUNTRIES = ["DE", "FR", "US", "JP", "BR", "IN", "ZA", "SE"]
STATUSES = ["new", "paid", "shipped", "returned", "cancelled"]


# ── data ──────────────────────────────────────────────────────────────────────────────────────────

def generate_data(data_dir):
    """Write customers.parquet, documents.parquet, orders.csv. Deterministic, ~6 MB."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    os.makedirs(data_dir, exist_ok=True)
    rnd = random.Random(241)
    base = datetime(2021, 1, 1, tzinfo=timezone.utc)
    ids, names, emails, countries, balances, tags, attrs, addrs, tss, births, uuids, avatars, refs = \
        ([] for _ in range(13))
    for i in range(1, N_CUSTOMERS + 1):
        ids.append(i)
        names.append(None if i % 97 == 0 else f"{NAMES[i % len(NAMES)]} {i}")
        emails.append(None if i % 13 == 0 else f"user{i}@example.{COUNTRIES[i % 8].lower()}")
        countries.append(COUNTRIES[rnd.randrange(8)])
        balances.append(None if i % 31 == 0 else
                        decimal.Decimal(rnd.randrange(-500_000, 9_000_000)) / 100)
        tags.append(None if i % 17 == 0 else [f"t{rnd.randrange(9)}" for _ in range(i % 4)])
        attrs.append(None if i % 19 == 0 else [(f"k{j}", rnd.randrange(1000)) for j in range(i % 3)])
        addrs.append(None if i % 23 == 0 else {
            "street": f"{rnd.randrange(1, 300)} Hauptstraße" if i % 2 else f"{i} Rue de l'Église",
            "zip": f"{rnd.randrange(10000, 99999)}",
            "geo": {"lat": round(rnd.uniform(-60, 60), 6), "lon": round(rnd.uniform(-170, 170), 6)}})
        tss.append(base + timedelta(seconds=rnd.randrange(0, 120_000_000), microseconds=rnd.randrange(10**6)))
        births.append(None if i % 11 == 0 else date(1950, 1, 1) + timedelta(days=rnd.randrange(20000)))
        uuids.append(str(uuidlib.UUID(int=rnd.getrandbits(128), version=4)))
        avatars.append(None if i % 7 == 0 else bytes(rnd.randrange(256) for _ in range(i % 24)))
        refs.append(rnd.randrange(1, N_CUSTOMERS + 1) if i % 5 else None)
    customers = pa.table({
        "id": pa.array(ids, pa.int64()),
        "name": pa.array(names, pa.string()),
        "email": pa.array(emails, pa.string()),
        "country": pa.array(countries, pa.string()),
        "balance": pa.array(balances, pa.decimal128(18, 2)),
        "tags": pa.array(tags, pa.list_(pa.string())),
        "attrs": pa.array(attrs, pa.map_(pa.string(), pa.int32())),
        "address": pa.array(addrs, pa.struct([("street", pa.string()), ("zip", pa.string()),
                                              ("geo", pa.struct([("lat", pa.float64()), ("lon", pa.float64())]))])),
        "signup_ts": pa.array(tss, pa.timestamp("us", tz="UTC")),
        "birth_date": pa.array(births, pa.date32()),
        "uuid": pa.array(uuids, pa.string()),
        "avatar": pa.array(avatars, pa.binary()),
        "referred_by": pa.array(refs, pa.int64()),
    })
    pq.write_table(customers, os.path.join(data_dir, "customers.parquet"), row_group_size=500)

    words = ["daten", "Straße", "naïve", "café", "数据", "λόγος", "данные", "🚀", "\"q\"", "a\\b", "x,y",
             "line\nfeed", "\ttab", "</script>", "&amp;", "{json}", "[1,2]"]
    docs = pa.table({
        "doc_id": pa.array(range(1, N_DOCS + 1), pa.int32()),
        "customer_id": pa.array([rnd.randrange(1, N_CUSTOMERS + 1) for _ in range(N_DOCS)], pa.int64()),
        "title": pa.array([f"Dokument {i} — {words[i % len(words)]}" for i in range(1, N_DOCS + 1)]),
        "body": pa.array([" ".join(words[rnd.randrange(len(words))] for _ in range(300 + (i * 37) % 4000))
                          for i in range(N_DOCS)]),
    })
    pq.write_table(docs, os.path.join(data_dir, "documents.parquet"))

    with open(os.path.join(data_dir, "orders.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, quoting=csv.QUOTE_MINIMAL)
        w.writerow(["order_id", "customer_id", "product_id", "ordered_at", "qty", "unit_price", "status", "note"])
        for oid in range(1, N_ORDERS + 1):
            ts = datetime(2023, 1, 1) + timedelta(minutes=rnd.randrange(0, 900_000))
            note = rnd.choice(["", "gift, wrap it", 'say "hi"', "multi\nline", "Grüße aus Köln", "备注"])
            w.writerow([oid, rnd.randrange(1, N_CUSTOMERS + 1), rnd.randrange(1, N_PRODUCTS + 1),
                        ts.strftime("%Y-%m-%d %H:%M:%S"), rnd.randrange(1, 12),
                        f"{rnd.randrange(99, 99_999) / 100:.2f}", rnd.choice(STATUSES), note])


# ── project ───────────────────────────────────────────────────────────────────────────────────────

FLAPI_YAML = """\
project-name: realistic
project-description: realistic regression project (#241)
template:
  path: ./sqls
connections:
  crm:
    properties:
      customers: ./data/customers.parquet
      documents: ./data/documents.parquet
  sales:
    properties:
      path: ./data/orders.csv
  warehouse:
    init: |
      ATTACH './warehouse.duckdb' AS wh;
      CREATE TABLE IF NOT EXISTS wh.products AS
        SELECT range::INTEGER AS product_id,
               'Produkt ' || range || CASE WHEN range % 3 = 0 THEN ' «Ü»' ELSE '' END AS name,
               (['tools', 'garden', 'kitchen', 'office', 'toys'])[(range % 5) + 1] AS category,
               ((range * 7919) % 100000 / 100.0)::DECIMAL(10, 2) AS list_price,
               CASE WHEN range % 9 = 0 THEN NULL ELSE range % 40 END AS stock
        FROM range(1, {n_products} + 1);
    properties:
      db: ./warehouse.duckdb
ducklake:
  enabled: true
  alias: cache
  metadata-path: ./cache.ducklake
  data-path: ./cache_data
  warmup: never
rate_limit:
  enabled: false
mcp:
  enabled: true
"""

AUTH_ADMIN = f"""\
auth:
  enabled: true
  type: basic
  users:
    - username: {ADMIN[0]}
      password: {ADMIN[1]}
      roles: [admin, analyst]
    - username: {ANALYST[0]}
      password: {ANALYST[1]}
      roles: [analyst]
    - username: {VIEWER[0]}
      password: {VIEWER[1]}
      roles: [viewer]
"""

ENDPOINTS = {
    # parquet, sections, an enum bound in quotes (unbound text), a bound number, a regex string
    "customers.yaml": """\
url-path: /customers
method: GET
template-source: customers.sql
connection: [crm]
request:
  - field-name: country
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [DE, FR, US, JP, BR, IN, ZA, SE]
  - field-name: min_balance
    field-in: query
    required: false
    validators:
      - type: number
        min: -100000
        max: 100000
  - field-name: name_prefix
    field-in: query
    required: false
    validators:
      - type: string
        regex: "^[A-Za-z ]{1,20}$"
mcp-tool:
  name: search_customers
  description: Search customers by country, balance and name prefix
""",
    "customers.sql": """\
SELECT id, name, email, country, balance, tags, address
FROM read_parquet('{{{ conn.customers }}}')
WHERE 1=1
{{#params.country}}
  AND country = '{{{ params.country }}}'
{{/params.country}}
{{#params.min_balance}}
  AND balance >= {{ params.min_balance }}
{{/params.min_balance}}
{{#params.name_prefix}}
  AND starts_with(name, '{{{ params.name_prefix }}}')
{{/params.name_prefix}}
ORDER BY id
""",
    # path parameter (bound, outside a section) and every nested type
    "customer.yaml": """\
url-path: /customers/:id
method: GET
template-source: customer.sql
connection: [crm]
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1
        max: 1000000
mcp-tool:
  name: get_customer
  description: One customer with every attribute
""",
    "customer.sql": """\
SELECT * FROM read_parquet('{{{ conn.customers }}}') WHERE id = {{ params.id }}
""",
    # join parquet x CSV x DuckDB file, path parameter, ORDER BY a non-unique column + tiebreak
    "customer_orders.yaml": """\
url-path: /customers/:id/orders
method: GET
template-source: customer_orders.sql
connection: [sales, crm, warehouse]
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1
  - field-name: status
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [new, paid, shipped, returned, cancelled]
""",
    "customer_orders.sql": """\
SELECT o.order_id, o.ordered_at, o.status, o.qty, o.unit_price,
       (o.qty * o.unit_price)::DECIMAL(18, 2) AS line_total,
       p.name AS product, p.category, c.name AS customer, c.address.geo AS geo
FROM read_csv('./data/orders.csv') o
JOIN wh.products p ON p.product_id = o.product_id
JOIN read_parquet('./data/customers.parquet') c ON c.id = o.customer_id
WHERE o.customer_id = {{ params.id }}
{{#params.status}}
  AND o.status = '{{{ params.status }}}'
{{/params.status}}
ORDER BY o.ordered_at DESC, o.order_id
""",
    # CSV, dates, no ORDER BY (insertion order must survive the paging wrapper), CSV format
    "orders.yaml": """\
url-path: /orders
method: GET
template-source: orders.sql
connection: [sales]
request:
  - field-name: status
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [new, paid, shipped, returned, cancelled]
  - field-name: from
    field-in: query
    required: false
    validators:
      - type: date
        min: "2020-01-01"
        max: "2030-12-31"
  - field-name: min_qty
    field-in: query
    required: false
    validators:
      - type: int
        min: 1
        max: 100
mcp-tool:
  name: list_orders
  description: Orders by status, date and quantity
""",
    "orders.sql": """\
SELECT order_id, customer_id, product_id, ordered_at, qty, unit_price, status, note
FROM read_csv('{{{ conn.path }}}')
WHERE 1=1
{{#params.status}} AND status = '{{{ params.status }}}' {{/params.status}}
{{#params.from}} AND ordered_at >= DATE '{{{ params.from }}}' {{/params.from}}
{{#params.min_qty}} AND qty >= {{ params.min_qty }} {{/params.min_qty}}
""",
    # DuckDB file, unpaged
    "products.yaml": """\
url-path: /products
method: GET
template-source: products.sql
connection: [warehouse]
with-pagination: false
request:
  - field-name: category
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [tools, garden, kitchen, office, toys]
mcp-tool:
  name: list_products
  description: The product catalogue
""",
    "products.sql": """\
SELECT * FROM wh.products
{{#params.category}} WHERE category = '{{{ params.category }}}' {{/params.category}}
ORDER BY product_id
""",
    # aggregates: GROUP BY, list(), DECIMAL sums, avg, count distinct, HAVING
    "stats.yaml": """\
url-path: /stats/countries
method: GET
template-source: stats.sql
connection: [crm, sales]
request:
  - field-name: min_orders
    field-in: query
    required: false
    validators:
      - type: int
        min: 0
mcp-tool:
  name: country_stats
  description: Revenue and customers per country
""",
    "stats.sql": """\
SELECT c.country,
       count(DISTINCT c.id) AS customers,
       count(o.order_id) AS orders,
       sum(o.qty * o.unit_price)::DECIMAL(18, 2) AS revenue,
       round(avg(o.qty), 4) AS avg_qty,
       min(o.ordered_at) AS first_order,
       max(o.ordered_at) AS last_order,
       list(DISTINCT o.status ORDER BY o.status) AS statuses,
       sum(c.balance) AS total_balance
FROM read_parquet('{{{ conn.customers }}}') c
LEFT JOIN read_csv('./data/orders.csv') o ON o.customer_id = c.id
GROUP BY c.country
HAVING count(o.order_id) >= {{#params.min_orders}}{{ params.min_orders }}{{/params.min_orders}}{{^params.min_orders}}0{{/params.min_orders}}
ORDER BY revenue DESC, c.country
""",
    # type showcase: computed types DuckDB and flAPI must agree on
    "types.yaml": """\
url-path: /types
method: GET
template-source: types.sql
connection: [crm]
mcp-tool:
  name: type_showcase
  description: Every type
""",
    "types.sql": """\
SELECT id,
       uuid::UUID AS uuid,
       signup_ts::TIMESTAMPTZ AS signup_tz,
       signup_ts::TIMESTAMP AS signup_naive,
       birth_date,
       signup_ts::TIMESTAMP::TIME AS signup_time,
       balance,
       balance::DECIMAL(38, 10) AS balance_wide,
       balance::DOUBLE / 3 AS balance_third,
       (id::HUGEINT * 100000000000000000000) AS huge,
       id::UBIGINT * 4000000000 AS ubig,
       id % 2 = 0 AS even,
       CASE WHEN id % 4 = 0 THEN NULL ELSE id % 3 = 0 END AS maybe,
       INTERVAL (id) DAY + INTERVAL (id % 60) MINUTE AS age,
       avatar,
       tags,
       attrs,
       address,
       [{'k': 'a', 'v': id}, {'k': 'b', 'v': NULL}] AS list_of_struct,
       MAP {'x': [1, 2], 'y': []} AS map_of_list,
       {'nested': {'deeper': [address.geo]}} AS deep,
       CASE id % 5 WHEN 0 THEN 'nan'::DOUBLE WHEN 1 THEN 'inf'::DOUBLE WHEN 2 THEN -0.0 WHEN 3 THEN 1e300
            ELSE 0.1 + 0.2 END AS floats,
       0.1::FLOAT AS f32,
       name
FROM read_parquet('{{{ conn.customers }}}')
WHERE id <= 600
  AND birth_date IS NOT NULL   -- a NULL DATE renders as garbage: test_null_dates_render_deterministically
ORDER BY id
""",
    # NULL dates from parquet (kept out of the matrix: see test_null_dates_render_deterministically)
    "birthdays.yaml": """\
url-path: /birthdays
method: GET
template-source: birthdays.sql
connection: [crm]
with-pagination: false
""",
    "birthdays.sql": """\
SELECT id, birth_date FROM read_parquet('{{{ conn.customers }}}') WHERE id <= 600 ORDER BY id
""",
    # large text, total-count: false (new key; the reference ignores it)
    "documents.yaml": """\
url-path: /documents
method: GET
template-source: documents.sql
connection: [crm]
total-count: false
request:
  - field-name: q
    field-in: query
    required: false
    validators:
      - type: string
        regex: '^[^'';\\\\]{2,24}$'
mcp-tool:
  name: search_documents
  description: Large text documents
""",
    "documents.sql": """\
SELECT d.doc_id, d.title, length(d.body) AS chars, d.body, c.name AS owner
FROM read_parquet('{{{ conn.documents }}}') d
LEFT JOIN read_parquet('{{{ conn.customers }}}') c ON c.id = d.customer_id
WHERE 1=1
{{#params.q}} AND contains(d.body, '{{{ params.q }}}') {{/params.q}}
ORDER BY d.doc_id
""",
    # DuckLake cache, full refresh, read through the cache
    "sales_daily.yaml": """\
url-path: /sales/daily
method: GET
template-source: sales_daily.sql
connection: [sales]
cache:
  enabled: true
  table: sales_daily
  template-file: sales_daily_cache.sql
  retention:
    keep-last-snapshots: 3
request:
  - field-name: status
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [new, paid, shipped, returned, cancelled]
mcp-tool:
  name: sales_daily
  description: Daily sales from the cache
""",
    "sales_daily_cache.sql": """\
CREATE OR REPLACE TABLE {{cache.catalog}}.{{cache.schema}}.{{cache.table}} AS
SELECT CAST(ordered_at AS DATE) AS day, status, count(*) AS orders, sum(qty) AS units,
       sum(qty * unit_price)::DECIMAL(18, 2) AS revenue
FROM read_csv('{{{ conn.path }}}')
GROUP BY ALL
""",
    "sales_daily.sql": """\
SELECT * FROM {{{cache.catalog}}}.{{{cache.schema}}}.{{{cache.table}}}
WHERE 1=1 {{#params.status}} AND status = '{{{ params.status }}}' {{/params.status}}
ORDER BY day, status
""",
    # auth with roles, auth context in the template
    "revenue.yaml": """\
url-path: /admin/revenue
method: GET
template-source: revenue.sql
connection: [sales, warehouse]
""" + AUTH_ADMIN.replace("auth:\n  enabled: true", "auth:\n  enabled: true\n  roles: [analyst]") + """\
request:
  - field-name: category
    field-in: query
    required: false
    validators:
      - type: enum
        allowedValues: [tools, garden, kitchen, office, toys]
""",
    "revenue.sql": """\
SELECT p.category, date_trunc('month', o.ordered_at) AS month,
       sum(o.qty * o.unit_price)::DECIMAL(18, 2) AS revenue, '{{{ auth.username }}}' AS asked_by
FROM read_csv('./data/orders.csv') o JOIN wh.products p USING (product_id)
WHERE o.status <> 'cancelled'
{{#params.category}} AND p.category = '{{{ params.category }}}' {{/params.category}}
GROUP BY ALL
ORDER BY month, p.category
""",
    "pii.yaml": """\
url-path: /admin/customers/:id/pii
method: GET
template-source: pii.sql
connection: [crm]
""" + AUTH_ADMIN.replace("auth:\n  enabled: true", "auth:\n  enabled: true\n  roles: [admin]") + """\
request:
  - field-name: id
    field-in: path
    required: true
    validators:
      - type: int
        min: 1
""",
    "pii.sql": """\
SELECT id, name, email, birth_date, avatar FROM read_parquet('{{{ conn.customers }}}') WHERE id = {{ params.id }}
""",
}


def write_project(root, data_src):
    """A fresh project tree in `root`; data files are copied from `data_src` (generated once)."""
    os.makedirs(os.path.join(root, "sqls"), exist_ok=True)
    shutil.copytree(data_src, os.path.join(root, "data"), dirs_exist_ok=True)
    with open(os.path.join(root, "flapi.yaml"), "w") as f:
        f.write(FLAPI_YAML.format(n_products=N_PRODUCTS))
    for name, text in ENDPOINTS.items():
        with open(os.path.join(root, "sqls", name), "w") as f:
            f.write(text)


# ── binaries, shim, server ────────────────────────────────────────────────────────────────────────

def current_binary():
    env = os.environ.get("FLAPI_BINARY")
    if env:
        return pathlib.Path(env)
    for c in ("release-2", os.getenv("FLAPI_BUILD_TYPE", "release"), "release", "debug"):
        p = REPO / "build" / c / "flapi"
        if p.exists():
            return p
    pytest.skip("flapi binary not found")


def reference_binary():
    return pathlib.Path(os.environ.get("FLAPI_REFERENCE_BINARY",
                                       os.environ.get("FLAPI_OLD_BINARY", REPO / "build" / "release-1.5" / "flapi")))


def build_shim(out_dir):
    """Compile realistic_shim.c; None when there is no C compiler."""
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return None
    so = os.path.join(out_dir, "realistic_shim.so")
    subprocess.run([cc, "-shared", "-fPIC", "-O2", "-o", so, str(HERE / "realistic_shim.c"), "-ldl"],
                   check=True, capture_output=True)
    return so


class Server:
    """One flapi process on a fresh copy of the project."""

    def __init__(self, binary, data_src, *, shim=None, nodelay=False, sink_port=None, shim_log=None,
                 telemetry=False, log_level=None, extra_env=None):
        log_level = log_level or os.environ.get("FLAPI_REAL_LOG_LEVEL", "warning")
        self.binary = pathlib.Path(binary)
        self.root = tempfile.mkdtemp(prefix=f"flapi_real_{self.binary.parent.name}_")
        write_project(self.root, data_src)
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.log_path = os.path.join(self.root, "server.log")
        env = {k: v for k, v in os.environ.items() if k not in ("DATAZOO_DISABLE_TELEMETRY", "FLAPI_NO_TELEMETRY")}
        env["TZ"] = "UTC"
        if not telemetry:
            env["DATAZOO_DISABLE_TELEMETRY"] = "1"
        if shim:
            env["LD_PRELOAD"] = shim
            if nodelay:
                env["FLAPI_SHIM_NODELAY"] = "1"
            if sink_port:
                env["FLAPI_SHIM_SINK_PORT"] = str(sink_port)
            if shim_log:
                env["FLAPI_SHIM_LOG"] = shim_log
        env.update(extra_env or {})
        self._log = open(self.log_path, "w")
        self.proc = subprocess.Popen(
            [str(self.binary), "-c", "flapi.yaml", "-p", str(self.port), "--log-level", log_level,
             "--config-service", "--config-service-token", TOKEN],
            cwd=self.root, stdout=self._log, stderr=subprocess.STDOUT, env=env, preexec_fn=os.setsid)
        deadline = time.time() + 120
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"{binary} exited during startup:\n{self.output()[-4000:]}")
            try:
                if requests.get(f"{self.base}/health/live", timeout=1).status_code == 200:
                    break
            except requests.RequestException:
                time.sleep(0.3)
        else:
            self.stop()
            raise RuntimeError("never became live:\n" + self.output()[-4000:])
        self.refresh_cache()

    def output(self):
        if not self._log.closed:
            self._log.flush()
        return pathlib.Path(self.log_path).read_text(errors="replace")

    def refresh_cache(self, session=None):
        r = (session or requests).post(f"{self.base}/api/v1/_config/endpoints/-sales-daily/cache/refresh",
                                       headers=CS_AUTH, timeout=120)
        assert r.status_code == 200, f"cache refresh: {r.status_code} {r.text}\n{self.output()[-3000:]}"
        return r

    def stop(self, timeout=30):
        """SIGTERM; returns the exit code (None if it had to be killed)."""
        code = self.proc.poll()
        if code is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                code = self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
                code = None
        if not self._log.closed:
            self._log.close()
        return code

    def cleanup(self):
        shutil.rmtree(self.root, ignore_errors=True)


# ── request matrix ────────────────────────────────────────────────────────────────────────────────

class Req:
    """One request in the matrix. `kind` is rest|csv|arrow|mcp."""

    def __init__(self, kind, path, params=None, auth=None, tool=None, args=None, name=None):
        self.kind, self.path, self.params, self.auth = kind, path, dict(params or {}), auth
        self.tool, self.args = tool, args
        self.name = name or (f"mcp:{tool}:{sorted((args or {}).items())}" if kind == "mcp" else
                             f"{kind}:{path}?{'&'.join(f'{k}={v}' for k, v in self.params.items())}"
                             + (f" as {auth[0]}:{auth[1][:2]}" if auth else ""))

    def send(self, session, base, timeout=60):
        if self.kind == "mcp":
            return session.post(f"{base}/mcp/jsonrpc", timeout=timeout, json={
                "jsonrpc": "2.0", "id": 7, "method": "tools/call",
                "params": {"name": self.tool, "arguments": self.args or {}}})
        params = dict(self.params)
        if self.kind == "csv":
            params["format"] = "csv"
        if self.kind == "arrow":
            params["format"] = "arrow"
        return session.get(f"{base}{self.path}", params=params, auth=self.auth, timeout=timeout)


def pages(path, total, limit, extra=None):
    """first / middle / short last / exactly-full last / past end / limit 0 / limit 1."""
    extra = dict(extra or {})
    out = [("first", {}), ("first_l", {"limit": limit}), ("middle", {"limit": limit, "offset": limit})]
    if total > limit:
        short = total - (total % limit or limit // 2)
        out.append(("short_last", {"limit": limit, "offset": short}))
        out.append(("full_last", {"limit": limit, "offset": total - limit}))
    out += [("past_end", {"limit": limit, "offset": total + 5}), ("limit0", {"limit": 0}),
            ("limit1", {"limit": 1, "offset": 3})]
    return [(n, {**extra, **p}) for n, p in out]


def build_matrix(totals):
    """Every endpoint x pages x formats x parameters, plus MCP calls and auth outcomes.

    `totals` maps (path, frozenset(params)) -> total rows, read from the server first.
    """
    m = []

    def paged(path, extra=None, formats=("rest", "csv", "arrow"), limit=37, auth=None):
        total = totals[(path, tuple(sorted((extra or {}).items())))]
        for name, p in pages(path, total, limit, extra):
            for kind in formats:
                m.append(Req(kind, path, p, auth=auth))

    paged("/customers")
    paged("/customers", {"country": "DE"})
    paged("/customers", {"country": "JP", "min_balance": 1000.5}, limit=20)
    paged("/customers", {"name_prefix": "Zo"}, limit=10)
    paged("/customers", {"name_prefix": "plain"}, formats=("rest",), limit=100)
    paged("/orders", limit=100)
    paged("/orders", {"status": "paid", "from": "2024-06-01", "min_qty": 5}, limit=50)
    paged("/customers/42/orders", limit=3)
    paged("/customers/7/orders", {"status": "shipped"}, formats=("rest", "csv"), limit=2)
    paged("/types", formats=("rest", "csv"), limit=50)   # Arrow: INTERVAL fails (test_arrow_interval)
    paged("/stats/countries", limit=3)
    paged("/stats/countries", {"min_orders": 2900}, formats=("rest",), limit=3)
    paged("/documents", formats=("rest", "csv"), limit=25)
    paged("/documents", {"q": "Straße"}, formats=("rest",), limit=25)
    paged("/sales/daily", {"status": "returned"}, limit=200)
    paged("/admin/revenue", auth=ADMIN, limit=17)
    paged("/admin/revenue", {"category": "toys"}, auth=ANALYST, formats=("rest",), limit=10)
    for cid in (1, 7, 97, 2400, 2401):          # 2401 does not exist: an empty page
        for kind in ("rest", "csv", "arrow"):
            m.append(Req(kind, f"/customers/{cid}"))
    m.append(Req("rest", "/admin/customers/13/pii", auth=ADMIN))
    for kind in ("rest", "csv"):
        m.append(Req(kind, "/products"))
        m.append(Req(kind, "/products", {"category": "garden"}))
    # error answers are part of the contract too
    m += [Req("rest", "/admin/revenue"), Req("rest", "/admin/revenue", auth=VIEWER),
          Req("rest", "/admin/customers/13/pii", auth=ANALYST), Req("rest", "/admin/revenue", auth=("admin", "nope")),
          Req("rest", "/customers", {"country": "XX"}), Req("rest", "/customers", {"name_prefix": "x'; DROP"}), Req("rest", "/customers", {"name_prefix": "O'Brien"}),
          Req("rest", "/customers/abc"), Req("rest", "/customers/0"), Req("rest", "/orders", {"from": "2024-13-01"}),
          Req("rest", "/nope")]
    # MCP: the same queries as tools
    for tool, args in [("search_customers", {}), ("search_customers", {"country": "FR"}),
                       ("search_customers", {"country": "US", "min_balance": 500}),
                       ("get_customer", {"id": 1}), ("get_customer", {"id": 97}), ("get_customer", {"id": 99999}),
                       ("list_orders", {"status": "new", "min_qty": 11}), ("list_products", {}),
                       ("list_products", {"category": "office"}), ("country_stats", {}),
                       ("type_showcase", {}), ("search_documents", {"q": "daten"}), ("sales_daily", {"status": "paid"}),
                       ("get_customer", {"id": "x"}), ("no_such_tool", {})]:
        m.append(Req("mcp", None, tool=tool, args=args))
    seen = {}
    for r in m:                       # names key the serial checksums: keep them unique
        seen[r.name] = seen.get(r.name, 0) + 1
        if seen[r.name] > 1:
            r.name += f" #{seen[r.name]}"
    return m


def totals_needed():
    keys = [("/customers", {}), ("/customers", {"country": "DE"}),
            ("/customers", {"country": "JP", "min_balance": 1000.5}), ("/customers", {"name_prefix": "Zo"}),
            ("/customers", {"name_prefix": "plain"}), ("/orders", {}),
            ("/orders", {"status": "paid", "from": "2024-06-01", "min_qty": 5}),
            ("/customers/42/orders", {}), ("/customers/7/orders", {"status": "shipped"}), ("/types", {}),
            ("/stats/countries", {}), ("/stats/countries", {"min_orders": 2900}), ("/documents", {}),
            ("/documents", {"q": "Straße"}), ("/sales/daily", {"status": "returned"}),
            ("/admin/revenue", {}), ("/admin/revenue", {"category": "toys"})]
    return keys


def read_totals(base, session=None):
    """Exact row counts, from an unpaged walk (limit large) so no total_count trust is needed."""
    s = session or requests.Session()
    totals = {}
    for path, extra in totals_needed():
        auth = ADMIN if path.startswith("/admin") else None
        n, offset = 0, 0
        while True:
            r = s.get(f"{base}{path}", params={**extra, "limit": 1000, "offset": offset}, auth=auth, timeout=60)
            assert r.status_code == 200, (path, r.status_code, r.text[:300])
            got = len(r.json()["data"])
            n += got
            if got < 1000:
                break
            offset += 1000
        totals[(path, tuple(sorted(extra.items())))] = n
    return totals


VOLATILE_HEADERS = {"date", "x-request-id", "traceparent", "x-trace-id", "server", "connection", "keep-alive",
                    "content-length", "transfer-encoding", "x-response-time", "x-flapi-request-id",
                    "content-encoding", "vary", "mcp-session-id"}


def fingerprint(resp):
    """What must be identical between runs: status, stable headers, body bytes."""
    headers = {k.lower(): v for k, v in resp.headers.items() if k.lower() not in VOLATILE_HEADERS
               and not k.lower().startswith("x-ratelimit")}
    return resp.status_code, headers, resp.content


def arrow_table(body):
    import io
    import pyarrow.ipc as ipc
    return ipc.open_stream(io.BytesIO(body)).read_all()


def digest(resp):
    """Checksum of the response. Arrow IPC bytes are not stable run to run (padding bytes are not
    zeroed, in both builds), so an Arrow body is checksummed as its decoded table."""
    st, h, body = fingerprint(resp)
    if "arrow" in resp.headers.get("Content-Type", "") and st == 200:
        t = arrow_table(body)
        body = repr((t.schema, t.to_pylist())).encode()
    return hashlib.sha256(repr((st, sorted(h.items()))).encode() + body).hexdigest()
