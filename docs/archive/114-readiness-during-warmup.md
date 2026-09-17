# Implementation plan — #114: serve health checks during cache warmup

**Issue:** [#114](https://github.com/DataZooDE/flapi/issues/114) — cache warmup blocks HTTP
server startup, making the service undeployable on platforms with a capped health-check window.

**Branch:** `fix/114-readiness-during-warmup`

**Method:** strict red/green TDD. Every step below names the failing test to write *first*.
Definition of done is the **integration** suite (`test/integration/`, real binary over real HTTP),
not unit tests alone.

---

## 1. The defect, verified in source

```
main()                                    src/main.cpp
  └─ initializeDatabase(config)
       └─ DatabaseManager::initializeDBManagerFromConfig()   database_manager.cpp:116  lock_guard(db_mutex)
            └─ cache_manager->warmUpCaches(config_manager)   database_manager.cpp:170  ← still under the lock
  ...
  └─ std::thread unified_server_thread                        main.cpp  ← listening socket opens ONLY here
```

Consequences:

1. The socket does not open until every cache-enabled endpoint has finished warming. A platform
   health check has nothing to connect to, so a warmup longer than the platform's maximum window
   makes the service **permanently undeployable** — every attempt fails and rolls back.
2. There is **no general health endpoint**. `/api/v1/_config/health` requires `--config-service`;
   `/mcp/health` is MCP-only. Operators are forced to point health checks at a data endpoint such
   as `/doc`, which is exactly what cannot answer during warmup.
3. `warmUpCaches()` is a sequential loop, so the startup cost is the **sum** of all caches.

## 2. What this PR changes, and what it explicitly does not

**Does:** open the listening socket *before* warmup; add an always-on health endpoint that
distinguishes `starting` from `ready`; return `503 + Retry-After` on data endpoints whose cache is
not yet populated; run warmup on a background thread; prevent the new concurrency hazards that
opening the socket early creates.

**Does not:** parallelise warmup across caches. That is a separate change with its own risks
(see §9) and it would not fix #114 anyway — the reporter has a single slow cache, and parallelism
across caches cannot help that. Keep the two apart so this PR stays reviewable.

**Preserves:** an endpoint never serves from a half-built cache. Answering requests early with
empty results would be a worse bug than the one being fixed.

---

## 3. Design

### 3.1 Startup order

```
initializeDatabase()          // NO warmup inside; connections/extensions only
create APIServer
start unified_server_thread   // socket opens here, immediately
start warmup thread           // CacheManager::warmUpCachesAsync()
join server thread
```

`warmUpCaches()` keeps its current synchronous signature and behaviour; a new
`warmUpCachesAsync()` wraps it on a `std::thread`. Moving the call out of
`initializeDBManagerFromConfig()` also takes it out from under `db_mutex` (`database_manager.cpp:116`),
which is a prerequisite for anything that later runs concurrently — a warmup worker reaching
`getConnection()` (`:346`, reachable via `vfs_adapter.cpp:252` when `template.path` is remote)
would otherwise deadlock against the lock held by the main thread.

### 3.2 Readiness state

New `CacheReadiness` owned by `CacheManager`, guarded by its own mutex:

| state | meaning |
|---|---|
| `starting` | warmup thread running, this cache not yet built |
| `ready` | cache populated successfully |
| `failed` | this cache's build failed (message retained) |

Keyed by `(catalog, schema, table)`. Endpoints without a cache block are always `ready`.
An endpoint that *reads* cache tables owned by other endpoints is ready only when all of them are
— otherwise `on-error: continue` reproduces #114's symptom: a healthy-looking endpoint answering
nothing.

### 3.3 Health endpoint

`GET /health` — always registered, no auth, no config service required.

```
200 {"status":"ready",   "caches":{"total":4,"ready":4,"failed":0},"uptime_s":31}
503 {"status":"starting","caches":{"total":4,"ready":1,"failed":0},"uptime_s":7,
     "pending":["norm_product_part","norm_part_frag","norm_locale"]}
503 {"status":"degraded","caches":{"total":4,"ready":3,"failed":1},
     "failed":[{"table":"norm_part_frag","error":"..."}]}
```

Rationale for 503 while starting: platforms treat 2xx as healthy. A deployment should not be
declared healthy before it can serve. What #114 needs is that the check gets an *answer* — a
refused connection is indistinguishable from a crash, a 503 is not. Operators who want the
deployment to succeed before caches finish can point the platform check at
`GET /health/live` (below) instead.

`GET /health/live` — liveness only. `200` as soon as the process is up, regardless of cache state.
This is the endpoint Rita's App Runner config should use; it is what makes the deployment succeed.

### 3.4 Data endpoints during warmup

A request to an endpoint whose cache is not `ready`:

```
503 Service Unavailable
Retry-After: 5
{"error":"cache_warming","message":"Cache for this endpoint is still being built",
 "table":"norm_product_part"}
```

Never a partial or empty result set. `failed` caches also return 503, with the error message, so a
broken cache is visible rather than silently empty.

### 3.5 New concurrency hazards created by opening the socket early

These do not exist today and are introduced by this change, so they are in scope:

1. **`HeartbeatWorker` collides with warmup.** `heartbeat_worker.cpp:81` calls
   `refreshCache()` on its own thread. Once the socket opens before warmup finishes, a scheduled
   refresh can issue a second `CREATE OR REPLACE TABLE` against a table a warmup worker is
   building. **Fix:** an in-flight registry keyed by `(catalog, schema, table)`, entered inside
   `CacheManager::refreshCache()` (not `warmUpCaches()`, since the heartbeat calls the former
   directly). A duplicate concurrent refresh is a logged no-op.
2. **Request threads read readiness while the warmup thread writes it.** Guard `CacheReadiness`
   with a mutex; keep the critical section to a map lookup.
3. **Warmup failure must not kill the process.** `refreshDuckLakeCache()` rethrows and nothing
   catches it; on the main thread today that hits `std::set_terminate` → `abort()`. On a
   background thread an escaping exception terminates the process with no log line. The warmup
   thread body must be `noexcept` at the boundary: catch, record `failed` + message, continue to
   the next cache.

---

## 4. TDD sequence — write each test first, watch it fail, then implement

### Step 1 — liveness endpoint exists (C++ unit)
**Red:** `test/cpp/health_endpoint_test.cpp` — `GET /health/live` returns 200 on a server with no
caches configured. Fails: route does not exist.
**Green:** register the route in `APIServer`.

### Step 2 — health reports cache counts (C++ unit)
**Red:** with two cache-enabled endpoints and a stub `CacheManager` reporting one ready, `GET /health`
returns 503, `status=starting`, `caches.ready=1`, `caches.total=2`.
**Green:** implement `CacheReadiness` + the `/health` handler.

### Step 3 — readiness transitions (C++ unit)
**Red:** `test/cpp/cache_readiness_test.cpp` — `starting → ready` on success; `starting → failed`
with the message on throw; unknown table defaults to `ready` (no cache block).
**Green:** implement the state map.

### Step 4 — warmup failure does not propagate (C++ unit)
**Red:** a stub adapter whose refresh throws; assert `warmUpCaches()` returns normally, marks that
cache `failed`, and still processes the *next* endpoint.
**Green:** wrap the per-endpoint call in try/catch.

### Step 5 — data endpoint 503s while warming (C++ unit)
**Red:** request an endpoint whose cache is `starting` → 503, `Retry-After` present, body
`error=cache_warming`. Assert the query was **not** executed.
**Green:** readiness check in the request path before query execution.

### Step 6 — in-flight registry (C++ unit)
**Red:** `test/cpp/cache_inflight_test.cpp` — two concurrent `refreshCache()` calls for the same
`(catalog, schema, table)`; assert the adapter executes **once** and the second returns a no-op.
Then assert two *different* tables both execute.
**Green:** implement the registry inside `refreshCache()`.

### Step 7 — socket opens before warmup (INTEGRATION — the test that proves #114 is fixed)
**Red:** `test/integration/test_warmup_readiness.py`

```python
def test_health_answers_during_warmup(slow_cache_server):
    # fixture: real flapi binary, config with a cache-populate template that
    # takes ~15s (e.g. a generate_series + heavy aggregate), started in background
    t0 = time.time()
    deadline = t0 + 10          # far below the cache build time
    while time.time() < deadline:
        try:
            r = requests.get(f"{base}/health/live", timeout=1)
            assert r.status_code == 200
            assert time.time() - t0 < 10     # answered long before warmup finished
            return
        except requests.ConnectionError:
            time.sleep(0.25)
    pytest.fail("server never accepted a connection during warmup")
```

Fails today with `ConnectionError` for the whole window — the exact production symptom.

### Step 8 — data endpoint 503 then 200 (INTEGRATION)
**Red:** against the same fixture, poll the data endpoint: assert it returns **503 with
`Retry-After`** while `/health` reports `starting`, and **200 with correct data** once `/health`
reports `ready`. Assert it never returns 200 with an empty result.

### Step 9 — health reflects a failed cache (INTEGRATION)
**Red:** a config whose cache template is deliberately invalid SQL. Assert the process stays up,
`/health/live` is 200, `/health` is 503 `degraded` naming the failed table, and the endpoint
returns 503 with the error — not 200-empty, and not a crashed container.

### Step 10 — heartbeat does not collide (INTEGRATION)
**Red:** a short `schedule` on a slow cache so a scheduled refresh fires during warmup. Assert no
error in the log, the cache ends `ready`, and the adapter ran the populate once.

---

## 5. Definition of done

- [ ] Steps 7–10 (integration) pass against a **real binary over real HTTP**; steps 1–6 green.
- [ ] `make test-all` passes — no regressions in the existing C++ and integration suites.
- [ ] A server with **no** cache-enabled endpoints behaves exactly as before (timing and routes).
- [ ] `/health/live` answers within 2 s of process start on the slow-cache fixture.
- [ ] No endpoint ever returns 200 with an empty body due to an unbuilt cache.
- [ ] Warmup failure leaves the process running and surfaces in `/health`.
- [ ] `docs/CONFIG_REFERENCE.md` and `docs/CLI_REFERENCE.md` document `/health`, `/health/live`,
      and the 503 contract, with an App Runner example pointing at `/health/live`.
- [ ] `CHANGELOG.md` entry.

---

## 6. Files expected to change

| File | Change |
|---|---|
| `src/main.cpp` | start server thread before warmup; launch warmup thread |
| `src/database_manager.cpp` | remove `warmUpCaches()` from `initializeDBManagerFromConfig()` (out from under `db_mutex`) |
| `src/cache_manager.cpp/.hpp` | `warmUpCachesAsync()`, readiness state, in-flight registry, per-endpoint try/catch |
| `src/api_server.cpp/.hpp` | register `/health`, `/health/live` |
| `src/request_handler.cpp` | readiness gate before query execution |
| `src/heartbeat_worker.cpp` | honour the in-flight registry |
| `test/cpp/*` | steps 1–6 |
| `test/integration/test_warmup_readiness.py` + fixture config | steps 7–10 |
| `docs/`, `CHANGELOG.md` | as above |

---

## 7. Constraints that must not be violated

1. **Never serve a partial cache.** 503 is the only acceptable answer for an unready endpoint.
2. **Never let an exception escape the warmup thread.**
3. **Do not modify `db_mutex`'s scope in this PR.** Moving the warmup call out from under it is
   sufficient here; narrowing the lock is a separate change with its own review.
4. **`concurrency` stays 1.** No parallel warmup in this PR.
5. **No behaviour change when no caches are configured.**
6. Endpoints without a `cache:` block must not acquire any new lock on the request path.

---

## 8. Risks

| Risk | Mitigation |
|---|---|
| Health check returns 200 too early; platform routes traffic to an unready service | `/health` is 503 until ready; only `/health/live` is unconditionally 200, and it is documented as liveness |
| Readiness lookup on every request adds latency | map lookup under a short-held mutex; skip entirely for endpoints without a cache block |
| Heartbeat/warmup collision corrupts a cache | in-flight registry (step 6) |
| Background warmup hides failures | `/health` reports `degraded` with the message; failures also logged at ERROR |
| Existing deployments depending on "socket closed until ready" as a readiness signal | documented in CHANGELOG as a behaviour change; `/health` preserves the semantics for anyone who needs them |

---

## 9. Follow-up, explicitly out of scope

- Parallel warmup (`cache.warmup.concurrency`). Bounded by `sum(tᵢ)/max(tᵢ)`, which is ≈1.0 for
  single-cache deployments; needs the in-flight registry from this PR, a conservative default of
  1, and care with memory — measured single builds have peaked at 1.9–3.6× their configured
  `memory_limit`.
- Narrowing `db_mutex` to the `db` handle lifecycle.
- `recordSyncEvent()` builds its INSERT by string concatenation with only a quote-swap on
  `message`, despite a prepared-statement template being written and discarded
  (`cache_manager.cpp:309-329`).
