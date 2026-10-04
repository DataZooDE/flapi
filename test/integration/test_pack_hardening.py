"""`flapi pack` / `unpack` / bundle loading must be hostile-input safe (#190).

Found by the crew review of the server and reproduced against a real binary:
`unpack` wrote an entry named `../escaped.txt` outside `--to`; `pack` followed a
symlink to a file outside the input tree and its secret deny list was
case-sensitive (`.ENV`, `SECRETS/token`, `server.PEM` were bundled); a bundle
could expand without limit; a damaged bundle aborted `info`/`unpack` or silently
fell back to whatever flapi.yaml sat next to it.
"""

import io
import os
import resource
import shutil
import subprocess
import tempfile
import time
import zipfile

import pytest

from otel_helpers import flapi_binary

_DIRS = []


def _mk(prefix):
    """A scratch dir, removed after the test: every hostile bundle is a copy of the
    73 MiB binary, and /tmp is often a small tmpfs."""
    path = tempfile.mkdtemp(prefix=prefix)
    _DIRS.append(path)
    return path


@pytest.fixture(autouse=True)
def _cleanup_scratch_dirs():
    yield
    while _DIRS:
        shutil.rmtree(_DIRS.pop(), ignore_errors=True)


MIN_CONFIG = ("project-name: p\nproject-description: d\ntemplate:\n  path: ./sqls\n"
              "connections:\n  c:\n    properties:\n      database: ':memory:'\n")


def _run(args, cwd=None, env=None, timeout=120):
    r = subprocess.run(args, capture_output=True, text=True, cwd=cwd, timeout=timeout,
                       env={**os.environ, "DATAZOO_DISABLE_TELEMETRY": "1", **(env or {})})
    return r.returncode, r.stdout + r.stderr


def _project(extra_files=None):
    tmp = _mk(prefix="flapi_packhard_")
    src = os.path.join(tmp, "in")
    os.makedirs(os.path.join(src, "sqls"))
    with open(os.path.join(src, "flapi.yaml"), "w") as f:
        f.write(MIN_CONFIG)
    for rel, text in (extra_files or {}).items():
        path = os.path.join(src, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    return tmp, src


def _hostile_bundle(tmp, entries):
    """The real flapi binary with a ZIP of `entries` appended (what a malicious bundle is)."""
    out = os.path.join(tmp, "hostile.bin")
    with open(flapi_binary(), "rb") as src, open(out, "wb") as dst:
        shutil.copyfileobj(src, dst)
    # Appended as a stream: an int entry is that many zero bytes, written in chunks so
    # this process stays small (a forked child inherits the parent's peak RSS).
    part = out + ".zip"  # offsets must be relative to the ZIP's own start
    with zipfile.ZipFile(part, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            if isinstance(data, int):
                with z.open(name, "w", force_zip64=True) as entry:
                    for _ in range(data // (1 << 20)):
                        entry.write(b"\0" * (1 << 20))
            else:
                z.writestr(name, data)
    with open(out, "ab") as dst, open(part, "rb") as zin:
        shutil.copyfileobj(zin, dst)
    os.remove(part)
    os.chmod(out, 0o755)
    return out


class TestUnpackCannotEscape:

    @pytest.mark.parametrize("name", ["../escaped.txt", "a/../../escaped.txt", "/abs-escaped.txt", "..\\escaped.txt"])
    def test_an_unsafe_entry_name_is_refused(self, name):
        tmp = _mk(prefix="flapi_zipslip_")
        binary = _hostile_bundle(tmp, [("flapi.yaml", MIN_CONFIG), (name, "ESCAPED")])
        dst = os.path.join(tmp, "dst", "inner")
        code, out = _run([binary, "unpack", "--to", dst])
        assert code != 0, out
        assert "unsafe" in out.lower() or "outside" in out.lower(), out
        assert not os.path.exists(os.path.join(tmp, "dst", "escaped.txt"))
        assert not os.path.exists(os.path.join(tmp, "escaped.txt"))
        assert not os.path.exists("/abs-escaped.txt")

    def test_a_symlink_already_in_the_destination_cannot_redirect_a_write(self):
        tmp = _mk(prefix="flapi_zipslip_link_")
        outside = os.path.join(tmp, "outside")
        os.makedirs(outside)
        dst = os.path.join(tmp, "dst")
        os.makedirs(dst)
        os.symlink(outside, os.path.join(dst, "link"))
        binary = _hostile_bundle(tmp, [("flapi.yaml", MIN_CONFIG), ("link/pwned.txt", "PWNED")])
        code, out = _run([binary, "unpack", "--to", dst])
        assert code != 0, out
        assert not os.path.exists(os.path.join(outside, "pwned.txt")), "a write followed a symlink out of --to"

    def test_a_normal_bundle_still_unpacks(self):
        tmp, src = _project({"sqls/x.sql": "SELECT 1\n"})
        out_bin = os.path.join(tmp, "app")
        assert _run([flapi_binary(), "pack", "--in", src, "--out", out_bin])[0] == 0
        dst = os.path.join(tmp, "dst")
        code, out = _run([out_bin, "unpack", "--to", dst])
        assert code == 0, out
        assert open(os.path.join(dst, "sqls", "x.sql")).read() == "SELECT 1\n"


class TestPackRefusesWhatItShouldNotBundle:

    def test_a_symlink_to_a_file_outside_the_input_tree_is_refused(self):
        tmp, src = _project()
        secret = os.path.join(tmp, "outside-secret.txt")
        open(secret, "w").write("TOPSECRET")
        os.makedirs(os.path.join(src, "data"))
        os.symlink(secret, os.path.join(src, "data", "public.txt"))
        out_bin = os.path.join(tmp, "app")
        code, out = _run([flapi_binary(), "pack", "--in", src, "--out", out_bin])
        assert code != 0, "a symlink out of the input tree was bundled\n" + out
        assert "symlink" in out.lower(), out
        assert not os.path.exists(out_bin)

    @pytest.mark.parametrize("rel", [".ENV", "prod.ENV", "SECRETS/token", "Secrets/db.txt",
                                      "server.PEM", "id.Key", ".env.local", ".ENV.production", "cfg/.Env"])
    def test_the_secret_deny_list_ignores_case_and_covers_env_variants(self, rel):
        tmp, src = _project({rel: "hunter2"})
        out_bin = os.path.join(tmp, "app")
        code, out = _run([flapi_binary(), "pack", "--in", src, "--out", out_bin])
        assert code != 0, f"{rel} was bundled without --allow-secrets\n" + out
        assert not os.path.exists(out_bin)
        # The documented override still works.
        assert _run([flapi_binary(), "pack", "--in", src, "--out", out_bin, "--allow-secrets"])[0] == 0

    def test_ordinary_files_are_not_caught_by_the_deny_list(self):
        tmp, src = _project({"sqls/environment.sql": "SELECT 1\n", "data/keys.csv": "a,b\n", "README.md": "x"})
        assert _run([flapi_binary(), "pack", "--in", src, "--out", os.path.join(tmp, "app")])[0] == 0


class TestBundleReadingIsBounded:

    def test_a_decompression_bomb_is_refused_without_exhausting_memory(self):
        tmp = _mk(prefix="flapi_bomb_")
        binary = _hostile_bundle(tmp, [("flapi.yaml", MIN_CONFIG), ("data/bomb.bin", 300 * 1024 * 1024)])
        start = time.time()
        proc = subprocess.Popen([binary, "info"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                env={**os.environ, "FLAPI_BUNDLE_MAX_MIB": "64", "DATAZOO_DISABLE_TELEMETRY": "1"})
        out = proc.stdout.read()
        _, status, usage = os.wait4(proc.pid, 0)  # THIS child's peak RSS, not every child so far
        proc.returncode = os.waitstatus_to_exitcode(status)
        assert proc.returncode != 0, out
        assert "limit" in out.lower() or "exceed" in out.lower(), out
        assert usage.ru_maxrss < 280 * 1024, f"peak RSS {usage.ru_maxrss // 1024} MiB: the bomb was expanded"
        assert time.time() - start < 60

    def test_too_many_entries_are_refused(self):
        tmp = _mk(prefix="flapi_many_")
        entries = [("flapi.yaml", MIN_CONFIG)] + [(f"f/{i}.txt", "x") for i in range(2000)]
        binary = _hostile_bundle(tmp, entries)
        code, out = _run([binary, "info"], env={"FLAPI_BUNDLE_MAX_ENTRIES": "1000"})
        assert code != 0, out
        assert "entries" in out.lower() and "limit" in out.lower(), out


class TestADamagedBundleIsAnError:

    def _damaged(self):
        tmp, src = _project({"sqls/x.sql": "SELECT 1\n"})
        good = os.path.join(tmp, "app")
        assert _run([flapi_binary(), "pack", "--in", src, "--out", good])[0] == 0
        _, info = _run([good, "info"])
        offset = int(next(l for l in info.splitlines() if l.startswith("Bundle offset")).split(":")[1])
        data = bytearray(open(good, "rb").read())
        data[offset:offset + 64] = b"\xff" * 64  # destroy the first local header
        bad = os.path.join(tmp, "bad")
        open(bad, "wb").write(bytes(data))
        os.chmod(bad, 0o755)
        return tmp, bad

    def test_info_and_unpack_report_an_error_instead_of_aborting(self):
        tmp, bad = self._damaged()
        for args in (["info"], ["unpack", "--to", os.path.join(tmp, "d")]):
            code, out = _run([bad] + args)
            assert code == 1, f"{args} exited {code} (an abort is 134)\n{out[-500:]}"
            assert "error" in out.lower() or "bundle" in out.lower(), out

    def test_a_damaged_bundle_does_not_fall_back_to_a_flapi_yaml_beside_it(self):
        tmp, bad = self._damaged()
        cwd = os.path.join(tmp, "elsewhere")
        os.makedirs(os.path.join(cwd, "sqls"))
        open(os.path.join(cwd, "flapi.yaml"), "w").write(MIN_CONFIG)
        code, out = _run([bad, "--validate-config", "-c", os.path.join(cwd, "flapi.yaml")], cwd=cwd)
        assert code != 0, "a damaged bundle was ignored and the on-disk config used\n" + out
        assert "bundle" in out.lower(), out
