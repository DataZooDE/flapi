#!/usr/bin/env python3
"""Every data member of crow::response must survive the handler offload.

The offload in api_server.cpp runs a handler on a pool thread against a LOCAL
crow::response, then posts the result back to the connection's io thread.
Completer::take has to carry every field a handler may have set.

Three were not. Dropping `compressed` silently corrupted Arrow IPC responses:
the Arrow path sets compressed=false (its payload has its own LZ4/ZSTD framing)
and an explicit Content-Length; the flag was lost, so Crow gzipped the body
while the length header described the uncompressed size and every client read a
truncated stream. It cost 15 of 18 Arrow integration tests, and two reviews
missed it.

A field added by a future crow version would be dropped the same way, compile
cleanly, and fail only at runtime on whichever feature uses it. This fails the
build instead.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Fields the offload is known to carry. Keep in sync with Completer::take.
CARRIED = {"code", "body", "headers", "compressed", "skip_body",
           "manual_length_header"}
# Of those, the ones assigned as `<field> = from.<field>` (code/body/headers
# use move/copy spellings checked separately).
EXPLICIT = {"compressed", "skip_body", "manual_length_header"}


def find_header():
    for path in (ROOT / "build").rglob("crow/http_response.h"):
        return path
    return None


def declared_fields(text):
    """Data members of `struct response`, up to its first member function."""
    start = re.search(r"^\s*struct response\s*$", text, re.M)
    if not start:
        sys.exit("ERROR: could not find `struct response` in crow's header")
    body = text[start.end():]
    # The first member function ends the data-member block.
    end = re.search(r"^\s*(?:void|bool|const|static|response|~response)\s+\w+\s*\(",
                    body, re.M)
    if not end:
        sys.exit("ERROR: could not find the end of crow::response's field block")
    block = body[:end.start()]

    fields = []
    decl = re.compile(r"^\s*(?!friend|template|using|typedef)"
                      r"[A-Za-z_][\w:<>, ]*?\s+(\w+)\s*[{=;]")
    for line in block.splitlines():
        line = re.sub(r"///.*$|//.*$", "", line)
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = decl.match(line)
        if m:
            fields.append(m.group(1))
    return fields


def main():
    header = find_header()
    if header is None:
        # NOT a skip. The first version returned 0 here, and the CI job that
        # ran it only downloads a prebuilt binary into build/ - it never
        # configures CMake, so no crow header existed and the guard protecting
        # the Arrow corruption passed vacuously on every run. A guard that
        # cannot find what it guards has failed, not passed.
        sys.exit(
            "ERROR: crow/http_response.h not found under build/.\n"
            "This guard needs a configured build tree. Run it from the job "
            "that builds flapi, not one that only downloads the binary.")

    fields = declared_fields(header.read_text())
    # A guard that finds nothing passes vacuously. This one refuses to.
    if len(fields) < 4:
        sys.exit(f"ERROR: parsed only {fields} from {header}; the parser is "
                 "broken, not the code. Fix this script rather than trusting it.")

    unknown = [f for f in fields if f not in CARRIED]
    if unknown:
        sys.exit(
            "ERROR: crow::response has data member(s) the offload does not "
            f"carry: {', '.join(unknown)}\n\n"
            "Add them to Completer::take and its completion lambda in "
            "src/api_server.cpp, then list them in CARRIED above.\n"
            "A dropped field is a silent, runtime-only corruption - see the "
            "Arrow/compressed case in this file's docstring.")

    source = (ROOT / "src" / "api_server.cpp").read_text()
    for field in sorted(EXPLICIT & set(fields)):
        if f"{field} = from.{field}" not in source:
            sys.exit(f"ERROR: Completer::take no longer carries "
                     f"crow::response::{field}")
    for spelling in ("code = from.code", "body = std::move(from.body)",
                     "headers = std::move(from.headers)"):
        if spelling not in source:
            sys.exit(f"ERROR: Completer::take no longer does `{spelling}`")

    print(f"OK: the offload carries every crow::response field "
          f"({', '.join(fields)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
