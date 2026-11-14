# Using flAPI with Claude Code

flAPI ships a [Claude Code](https://claude.com/claude-code) plugin. With it
installed, Claude knows how to set up a flAPI project, write REST endpoints and
MCP tools, design and tune DuckLake caches, secure, test, deploy and operate a
server. It checks its own edits against your config, and it can manage a running
server through flAPI's MCP config tools.

## Install

In Claude Code:

```
/plugin marketplace add DataZooDE/flapi
/plugin install flapi@flapi
```

The plugin needs the `flapi` server binary for validation and the `flapii` CLI for
a running server. `pip install flapi-io` installs both.

To update it, run `/plugin marketplace update flapi`. The plugin version follows
flAPI's release version, so use the plugin release that matches your server.

## What you get

### Skills

Claude loads a skill when your request matches it. You do not have to invoke one.

| Phase | Skill | Covers |
|---|---|---|
| Setup | `flapi-setup` | Installing, `flapii project init`, `flapi.yaml`, connections, environment-variable whitelist, DuckDB extensions, validation |
| Develop | `flapi-rest` | Endpoint YAML, parameters and validators, Mustache SQL templates, pagination, write endpoints, YAML includes, rate limits |
| Develop | `flapi-mcp` | MCP tools, resources and prompts, descriptions an agent can act on, async tasks, one endpoint for REST and MCP, connecting clients |
| Develop | `flapi-auth` | Basic, bearer JWT, OIDC with provider presets, roles, MCP OAuth, the config-service token, CORS |
| Develop | `flapi-cache-design` | Full, append and merge caches, writing the populate SQL, cursors and watermarks, deletes, warm-up, write-through, replicas, prebuilt caches, time travel |
| Develop | `flapi-cache-tuning` | Measuring refresh time and cutting it down: predicate pushdown, incremental loads, schedules, inlining, compaction, retention |
| Develop | `flapi-testing` | Template expansion, `--validate-config`, Tavern REST tests, MCP tests, CI |
| Develop | `flapi-live-config` | The config service, `flapii`, the `flapi_*` MCP config tools, hot reload |
| Deploy | `flapi-deploy` | `flapi pack` single binaries, Docker, Cloud Run / serverless, secrets, offline extensions |
| Operate | `flapi-operate` | Health and readiness, runtime metrics, logs, tracing, cache operations, troubleshooting |

### Slash commands

| Command | Does |
|---|---|
| `/flapi:init` | Scaffolds a project from a description of your data, then validates and starts it |
| `/flapi:new-endpoint` | Creates the YAML, SQL, validators, an optional MCP tool and a test, then tries them |
| `/flapi:add-cache` | Picks a cache mode, writes the populate SQL, refreshes, measures and tunes |
| `/flapi:auth-setup` | Configures authentication and roles, and proves the 401/403/200 paths with curl |
| `/flapi:deploy-check` | Runs a pre-deployment checklist and builds the artifact |
| `/flapi:diagnose` | Goes from an error to a root cause and a verified fix |
| `/flapi:metrics` | Summarizes slow or failing endpoints, rejections and slow or stale caches |

### MCP server

The plugin registers a `flapi` MCP server at `$FLAPI_URL/mcp/jsonrpc` (default
`http://localhost:8080`). It sends two headers:

- `X-Config-Token: $FLAPI_CONFIG_SERVICE_TOKEN`, for the `flapi_*` config tools;
- `Authorization: Bearer $FLAPI_MCP_TOKEN`, for `mcp.auth` (a JWT the server's
  `mcp.auth` accepts). With `FLAPI_MCP_TOKEN` unset the header is an empty
  `Bearer `, which a server without `mcp.auth` ignores and a server with it
  answers 401 `Authentication required for method: ...`.

Both variables default to empty (`${VAR:-}` in `.mcp.json`), so the server
registers even when neither is set. The config token used to go in
`Authorization`, so once `mcp.auth` was on every plugin call was a 401.

Claude then sees your endpoints as MCP tools. If the server runs with
`--config-service`, Claude also sees the `flapi_*` config tools. With those it can
list and edit endpoints, expand templates, refresh caches and read
`flapi_get_metrics`. Every `flapi_*` tool needs the config-service token, which
`X-Config-Token` carries. Endpoint changes show up in `tools/list` at once,
without a restart.

```bash
export FLAPI_CONFIG_SERVICE_TOKEN=$(openssl rand -hex 32)
export FLAPI_MCP_TOKEN=...   # only when mcp.auth is enabled
flapi -c flapi.yaml --config-service --config-service-token "$FLAPI_CONFIG_SERVICE_TOKEN"
# then start Claude Code in the same shell
```

### Hook: validate, hot-reload, then check the templates

After Claude edits a `.yaml`, `.yml` or `.sql` file inside a flAPI project (a
directory with a `flapi.yaml` in it or above it, or inside the `template.path` of
the config `FLAPI_CONFIG` names, e.g. a `../sqls` shared by a writer and a
replica), the plugin:

1. runs `flapi -c <flapi.yaml> --validate-config`, if `flapi` is on `PATH` or
   `FLAPI_BIN` is set. Any errors go back to Claude, which fixes them.
2. reloads the edited endpoint file on the running server, if validation passed
   and `FLAPI_CONFIG_SERVICE_TOKEN` is set (`POST /api/v1/_config/endpoints/reload-file`
   on `FLAPI_URL`; a new file is loaded too). For a `.sql` file, every endpoint
   YAML that uses it as `template-source` or cache `template-file` is reloaded.
   A file that no longer loads is rejected with its reason, and the endpoint
   keeps its previous configuration.
3. checks the affected templates without running them
   (`template/expand?validate_only=1`, see the
   [API reference](../CONFIG_SERVICE_API_REFERENCE.md)): the serving template
   once with no parameters and once with a sample value for every typed request
   field (`int` -> its `min` or 1, `string` -> a value matching its regex, `enum`
   -> the first value, `date` -> today, ...), so SQL inside `{{#params.x}}`
   sections is checked as well; a cache `template-file` with
   `target=cache&validate_only=1`, i.e. with the live watermark, per statement,
   with the bare-SELECT rule. Parse and bind errors (an unknown column, `MERGE
   INTOO`) go back to Claude. The message names the parameter sets it checked.
   A parameter used inside `AT (VERSION => ...)` / `AT (TIMESTAMP => ...)` gets
   no sample value: an invented snapshot does not exist.
   A cached endpoint whose cache table does not exist yet (seen on the
   no-parameter run) is a note ("cache not built yet; run `flapii cache refresh
   <path>`"), not an error; a failing sample run is always reported as the
   error it is. A validation that had to drop the cache's watermark says so:
   the next refresh would load the full source.

The hook first checks that the server at `FLAPI_URL` is this project's, and
only then sends the token: `GET /doc.yaml` (no token) must name this
`flapi.yaml`'s `project-name`, and then `template.path` must be this project's
directory. Otherwise it says which URL it found and sends nothing there. It
does not reload into a server that cannot take the edit either: a server
running from a packed bundle (`flapi pack`; edits on disk cannot reach it), or
one started before `flapi.yaml` changed when the `ducklake`, `auth` or `mcp`
settings it reports differ from the file ("restart the server first").

After a shell command that deletes an endpoint YAML in `template.path` (`rm`,
`git rm`, `unlink`, `mv` elsewhere), or an edit whose file no longer exists, the
hook passes the file to `reload-file`, which unloads what it declared
(`200 {"status":"removed","removed":[...]}`), and reports the removed endpoints.
A 404 means nothing was loaded from that file (a server too old to unload
deleted files answers 404 as well and keeps serving them until it restarts).

If something is missing (no binary, no project, no token, no server), the hook
does nothing. It never blocks an edit.

| Variable | Default | Used by |
|---|---|---|
| `FLAPI_URL` | `http://localhost:8080` | MCP server, hook, `flapii` (when `FLAPI_BASE_URL` is unset) |
| `FLAPI_CONFIG_SERVICE_TOKEN` | — | MCP server (`X-Config-Token`), hook, `flapii` |
| `FLAPI_MCP_TOKEN` | — | MCP server (`Authorization: Bearer`, for `mcp.auth`) |
| `FLAPI_BIN` | `flapi` on `PATH` | hook |
| `FLAPI_CONFIG` | nearest `flapi.yaml` | hook (also for files under its `template.path`) |

## Working on the plugin

The plugin lives in this repository under [`plugins/flapi/`](../../plugins/flapi/).
To try local changes without installing:

```bash
claude --plugin-dir ./plugins/flapi
```

`make check-skills` checks every skill against the real code and binaries:

- configuration keys against the parser
- `flapi`/`flapii` commands and options against `--help`
- cache template variables
- the version stamp
- every tagged example, which it runs through `flapi --validate-config` and a live server

CI runs the same check. Update a skill in the same pull request as the feature it
describes.
