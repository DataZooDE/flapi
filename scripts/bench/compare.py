#!/usr/bin/env python3
"""Compare two bench.py results. Exit 1 when the new binary regresses past the gates."""
import json
import sys

GATES = {"p95": 1.10, "rps": 0.90, "startup": 1.10, "idle_rss": 1.15, "peak_rss": 1.20}


def main():
    old, new = (json.load(open(p)) for p in sys.argv[1:3])
    bad = []

    def check(name, o, n, limit, lower_is_better=True):
        if not o or not n:
            return
        ratio = n / o
        worse = ratio > limit if lower_is_better else ratio < limit
        print(f"{'FAIL' if worse else 'ok  '} {name:28s} {o:12.2f} -> {n:12.2f}  x{ratio:.2f}")
        if worse:
            bad.append(name)

    check("startup_s", old["startup_s"], new["startup_s"], GATES["startup"])
    check("idle_rss_kb", old["idle_rss_kb"], new["idle_rss_kb"], GATES["idle_rss"])
    for k, o in old["scenarios"].items():
        n = new["scenarios"].get(k)
        if not n:
            continue
        check(f"{k} p95_ms", o["p95_ms"], n["p95_ms"], GATES["p95"])
        check(f"{k} rps", o["rps"], n["rps"], GATES["rps"], lower_is_better=False)
        check(f"{k} peak_rss_kb", o["peak_rss_kb"], n["peak_rss_kb"], GATES["peak_rss"])
        if n["errors"] > o["errors"]:
            print(f"FAIL {k} errors {o['errors']} -> {n['errors']}")
            bad.append(k + " errors")
    print("RESULT:", "regression in " + ", ".join(bad) if bad else "within thresholds")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
