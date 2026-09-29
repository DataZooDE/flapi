"""`template.environment-whitelist` is enforced for YAML config files (#157).

`{{env.NAME}}` in a YAML file is documented as requiring `NAME` to match a
pattern in `template.environment-whitelist`. It never did: the whitelist was
read only for SQL templates, and the parser that substitutes variables into
config files was built with an EMPTY whitelist, which it treated as "allow
everything". Verified before this fix - a variable substituted identically with
a whitelist that excluded it, one that included it, and none at all.

So the documented control did nothing, and the shipped S3/GCS/Azure examples
made it worse by putting `environment-whitelist:` at the TOP level, where
nothing reads it either - an operator copying them believed they had restricted
which variables a config could read.

Now:
  * a variable that is not whitelisted STOPS startup, naming the variable and
    the key to add it to (never a silent literal `{{env.NAME}}`);
  * an empty or missing whitelist allows no variables - the same rule SQL
    templates already had;
  * a top-level `environment-whitelist:` is an error.

Asserted through --validate-config, the path operators run in CI.
"""

import glob
import os
import re
import subprocess
import tempfile

import yaml

from otel_helpers import flapi_binary

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SECRET = "hunter2-probe"


def _write(base, rel, text):
    path = os.path.join(base, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    return path


def _validate(main_extra="", template_extra="", project_name="probe",
              endpoint=None, top_level=""):
    """Run --validate-config on a generated config. Returns (code, output)."""
    tmp = tempfile.mkdtemp(prefix="flapi_envwl_")
    os.makedirs(os.path.join(tmp, "sqls"))
    if endpoint is not None:
        _write(tmp, "sqls/ep.yaml", endpoint)
        _write(tmp, "sqls/ep.sql", "SELECT 1 AS n\n")
    cfg = _write(tmp, "flapi.yaml",
                 f'project-name: "{project_name}"\n'
                 "project-description: environment whitelist probe\n"
                 + top_level +
                 "template:\n  path: ./sqls\n" + template_extra +
                 "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n"
                 + main_extra)
    env = {**os.environ, "FLAPI_PROBE_SECRET": SECRET, "OTHER_PROBE_VAR": "other",
           "DATAZOO_DISABLE_TELEMETRY": "1"}
    r = subprocess.run([flapi_binary(), "-c", cfg, "--validate-config",
                        "--log-level", "debug"],
                       capture_output=True, text=True, cwd=tmp, env=env, timeout=60)
    return r.returncode, r.stdout + r.stderr


def _wl(*patterns):
    return "  environment-whitelist:\n" + "".join(f"    - '{p}'\n" for p in patterns)


class TestUnlistedVariablesStopStartup:

    def test_an_unlisted_variable_fails_and_says_how_to_fix_it(self):
        code, out = _validate(project_name="p-{{env.FLAPI_PROBE_SECRET}}",
                              template_extra=_wl("ONLY_THIS_ONE"))
        assert code != 0, (
            "a variable outside the whitelist was substituted anyway\n" + out[-2000:])
        assert "FLAPI_PROBE_SECRET" in out, out[-2000:]
        assert "template.environment-whitelist" in out, out[-2000:]
        # The secret's VALUE must never reach the log or the error.
        assert SECRET not in out, "the unlisted variable's value was logged\n" + out[-2000:]

    def test_a_listed_variable_is_substituted(self):
        code, out = _validate(project_name="p-{{env.FLAPI_PROBE_SECRET}}",
                              template_extra=_wl("FLAPI_PROBE_SECRET"))
        assert code == 0, out
        assert f"Project Name: p-{SECRET}" in out, out[-2000:]

    def test_a_regex_pattern_matches(self):
        code, out = _validate(project_name="p-{{env.FLAPI_PROBE_SECRET}}",
                              template_extra=_wl("^FLAPI_PROBE_.*"))
        assert code == 0, out
        assert f"Project Name: p-{SECRET}" in out, out[-2000:]

    def test_a_pattern_is_a_full_match_not_a_substring(self):
        code, out = _validate(project_name="p-{{env.FLAPI_PROBE_SECRET}}",
                              template_extra=_wl("PROBE"))
        assert code != 0, "the pattern PROBE matched FLAPI_PROBE_SECRET\n" + out[-2000:]

    def test_every_unlisted_variable_is_named_at_once(self):
        code, out = _validate(
            project_name="p-{{env.FLAPI_PROBE_SECRET}}-{{env.OTHER_PROBE_VAR}}",
            template_extra=_wl("ONLY_THIS_ONE"))
        assert code != 0, out
        assert "FLAPI_PROBE_SECRET" in out and "OTHER_PROBE_VAR" in out, (
            "fixing one at a time is a bad experience\n" + out[-2000:])


class TestTheWhitelistIsReadBeforeTheFileIsValidYaml:
    """The whitelist lives in the file being substituted, so it is read from the
    RAW text. A config that is not valid YAML until its `{{env.X}}` references
    are substituted - a bare reference with trailing text, an env variable inside
    an include path - must still find its whitelist. Found by a Codex review: the
    first version probed with YAML::Load, failed on such files, ran the real parse
    with an EMPTY whitelist, and rejected variables that WERE listed."""

    def test_a_bare_reference_with_trailing_text_and_a_listed_variable(self):
        code, out = _validate(main_extra="tags:\n  - {{env.FLAPI_PROBE_SECRET}}-x\n",
                              template_extra=_wl("FLAPI_PROBE_SECRET"))
        assert code == 0, (
            "a listed variable was rejected because the file is not valid YAML "
            "before substitution\n" + out[-2000:])

    def test_a_sequence_item_that_is_a_bare_reference(self):
        code, out = _validate(main_extra="tags:\n  - {{env.FLAPI_PROBE_SECRET}}\n",
                              template_extra=_wl("FLAPI_PROBE_SECRET"))
        assert code == 0, out

    def test_an_env_variable_inside_an_include_path(self):
        tmp = tempfile.mkdtemp(prefix="flapi_envwl_inc_")
        os.makedirs(os.path.join(tmp, "sqls"))
        os.makedirs(os.path.join(tmp, "common"))
        _write(tmp, "common/settings.yaml", "extra:\n  key: 1\n")
        cfg = _write(tmp, "flapi.yaml",
                     "project-name: p\nproject-description: include path probe\n"
                     "{{include from {{env.FLAPI_PROBE_DIR}}/settings.yaml}}\n"
                     "template:\n  path: ./sqls\n" + _wl("FLAPI_PROBE_DIR") +
                     "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
        env = {**os.environ, "FLAPI_PROBE_DIR": "common", "DATAZOO_DISABLE_TELEMETRY": "1"}
        r = subprocess.run([flapi_binary(), "-c", cfg, "--validate-config"],
                           capture_output=True, text=True, cwd=tmp, env=env, timeout=60)
        assert r.returncode == 0, (
            "a whitelisted variable in an include path was rejected\n"
            + (r.stdout + r.stderr)[-2000:])

    def test_the_unlisted_variable_in_such_a_file_is_still_refused(self):
        # The fence: fixing the probe must not have loosened enforcement.
        code, out = _validate(main_extra="tags:\n  - {{env.FLAPI_PROBE_SECRET}}-x\n",
                              template_extra=_wl("ONLY_THIS_ONE"))
        assert code != 0, out
        assert "FLAPI_PROBE_SECRET" in out, out[-2000:]


class TestBlockScalars:
    """A line starting with `#` inside a block scalar (`key: |`) is CONTENT - a
    Markdown heading in a prompt - not a comment. Skipping it left a literal
    {{env.X}} in the text with no error: the silent failure enforcement exists
    to prevent."""

    ENDPOINT = ("url-path: /ep\nmethod: GET\ntemplate-source: ep.sql\n"
                "connection: [inmem]\n"
                "description: |\n"
                "  Intro text.\n"
                "  # Heading for {{env.FLAPI_PROBE_SECRET}}\n"
                "  More text.\n")

    def test_an_unlisted_variable_on_a_hash_line_in_a_block_scalar_is_refused(self):
        code, out = _validate(endpoint=self.ENDPOINT, template_extra=_wl("ONLY_THIS_ONE"))
        assert code != 0, (
            "a `#` line inside a block scalar was treated as a comment, so its "
            "unlisted variable was silently skipped\n" + out[-2000:])
        assert "FLAPI_PROBE_SECRET" in out, out[-2000:]

    def test_a_listed_variable_there_is_fine(self):
        code, out = _validate(endpoint=self.ENDPOINT, template_extra=_wl("FLAPI_PROBE_SECRET"))
        assert code == 0, out

    def test_a_real_comment_after_the_block_scalar_is_still_a_comment(self):
        endpoint = self.ENDPOINT + "# a real comment about {{env.OTHER_PROBE_VAR}}\n"
        code, out = _validate(endpoint=endpoint, template_extra=_wl("FLAPI_PROBE_SECRET"))
        assert code == 0, out


class TestConfigServiceMetadataUsesTheSamePolicy:
    """GET /api/v1/_config/filesystem parses YAML for metadata with its own
    parser. It kept the old default, so once an empty whitelist meant "none" it
    showed a literal `{{env.X}}` for a variable that IS whitelisted."""

    def test_the_file_tree_shows_a_whitelisted_variable_substituted(self):
        import time
        import requests
        tmp = tempfile.mkdtemp(prefix="flapi_envwl_cs_")
        os.makedirs(os.path.join(tmp, "sqls"))
        _write(tmp, "sqls/ep.yaml", "url-path: /probe-{{env.FLAPI_PROBE_PATH}}\nmethod: GET\n"
                                    "template-source: ep.sql\nconnection: [inmem]\n")
        _write(tmp, "sqls/ep.sql", "SELECT 1 AS n\n")
        from otel_helpers import free_port
        port = free_port()
        cfg = _write(tmp, "flapi.yaml",
                     "project-name: p\nproject-description: metadata\n"
                     f"http-port: {port}\ntemplate:\n  path: ./sqls\n"
                     + _wl("FLAPI_PROBE_PATH") +
                     "connections:\n  inmem:\n    properties:\n      database: ':memory:'\n")
        env = {**os.environ, "FLAPI_PROBE_PATH": "resolved", "DATAZOO_DISABLE_TELEMETRY": "1"}
        token = "envwl-test-token-123"
        proc = subprocess.Popen([flapi_binary(), "-c", cfg, "-p", str(port),
                                 "--config-service", "--config-service-token", token,
                                 "--log-level", "warning"],
                                cwd=tmp, env=env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        try:
            base = f"http://127.0.0.1:{port}"
            for _ in range(120):
                try:
                    if requests.get(f"{base}/health/live", timeout=1).status_code == 200:
                        break
                except requests.RequestException:
                    pass
                time.sleep(0.25)
            r = requests.get(f"{base}/api/v1/_config/filesystem",
                             headers={"Authorization": f"Bearer {token}"}, timeout=10)
            assert r.status_code == 200, r.text[:500]
            assert "/probe-resolved" in r.text, (
                "the file tree showed a literal {{env.X}} for a whitelisted variable\n"
                + r.text[:1500])
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()


class TestComments:
    """Operators document their configs with comments like
    `# password comes from {{env.DB_PASSWORD}}`. Enforcement must not turn that
    into a startup failure - a comment cannot reference anything. (Found when
    this change's own explanatory comment stopped the shipped example starting.)"""

    def test_a_full_line_comment_mentioning_an_unlisted_variable_is_fine(self):
        code, out = _validate(main_extra=(
            "# the password comes from {{env.FLAPI_PROBE_SECRET}}\n"
            "  # indented comment about {{env.OTHER_PROBE_VAR}} too\n"))
        assert code == 0, out
        assert SECRET not in out, "a comment pulled a secret into the log\n" + out[-2000:]

    def test_a_trailing_comment_is_still_checked_and_says_so(self):
        # A `#` after a value can be a comment or part of a quoted string;
        # telling them apart needs a tokenizer. Failing loudly is the safe way
        # to be wrong: the alternative is a literal {{env.X}} in a password.
        code, out = _validate(project_name="probe # see {{env.FLAPI_PROBE_SECRET}}")
        assert code != 0, out
        assert "FLAPI_PROBE_SECRET" in out, out[-2000:]

    def test_a_real_reference_next_to_a_comment_is_still_enforced(self):
        code, out = _validate(
            project_name="p-{{env.FLAPI_PROBE_SECRET}}",
            main_extra="# unrelated comment\n",
            template_extra=_wl("ONLY_THIS_ONE"))
        assert code != 0, out


class TestEmptyMeansNone:

    def test_no_whitelist_allows_no_variables(self):
        code, out = _validate(project_name="p-{{env.FLAPI_PROBE_SECRET}}")
        assert code != 0, (
            "with no whitelist a variable was substituted; an unset whitelist "
            "must allow nothing\n" + out[-2000:])
        assert "FLAPI_PROBE_SECRET" in out, out[-2000:]

    def test_an_empty_whitelist_allows_no_variables(self):
        code, out = _validate(project_name="p-{{env.FLAPI_PROBE_SECRET}}",
                              template_extra="  environment-whitelist: []\n")
        assert code != 0, out

    def test_a_config_that_uses_no_variables_needs_no_whitelist(self):
        # The fence: enforcement must not tax configs that never read the env.
        code, out = _validate()
        assert code == 0, out


class TestEndpointFilesFollowTheSameRule:

    ENDPOINT = ("url-path: /ep\nmethod: GET\ntemplate-source: ep.sql\n"
                "connection: [inmem]\n"
                "description: 'x-{{env.FLAPI_PROBE_SECRET}}'\n")

    def test_an_unlisted_variable_in_an_endpoint_file_stops_startup(self):
        code, out = _validate(endpoint=self.ENDPOINT,
                              template_extra=_wl("ONLY_THIS_ONE"))
        assert code != 0, (
            "an endpoint file substituted a variable outside the whitelist\n"
            + out[-2000:])
        assert "FLAPI_PROBE_SECRET" in out, out[-2000:]

    def test_a_listed_variable_in_an_endpoint_file_works(self):
        code, out = _validate(endpoint=self.ENDPOINT,
                              template_extra=_wl("FLAPI_PROBE_SECRET"))
        assert code == 0, out


class TestAMisplacedWhitelistIsAnError:
    """The shipped S3/GCS/Azure examples set `environment-whitelist:` at the top
    level, where nothing reads it. The same trap as a `server:` block (#153),
    on a security setting - so it fails closed instead of warning."""

    def test_a_top_level_whitelist_fails_and_names_the_right_key(self):
        code, out = _validate(top_level="environment-whitelist:\n  - FLAPI_PROBE_SECRET\n")
        assert code != 0, (
            "a top-level environment-whitelist was accepted; nothing reads it, so "
            "the operator would believe variables were restricted\n" + out[-2000:])
        assert "template.environment-whitelist" in out, out[-2000:]

    def test_it_is_an_error_even_when_the_right_one_is_also_set(self):
        code, out = _validate(
            top_level="environment-whitelist:\n  - FLAPI_PROBE_SECRET\n",
            template_extra=_wl("FLAPI_PROBE_SECRET"))
        assert code != 0, out

    def test_it_is_reported_before_an_unlisted_variable_masks_it(self):
        # The variable is unlisted (the top-level key is not the real one), so
        # both problems exist; the misplaced key is the ROOT cause and must win.
        code, out = _validate(project_name="p-{{env.FLAPI_PROBE_SECRET}}",
                              top_level="environment-whitelist:\n  - FLAPI_PROBE_SECRET\n")
        assert code != 0, out
        assert "template.environment-whitelist" in out, out[-2000:]
        assert "top level" in out or "top-level" in out, out[-2000:]


class TestMalformedWhitelists:

    def test_an_invalid_regex_is_reported_at_startup_naming_the_pattern(self):
        code, out = _validate(template_extra=_wl("([unclosed"))
        assert code != 0, out
        assert "([unclosed" in out, out[-2000:]
        assert "environment-whitelist" in out, out[-2000:]

    def test_a_non_list_whitelist_fails(self):
        code, out = _validate(template_extra="  environment-whitelist: FLAPI_PROBE_SECRET\n")
        assert code != 0, out


_ENV_REF = re.compile(r"\{\{\s*env\.([A-Za-z_][A-Za-z0-9_]*)\s*\}\}")


def _load_neutralised(path):
    """yaml.safe_load a config, with each {{env.X}} made an inert plain scalar.

    flAPI substitutes those BEFORE parsing, so an unquoted `key: {{env.X}}` is
    valid for it and a flow-mapping error for PyYAML. Skipping such a file made
    this guard blind to test/integration/api_configuration/flapi.yaml - which had
    the very misplaced whitelist the guard exists to catch."""
    with open(path) as f:
        return yaml.safe_load(_ENV_REF.sub(r"ENVREF_\1", f.read()))


def _main_configs():
    """(path, doc) for every shipped config that declares project-name."""
    found = []
    roots = [os.path.join(REPO, "examples"),
             os.path.join(REPO, "test", "integration")]
    for root in roots:
        for path in glob.glob(os.path.join(root, "**", "*.yaml"), recursive=True):
            try:
                doc = _load_neutralised(path)
            except Exception:
                continue
            if isinstance(doc, dict) and "project-name" in doc:
                found.append((path, doc))
    return found


class TestShippedConfigsAreConsistent:

    def test_the_glob_actually_finds_configs(self):
        assert len(_main_configs()) >= 8

    def test_the_guard_can_see_the_unquoted_env_fixture(self):
        # The file that hid from the first version of this guard.
        found = {os.path.relpath(p, REPO) for p, _ in _main_configs()}
        assert "test/integration/api_configuration/flapi.yaml" in found

    def test_no_shipped_config_has_a_top_level_whitelist(self):
        offenders = [os.path.relpath(p, REPO) for p, d in _main_configs()
                     if "environment-whitelist" in d]
        assert not offenders, (
            f"a top-level environment-whitelist is ignored by the parser and now "
            f"rejected: {offenders}")

    def test_every_env_reference_in_a_shipped_config_is_whitelisted(self):
        """Statically: for each local config, every {{env.X}} in its own file and
        in the YAML files under its template path must match its whitelist.
        Otherwise the example would fail its own startup now that the whitelist
        is enforced."""
        pat = _ENV_REF
        problems = []
        for path, doc in _main_configs():
            template = doc.get("template") or {}
            tpath = str(template.get("path", ""))
            if "://" in tpath or not tpath:
                continue                       # remote templates need cloud access
            wl = [re.compile(p, re.IGNORECASE)
                  for p in (template.get("environment-whitelist") or [])]
            base = os.path.dirname(path)
            files = [path] + glob.glob(os.path.join(base, tpath, "**", "*.yaml"),
                                       recursive=True)
            for f in files:
                with open(f) as fh:
                    # Full-line comments are not references (see TestComments).
                    body = "".join(l for l in fh.readlines()
                                   if not l.lstrip().startswith("#"))
                    for name in pat.findall(body):
                        if not any(r.fullmatch(name) for r in wl):
                            problems.append(f"{os.path.relpath(path, REPO)}: "
                                            f"{name} used in {os.path.relpath(f, REPO)}")
        assert not problems, "\n".join(sorted(set(problems)))
