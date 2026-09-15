# `test/load/` — flAPI load harness

The performance gate for the observability epic, and the replacement for the
load suite that was `pytest.mark.skip`-ped, `--ignore`-d in CI, and carrying an
unresolved `xfail` for *"server performance degrades under high concurrent
load"*.

## Why k6 and not the previous Python suite

`test_load_testing.py` generated load with `concurrent.futures` + `requests`.
Under the GIL that measures Python, not flAPI, and it is the most plausible
reason the suite "hangs in CI". k6 runs the load out of process.

## Usage

```bash
make load-test          # cheap profile, 3 runs, gated against the newest baseline
make load-test-full     # the full mix
make load-baseline      # record a NEW baseline at the current commit
make perf-proxy         # the allocation-count gate

# or directly, e.g. with a vendored k6:
K6_BIN=/path/to/k6 ./test/load/run_load.sh --profile cheap --runs 5 --out out.json
python3 test/load/compare.py --baseline test/load/baselines/<sha>-cheap.json --current out.json
```

k6 is not vendored into the repo. Download the static binary and point `K6_BIN`
at it:

```bash
curl -sSL https://github.com/grafana/k6/releases/download/v0.55.0/k6-v0.55.0-linux-amd64.tar.gz | tar xz
```

## Profiles

| Profile | Shape | Purpose |
|---|---|---|
| `cheap` | 4 VUs, 30 s, only health / unmatched / small read | **The sensitive gate.** Isolates per-request cost from serialization. |
| `smoke` | 5 VUs, 20 s, full mix | Quick end-to-end check that the harness and server work. |
| `full` | 30 VUs, 60 s, full mix | Reads, path params, pagination, Arrow IPC, MCP, health probes, unmatched paths. Catches gross regressions and exercises contention. |

Each run gets its own copy of `examples/`, because the DuckLake paths in
`examples/flapi.yaml` are relative to the config file and two runs sharing them
fight over a DuckDB file lock.

## What these gates can and cannot resolve

Measured on this repo at `1b655a0` (5 runs, dedicated local machine — a shared
CI runner will be worse):

| Statistic of `flapi_health_duration` | Median | Run-to-run spread |
|---|---|---|
| **`med`** | **0.200 ms** | **6.7 %** |
| `avg` | 0.216 ms | 20.9 % |
| `p(95)` | 0.277 ms | 23.3 % |
| `p(99)` | 0.350 ms | 195.6 % |

And across scenarios, `p(99)` of the aggregate varies **11 %**, dominated by
Arrow serialization at **63 %**.

Three conclusions, all of which shaped the gate:

1. **Gate on the median of time-to-first-byte, not p99.** On a sub-millisecond
   metric a single scheduling outlier moves p99 by 200 %.
2. **Gate on the cheap routes, not the aggregate.** A fixed per-request cost is
   proportionally largest on the cheapest request, and the aggregate is buried
   under Arrow.
3. **Measure the server, not the wire.** `timings.waiting` (TTFB) is flAPI's own
   cost; `timings.duration` adds body transfer, which contributes a stable ~46 ms
   loopback floor here and would hide a middleware costing microseconds.

**The limit, stated plainly:** this harness can see a per-request regression of
roughly **tens of microseconds** on a 200 µs request. It cannot see single-digit
microseconds. So the epic's "≤ 0.5 %" and "zero cost when disabled" claims are
**not** carried by wall-clock load testing — they are carried by
`test/cpp/test_alloc_budget.cpp`, which counts allocations deterministically.
A wall-clock budget tighter than the noise floor is not a gate; it is a coin
flip that gets waived, which is exactly how the previous suite was lost.

## Gate semantics

`compare.py` fails a build when a watched metric regresses past its budget. The
effective budget is `max(requested, 2 × measured noise for that metric)`, so a
gate can never be tighter than the machine can resolve. Throughput is inverted —
a *drop* is the regression.

## Baselines

`baselines/<sha>-<profile>.json`, produced by `make load-baseline`. Record one
deliberately: a baseline captured after a regression has landed silently blesses
that regression. Each file holds every run, the median, and the spread.
