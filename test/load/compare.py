#!/usr/bin/env python3
"""Compare a load run against a recorded baseline and fail on regression.

This is the gate referenced by the epic's definition of done. It is mechanical on
purpose: a budget argued in review is a budget that gets waived.

Budgets are expressed as a percentage regression against the baseline median, and
the effective budget is max(requested, 2 x measured noise floor) so a gate can
never be tighter than the machine can resolve.
"""
import argparse, json, sys

# Which metric resolves a regression, measured on this repo (issue 0a):
#
#   stat   flapi_health_duration   run-to-run spread
#   med           0.200 ms               6.7%
#   avg           0.216 ms              20.9%
#   p(95)         0.277 ms              23.3%
#   p(99)         0.350 ms             195.6%
#
# The MEDIAN of time-to-first-byte on the cheap routes is the gate. p99 is
# worthless on a sub-millisecond metric - a single scheduling outlier moves it by
# 200% - and the aggregate p99 is dominated by Arrow serialization (63% spread).
#
# What this gate CAN see: a per-request cost of tens of microseconds on a 200us
# request. What it CANNOT see: single-digit microseconds. The plan's +-0.5%
# claims are therefore carried by the proxy gates (allocation counts, instruction
# counts), never by wall-clock. That is a measured limit, not a preference.
WATCHED = [
    ("flapi_health_duration.med",     "health probe TTFB   [sensitive]"),
    ("flapi_unmatched_duration.med",  "unmatched TTFB      [sensitive]"),
    ("flapi_read_duration.med",       "read TTFB           [sensitive]"),
    ("flapi_mcp_duration.med",        "mcp TTFB            [sensitive]"),
    ("flapi_read_param_duration.med", "path-param TTFB     [sensitive]"),
    ("throughput_rps",                "throughput"),
    ("http_req_waiting.med",          "overall TTFB        [coarse]"),
    ("server_peak_rss_kib",           "peak RSS"),
]

# Throughput regressing is a *decrease*; everything else regresses upward.
LOWER_IS_WORSE = {"throughput_rps"}


def median_of(doc, key):
    if "median" in doc:
        return doc["median"].get(key)
    # A single-run k6 summary, not an aggregate.
    if "." in key:
        metric, stat = key.rsplit(".", 1)
        return doc.get("metrics", {}).get(metric, {}).get(stat)
    return doc.get("flapi", {}).get(key)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--current", required=True)
    ap.add_argument("--budget-pct", type=float, default=15.0,
                    help="allowed regression vs baseline median (default 15%%, ~2x the\n"
                         "measured noise floor on the sensitive TTFB medians)")
    ap.add_argument("--rss-budget-pct", type=float, default=35.0,
                    help="RSS is the noisiest signal here (~29%% spread); coarse net only")
    a = ap.parse_args()

    base = json.load(open(a.baseline))
    cur = json.load(open(a.current))
    noise = base.get("spread_pct", {})

    print(f"{'metric':<26} {'baseline':>12} {'current':>12} {'delta':>9} {'budget':>8}  result")
    print("-" * 82)

    failures = []
    for key, label in WATCHED:
        b, c = median_of(base, key), median_of(cur, key)
        if b is None or c is None or not b:
            print(f"{label:<26} {'n/a':>12} {'n/a':>12} {'-':>9} {'-':>8}  SKIP")
            continue

        requested = a.rss_budget_pct if key == "server_peak_rss_kib" else a.budget_pct
        # Never gate tighter than the machine can resolve.
        effective = max(requested, 2 * noise.get(key, 0.0))
        delta = (c - b) / b * 100.0
        if key in LOWER_IS_WORSE:
            delta = -delta   # a throughput drop is the regression
        ok = delta <= effective
        if not ok:
            failures.append((label, delta, effective))
        unit = "KiB" if key == "server_peak_rss_kib" else ("rps" if key == "throughput_rps" else "ms")
        print(f"{label:<26} {b:>9.2f}{unit:>3} {c:>9.2f}{unit:>3} "
              f"{delta:>+8.2f}% {effective:>7.1f}%  {'ok' if ok else 'FAIL'}")

    print()
    if failures:
        print("LOAD GATE FAILED:", file=sys.stderr)
        for label, delta, budget in failures:
            print(f"  {label} regressed {delta:+.2f}% (budget {budget:.1f}%)", file=sys.stderr)
        return 1

    print("LOAD GATE PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
