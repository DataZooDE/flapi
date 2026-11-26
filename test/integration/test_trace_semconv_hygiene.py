"""Span attributes flAPI declares are attributes it sets (#279).

`flapi.result.bytes` was declared in trace_semconv.hpp, and a comment in
query_executor.cpp said it "carries the response size", but nothing ever set
it. The size is `http.response.body.size` on the request span. A declared but
unset attribute is how the docs came to promise it.
"""
import os
import pathlib

REPO = pathlib.Path(__file__).resolve().parents[2]
SRC = REPO / "src"


def _sources():
    for path in SRC.rglob("*"):
        if path.suffix in (".cpp", ".hpp", ".h"):
            yield path


def test_flapi_result_bytes_is_not_declared_or_promised():
    offenders = [str(p.relative_to(REPO)) for p in _sources()
                 if "flapi.result.bytes" in p.read_text(errors="replace")
                 or "kResultBytes" in p.read_text(errors="replace")]
    assert not offenders, (
        "flapi.result.bytes is never set; the response size is "
        f"http.response.body.size. Still named in: {offenders}")


def test_the_response_size_attribute_is_set():
    text = (SRC / "request_context_middleware.cpp").read_text()
    assert "semconv::http::kResponseBodySize" in text
