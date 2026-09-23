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


def find_headers():
    """EVERY copy under build/, not an arbitrary one.

    There are typically four - build/, build/release/, build/debug/,
    build/tracing-off/ - and the first version returned whichever rglob
    yielded first. A guard that inspects a copy the binary was not built from
    is checking nothing, and it silently passed when a member was added to the
    copy that mattered.
    """
    return sorted((ROOT / "build").rglob("crow/http_response.h"))


# Data members crow::response has that the offload deliberately does NOT carry.
# Empty today. A new member must be added here with a reason, or carried -
# never silently ignored.
NOT_CARRIED: dict[str, str] = {
    # PRIVATE in crow::response, and settable only through
    # set_static_file_info(), which flAPI never calls - the offloaded route
    # serves query results, not files. It is unreachable rather than ignored:
    # the Completer could not copy it even if it wanted to. If flAPI ever does
    # serve a static file from an offloaded handler, that response needs
    # different handling entirely (crow streams the file), not a field copy.
    "file_info": "private in crow::response; only set_static_file_info() writes it, "
                 "and flAPI never calls it",

    # Added by flAPI's own crow overlay patch. It is the connection keepalive
    # the offload relies on, and the REAL response already holds it - the
    # worker's local response never has one, and copying it back would be
    # meaningless. The Completer captures the keepalive separately, by value,
    # which is what keeps the connection alive across the hop.
    "connection_keepalive": "flAPI's own field; the Completer captures the keepalive "
                            "directly rather than copying it off a local response",
}


def declared_fields(text):
    """Data members of `struct response`, found by balancing braces.

    The first version matched member declarations with one regex, so a member
    whose declaration it did not match simply did not exist as far as the
    guard was concerned - and the guard's whole purpose is to notice a member
    it has not seen before. It now walks the struct body brace by brace and
    treats anything it cannot classify as a finding rather than as absent.
    """
    start = re.search(r"^\s*struct response\s*$", text, re.M)
    if not start:
        sys.exit("ERROR: could not find `struct response` in crow's header")

    body = text[start.end():]
    open_brace = body.index("{")
    depth = 0
    end = None
    for i, ch in enumerate(body[open_brace:], start=open_brace):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end is None:
        sys.exit("ERROR: could not find the end of `struct response`")
    block = body[open_brace + 1:end]

    # Drop nested bodies (methods, nested types) so only declarations remain.
    flattened, depth = [], 0
    for ch in block:
        if ch == "{":
            depth += 1
            continue
        if ch == "}":
            depth -= 1
            continue
        if depth == 0:
            flattened.append(ch)
    block = "".join(flattened)

    fields = []
    decl = re.compile(r"^\s*(?!friend|template|using|typedef|public|private|protected|return)"
                      r"[A-Za-z_][\w:<>,\s*&]*?\s+(\w+)\s*(?:=[^;]*)?$")
    for statement in block.split(";"):
        statement = re.sub(r"///.*|//.*", "", statement)
        statement = "\n".join(line for line in statement.splitlines()
                               if not line.lstrip().startswith("#"))
        if not statement.strip() or "(" in statement:
            continue   # a function declaration, not a data member
        m = decl.match(statement.strip("\n"))
        if m:
            fields.append(m.group(1))
    return fields


def main():
    headers = find_headers()
    if not headers:
        # NOT a skip. The first version returned 0 here, and the CI job that
        # ran it only downloads a prebuilt binary into build/ - it never
        # configures CMake, so no crow header existed and the guard protecting
        # the Arrow corruption passed vacuously on every run. A guard that
        # cannot find what it guards has failed, not passed.
        sys.exit(
            "ERROR: crow/http_response.h not found under build/.\n"
            "This guard needs a configured build tree. Run it from the job "
            "that builds flapi, not one that only downloads the binary.")

    # EVERY tree is checked, and each independently: a stale build directory
    # may legitimately predate flAPI's crow overlay patch and so lack
    # connection_keepalive. What must hold in each is that every field it
    # declares is classified - carried, or an exception with a reason.
    problems = []
    seen_fields = set()
    for header in headers:
        fields = declared_fields(header.read_text())
        # A guard that finds nothing passes vacuously. This one refuses to.
        if len(fields) < 4:
            sys.exit(f"ERROR: parsed only {fields} from {header}; the parser is "
                     "broken, not the code. Fix this script rather than trusting it.")
        seen_fields.update(fields)
        for field in fields:
            if field not in CARRIED and field not in NOT_CARRIED:
                problems.append((header, field))

    if problems:
        detail = "\n".join(f"  {f}  (in {h})" for h, f in problems)
        sys.exit(
            "ERROR: crow::response has data member(s) the offload neither "
            f"carries nor documents:\n{detail}\n\n"
            "Add them to Completer::take and its completion lambda in "
            "src/api_server.cpp and list them in CARRIED, or add them to "
            "NOT_CARRIED with the reason they cannot or need not make the "
            "hop.\nA dropped field is a silent, runtime-only corruption - see "
            "the Arrow/compressed case in this file's docstring.")

    fields = sorted(seen_fields)

    source = (ROOT / "src" / "api_server.cpp").read_text()
    for field in sorted(EXPLICIT & set(fields)):
        if f"{field} = from.{field}" not in source:
            sys.exit(f"ERROR: Completer::take no longer carries "
                     f"crow::response::{field}")
    for spelling in ("code = from.code", "body = std::move(from.body)",
                     "headers = std::move(from.headers)"):
        if spelling not in source:
            sys.exit(f"ERROR: Completer::take no longer does `{spelling}`")

    # take() is only half the hop. The original bug was a field never reaching
    # the RESPONSE, so the completion lambda has to write each one back - an
    # edit that drops `res->compressed = compressed` while leaving take()
    # intact reproduces the Arrow corruption and would otherwise pass.
    for field in sorted(EXPLICIT & set(fields)):
        if f"res->{field} = {field}" not in source:
            sys.exit(f"ERROR: the offload's completion lambda no longer writes "
                     f"crow::response::{field} back onto the response")

    carried = [f for f in fields if f in CARRIED]
    excepted = [f for f in fields if f in NOT_CARRIED]
    print(f"OK: the offload carries {', '.join(carried)}"
          + (f"; documented exceptions: {', '.join(excepted)}" if excepted else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
