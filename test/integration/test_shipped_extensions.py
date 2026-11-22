"""DuckDB extensions shipped next to the flapi binary (#245).

Release archives, wheels and the Docker image carry the signed default extensions in
`extensions/<duckdb version>/<platform>/` beside the executable. These tests build that
layout around a copy of the real binary and check that:

- with an empty HOME and no network, every default extension loads from it, and
  nothing is downloaded into the user directory (online and FLAPI_EXTENSIONS_OFFLINE);
- it is found through a symlink to the binary, and with the wheel's flapi_io/bin layout;
- FLAPI_EXTENSION_DIRECTORY and duckdb.extension_directory still win over it;
- a shipped directory for another DuckDB version is ignored with one log line;
- an extension that is not shipped still installs, into the user directory, never into
  the shipped one;
- `flapi extensions install --dir` copies shipped extensions instead of downloading.

Set FLAPI_TEST_SHIPPED_EXTENSIONS to an existing `extensions/` directory to reuse it
(CI passes the release artifact); otherwise it is installed once from the network.
"""
import os
import shutil
import subprocess
import tempfile
import time

import pytest
import requests

from otel_helpers import flapi_binary, free_port

pytestmark = pytest.mark.standalone_server

SHIPPED = ["httpfs", "ducklake", "fts", "postgres", "sqlite"]
# duckdb_extensions() names (postgres/sqlite are aliases)
SHIPPED_FILES = ["httpfs", "ducklake", "fts", "postgres_scanner", "sqlite_scanner"]
# A proxy nobody listens on: any download attempt fails instead of succeeding.
NO_NETWORK = {"HTTP_PROXY": "http://127.0.0.1:9", "HTTPS_PROXY": "http://127.0.0.1:9",
              "http_proxy": "http://127.0.0.1:9", "https_proxy": "http://127.0.0.1:9",
              "NO_PROXY": "", "no_proxy": ""}


def _unshare_available():
    try:
        return subprocess.run(["unshare", "-rn", "true"], capture_output=True, timeout=10).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def _network_available():
    try:
        requests.head("http://extensions.duckdb.org", timeout=5)
        return True
    except requests.RequestException:
        return False


@pytest.fixture(scope="module")
def shipped_extensions(tmp_path_factory):
    """An `extensions/` directory holding the default set for this binary."""
    reuse = os.environ.get("FLAPI_TEST_SHIPPED_EXTENSIONS")
    if reuse:
        assert os.path.isdir(reuse), reuse
        return reuse
    if not _network_available():
        pytest.skip("no network to fetch the extensions, and FLAPI_TEST_SHIPPED_EXTENSIONS is not set")
    root = str(tmp_path_factory.mktemp("shipped") / "extensions")
    home = str(tmp_path_factory.mktemp("installhome"))
    res = subprocess.run([flapi_binary(), "extensions", "install", "--dir", root, *SHIPPED],
                         env={**os.environ, "HOME": home}, capture_output=True, text=True, timeout=900)
    assert res.returncode == 0, res.stdout + res.stderr
    return root


def _layout(extensions_root):
    """(version, platform) of the single <version>/<platform> directory below `extensions_root`."""
    versions = [v for v in os.listdir(extensions_root) if os.path.isdir(os.path.join(extensions_root, v))]
    assert len(versions) == 1, versions
    platforms = os.listdir(os.path.join(extensions_root, versions[0]))
    assert len(platforms) == 1, platforms
    return versions[0], platforms[0]


def _extension_files(root):
    return sorted(f for _, _, fs in os.walk(root) for f in fs if f.endswith(".duckdb_extension"))


class Release:
    """A copy of the flapi binary laid out like a release archive, plus a project."""

    def __init__(self, extensions_root, bin_subdir="", version_dir=None):
        self.dir = tempfile.mkdtemp(prefix="flapi_release_")
        self.bin_dir = os.path.join(self.dir, "release", bin_subdir)
        os.makedirs(self.bin_dir)
        self.binary = os.path.join(self.bin_dir, "flapi")
        shutil.copy2(flapi_binary(), self.binary)
        self.extensions = os.path.join(self.bin_dir, "extensions")
        if extensions_root is not None:
            version, platform = _layout(extensions_root)
            target = os.path.join(self.extensions, version_dir or version, platform)
            shutil.copytree(os.path.join(extensions_root, version, platform), target)
            # Read-only, like a system install: flapi must never write here.
            for path, dirs, files in os.walk(self.extensions):
                for name in files:
                    os.chmod(os.path.join(path, name), 0o444)
            for path, dirs, files in os.walk(self.extensions, topdown=False):
                os.chmod(path, 0o555)
        self.home = os.path.join(self.dir, "home")
        os.makedirs(self.home)
        self.project = os.path.join(self.dir, "project")
        os.makedirs(os.path.join(self.project, "sqls"))
        with open(os.path.join(self.project, "sqls", "exts.yaml"), "w") as f:
            f.write("url-path: /exts\nmethod: GET\nwith-pagination: false\n"
                    "template-source: exts.sql\nconnection: []\n")
        with open(os.path.join(self.project, "sqls", "exts.sql"), "w") as f:
            f.write("SELECT extension_name, install_path FROM duckdb_extensions() "
                    "WHERE loaded AND install_mode <> 'STATICALLY_LINKED' ORDER BY extension_name\n")
        self.port = free_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.write_config()
        self.proc = None
        self.log_path = os.path.join(self.dir, "server.log")

    def write_config(self, extra_duckdb=""):
        self.config = os.path.join(self.project, "flapi.yaml")
        with open(self.config, "w") as f:
            f.write("project-name: shipped-extensions\nproject-description: shipped extension lookup\n"
                    f"http-port: {self.port}\ntemplate:\n  path: ./sqls\n"
                    f"duckdb:\n  db_path: ':memory:'\n{extra_duckdb}connections: {{}}\n")

    def env(self, **extra):
        env = {k: v for k, v in os.environ.items()
               if k not in ("FLAPI_EXTENSION_DIRECTORY", "FLAPI_EXTENSIONS_OFFLINE")}
        return {**env, "HOME": self.home, "DATAZOO_DISABLE_TELEMETRY": "1", "FLAPI_NO_TELEMETRY": "1",
                **extra}

    def run(self, *args, env=None, binary=None, timeout=120):
        return subprocess.run([binary or self.binary, *args], cwd=self.project, env=env or self.env(),
                              capture_output=True, text=True, timeout=timeout)

    def start(self, env=None, binary=None, prefix=()):
        log = open(self.log_path, "w")
        self.proc = subprocess.Popen([*prefix, binary or self.binary, "-c", self.config, "-p", str(self.port)],
                                     cwd=self.project, env=env or self.env(), stdout=log,
                                     stderr=subprocess.STDOUT)
        return self

    def wait_http(self):
        deadline = time.time() + 60
        while time.time() < deadline:
            if self.proc.poll() is not None:
                pytest.fail(f"server exited {self.proc.returncode}:\n{self.log()}")
            try:
                if requests.get(f"{self.base_url}/health/live", timeout=1).status_code == 200:
                    return self
            except requests.RequestException:
                pass
            time.sleep(0.2)
        pytest.fail("server did not start:\n" + self.log())

    def wait_log(self, needle, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log():
                return self.log()
            if self.proc.poll() is not None:
                pytest.fail(f"server exited {self.proc.returncode} before '{needle}':\n{self.log()}")
            time.sleep(0.2)
        pytest.fail(f"'{needle}' never logged:\n{self.log()}")

    def loaded(self):
        body = requests.get(f"{self.base_url}/exts", timeout=10).json()
        rows = body["data"] if isinstance(body, dict) else body
        return {r["extension_name"]: r["install_path"] for r in rows}

    def log(self):
        with open(self.log_path) as f:
            return f.read()

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    def cleanup(self):
        self.stop()
        for path, dirs, files in os.walk(self.dir):
            os.chmod(path, 0o755)
        shutil.rmtree(self.dir, ignore_errors=True)

    def downloaded(self):
        """Extension files that ended up in the user directory (HOME)."""
        return _extension_files(self.home)


@pytest.fixture
def release(shipped_extensions):
    r = Release(shipped_extensions)
    yield r
    r.cleanup()


def assert_all_shipped_loaded(release):
    loaded = release.loaded()
    for name in SHIPPED_FILES:
        assert name in loaded, (name, loaded, release.log())
        assert loaded[name].startswith(release.extensions), (name, loaded[name])


class TestShippedDirectory:
    @pytest.mark.parametrize("offline", [False, True], ids=["online-mode", "FLAPI_EXTENSIONS_OFFLINE"])
    def test_every_default_extension_loads_with_an_empty_home_and_no_network(self, release, offline):
        env = release.env(**NO_NETWORK, **({"FLAPI_EXTENSIONS_OFFLINE": "1"} if offline else {}))
        release.start(env=env).wait_http()
        assert_all_shipped_loaded(release)
        assert release.downloaded() == []
        assert "shipped with flapi" in release.log()

    def test_starts_inside_a_network_namespace(self, release):
        if not _unshare_available():
            pytest.skip("unshare -rn is not available")
        release.start(prefix=("unshare", "-rn"))
        log = release.wait_log("Successfully loaded")
        assert f"Successfully loaded {len(SHIPPED)} out of {len(SHIPPED)} extensions" in log, log
        assert release.downloaded() == []

    def test_found_through_a_symlink_to_the_binary(self, release):
        link_dir = os.path.join(release.dir, "usr-local-bin")
        os.makedirs(link_dir)
        link = os.path.join(link_dir, "flapi")
        os.symlink(release.binary, link)
        release.start(env=release.env(FLAPI_EXTENSIONS_OFFLINE="1", **NO_NETWORK), binary=link).wait_http()
        assert_all_shipped_loaded(release)


class TestPrecedence:
    def test_flapi_extension_directory_wins(self, release):
        empty = os.path.join(release.dir, "empty")
        os.makedirs(empty)
        res = release.run("-c", release.config, "-p", str(release.port),
                          env=release.env(FLAPI_EXTENSION_DIRECTORY=empty, FLAPI_EXTENSIONS_OFFLINE="1",
                                          **NO_NETWORK), timeout=60)
        out = res.stdout + res.stderr
        assert res.returncode != 0, out
        assert "httpfs" in out and "flapi extensions install" in out

    def test_duckdb_extension_directory_in_the_config_wins(self, release):
        empty = os.path.join(release.dir, "empty")
        os.makedirs(empty)
        release.write_config(f"  extension_directory: {empty}\n")
        res = release.run("-c", release.config, "-p", str(release.port),
                          env=release.env(FLAPI_EXTENSIONS_OFFLINE="1", **NO_NETWORK), timeout=60)
        assert res.returncode != 0, res.stdout + res.stderr

    def test_a_directory_for_another_duckdb_version_is_ignored_with_one_log_line(self, shipped_extensions):
        r = Release(shipped_extensions, version_dir="v0.0.1")
        try:
            res = r.run("-c", r.config, "-p", str(r.port),
                        env=r.env(FLAPI_EXTENSIONS_OFFLINE="1", **NO_NETWORK), timeout=60)
            out = res.stdout + res.stderr
            assert res.returncode != 0, out
            mentions = [line for line in out.splitlines() if "v0.0.1" in line]
            assert len(mentions) == 1, out
            assert "ignor" in mentions[0].lower(), mentions[0]
        finally:
            r.cleanup()

    def test_no_shipped_directory_is_silent(self):
        r = Release(None)
        try:
            res = r.run("-c", r.config, "-p", str(r.port), "--validate-config", timeout=60)
            out = res.stdout + res.stderr
            assert res.returncode == 0, out
            assert "extensions shipped" not in out and "shipped with flapi" not in out, out
        finally:
            r.cleanup()


class TestWheelLayout:
    def test_flapi_io_bin_layout(self, shipped_extensions):
        # bin-to-wheel installs site-packages/flapi_io/bin/flapi and execs it; the extensions
        # are added beside it at flapi_io/bin/extensions.
        r = Release(shipped_extensions, bin_subdir=os.path.join("site-packages", "flapi_io", "bin"))
        try:
            r.start(env=r.env(FLAPI_EXTENSIONS_OFFLINE="1", **NO_NETWORK)).wait_http()
            assert_all_shipped_loaded(r)
        finally:
            r.cleanup()


class TestInstallAlongside:
    def test_a_non_shipped_extension_installs_into_the_user_directory(self, release):
        if not _network_available():
            pytest.skip("needs network to install a non-shipped extension")
        release.write_config("  default-extensions: [httpfs, ducklake, fts, postgres, sqlite, inet]\n")
        release.start().wait_http()
        loaded = release.loaded()
        assert "inet" in loaded, release.log()
        assert loaded["inet"].startswith(release.home), loaded["inet"]
        assert release.downloaded() == ["inet.duckdb_extension"]
        assert_all_shipped_loaded(release)
        assert "inet.duckdb_extension" not in _extension_files(release.extensions)

    def test_extensions_install_copies_shipped_files_without_network(self, release):
        target = os.path.join(release.dir, "copied")
        res = release.run("extensions", "install", "--dir", target, *SHIPPED, env=release.env(**NO_NETWORK),
                          timeout=300)
        assert res.returncode == 0, res.stdout + res.stderr
        assert _extension_files(target) == sorted(f + ".duckdb_extension" for f in SHIPPED_FILES)
        # ... and the copy works on its own.
        release.stop()
        release.start(env=release.env(FLAPI_EXTENSION_DIRECTORY=target, FLAPI_EXTENSIONS_OFFLINE="1",
                                      **NO_NETWORK)).wait_http()
        loaded = release.loaded()
        assert all(loaded[n].startswith(target) for n in SHIPPED_FILES), loaded
