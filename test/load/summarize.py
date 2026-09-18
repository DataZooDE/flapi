#!/usr/bin/env python3
"""Aggregate N k6 summaries into one record, with the run-to-run spread.

The spread is the point: it is what issue 0a measures, and it decides whether a
wall-clock load gate can be tighter than the noise floor. A budget below the
measured variance is not a gate - it is a coin flip that will be waived.
"""
import argparse, json, statistics, sys

METRICS = [
    # http_req_waiting is time-to-first-byte: the server's own cost, excluding
    # body transfer. It is the primary signal; http_req_duration is kept for
    # context but is contaminated by response size and loopback bandwidth.
    "http_req_waiting", "http_req_duration", "flapi_read_duration", "flapi_read_param_duration",
    "flapi_paged_duration", "flapi_arrow_duration", "flapi_mcp_duration",
    "flapi_health_duration", "flapi_unmatched_duration",
]
STATS = ["avg", "med", "p(95)", "p(99)", "max"]


def extract(doc):
    out = {}
    metrics = doc.get("metrics", {})
    for name in METRICS:
        m = metrics.get(name)
        if not m:
            continue
        out[name] = {s: m[s] for s in STATS if s in m}
    reqs = metrics.get("http_reqs", {})
    out["throughput_rps"] = reqs.get("rate", 0.0)
    out["iterations"] = metrics.get("iterations", {}).get("count", 0)
    failed = metrics.get("http_req_failed", {})
    out["error_rate"] = failed.get("value", failed.get("rate", 0.0))
    out["server_peak_rss_kib"] = doc.get("flapi", {}).get("server_peak_rss_kib", 0)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()

    runs = [extract(json.load(open(f))) for f in a.files]
    agg = {"runs": len(runs), "per_run": runs, "median": {}, "spread_pct": {}}

    def collect(path):
        vals = []
        for r in runs:
            cur = r
            for key in path:
                if not isinstance(cur, dict) or key not in cur:
                    return None
                cur = cur[key]
            vals.append(cur)
        return vals or None

    paths = [(m, s) for m in METRICS for s in STATS] + \
            [("throughput_rps",), ("error_rate",), ("server_peak_rss_kib",)]

    for path in paths:
        vals = collect(path)
        if not vals:
            continue
        key = ".".join(path)
        med = statistics.median(vals)
        agg["median"][key] = med
        # Spread as a percentage of the median: the honest noise figure.
        if med:
            agg["spread_pct"][key] = round((max(vals) - min(vals)) / med * 100, 2)
        else:
            agg["spread_pct"][key] = 0.0

    json.dump(agg, open(a.out, "w"), indent=2)

    p99 = agg["spread_pct"].get("http_req_waiting.p(99)")
    print(f"runs={len(runs)}  median server p99={agg['median'].get('http_req_waiting.p(99)', 0):.2f}ms  "
          f"p99 spread={p99}%  rps={agg['median'].get('throughput_rps'):.1f}", file=sys.stderr)
    if p99 is not None:
        print(f"\nNOISE FLOOR: p99 varies {p99}% run-to-run on this machine.", file=sys.stderr)
        print(f"Any wall-clock gate tighter than ~{max(p99, 1.0) * 2:.0f}% is below the noise.",
              file=sys.stderr)


if __name__ == "__main__":
    main()
