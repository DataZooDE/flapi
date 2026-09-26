#!/usr/bin/env python3
"""Keep server.json's version in step with the release, without a human.

server.json is the MCP Registry manifest. Its version was hand-maintained, and
it drifted twice at once: it said 26.07.13 when the release was 26.09.23, and
26.07.13 was never a version PyPI had at all - the wheel build normalises CalVer
and strips leading zeros, so what PyPI serves is 26.7.13. A client resolving
that manifest would have found nothing.

So the version is no longer written by anyone. The release job builds the
flapi-io wheels and then runs `stamp`, which reads the version from those wheel
FILENAMES - the exact artefacts being uploaded, already in PyPI's normalised
form - and writes it into a copy of server.json that is attached to the GitHub
release. The manifest cannot disagree with the wheels, because it is derived
from them.

The copy in the repository is a template. Its version is PLACEHOLDER, and
`check` fails CI if anyone replaces that with a real-looking number: a
hand-bumped value is exactly what went stale last time, and a plausible stale
version is worse than an obviously unreleased one.

    stamp_server_json.py stamp --wheels-dir DIR --in server.json --out OUT
    stamp_server_json.py check --in server.json
    stamp_server_json.py self-test
"""

import argparse
import json
import pathlib
import re
import sys
import tempfile

PLACEHOLDER = "0.0.0-dev"
PACKAGE = "flapi-io"

# PEP 427: {distribution}-{version}(-{build})?-{python}-{abi}-{platform}.whl.
# bin-to-wheel emits no build tag; anything else is refused rather than guessed.
_WHEEL = re.compile(
    r"^flapi_io-(?P<version>[^-]+)-[^-]+-[^-]+-[^-]+\.whl$")


def wheel_version(wheels_dir):
    """The one version every flapi-io wheel in `wheels_dir` carries."""
    names = sorted(p.name for p in pathlib.Path(wheels_dir).glob("*.whl"))
    if not names:
        raise ValueError(f"no .whl files in {wheels_dir}")
    versions = set()
    for name in names:
        match = _WHEEL.match(name)
        if not match:
            raise ValueError(f"not a flapi-io wheel filename: {name}")
        versions.add(match["version"])
    if len(versions) != 1:
        raise ValueError(f"the wheels disagree on their version: {sorted(versions)}")
    return versions.pop()


def _pypi_entries(manifest):
    return [p for p in manifest.get("packages", [])
            if p.get("registryType") == "pypi" and p.get("identifier") == PACKAGE]


def stamp(manifest, version):
    """Set the server version and every flapi-io PyPI package version."""
    entries = _pypi_entries(manifest)
    if not entries:
        raise ValueError(f"server.json has no pypi package entry for {PACKAGE}")
    manifest["version"] = version
    for entry in entries:
        entry["version"] = version
    return manifest


def template_problems(manifest):
    """Why the repository copy is not a valid template (empty when it is)."""
    problems = []
    if manifest.get("version") != PLACEHOLDER:
        problems.append(
            f'"version" is {manifest.get("version")!r}, expected {PLACEHOLDER!r}')
    entries = _pypi_entries(manifest)
    if not entries:
        problems.append(f"no pypi package entry for {PACKAGE}")
    for entry in entries:
        if entry.get("version") != PLACEHOLDER:
            problems.append(
                f'packages[{PACKAGE}].version is {entry.get("version")!r}, '
                f"expected {PLACEHOLDER!r}")
    return problems


def _read(path):
    with open(path) as f:
        return json.load(f)


def _write(manifest, path):
    with open(path, "w") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")


def _self_test():
    template = {"version": PLACEHOLDER,
                "packages": [{"registryType": "pypi", "identifier": PACKAGE,
                              "version": PLACEHOLDER},
                             {"registryType": "npm", "identifier": "other",
                              "version": "9.9.9"}]}
    assert template_problems(template) == []

    with tempfile.TemporaryDirectory() as d:
        for plat in ("manylinux_2_17_x86_64", "manylinux_2_17_aarch64",
                     "macosx_11_0_arm64", "win_amd64"):
            (pathlib.Path(d) / f"flapi_io-26.9.23-py3-none-{plat}.whl").touch()
        assert wheel_version(d) == "26.9.23"

        stamped = stamp(json.loads(json.dumps(template)), wheel_version(d))
        assert stamped["version"] == "26.9.23"
        assert stamped["packages"][0]["version"] == "26.9.23"
        assert stamped["packages"][1]["version"] == "9.9.9", "touched a foreign package"

        # Wheels that disagree must stop the release, not pick one.
        (pathlib.Path(d) / "flapi_io-26.9.24-py3-none-win_amd64.whl").touch()
        try:
            wheel_version(d)
        except ValueError:
            pass
        else:
            raise AssertionError("disagreeing wheels were accepted")

    with tempfile.TemporaryDirectory() as d:
        try:
            wheel_version(d)
        except ValueError:
            pass
        else:
            raise AssertionError("an empty wheels directory was accepted")

    # The exact regression this exists for: a hand-maintained real version.
    stale = json.loads(json.dumps(template))
    stale["version"] = stale["packages"][0]["version"] = "26.07.13"
    assert len(template_problems(stale)) == 2

    print("OK: stamp_server_json self-test passed")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Keep server.json's version in step with the release.")
    sub = parser.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stamp", help="write the released version into a copy")
    s.add_argument("--wheels-dir", required=True)
    s.add_argument("--in", dest="src", required=True)
    s.add_argument("--out", required=True)
    c = sub.add_parser("check", help="verify the repository copy is a template")
    c.add_argument("--in", dest="src", required=True)
    sub.add_parser("self-test")
    args = parser.parse_args(argv)

    try:
        if args.cmd == "self-test":
            _self_test()
        elif args.cmd == "check":
            problems = template_problems(_read(args.src))
            if problems:
                print(f"ERROR: {args.src} must stay a template - its version is "
                      f"stamped at release time from the built wheels, never by "
                      f"hand:", file=sys.stderr)
                for problem in problems:
                    print(f"  - {problem}", file=sys.stderr)
                return 1
            print(f"OK: {args.src} is a template ({PLACEHOLDER}); the release "
                  f"stamps the real version.")
        else:
            version = wheel_version(args.wheels_dir)
            _write(stamp(_read(args.src), version), args.out)
            print(f"stamped {args.out} with {version}")
    except ValueError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
