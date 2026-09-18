// flAPI load scenarios — issue 0b.
//
// Deliberately a realistic MIX, not a single-endpoint burst: the point is to
// measure the request pipeline the observability epic is about to add a
// middleware to, under the shape of traffic flAPI actually sees.
//
// k6 runs OUT OF PROCESS, unlike the previous concurrent.futures + requests
// suite, which under the GIL measured Python rather than flAPI and is the most
// likely reason that suite "hangs in CI".
//
// Env:
//   BASE_URL   default http://127.0.0.1:8080
//   PROFILE    smoke | full          (default full)
//   VUS, DURATION                    (override the profile)
import http from 'k6/http';
import { check } from 'k6';
import { Trend, Counter } from 'k6/metrics';

// The unmatched-path scenario expects 404 by design. Without this, k6's default
// "2xx or bust" rule books those as http_req_failed and the error-rate threshold
// fires on correct behaviour.
http.setResponseCallback(http.expectedStatuses(200, 404));

const BASE = __ENV.BASE_URL || 'http://127.0.0.1:8080';
const PROFILE = __ENV.PROFILE || 'full';

// Per-scenario latency, so a regression can be attributed to a stage rather
// than smeared across one aggregate number.
const tRead      = new Trend('flapi_read_duration', true);
const tReadParam = new Trend('flapi_read_param_duration', true);
const tPaged     = new Trend('flapi_paged_duration', true);
const tArrow     = new Trend('flapi_arrow_duration', true);
const tMcp       = new Trend('flapi_mcp_duration', true);
const tHealth    = new Trend('flapi_health_duration', true);
const tUnmatched = new Trend('flapi_unmatched_duration', true);
const errors     = new Counter('flapi_errors');

function record(res, trend, expected) {
    // timings.waiting is time-to-first-byte: flAPI's own cost. timings.duration
    // additionally includes body transfer, which on loopback contributes a stable
    // ~46ms floor here and would bury a middleware costing microseconds. Measured:
    // per-scenario TTFB spread is ~0.1%, duration spread is dominated by that floor.
    trend.add(res.timings.waiting);
    const ok = check(res, {
        [`status is ${expected}`]: (r) => r.status === expected,
    });
    if (!ok) { errors.add(1); }
    return ok;
}

// Weighted to resemble production: reads dominate, health probes are frequent
// (in Kubernetes they are the highest-volume route), writes are rare.
const SMOKE = { vus: 5,  duration: '20s' };
const FULL  = { vus: 30, duration: '60s' };
// The sensitivity profile. Measured noise on this box: the aggregate p99 varies
// ~11% run-to-run, dominated by the Arrow scenario (13.4%), which makes it
// useless for detecting a middleware that costs microseconds. The cheap routes
// are both the most sensitive to a fixed per-request cost (proportionally
// largest) and by far the most stable (~0.6% spread). 'cheap' drops the
// expensive scenarios and the concurrency that makes them queue, so a
// per-request regression is visible well above the noise.
const CHEAP = { vus: 4,  duration: '30s' };
const base  = PROFILE === 'smoke' ? SMOKE : (PROFILE === 'cheap' ? CHEAP : FULL);

export const options = {
    vus:      parseInt(__ENV.VUS || base.vus, 10),
    duration: __ENV.DURATION || base.duration,
    // Thresholds are documentation, not the gate. The gate is compare.py against
    // a recorded baseline, because an absolute number is meaningless without one.
    thresholds: {
        'checks': ['rate>0.99'],
        'http_req_failed': ['rate<0.01'],
    },
    summaryTrendStats: ['avg', 'min', 'med', 'p(90)', 'p(95)', 'p(99)', 'max'],
    // Bodies are read (so serialization cost is real) but note that the gate keys
    // off http_req_waiting - time to first byte - not http_req_duration. Duration
    // includes body transfer, which measures the loopback and the client as much
    // as it measures flAPI.
    discardResponseBodies: false,
};

export default function () {
    // Sensitivity profile: only the cheap, low-variance routes, so that a fixed
    // per-request cost (a new middleware, a context object, an extra endpoint
    // lookup) is measurable rather than buried under Arrow serialization.
    if (PROFILE === 'cheap') {
        const c = Math.random();
        if (c < 0.40) {
            record(http.get(`${BASE}/health/live`, { tags: { scenario: 'health' } }), tHealth, 200);
        } else if (c < 0.70) {
            record(http.get(`${BASE}/${Math.random().toString(36).slice(2)}`,
                            { tags: { scenario: 'unmatched' } }), tUnmatched, 404);
        } else {
            record(http.get(`${BASE}/northwind/products/?limit=5`,
                            { tags: { scenario: 'read' } }), tRead, 200);
        }
        return;
    }

    const r = Math.random();

    if (r < 0.34) {
        // Plain list read - the bread-and-butter data endpoint.
        record(http.get(`${BASE}/northwind/products/`, { tags: { scenario: 'read' } }),
               tRead, 200);

    } else if (r < 0.50) {
        // Path-parameter read - exercises RouteTranslator's regex path, which is
        // where the pre-compiled-regex prerequisite (section 4.2) pays off.
        const id = 1 + Math.floor(Math.random() * 77);
        record(http.get(`${BASE}/northwind/products/${id}`, { tags: { scenario: 'read_param' } }),
               tReadParam, 200);

    } else if (r < 0.62) {
        // Paginated + filtered read - more validators, more template work.
        const off = Math.floor(Math.random() * 40);
        record(http.get(`${BASE}/northwind/products/?limit=10&offset=${off}&discontinued=0`,
                        { tags: { scenario: 'paged' } }), tPaged, 200);

    } else if (r < 0.70) {
        // Arrow IPC - the serialization path with its own counters.
        record(http.get(`${BASE}/northwind/orders/?limit=500`, {
            headers: { Accept: 'application/vnd.apache.arrow.stream' },
            tags: { scenario: 'arrow' },
        }), tArrow, 200);

    } else if (r < 0.78) {
        // Another table, to keep one query plan from dominating the profile.
        record(http.get(`${BASE}/northwind/customers/`, { tags: { scenario: 'read' } }),
               tRead, 200);

    } else if (r < 0.86) {
        // MCP tools/call - the agent surface, same execution core.
        const body = JSON.stringify({
            jsonrpc: '2.0', id: __ITER, method: 'tools/list', params: {},
        });
        record(http.post(`${BASE}/mcp/jsonrpc`, body, {
            headers: { 'Content-Type': 'application/json' },
            tags: { scenario: 'mcp' },
        }), tMcp, 200);

    } else if (r < 0.97) {
        // Health probes. In Kubernetes this is the highest-volume route, and it
        // is exactly the one a leftmost middleware must not tax (finding F2).
        record(http.get(`${BASE}/health/live`, { tags: { scenario: 'health' } }), tHealth, 200);

    } else {
        // Unmatched paths - scanner-shaped traffic. Must stay cheap and must
        // collapse to a single route label once tracing lands (BR-23).
        record(http.get(`${BASE}/${Math.random().toString(36).slice(2)}`,
                        { tags: { scenario: 'unmatched' } }), tUnmatched, 404);
    }
}
