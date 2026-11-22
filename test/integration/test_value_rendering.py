"""Value rendering and Arrow fixes found by the realistic suite (#241).

* INTERVAL renders as DuckDB's own text ("3 days 04:00:00"), not "months:days.millis";
* an Arrow stream is limited by what it holds, not by 100 bytes per row (every result over ~2.7M rows
  failed with 'Exceeded memory limit', however narrow);
* Arrow honours an explicit ?limit= / ?offset= and otherwise still streams the full result.
"""
import io

import pytest
import requests

from test_realistic_differential import mini_server

pa = pytest.importorskip("pyarrow")
import pyarrow.ipc as ipc  # noqa: E402


def arrow(url):
    r = requests.get(url, headers={"Accept": "application/vnd.apache.arrow.stream"}, timeout=120)
    assert r.status_code == 200, r.text[:300]
    return ipc.open_stream(io.BytesIO(r.content)).read_all()


def test_intervals_render_as_duckdb_text(tmp_path):
    run = mini_server(tmp_path, "SELECT INTERVAL '3 days 4 hours' AS a, INTERVAL '14 months' AS b, "
                                "INTERVAL '1.5 seconds' AS c\n")
    try:
        row = requests.get(f"{run.base}/q", timeout=30).json()[0]
        table = arrow(f"{run.base}/q")
    finally:
        run.stop()
    assert row == {"a": "3 days 04:00:00", "b": "1 year 2 months", "c": "00:00:01.5"}, row
    assert table.column("a").to_pylist() == ["3 days 04:00:00"]


def test_a_narrow_arrow_result_over_three_million_rows_streams(tmp_path):
    run = mini_server(tmp_path, "SELECT range::TINYINT AS b FROM range(3000000) WHERE true AND range < 3000000\n"
                      .replace("range::TINYINT", "(range % 100)::TINYINT"))
    try:
        table = arrow(f"{run.base}/q")
    finally:
        run.stop()
    assert table.num_rows == 3_000_000


def test_arrow_pages_only_when_the_client_asks(tmp_path):
    run = mini_server(tmp_path, "SELECT range AS id FROM range(1000) ORDER BY id\n", paginated=True)
    try:
        full = arrow(f"{run.base}/q")
        page = arrow(f"{run.base}/q?limit=10&offset=20")
        json_page = requests.get(f"{run.base}/q", timeout=30).json()
    finally:
        run.stop()
    assert full.num_rows == 1000                       # no silent 100-row default for Arrow
    assert page.column("id").to_pylist() == list(range(20, 30))
    assert len(json_page["data"]) == 100              # JSON keeps its default page


def test_doubles_in_exponent_form_keep_their_exponent(tmp_path):
    # crow 1.2's trailing-zero trimmer cut the exponent: 1e300 came out as 1000.0 (crow 1.3.4 fixed it).
    run = mini_server(tmp_path, "SELECT 1e300::DOUBLE AS big, 1.5e-7::DOUBLE AS small, [2e21::DOUBLE] AS l\n")
    try:
        row = requests.get(f"{run.base}/q", timeout=30).json()[0]
    finally:
        run.stop()
    assert row == {"big": 1e300, "small": 1.5e-7, "l": [2e21]}, row
