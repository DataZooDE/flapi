#!/usr/bin/env python3
"""Ship daily PyPI download counts to PostHog.

Pulls per-day download totals (mirrors excluded) from the pypistats.org API and
sends one ``pypi_download_daily`` event per (package, day) to PostHog's
``/batch/`` endpoint.

Idempotency: every run re-sends the last ``LOOKBACK_DAYS`` complete days with a
deterministic ``uuid5(package, date)``; PostHog de-duplicates on the event UUID,
so a delayed, skipped or doubled cron run does not produce duplicate days.

Stdlib only -- no dependencies to resolve in CI.

Usage:
    POSTHOG_API_KEY=phc_... python scripts/pypi_to_posthog.py <package> [<package> ...]
    python scripts/pypi_to_posthog.py --dry-run <package>

Environment:
    POSTHOG_API_KEY   project (write-only, ``phc_``) key -- required unless --dry-run
    POSTHOG_HOST      ingestion host, default https://eu.i.posthog.com
    LOOKBACK_DAYS     how many complete days to (re)send, default 7
"""

from __future__ import annotations

import datetime as dt
import json
import os
import sys
import time
import urllib.error
import urllib.request
import uuid

PYPISTATS = "https://pypistats.org/api/packages/{pkg}/overall?mirrors=false"
USER_AGENT = "pypi-to-posthog/1.0 (+https://github.com/DataZooDE)"
EVENT_NAME = "pypi_download_daily"
DEFAULT_HOST = "https://eu.i.posthog.com"
DEFAULT_LOOKBACK = 7


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def http_get_json(url: str, retries: int = 5) -> dict:
    """GET with exponential backoff; pypistats rate-limits aggressively (429)."""
    delay = 5.0
    for attempt in range(1, retries + 1):
        req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return json.load(resp)
        except urllib.error.HTTPError as e:
            if e.code == 429 and attempt < retries:
                log(f"  429 from pypistats, retry {attempt}/{retries} in {delay:.0f}s")
                time.sleep(delay)
                delay *= 2
                continue
            raise
    raise RuntimeError("unreachable")


def fetch_daily(pkg: str, lookback_days: int, today: dt.date) -> list[tuple[dt.date, int]]:
    """Return (date, downloads) for complete days inside the lookback window."""
    payload = http_get_json(PYPISTATS.format(pkg=pkg))
    first = today - dt.timedelta(days=lookback_days)
    rows = []
    for row in payload.get("data", []):
        if row.get("category") != "without_mirrors":
            continue
        day = dt.date.fromisoformat(row["date"])
        if first <= day < today:  # strictly before today: yesterday is the newest complete day
            rows.append((day, int(row["downloads"])))
    rows.sort()
    return rows


def build_event(pkg: str, day: dt.date, downloads: int) -> dict:
    iso = day.isoformat()
    return {
        "event": EVENT_NAME,
        "uuid": str(uuid.uuid5(uuid.NAMESPACE_URL, f"pypi:{pkg}:{iso}")),
        # 12:00 UTC lands on the right calendar day in any project timezone.
        "timestamp": f"{iso}T12:00:00Z",
        "properties": {
            "distinct_id": f"pypi:{pkg}",
            "package": pkg,
            "downloads": downloads,
            "date": iso,
            "mirrors": False,
            "source": "pypistats",
            "$process_person_profile": False,
        },
    }


def send_batch(host: str, api_key: str, events: list[dict]) -> None:
    body = json.dumps({"api_key": api_key, "batch": events}).encode()
    req = urllib.request.Request(
        host.rstrip("/") + "/batch/",
        data=body,
        headers={"Content-Type": "application/json", "User-Agent": USER_AGENT},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        if not 200 <= resp.status < 300:
            raise RuntimeError(f"PostHog returned HTTP {resp.status}")


def main(argv: list[str]) -> int:
    dry_run = "--dry-run" in argv
    packages = [a for a in argv if not a.startswith("--")]
    if not packages:
        log(__doc__)
        return 2

    host = os.environ.get("POSTHOG_HOST", DEFAULT_HOST)
    api_key = os.environ.get("POSTHOG_API_KEY", "")
    lookback = int(os.environ.get("LOOKBACK_DAYS", DEFAULT_LOOKBACK))
    if not dry_run and not api_key:
        log("POSTHOG_API_KEY is not set (use --dry-run to skip sending)")
        return 2

    today = dt.datetime.now(dt.timezone.utc).date()
    failures = 0
    for i, pkg in enumerate(packages):
        if i:
            time.sleep(2)  # be polite to pypistats
        try:
            rows = fetch_daily(pkg, lookback, today)
        except Exception as e:  # noqa: BLE001
            log(f"{pkg}: fetch failed: {e}")
            failures += 1
            continue
        events = [build_event(pkg, d, n) for d, n in rows]
        if not events:
            log(f"{pkg}: no complete days in the last {lookback} days")
            continue
        span = f"{rows[0][0]} .. {rows[-1][0]}"
        if dry_run:
            log(f"{pkg}: {len(events)} event(s); {span} (dry-run)")
            for ev in events:
                print(json.dumps(ev, separators=(",", ":")))
            continue
        try:
            send_batch(host, api_key, events)
        except Exception as e:  # noqa: BLE001
            log(f"{pkg}: send failed: {e}")
            failures += 1
            continue
        log(f"sent {len(events)} events for {pkg} ({span})")

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
