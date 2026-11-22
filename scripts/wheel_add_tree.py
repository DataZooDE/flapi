#!/usr/bin/env python3
"""Add a directory tree to a built wheel and rewrite its RECORD (#245).

bin-to-wheel packs binaries only; the flapi-io wheels also carry the DuckDB extensions
shipped next to the flapi binary, at flapi_io/bin/extensions/<version>/<platform>/. This
adds them after the fact: every file under SRC goes in below DEST, RECORD is regenerated
(sha256 + size per PEP 376/427), and the wheel is rewritten in place.

    wheel_add_tree.py WHEEL --src DIR --dest flapi_io/bin/extensions [--max-mb 100]

With --max-mb, a result above that size is NOT written: the wheel stays as it was, the
script prints a GitHub ::warning:: and exits 0, so an over-limit wheel never fails the
release (PyPI refuses files above a project's upload limit, 100 MB unless raised). The
binary still downloads its extensions on first start in that case.
"""
import argparse
import base64
import csv
import hashlib
import io
import os
import sys
import tempfile
import zipfile


def record_hash(data: bytes) -> str:
    return "sha256=" + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()


def add_tree(wheel: str, src: str, dest: str, max_mb: float | None) -> bool:
    with zipfile.ZipFile(wheel) as zin:
        entries = [(info, zin.read(info.filename)) for info in zin.infolist()]
    record_name = next((i.filename for i, _ in entries if i.filename.endswith(".dist-info/RECORD")), None)
    if record_name is None:
        raise SystemExit(f"{wheel}: no .dist-info/RECORD")

    dest = dest.strip("/")
    existing = {info.filename for info, _ in entries}
    added = []
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            arcname = dest + "/" + os.path.relpath(path, src).replace(os.sep, "/")
            if arcname in existing:
                raise SystemExit(f"{wheel}: already contains {arcname}")
            with open(path, "rb") as f:
                added.append((arcname, f.read()))
    if not added:
        raise SystemExit(f"{src}: no files to add")

    files = [(i, d) for i, d in entries if i.filename != record_name]
    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\n")
    for info, data in files:
        writer.writerow([info.filename, record_hash(data), str(len(data))])
    for arcname, data in added:
        writer.writerow([arcname, record_hash(data), str(len(data))])
    writer.writerow([record_name, "", ""])

    fd, tmp = tempfile.mkstemp(suffix=".whl", dir=os.path.dirname(os.path.abspath(wheel)))
    os.close(fd)
    try:
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zout:
            for info, data in files:
                zout.writestr(info, data)
            for arcname, data in added:
                info = zipfile.ZipInfo(arcname, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (0o644 & 0xFFFF) << 16
                zout.writestr(info, data, compresslevel=9)
            zout.writestr(record_name, out.getvalue())
        size_mb = os.path.getsize(tmp) / 1e6
        if max_mb is not None and size_mb > max_mb:
            print(f"::warning::{os.path.basename(wheel)} with {len(added)} extension files would be "
                  f"{size_mb:.1f} MB, above the {max_mb:g} MB upload limit: published WITHOUT them "
                  "(flapi downloads them on first start). Raise the PyPI limit and PYPI_WHEEL_LIMIT_MB.")
            return False
        os.replace(tmp, wheel)
        print(f"{os.path.basename(wheel)}: +{len(added)} files under {dest}/ -> {size_mb:.1f} MB")
        return True
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("wheel")
    parser.add_argument("--src", required=True, help="directory whose contents are added")
    parser.add_argument("--dest", required=True, help="path inside the wheel, e.g. flapi_io/bin/extensions")
    parser.add_argument("--max-mb", type=float, default=None,
                        help="leave the wheel unchanged (and warn) when the result would exceed this")
    args = parser.parse_args()
    add_tree(args.wheel, args.src, args.dest, args.max_mb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
