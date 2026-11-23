"""Reused connections and cached prepared statements never serve rows from before a cache refresh (#241).

Every request thread keeps its DuckDB connection and its prepared SELECTs. An incremental (merge)
refresh changes a DuckLake table's data without any DDL; a cached plan that pinned the old snapshot
or file list would keep answering with the old rows on the threads that prepared it.
"""
import concurrent.futures

import pytest
import requests

from test_cache_catalog_f1 import (MERGE_ENDPOINT, MERGE_POPULATE, SERVE, base_config, root, servers,  # noqa: F401
                                   write_source, write_tree)

pytestmark = pytest.mark.standalone_server


def all_threads_get(base, n=64):
    # Many parallel requests so every handler thread prepares (and caches) the statement.
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        bodies = list(pool.map(lambda _: requests.get(f"{base}/merge", timeout=30).json()["data"], range(n)))
    return bodies


def test_cached_plans_see_an_incremental_refresh(root, servers):  # noqa: F811
    write_tree(root, base_config(), {"merge.yaml": MERGE_ENDPOINT, "merge_cache.sql": MERGE_POPULATE,
                                     "serve.sql": SERVE})
    write_source(root, [(1, "a", 1), (2, "b", 1)])
    s = servers(root)
    s.refresh("merge")
    before = all_threads_get(s.base)
    assert all([r["name"] for r in body] == ["a", "b"] for body in before), before[:2]

    write_source(root, [(1, "a", 1), (2, "b2", 2), (3, "c", 2)])
    s.refresh("merge")                      # MERGE: data changes, no DDL
    after = all_threads_get(s.base)
    stale = [body for body in after if [r["name"] for r in body] != ["a", "b2", "c"]]
    assert not stale, f"{len(stale)} of {len(after)} responses served pre-refresh rows: {stale[:1]}"


def test_cached_plans_see_a_full_rebuild(root, servers):  # noqa: F811
    write_tree(root, base_config(), {"merge.yaml": MERGE_ENDPOINT, "merge_cache.sql": MERGE_POPULATE,
                                     "serve.sql": SERVE})
    write_source(root, [(1, "a", 1)])
    s = servers(root)
    s.refresh("merge")
    all_threads_get(s.base)
    write_source(root, [(7, "z", 1)])
    s.refresh("merge", full=True)            # DROP + CREATE: a new table id
    after = all_threads_get(s.base)
    assert all([r["id"] for r in body] == [7] for body in after), after[:2]
