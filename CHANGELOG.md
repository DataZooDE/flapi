# Changelog

All notable changes to flAPI are documented here. Versions follow `vYY.MM.DD` (the date the binary set was cut). Earlier history is in the git log.

## Unreleased

### Fixed

- **A POST whose template is one `SELECT` returns its rows (#271).** With
  `operation.returns-data: true` the rows of the template's last statement are `data`
  when DuckDB's parser reads it as a `SELECT`, also when it is the only statement and
  whatever it starts with (`WITH`, `FROM`, a comment). It used to need a second
  statement beginning with the word `SELECT`; anything else answered `"data": []`.
- **`operation.type: read` on a POST is a read over REST too (#271).** Such an endpoint
  now runs the read path with its parameters taken from the JSON body: validators,
  cache serving, pagination (`limit`/`offset`, also in the body) and the
  `{data, next, total_count}` envelope with `200`. MCP `tools/call` and `/doc.yaml`
  already treated it as a read; REST ran it as a write.
- **`RETURNING` is recognised from the statement's result, not its text (#277).** A
  column such as `returning_note`, a `'returning'` literal or a comment made a plain
  `UPDATE` report `rows_affected: 1` and return DuckDB's `Count` row as `data`, and
  hid a final `SELECT`.
- **The `next` link echoes the client's query string, percent-encoded.** It used to
  repeat every entry of the template context, unencoded: header-field values (a tenant
  header, say) and server-side `default:` values (which may come from `{{env.X}}`) went
  into the URL and `X-Next`, and a value with `&`, a space or `#` broke the link or added
  parameters. It is now the path, the new `offset`/`limit` and the query string as sent,
  minus `__auth_*` keys and names read from a header, the body or the path.
- **A read over POST pages forward.** Its `next` carries its query string; POSTing the
  same body to it moves on, because the query's `offset`/`limit` win over the body's
  (they used to be ignored, so `next` re-ran page one). Arrow pages with the body's
  window too.
- **`limit`/`offset` are validated wherever they are accepted.** A bad value is a JSON
  `400` `{"errors": [{"field": "limit", ...}]}` (it was plain text without a
  Content-Type). In a write's body they reach the template and must be plain
  non-negative integers. MCP `tools/call` with `"limit": "abc"` or `-1` is an `isError`
  saying `limit must be a non-negative integer` (it failed with `stoll`, or returned no
  rows).
- **A request body that is not a JSON object is a 400** (`request body is not a JSON
  object`), on writes and POST reads. It was read as empty: a POST read then returned
  every row, a write reported its fields missing.
- **A POST read refuses unknown body keys** (while `validate-before-write` is on, the
  default) and repeated query parameters, as writes, GETs and MCP do.
- **An unknown `operation.type`** (`raed`, `bogus`) stops startup and
  `--validate-config` with the allowed values; it silently kept the method default.
- **/doc.yaml documents a POST read's body** as `requestBody`, and its `200` response.
- **A comment after a write template's final `SELECT`** (`...; SELECT ...;\n-- done`) no
  longer hides the SELECT's rows.
- **`DatabaseManager::getTableColumns` binds its schema and table (#264).** It passed
  them to a call that never binds, so it returned no columns, and the names it would
  have returned were JSON-quoted.
- **`flapi.url.has_query` is `true` on a request with a query string (#279).** It was
  read from Crow's path-only `url`, so it was `false` on every span.
- **`GET /api/v1/_config/log-level` reports the running level (#279).** It answered
  `info` until a `PUT`, whatever `--log-level`, `FLAPI_LOG_LEVEL` or `log-level:` set.
- **REST audit lines carry a row count (#279).** `row_count` was `-1` on every REST
  line, and the request span had no `db.response.returned_rows`. A read records the
  rows of the page it returned (not `total_count`), a write its `rows_affected`.
- **`--validate-config` no longer creates the audit file (#279).** The file was opened
  while the configuration was parsed, so validation (and `flapi pack`) created it, and a
  missing directory failed as a bare configuration error. The server now opens it at
  startup; validation reports a missing directory, or a `file` sink without `path`, as an
  error on `audit.path`.

### Upgrade notes

- A POST endpoint with `operation.type: read` now answers `200` and the read envelope
  instead of `201` and `{"rows_affected": ...}`, and its template no longer runs in a
  write transaction. Remove `type: read` to keep the write behaviour.
- A POST read with `operation.type: read` now refuses body keys it does not declare
  (`400`) while `validate-before-write` is on, which is the default; set it to `false`
  to accept them.
- A non-object request body on a POST/PUT/PATCH/DELETE endpoint is now a `400`.
- `operation.type` must be `read` or `write`; run `flapi --validate-config`.
- The pagination `400` is now JSON in the validators' shape.
- `next` / `X-Next` no longer repeat header-field values or server-side defaults.
- A write endpoint with `returns-data: true` whose last statement is a `SELECT` that
  starts with `WITH`, `FROM` or a comment, or is the only statement, now returns that
  `SELECT`'s rows in `data` where it returned `[]`.
- REST audit lines now carry `row_count` (rows returned by a read, `rows_affected` by a
  write) where they said `-1`, and REST request spans gain `db.response.returned_rows`.
  A dashboard that filtered on `row_count = -1` to find REST traffic must filter on
  `method` instead.
- `--validate-config` with `audit.sink: file` and no `audit.path` now fails validation
  (it failed loading before). The server still refuses to start when the audit file
  cannot be opened, now after the database is up rather than while parsing.

## v26.11.26 — auth.* identity values stay inside their SQL literal, OIDC works again, and a task guide for every feature

### Security

- **`auth.*` template values are SQL-quoted (GHSA-r36p-v9m5-4f8q).** `auth.username`,
  `auth.email`, `auth.roles` and `auth.type` are rendered with every single quote doubled
  on REST and MCP (the config service has no caller identity, so `auth.*` renders empty
  there), so they are safe inside the documented `'{{{ auth.username }}}'` literal and a
  name such as `O'Brien` still matches its rows.
- **`auth.*` outside a plain top-level `'...'` literal of the outer query is an error.**
  Unquoted, in a comment, an `E'...'` string, a `"..."` identifier, a `$$` string, or in SQL
  text that another function executes (`query()`, `bigquery_query()`, `postgres_query()`,
  any `*_query`/`*_execute` call, a literal holding `''`, `$$` or `"`, or a call whose other
  arguments do, such as a macro that runs its argument): such a template stops startup,
  reload and `--validate-config`, is refused by the config service, and the rendered SQL of
  every request is checked before the values are substituted. Filter in the outer query
  instead. `{{ auth.x }}` now renders the same quote-doubled value as `{{{ auth.x }}}`.
- **An identity containing a control character, or a role containing a comma, is refused**
  with 401 on REST and MCP. `auth.roles` joins roles with commas and MCP `allowed-roles`
  splits on them, so a role `reader,admin` would read as two roles.

### Upgrade notes

- **Run `flapi --validate-config` before upgrading.** A template that places `auth.*`
  anywhere but a plain top-level `'...'` literal of the outer query now stops startup,
  reload and validation; move the filter into such a literal of the outer query.
- **`{{ auth.x }}` output changes** from HTML-escaped to quote-doubled, the same as
  `{{{ auth.x }}}`: a template that compared against `&#39;` or `&amp;` forms now matches
  the real value.
- Identity providers issuing a role name with a comma in it must rename that role.

### Documentation

- **A task guide for every feature area (#272), each run in CI.** New: Writing SQL
  templates, Connecting data sources, Observability, Managing flAPI from the command
  line. Reworked: Caching, Creating MCP tools, Authentication and authorization. Each
  guide has its own projects, a data download, the commands a reader runs and their
  real output, common mistakes and a production section; an agent crew (newcomer,
  fact-check against src, adopter) reviewed each one. Every Readme feature area links
  its guide.
- `make check-skills` runs the guides' examples: each project passes
  `--validate-config` with no warnings and its requests (REST, MCP `tools/call`, with
  credentials or headers) are checked against a live server. Test annotations are
  HTML comments readers never copy, every guide brings its own config, and every
  request CI makes is also shown to the reader.
- Reference fixes found while writing them: POST answers `201`; audit `status` values
  and fields; the response size is `http.response.body.size` (`flapi.result.bytes` is
  never set); `init` runs at startup; MCP denial is `-32000`/403; `_dryRun` runs no
  `EXPLAIN`; per-tool rate-limit defaults; `tasks/cancel` interrupts a running query;
  included files get `{{env.*}}` substitution.

- **The Readme is the entry point again (#268).** A Quick Start that needs only `uv`
  (`flapii project init`, then the same endpoint over REST and as an MCP tool), the
  v26.11 features (`duckdb-hardening`, `log-http-requests`, shipped extensions, the
  `-runtime` image, remote configs), an example that protects REST and MCP the way
  flAPI actually does (`auth:` for REST, `mcp.auth` plus `allowed-roles` for tools,
  no literal passwords), and corrected build, telemetry and `/mcp/health` facts.
- `make check-skills` now also checks `Readme.md`, `TELEMETRY.md` and `docs/`
  (not `docs/archive`, `docs/blog`, `docs/promotion`): forbidden facts, cache
  template variables, `flapi`/`flapii` commands and options, relative links and
  `#anchors`, and it runs the Readme Quick Start against the built binaries.
- Fixed: broken table-of-contents anchors in CONFIG_REFERENCE and CLI_REFERENCE;
  telemetry described as startup/shutdown events (it is `server_started` plus
  per-minute aggregates; `telemetry.sample_rate` is now documented); a "scheme
  whitelisting" section describing a check that never ran (template reads are
  governed by `duckdb-hardening`); MCP_REFERENCE's protocol table now lists
  `2026-07-28`, and `Mcp-Session-Id` is no longer described as required.
- `flapii project init` linked to a docs host that does not exist; it now links
  the guides on GitHub.
- The Docker image labels name the DataZooDE repository; the stale `version`
  label is gone (CI sets it from the tag).
- The `posthog-telemetry` submodule is cloned over HTTPS, so
  `git clone --recurse-submodules` works without a GitHub SSH key.

### Fixed

- OIDC: valid tokens were rejected (#273). Every token got 401 and the log said
  `JWT header missing 'alg' field`: the decoded JWT header and payload pointed into
  memory that was freed before they were read. REST (`auth.type: oidc`) and MCP
  (`mcp.auth.type: oidc`) were both affected. A token without `exp` is now refused
  by name (`Token missing 'exp' claim`) instead of as expired. A new end-to-end test
  runs the binary against a mock issuer.

## v26.11.25 — templates of remote configs are sandboxed, a DuckDB HTTP request log, and Secrets-Manager users out of SQL reach

### Added

- **A log of every HTTP request DuckDB makes (#252).** `log-http-requests: true`
  (or `FLAPI_LOG_HTTP_REQUESTS=1`, which wins when set) writes one `info` line per
  S3/GCS/HTTP(S) request into the flAPI log (Azure unverified):
  `duckdb http GET <url> -> 206 4ms request_id=req-...`. The id is the request
  whose query caused it, also on DuckDB's worker threads, or `-` for startup reads
  and the fetch of a remote `--config`. `ERR` marks a request that got no
  response. Headers are never printed; the URL keeps scheme, host and path, with
  userinfo dropped and the whole query `?redacted`, even if a template turns
  DuckDB's own redaction off. Secrets in URL paths are not redacted: a debugging
  switch, not for production. Off by default, and off it costs nothing: DuckDB
  logging is not enabled.
- The text log format escapes CR and LF in every message, so no logged value
  can forge a line; `PathSchemeUtils::RedactUrl` (used on every log line) now
  takes userinfo up to the last `@` of the authority, matches percent-decoded
  query keys, splits on `;` too, and knows more credential keys.

### Security

- **Secrets-Manager users are no longer readable by SQL templates (#254).** `auth.from-aws-secretmanager` stored
  the fetched user list (usernames, passwords, roles) in a table `auth_<name>` of the main DuckDB, so any
  template - including one created through the config service - could `SELECT` it, and the config service's
  schema API listed it. The list is now parsed once into flAPI's memory. **Behaviour change:** a template that
  read that table gets an error; `secret-table` is accepted and ignored with a warning. Tables an older version
  left in a file `duckdb.db_path` (`auth_<name>`, or the configured `secret-table`) are dropped at startup with a
  warning. **If you ran with a persistent `db_path`, rotate those passwords**: any template could read them.
- **The Secrets-Manager bootstrap credentials are no longer a DuckDB secret of the main database (#254).**
  `secret-id`/`secret-key` became `CREATE SECRET ... (TYPE S3, KEY_ID '..', SECRET '..')` in the database
  templates run on, where `duckdb_secrets()` listed it. They now go straight to the AWS SDK; with neither set,
  the SDK's default credential chain is used. A custom `init:` runs in a private DuckDB instance; a DuckDB secret
  of that name in the main database (a connection `init:`, `~/.duckdb/stored_secrets`) is still used as a
  fallback, with a deprecation warning.
- **Credentials are not logged (#254).** The parser logged the generated init SQL - `KEY_ID` and `SECRET` in
  clear - at debug level, and a failing init statement (any connection `init:` as well) put the statement into
  the startup error. Init errors now name the statement position and DuckDB's error with string literals and
  the statement excerpt redacted: every literal, quoted identifier and dollar-quoted body of the statement is
  replaced wherever the error repeats it, and connection-string secrets and URL credentials are masked. AWS error
  messages are logged as their first line, at most 300 characters, with access key ids masked.
- **Connection `init:` literals are no longer returned by the config APIs.** `GET /config`,
  `GET /api/v1/_config/project` and the `flapi_get_project_config` MCP tool returned every connection's `init`
  verbatim, passwords in `CREATE SECRET`/`ATTACH` literals included; string literals now read `'<redacted>'`.
- **Basic auth does not reveal which usernames exist.** An unknown username is checked against a dummy hash of the
  same kind and cost as the stored ones (inline `users:` and Secrets Manager alike), and plaintext and MD5 passwords
  are compared in constant time.
- **Templates of a remote config run with DuckDB's external access off (#253).** New top-level block
  `duckdb-hardening` (`external-access: auto|full|restricted`, `allowed-directories`, `allowed-paths`). `auto`, the
  default, is `restricted` when `--config` is remote (`s3://`, `gs://`, `az://`, `http(s)://`) and `full` otherwise,
  so local configs and packed bundles see no change. `restricted` preloads the extensions flAPI needs, allows the
  config's directory or URL prefix, `template.path`, the DuckLake `data-path`, `embed://` in a bundle and the listed
  extras, switches `enable_external_access` off and locks the DuckDB configuration. Before, a template could read
  any local file (`read_csv('/etc/passwd')`), any URL or bucket, ATTACH or COPY anywhere and LOAD any extension.
- While restricted, each template statement must be of an allowed type (queries, DML, MERGE, CREATE/DROP/ALTER of
  tables, views, macros, schemas, COPY, transactions, CALL, session-scoped SET; EXPLAIN and PREPARE judged by what
  they wrap). Refused: PRAGMA and IMPORT/EXPORT DATABASE, ATTACH/DETACH, INSTALL/LOAD, CREATE/DROP SECRET, SET GLOBAL.
  A template's temporary S3 secret with its own `ENDPOINT` redirected every endpoint's reads of the config bucket -
  and flAPI's own template reads - to another host.
- Restricted startup loads persistent secrets and allows the system CA bundle (plus `SSL_CERT_FILE`/`ca_cert_file`),
  so https and stored secrets keep working; `flapi cache refresh` applies the same policy.
- The policy is logged once at startup: `external access: restricted (...; allowed: ...)` or
  `external access: full (...)`.

### Upgrade notes

- **Remote-config deployments that read other buckets or local paths** must list them in
  `duckdb-hardening.allowed-directories` (or `allowed-paths`), or set `duckdb-hardening.external-access: full`.
  Without that, those reads now fail with `Permission Error: ... disabled by configuration`.
- Under `restricted`, an extension a template needs must be loaded at startup: `LOAD` it in a connection `init`, or
  list it in `duckdb.default-extensions`, which replaces the defaults (`httpfs, ducklake, fts, json, postgres,
  sqlite, parquet`). Secrets go in a connection `init`. A template may only `SET SESSION` (or `SET` a session-default
  setting) of `TimeZone`, `default_order`, `enable_profiling`, `tracked_metrics`, `schema`/`search_path`; `ATTACH`,
  `PRAGMA` and `SET GLOBAL` in templates fail.
- Under `restricted`, an endpoint's `auth.from-aws-secretmanager.init` runs with external access off and a locked
  configuration in its private DuckDB: creating its secret works; `INSTALL`, file and network access in it fail.
- Under `restricted`, `duckdb_extensions()` in a template fails: it scans the extension directory, which is not
  allowed (allowed directories are writable, and a file planted there would load at the next start).

### Fixed

- **The `-runtime` Docker image has `bigquery` again** (#251). `bigquery@community` is now published for DuckDB
  2.0.1; v26.11.23-runtime was built without it. The image build now fails instead of silently leaving it out,
  and CI starts the image with no network and `bigquery` in `duckdb.default-extensions`.
- **Secrets-Manager auth works without a session token (#254).** A DuckDB secret without `session_token` read
  back as the string `NULL`, so every request carried `X-Amz-Security-Token: NULL` and AWS refused it ("The
  security token included in the request is invalid"); an omitted `region` became the region `NULL`. The region
  is now the configured one, else `AWS_REGION`, else `AWS_DEFAULT_REGION`, else the active AWS profile's region
  (`AWS_PROFILE`, `AWS_CONFIG_FILE`), else a startup error naming the key.
- **A `secret-key` containing `'` works (#254)**: it broke the generated SQL and stopped startup.
- **Secrets-Manager endpoint override (#254).** `AWS_ENDPOINT_URL_SECRETS_MANAGER` is used as the Secrets Manager
  endpoint unless `AWS_IGNORE_CONFIGURED_ENDPOINT_URLS` is true, and startup warns while it is set: that endpoint
  decides who can log in. The generic `AWS_ENDPOINT_URL` (usually MinIO, for S3) is deliberately not used. Plain
  `http://` is accepted only to a loopback host, or with `FLAPI_ALLOW_INSECURE_SECRETS_ENDPOINT=1`.
- **Each secret is fetched once (#254)**, however many endpoints name it; endpoints inheriting the top-level
  `auth:` share its fetch. Blocks that share a `secret-name` but differ in region, credentials or `init` stop
  startup with an error naming both endpoints. The top-level `auth:` now reads `from-aws-secretmanager` at all - it
  was ignored, so its endpoints refused every login - but only a `type: basic` block uses it: a bearer or OIDC
  block with a leftover `from-aws-secretmanager` fetches nothing.
- `secret-id` without `secret-key` (or the reverse) is a startup error; it used to fall back to the ambient
  credentials silently. **Behaviour change:** without `secret-id`/`secret-key` and without `init:`, the credentials
  come from the AWS SDK's default chain (environment, `~/.aws/config` and `credentials`, including
  `credential_process`, SSO, web identity, container and instance roles) instead of a DuckDB `CREDENTIAL_CHAIN`
  secret.
- **Behaviour change:** `init:` no longer runs on the main database, so its statements have no side effects there
  (an `ATTACH` or `SET` in it is gone for templates). An `init:` that creates no matching S3 secret, with none of
  that name in the main database either, stops startup. One that uses `PROVIDER CREDENTIAL_CHAIN` loads the `aws`
  extension in the private instance.
- **Behaviour change:** a secret whose value is not `{"auth": [{"username", "password", "roles"}]}` - or has an
  entry without a non-empty username or password - stops startup, with an error that does not quote it.

## v26.11.23 — DuckDB 2.0.1, a faster and leaner server, and releases that ship their extensions

### DuckDB 2.0.1, and a faster, leaner server (#241)

- **DuckDB 1.5.5 → 2.0.1.** core_functions, parquet, json and icu are built into the binary (no download, never
  installed or packed); a state directory written by 1.5.5 (`duckdb.db_path`, DuckLake catalogs) keeps working
  unchanged (tested); files 2.0.1 *creates* use its newest formats (see Upgrade notes).
- **Throughput, latency and memory** (same 50-endpoint benchmark, 24 cores, against v26.11.21): a simple endpoint
  went from ~1,500 to ~7,600 req/s at 64 clients and from 22 to ~5,500 req/s on a single keep-alive client; point
  lookups 1.6-15x, parquet scans 2.4x at 8 clients and equal at 64; peak memory is 10-50% lower in every scenario,
  idle memory 214 -> 175 MB. Row-heavy pages at 64 clients are ~20% slower (DuckDB 2.0's prepared-statement
  execution) and startup takes ~0.1 s instead of ~0.05 s (DuckDB 2.0's database open). Causes, each measured on
  its own:
  - `TCP_NODELAY` on every connection: every keep-alive response after the first waited ~40 ms before.
  - The `COUNT(*)` for `total_count` runs only when the page cannot tell the total (a full page, or an empty page
    past the end); MCP tool calls never count; new endpoint option `total-count: false` drops `total_count` and
    `X-Total-Count` and never counts (`next` still works).
  - Per-thread connection reuse with cached prepared statements (`FLAPI_NO_CONNECTION_REUSE=1` opts out).
  - Rows are written straight into the response text (REST JSON, CSV, MCP); a 200k-row response peaked at
    ~640 MB before, ~260 MB now, and is ~3x faster.
- **Memory and threads.** DuckDB's async I/O pool is off when no remote storage is configured, and 2x the worker
  threads when it is; DuckDB `threads`, the HTTP handler pool and the allocator arenas follow the CPUs the process
  may use (affinity mask, container CPU quota) instead of every host CPU. Configured values (`duckdb: threads`,
  `duckdb: async_threads`, `DUCKDB_JE_MALLOC_CONF`) win.
- **Startup.** First start on an empty host 1.8 s → 0.1 s; caches whose table exists are ready at once after a
  restart (no burst of `503 cache_warming`).
- **Fixed:** an ENUM column could crash the server; NULL DATE/TIME/INTERVAL/ENUM values printed leftover memory;
  INTERVAL prints as DuckDB's text (`3 days 04:00:00`) and works in Arrow (was a 500); doubles in exponent form
  (`1e300` printed as `1000.0`); Arrow responses over ~2.7M rows failed; a response body over 1 MiB reset the next
  request on the same keep-alive connection; time travel to a future timestamp answers `400 snapshot_in_future`
  (was 500); populate-template validation missed errors under DuckDB 2.0 (`ATTACH ... (READ_ONLY)`, CTAS).
- **Telemetry** sends one aggregated event per route/status per minute (count, mean and max duration) instead of
  one per request.
- Crow 1.2.0 → 1.3.4.

### Releases carry their DuckDB extensions; stripped binaries (#244, #245)

- **Extensions ship with the release.** The release archives and the Docker images carry
  the signed default extensions (`httpfs`, `ducklake`, `fts`, `postgres`, `sqlite`) for their DuckDB version and
  platform in `extensions/<duckdb version>/<platform>/` next to the binary (`/app/extensions` in the images). flapi
  finds the directory by its real executable path (symlinks resolved), so a fresh
  host starts with no download and no network; `FLAPI_EXTENSIONS_OFFLINE=1` works with that directory alone. CI
  installs the files with each platform's own binary on a native runner and checks that every one loads from
  there with an empty HOME, offline.
- **Lookup order** (CONFIG_REFERENCE § 2.4): `duckdb.extension_directory` > `FLAPI_EXTENSION_DIRECTORY` > a packed
  bundle's extensions (each replaces the rest, so a packed binary never loads a second copy) > the shipped
  directory, searched after `~/.duckdb/extensions`. The shipped directory is only read: anything else installs
  into `~/.duckdb/extensions`, and a copy installed there on purpose (`FORCE INSTALL`) wins over the shipped one.
  A shipped directory for another DuckDB version is ignored with one warning.
- **Startup loads first and downloads only what is missing** from every search directory (it ran `INSTALL` for
  each default extension, which looks at the first directory only); the same for httpfs/azure on the remote-config
  and credential paths.
- **`flapi extensions install --defaults`** installs the default set; a release binary copies the shipped files
  into `--dir` instead of downloading them, so `flapi pack --with-extensions` works offline. `pack` still embeds
  extensions only when asked.
- **The default Docker image now has the extensions**; `-runtime` adds `bigquery` (best effort) to
  `/app/extensions` and no longer sets `FLAPI_EXTENSION_DIRECTORY`.
- **Stripped release binaries.** New CMake option `FLAPI_STRIP_BINARY` (off for local builds, on in CI) links
  `flapi` without its static symbol table: Linux amd64 122 MB → 102 MB. The dynamic symbols that loadable
  extensions resolve against are kept (verified: ducklake, postgres, sqlite, fts and httpfs load from a stripped
  binary). It replaces the release job's post-build `strip`, which skipped the arm64 and macOS binaries silently
  and stripped a different binary than the one the tests ran; macOS drops local symbols at link time (`ld -x`),
  so the linker's signature stays valid for `flapi pack`.

### Cached total_count and less work per request (#242, #243)

- **total_count of a DuckLake-cached endpoint is reused** until the catalog changes: the `COUNT(*)` behind a full
  page cost about as much as the page. The count is keyed by the rendered SQL and bound values (time travel
  included) and tagged with the catalog's newest snapshot id, read with one small metadata query per request, so
  any committed change - a refresh, a full rebuild, a write, a refresh by another process or the writer of a
  read-only replica - is seen by the next request and the total stays exact. Only queries that read nothing but
  `<alias>.<schema>.<table>` DuckLake tables (not views), do not sample and call no time-, session- or
  random-dependent function qualify - `age(ts)` included, which DuckDB reports as stable although it reads the
  transaction start (rule in docs/spec/components/caching.md). If the snapshot id cannot be read the cache is
  off for 30 s, then tried again. Measured on a 2M-row cached table (8 server CPUs): +15% req/s on one client and +20% on 16 for an unfiltered
  page, +50% / +30% for a filtered one.
- **Less CPU per request:** the endpoint is resolved once per request instead of three times (rate limit, auth,
  handler); a local SQL template is cached and revalidated with one `stat` instead of re-read (an edit is still
  served by the next request; a template written through the config service or a reload drops the cache, since
  two same-size writes within one filesystem timestamp tick look alike); the secret scrubber no longer copies the environment. Measured with 200 endpoints and a
  trivial query: +21% req/s on one client, +30% on four.

### Fixed

- **A rare SIGSEGV on SIGTERM** (#249): the server thread's stop poll called Crow's `Server::stop()` without
  the lock `APIServer::stop()` holds, so two threads could run it at once (Crow's stop is not thread-safe:
  both closed the same acceptor). Seen once in CI with an MCP task in flight; the shutdown tests now assert the
  acceptor is closed exactly once.

### Upgrade notes

- **`-runtime` image:** extensions moved from `/opt/flapi/extensions` to `/app/extensions` and
  `FLAPI_EXTENSION_DIRECTORY` is no longer set. A derived image that installs into `/opt/flapi/extensions` must
  set `FLAPI_EXTENSION_DIRECTORY=/opt/flapi/extensions` itself (then it holds the only directory) or install into
  `/app/extensions`.
- **The `flapi-io` wheels carry no extensions:** with them a wheel would be ~155 MB, above PyPI's 100 MB file
  limit. A pip-installed flapi downloads them on first start, as before (or set `FLAPI_EXTENSION_DIRECTORY`).
- **New database files and DuckLake catalogs are 2.0-only.** Files created by this version (a new `duckdb.db_path`,
  a new DuckLake catalog) use DuckDB 2.0's storage version and DuckLake spec 1.1, which DuckDB 1.x cannot read. To
  roll back to a 1.5-based flAPI keep a copy of the state directory, or rebuild the caches. Existing files are not
  converted.
- **`-runtime` Docker image:** `bigquery@community` is installed only when the community extension exists for
  DuckDB 2.0.1 (not yet at the time of writing); `INSTALL bigquery FROM community` at runtime works once it does.
- **Extension directory is per DuckDB version.** Offline images (`FLAPI_EXTENSIONS_OFFLINE`) must re-populate
  `FLAPI_EXTENSION_DIRECTORY` for v2.0.1 (`flapi extensions install`); core_functions, parquet, json and icu are no
  longer needed there.
- **SQL behaviour changes in DuckDB 2.0** that can affect templates: the single-arrow lambda `x -> ...` is an error
  (write `lambda x : ...`); `//` and `%` by zero raise an error; any error inside an explicit transaction aborts it;
  `MERGE`/`UPDATE ... FROM` with several matching source rows give a nondeterministic value (keep cache merge keys
  unique). See DuckDB's migration guide.
- **Arrow** honours an explicit `?limit=` / `?offset=` (it ignored them); without them it still streams the full
  result.
- **INTERVAL** values render as DuckDB's text (`1 day 00:01:00`), not `months:days.millis`.
- **`flapi.db.result_set_bytes`** (`tracing.db_profiling: summary`) is gone: DuckDB 2.0 has no result-size metric;
  the response size is on the request span as `flapi.result.bytes`.
- Nested STRUCT/MAP key order inside a row is not specified and can differ from earlier versions.

## v26.11.21 — remote templates authenticate on S3, GCS and Azure, and stay safe to operate

### Remote templates: credentials, hardening and operations (#234, #235, #236, #238)

- **`gs://` and `az://` authenticate for a remote `--config` / `template.path`** (#234): `GCS_ACCESS_KEY_ID` /
  `GCS_SECRET_ACCESS_KEY` (HMAC key, optional `GCS_ENDPOINT_URL`) and `AZURE_STORAGE_*` (through DuckDB's `azure`
  extension, loaded only when something reads `az://`). A custom `AWS_ENDPOINT_URL` works for S3-compatible stores.
- **Credentials are DuckDB secrets, not readable settings.** S3 (`zz_flapi_s3`), GCS and Azure credentials from the
  environment are created as secrets; httpfs's env-mirrored `s3_*` settings are blanked, so a SQL template can no
  longer read the key, secret or session token. A value you set yourself (`duckdb:` settings, a secret in a
  connection `init`) wins over the environment (#235).
- **Hardening for remote origins.** A `template-source` outside `template.path` (absolute local path, `..` in any
  spelling, another bucket or host) is refused when a remote config/template is loaded; `{{include}}` in remote YAML
  only reaches files below its own remote directory; URLs are redacted in logs, errors and config-service/MCP
  answers (`user:pass@`, credential-looking query values, fragments). The trust model is documented: write access
  to the bucket is code execution.
- **Config service and MCP tools with remote templates.** `GET .../template` reads them from storage;
  create/update/delete/reload and `reload-file` answer 501 with the next step instead of reporting success and
  writing a stray local directory.
- **Operations.** Startup fetches each remote file once (about 4 requests per endpoint, one listing request) and logs
  progress; a template past its cache TTL is refreshed by one reader while the others are served the old copy, a failed refresh serves it stale (15 s retry), and simultaneous first reads share one fetch; failing reads report
  the store's status and redacted URL; a failing remote `--config` reports the real cause; an intermittent SIGABRT
  on SIGTERM after remote reads is fixed; a remote base URL's `?query` (SAS token, signature) is kept on every file;
  relative `ducklake.*` / connection paths in a remote config resolve against the working directory;
  `flapi pack` warns that remote templates are not bundled.
- **`--validate-config` fails (exit 1) when a `template-source` file does not exist** (#236).

### Remote configuration and templates (#219, #198, #213)

- **A remote `template.path` loads.** `s3://`, `gs://`, `az://` and `http(s)://` template directories used to be
  joined onto the config directory and normalised into `<cwd>/s3:/bucket/...`, so startup failed with "Template
  path does not exist". They are now kept as URLs: endpoint YAMLs are listed with a glob (s3/gs/az) or a
  `manifest.txt` at the root (plain http(s), and the fallback when a glob finds nothing), and templates are read
  through DuckDB's httpfs. `--validate-config` checks remote templates too.
- **`--config http(s)://...` / `s3://...` boots.** Remote files are read through a private in-memory DuckDB (httpfs,
  credentials from the environment) that exists before `DatabaseManager` does.
- **`DuckDBVFSProvider` no longer caches a `FileSystem` pointer** (a dangling pointer after a database reset), and
  `FileExists` works for http(s) (httpfs answered `false` for every URL).

### Upgrade notes

- **Remote templates: behaviour changes.** (a) `--validate-config` now exits 1 for a missing `template-source` /
  `cache.template-file` file (it was a warning): a green CI gate may turn red; an endpoint YAML written before
  its SQL is the usual cause. (b) With a remote config or `template.path`, a `template-source` outside
  `template.path` now fails startup (local configurations only warn, as before). (c) `storage.cache` (on by
  default, `ttl` 300 s) now actually caches remote template reads: an edited remote `.sql` shows up within one TTL;
  set `storage.cache.enabled: false` for immediate edits. (d) Config-service and `flapi_*` MCP create/update/
  delete/reload answer 501 under a remote `template.path`. (e) S3/GCS/Azure credentials from the environment are
  secrets: templates that read `s3_secret_access_key` (or similar) from `duckdb_settings()` get empty values; a custom
  `AWS_ENDPOINT_URL` selects SSL from its scheme and uses path-style URLs (except `amazonaws.com` hosts).
  (f) Log and error text now redacts URL credentials/signature values: update log greps that matched them.
- **DuckDB's session `TimeZone` is `UTC` unless you set it.** Left alone, DuckDB takes the host zone (`TZ`,
  `/etc/localtime`) once its ICU extension loads, which happens lazily on the first query that needs it. A
  server in `Asia/Tokyo` then answered `now()::TIMESTAMP`, `date_trunc` and `TIMESTAMPTZ` -> `TIMESTAMP` casts
  nine hours off, from some minutes after startup. flAPI now applies `SET GLOBAL TimeZone` right after the
  engine opens and logs `DuckDB TimeZone: UTC`. Set `duckdb: {TimeZone: Europe/Berlin}` (key case does not
  matter) to choose another zone; if the host zone is what you relied on, set it explicitly there. If ICU
  cannot be loaded at that point (offline image without it) a warning says so and each connection tries its
  own `SET`.
- **`DECIMAL` values with more than 15 significant digits are JSON strings.** `DECIMAL(38,9)` `12345678901234567890.123456789`
  used to be rounded to the nearest double (`12345678901234567168`); it is now the exact string
  `"12345678901234567890.123456789"`. Decimals with at most 15 significant digits (and integer-valued ones up to
  18 digits) stay JSON numbers, so `price DECIMAL(10,2)` columns are unchanged. A typed client reading a wide
  `DECIMAL` as a number must parse the string.
- **A cache populate now waits for writes on a serialised source (and vice versa).** A populate that reads a
  SQLite attachment takes the connection's `serialize-access` lock, so a `POST` during a slow populate queues
  behind it (it used to run concurrently and fail the populate or the write with `database is locked`).
  Lock order is fixed: catalog write lock first, source lock second.
- **Identifiers flAPI builds are quoted.** A DuckLake `alias` or cache `schema` with a hyphen (`my-lake`,
  `my-schema`) booted into a syntax error or silently skipped the schema; the `ATTACH`, `CREATE SCHEMA`, audit
  table and `ducklake_*` calls now quote them. Your own templates must still quote
  (`"{{{cache.catalog}}}"."{{{cache.schema}}}"."{{{cache.table}}}"`).
- **Debug logging no longer prints connection secrets.** The `Template context:` line (which dumped
  `conn.*`, passwords included) is gone, and the `Processed query:` line has connection credential values
  replaced with `<redacted>`.


- **A refresh skipped because one of the same table is already running answers `409` with `Retry-After: 5`**
  (`{"status":"already_running"}`), no longer `200`. Nothing was refreshed, which a script could not tell from
  a refresh that ran. `flapii cache refresh` prints it and exits non-zero. `cache/gc` also reports `pinned_by`
  and `pinned_bytes` when another cached table's retained snapshot still pins this table's old files (it read
  `nothing_to_expire` / 0 bytes), and validating a populate template against an alias attached before an
  atomic rename of its file (`?target=cache&validate_only=1`) answers a `stale` warning instead of a bind
  error against the old file (Linux).

- **Path parameters are percent-decoded** (#220). The route still matches on the raw path, then each captured
  value is decoded once before validation: `/customers/%37` is id `7`, `/names/John%20Doe` reaches the template
  as `John Doe`, `%2F` decodes to `/` inside the one parameter, `+` stays a plus. A malformed escape (`%zz`),
  `%00` or invalid UTF-8 is `400`. A client that sent already-encoded text and relied on the server not
  decoding it now gets the decoded value.
- **`field-in` is honoured per declared source** (#192). A declared field is read only from its source: a query
  parameter no longer satisfies a `body` field (a required one is `400`), a body key no longer replaces a
  `query` field, and a route parameter (declared or not) cannot be overridden by `?id=` or a body key. Clients
  that passed a body field in the query string, or the other way round, must move it.
- **`{{{ params.x }}}` is bound for typed fields** (#192). Outside sections, `= {{{ x }}}`, `= '{{{ x }}}'`
  and `LIKE '%{{{ x }}}%'` now go to DuckDB as parameters (`?`, `?`, `('%' || ? || '%')`). Inside sections,
  comments, enum identifiers, typed literals (`DATE '...'`), names after `FROM`/`JOIN`/`BY`, and
  `AT (VERSION|TIMESTAMP => ...)` they stay raw text. An absent value in a quoted site still behaves as `''`.
  A template that relied on a typed field's triple-brace value being spliced as SQL (for example
  `IN ({{{ params.ids }}})` with a string validator) now compares against one text value; use a section or an
  `enum` instead. See `docs/CONFIG_REFERENCE.md` §5.9.
- **Endpoints whose routes differ only in parameter names are duplicates.** `GET /items/:id` and
  `GET /items/:item_id` match the same requests; both used to load and load order decided which one answered,
  so an endpoint with `auth: {enabled: false}` could shadow an authenticated one. Startup, reload and create now
  refuse the second one with `Duplicate endpoint GET /items/:item_id (same route as /items/:id)`.
- **SECURITY: request routing picks the most specific route, never the first one loaded.** Overlapping
  routes of one method (`/customers/7` and `/customers/:id`, `/a/:x/b` and `/:z/:x/b`) were resolved by
  directory order, and a reload moved the file to the end, so an open endpoint could answer a protected
  endpoint's URLs and a reload could flip a live URL from 401 to 200. Now, segment by segment from the left,
  a literal beats a `:param`, and rate limiting, auth and the handler always resolve the same endpoint.
  Endpoint files load in sorted path order.
- **SECURITY: overlapping routes whose more specific endpoint has looser auth are refused** (since
  widened to any auth difference, see below). Startup,
  reload, reload-file, create and update now fail with `Overlapping routes with different auth: GET
  /customers/7 (open) takes precedence over GET /customers/:id (basic, roles [admin]) for /customers/7 ...`
  (409 from the config service and the MCP tools). Set `allow-shadowing: true` on the more specific endpoint
  if it is meant to answer those URLs. Allowed overlaps with different auth log a WARNING, with equal auth
  an INFO line. A configuration that relied on the old first-match order may now stop at startup: fix the
  routes or the auth, or opt in.
- **SECURITY: overlapping routes are refused whenever their auth differs, in either direction.** A
  protected literal over an open `:param` route (`/customers/7` with basic auth beside an open
  `/customers/:id`) used to load with a WARNING, but it protects only the string `7`:
  `/customers/07` and `/customers/+7` reached the open endpoint and returned the same row without
  credentials. Such a pair is now refused like the reverse, unless the more specific endpoint sets
  `allow-shadowing: true`. The message says which endpoint wins for which URLs and that the other still
  serves every other URL its pattern matches. To protect a record, give both routes the same auth or filter
  rows by `auth.*` inside the general endpoint. **A configuration with a protected literal over an open or
  differently protected general route now stops at startup**: fix the auth or add `allow-shadowing: true`
  to the literal.
- **SECURITY: "same auth" includes the identity source.** Overlap checks compared only enabled, type and
  roles, so a literal with its own `guest` user (or another `jwt-secret`, `jwt-issuer`, OIDC issuer /
  audience / client, or AWS secret) counted as the same auth and served the protected URL to guest. Users
  (name, password, per-user roles), a digest of `jwt-secret`, `jwt-issuer`, the OIDC settings (including `scopes`,
  `verify-expiration` and `clock-skew-seconds`) and the `from-aws-secretmanager` source are now compared; any difference is "different auth" and refused as
  above. Messages name the differing field (`auth differs in: users`), never a secret.
- **`url-path` refuses `.` and `..` segments and a repeated `:param` name** (`/a/../b`, `/a/:id/b/:id`).
  A non-ASCII character in a url-path is now named whole with its code point (`'é' (U+00E9)`) instead of
  a broken byte.
- **The plugin hook passes `Overlapping routes with different auth` warnings** from `--validate-config`
  (an `allow-shadowing` split) to Claude as context, next to SECURITY WARNINGS.
- **SECURITY: `method` is normalised and limited to GET, POST, PUT, PATCH, DELETE.** It is trimmed and
  upper-cased at every entry point (endpoint files, the config service, `flapii endpoints create`, the MCP
  `flapi_create_endpoint` / `flapi_update_endpoint` tools); empty means GET. `method: get` beside a protected
  `GET` on the same route used to load (the duplicate check compared case-sensitively) and could answer
  without credentials; it is now `Duplicate endpoint GET ...`. Any other method (`HEAD`, `FETCH`, ...) is a
  load error / 400 naming the endpoint; such endpoints were never routable. A PUT or MCP update with
  `"method": "get"` on a GET endpoint is no change and succeeds (the REST PUT used to answer 400).
- **`url-path` is validated.** Segments must be literals of `A-Z a-z 0-9 . _ ~ -` or `:name`; one trailing
  `/` is allowed, `//` is not. `url-path: /customers/{cid}` used to pass `--validate-config` and then hang
  every request that reached it in the route scan (health checks included); it is now a load error / 400
  that suggests `:cid`. Literal segments are matched literally (`.` used to match any character).
- **MCP `flapi_create_endpoint` takes an `auth` argument** (the shape of a YAML `auth:` block), creates the
  endpoint through the same loader as `POST /api/v1/_config/endpoints` (so a top-level `auth:` block is
  inherited and duplicate/overlap checks apply), names the conflicting endpoint when it refuses, and
  reports `"auth"` plus a `"warning"` (and a log line) when the new endpoint is unauthenticated. Creating
  `POST /x` beside `GET /x`, or `/x/7` beside `/x/:id`, is no longer refused as "Endpoint already exists"
  (it still is when the file it would write, e.g. `x.yaml`, exists).
- **Cache keys `rollback-window` and `delete-handling` are deprecated.** They were accepted but never did
  anything. They still load, and startup now logs a warning for each. Handle deleted rows in the merge
  template instead; see [Deletes in merge mode](docs/guides/caching.md#deletes-in-merge-mode).

Authentication and requests:

- **A top-level `auth:` block now protects every endpoint that has no `auth:` block of its own.** Before, it
  was ignored for REST endpoints. A deployment with global auth enabled now answers 401 on endpoints that were
  open. An endpoint's own block replaces the global one (no merge); keep an endpoint public with
  `auth: {enabled: false}`. Startup logs how many endpoints inherit the global block and how many opt out.
- **SECURITY: an `auth:` block without `enabled` is on when it configures authentication.** A block that sets
  `type`, `roles`, `users`, `jwt-secret`, `jwt-issuer`, `oidc` or `from-aws-secretmanager` but no `enabled`
  used to default to `enabled: false`: such an endpoint was open, and under a global `auth:` block it counted
  as an opt-out. It now requires authentication; only an explicit `enabled: false` turns auth off. Endpoints
  configured this way that answered without credentials now answer 401. The same rule applies to the
  top-level block, which used to fail startup without `enabled`. Startup logs the effective setting.
- **`auth.roles` on REST endpoints is enforced.** A caller who authenticates but holds none of the listed roles
  gets 403 `{"error":"Forbidden",...}`. Before, any authenticated user got through.
- **An endpoint `auth:` block with no `type` and no credentials (e.g. `roles:` only) uses the top-level
  block's type and credentials** and applies its own roles on top. Before, it had type `""` and answered
  401 to every request. Without an enabled top-level block such a block now stops startup with
  `AUTH_NO_TYPE`, and so does a block with `users:` or a secret but no `type:` (both refused every request
  before).
- **401 responses carry a JSON body** `{"error":"Unauthorized","message":"..."}` instead of the plain text
  `401 Unauthorized`, and a `WWW-Authenticate` challenge for the endpoint's own scheme: `Bearer realm="flAPI"`
  for bearer and OIDC endpoints (with `error="invalid_token"` when a token was refused), `Basic
  realm="flAPI"` for basic. Before, every endpoint challenged `Basic`, and a refused token got no challenge.
  Clients that matched the old text or the Basic challenge on bearer endpoints must be updated.
- **`field-in: header` fields are read from the request headers** (case-insensitive), on GET and on writes. A
  query or body parameter of the same name no longer satisfies them.
- **`method: DELETE` endpoints run their SQL template** like the other write methods (parameters, validation,
  cache handling) and answer 200 `{"rows_affected": n}`. Before, a DELETE endpoint only invalidated its cache
  or answered 405.
- **Write endpoints report the real `rows_affected`:** the number of rows the statements changed (summed over a
  multi-statement template; with `RETURNING`, the rows returned). Before, a statement without `RETURNING`
  always reported 1 - an `UPDATE` of five rows and a `DELETE` of a missing id alike.
- **A REST 500 answer is JSON and no longer contains the database error:**
  `{"error":"Internal Server Error","message":"...","request_id":"req-..."}`. The full error is in the server
  log under that request id.
- **JSON output of numbers and times changed:** DECIMAL values are exact (for scale 0 with up to 18 digits, or
  scale up to 6 with up to 9 integer digits) instead of rounded through a double; TIMESTAMP and TIMESTAMPTZ
  keep microseconds, TIMESTAMP_S/_MS are correct, TIMESTAMP_NS has 9 fractional digits, NULL is `null`.
- **An unknown `log-level` in the config stops startup** with the list of valid levels.
- **OIDC:** an `issuer-url` that still contains `{tenant}`, `{realm}` or `{domain}` after the preset is applied
  stops startup with a message naming the key to set.
- **GCS:** `GOOGLE_*` environment variables are not used for `gs://` access (DuckDB cannot use service-account
  credentials); startup now warns. Create an HMAC key and add `CREATE SECRET (TYPE gcs, KEY_ID ..., SECRET ...)`
  to a connection's `init`.
- **An unknown `auth.type` stops startup and is refused on reload** with `AUTH_UNKNOWN_TYPE`, naming the
  endpoint and the valid types (`basic`, `bearer`, `jwt` as an alias of bearer, `oidc`). This applies to
  endpoint blocks, the top-level `auth:` block and `mcp.auth.type` when enabled. Before, a typo such as
  `type: basci` loaded, answered 401 to every request and logged nothing. Fix the spelling. `mcp.auth.type:
  jwt` now works as an alias, too.

Templates, requests and responses:

- **A template with unbalanced Mustache sections is a configuration error.** A stray `{{/x}}` crashed the
  server and an unclosed `{{#x}}` hung it (and grew memory without bound). Now unbalanced or crossed
  sections, `{{{` not closed by `}}}`, an unterminated `{{`, empty tags, partials (`{{> x}}`) and
  set-delimiter tags (`{{= =}}`) stop startup, fail a reload (400) and a config-service template update
  (400, file unchanged), and make `--validate-config` exit 1, each naming `<file>:<line>:<col>`. A
  template edited on disk without a reload answers 500 instead of crashing. A template that used partials
  or `{{= =}}` must be rewritten without them.
- **JSON body numbers keep their value.** `9.99` was stored as `9`, and `301.7` passed an `int` validator
  as `301`. An `int` field now rejects any non-integer, including `5.0` and `1e3`.
- **`number` validators read `min` and `max`** (decimals allowed) and enforce them. A config that set them
  (ignored until now) may now reject requests.
- **A query parameter given twice (`?x=1&x=2`) is a 400** `{"error":"Duplicate query parameter: x"}`. Before,
  the first value was used.
- **A `method: DELETE` endpoint is a write**, also as an MCP tool: the tool reports the write result and the
  endpoint's cache is maintained. Before, the row was deleted but the tool reported an error.
- **Validator range messages name the limit**: `Integer is greater than the maximum allowed value (max 1000)`
  (also for string length, number, date and time bounds). Clients that compare the exact text must update.
- **`template/expand?validate_only=1` binds the SQL that runs, statement by statement.** It used to EXPLAIN
  the pasted rendering as one string; it now validates the prepared rendering (typed `{{ params.x }}` as `?`),
  parses each statement separately and binds it in a rolled-back transaction. A template that was reported
  valid can now be `valid: false` (a binder error inside a rendered section, a broken second statement), and
  one that was reported invalid only because of pasted values (`c = ALFKI`) is now valid. Errors carry
  `statement` and `kind`; a cached endpoint reading a cache not built yet is a `cache_not_built` warning, not an
  error.
- **`template/expand?target=cache` of a template with a Mustache syntax error is a 400** naming
  `<file>:<line>:<col>`; it was a 500.
- **The bare-SELECT refusal of a populate template is worded for a guard:** a SELECT before the last statement
  is refused with "cache template statement N of M is a bare SELECT; its result is discarded. Wrap a check in
  CREATE OR REPLACE TEMP TABLE _chk AS SELECT ...". Clients that compare the old text must update.
- **Response formatting:** row keys (JSON and CSV) follow the `SELECT` column order; `DOUBLE`/`FLOAT` print
  in shortest form (`711.56`, not `711.55999999999994543`); a naive `TIMESTAMP` has no `Z` suffix (only
  `TIMESTAMPTZ` does). Applies to REST, MCP `tools/call` and `resources/read`.

Caching:

- **Cache refreshes run one at a time per DuckLake catalog.** A refresh of another table waits for the running
  one (scheduled, manual, warm-up, one-shot and write-triggered alike); a second refresh of the same table still
  answers `already_running` at once. Compaction, garbage collection, `drop-table` and
  `?target=cache&validate_only=1` take the same lock. A manual refresh can therefore take longer to answer
  while another cache refreshes.
- **`/health/live` answers 503 `{"status":"fatal","reason":...}` when the DuckDB database has been
  invalidated** by an internal error, and so does `/health`; data and config-service requests answer 503
  `database_invalidated` (they were a 500 each, forever). A liveness probe now restarts such a process.
- **The `full` field of a refresh answer is what the refresh did**: `true` also for a full-mode cache and for an
  incremental cache's first build, not only for `?full=true`. The audit table gets a `full` column
  (added to existing catalogs by the writer at start); the audit routes return it.
- **The cache scheduler is on by default.** Every `cache.schedule` now runs without
  `ducklake.scheduler.enabled: true`; set it to `false` to keep the old behaviour (startup warns).
- **No refresh at startup beyond the warm-up.** A cache's first scheduled refresh is one `schedule` after its
  last real refresh (this run's warm-up, the newest snapshot, or process start). Manual and config-service
  refreshes count.
- **`ducklake.scheduler.scan-interval` is honoured** (default: the shorter of `heartbeat.worker-interval` and the
  shortest `cache.schedule`, at least 1 s). `heartbeat.worker-interval` defaults to 60 s whether or not a
  `heartbeat:` block exists (it was 10 s with one). An invalid scan interval or a worker interval of 0 or less
  stops startup.
- **`refresh-on-write` now refreshes the cache**, after the response (it did nothing before).
  `invalidate-on-write` refreshes before the response, as before. With both set, only the synchronous refresh
  runs. MCP tool writes follow the same rules.
- **A populate template that is a bare `SELECT` is refused** before it runs, and a template that does not leave
  the cache table in place fails and rolls back. Before, both "succeeded" and the endpoint then failed.
- **A cache without `template-file` materialises the endpoint template.** Its `template-source` was the populate
  SQL and, as a bare `SELECT`, built nothing. When it renders to a single `SELECT` it now runs as
  `CREATE OR REPLACE TABLE <cache table> AS <select>`: a full refresh every time, whatever `primary-key` and
  `cursor` imply (audited as `full`). Startup warns `cache on <endpoint> has no template-file`. Declare
  `template-file` for incremental refreshes.
- **An endpoint whose cache failed its first build answers 503 `{"error":"cache_build_failed"}`** (with
  `Retry-After: 5`) instead of `cache_warming`.
- **`ducklake.compaction.schedule` is parsed:** an interval (`6h`, `1d`) or `@hourly`, `@daily`, `@midnight`,
  `@weekly`. Anything else (for example a cron expression) stops startup. Before, compaction ran every 24 hours
  whatever the value.
- **Global `ducklake.retention` applies** to every cache without its own `retention:` block (an endpoint block
  replaces it). Caches that kept every snapshot may now expire old ones, so time travel to them stops working.
- **Expired snapshots' files are deleted** after retention or `cache gc` runs, so disk space is reclaimed.
- **The cache audit log keeps the newest 1000 rows per cache table.**
- **`POST .../cache/refresh` answers JSON:** `{"status":"refreshed","endpoint","mode","full","duration_ms",
  "rows_affected","snapshot_id"}` (or `"status":"already_running"` when a refresh of that table is in flight),
  and errors as `{"error":...,"reason":...}`. Before, success was an empty 200 and errors plain text.
  `flapii cache refresh` prints these fields.
- **Full rebuild drops the table:** `flapii cache refresh <path> --full`, `flapi cache refresh --full` and
  `POST .../cache/refresh?full=true` erase the watermark and drop the cache table in the same transaction, so an
  append cache is rebuilt instead of re-appended.
- **Retention now expires what it should, so more history goes.** It never matched a cache in the default
  schema `cache` (DuckLake records the create as `'"cache".t'`) or with a keyword table name, nor the earlier
  incarnations of a cache (each CTAS full refresh and each `--full` rebuild makes a new table id). Such caches
  kept every snapshot and file; under `keep-last-snapshots` / `max-snapshot-age` they now keep only what the
  policy says, and time travel to the expired snapshots stops working.
- **`POST .../cache/gc` answers JSON** `{"status":"expired"|"nothing_to_expire"|"no_policy"|"error",
  "expired_snapshots","files_deleted","bytes_freed","message"}` instead of the text "Garbage collection
  completed", and a failed GC is HTTP 500 (was 200). Without a retention policy it is `no_policy`.
- **`snapshot_id` in the refresh answer and the audit is the snapshot that refresh committed**, or absent/null
  when it committed none or it is unknown. It used to be a guess (often another schema's table, or a previous
  refresh).
- **A read-only replica answers 409 `read_only_cache` to `PUT .../cache/template`** (it overwrote the
  writer's populate SQL). The 409 messages now say what to do for each operation.
- **Time travel to an unknown or expired snapshot on a cached endpoint is 404**
  `{"error":"snapshot_not_found"}` (was 500).
- **New key `ducklake.compaction.delete-threshold`** (0 < x <= 1): a value outside that range stops startup.

- **SECURITY: MCP `resources/read` validates uri-template values.** A value bound from
  `flapi://customers/{id}` now gets the field's `default:` and validators (regex, enum, int, ...) and the
  unknown-parameter check, like `tools/call`; a failing value answers `-32602`. Before, it reached the template
  unvalidated. A `{var}` without a matching `request:` field is now refused as an unknown parameter: declare it.
- **`resources/read` is capped at 1000 rows by default** (`mcp-resource.response.max-rows`, else the file's
  `mcp-tool.response.max-rows`); a capped read carries `_meta.truncated`. `redact-columns` applies too.
- **A config-service slug served by several endpoints needs `?method=`** (`GET /orders` + `POST /orders`):
  without it every `/api/v1/_config/endpoints/{slug}/...` route answers 400 "Ambiguous ... specify
  ?method=GET|POST" instead of acting on one of them. `flapii` takes `--method`; the `flapi_*` endpoint tools
  take `method`. `GET /api/v1/_config/endpoints` keys such endpoints `"GET /orders"` / `"POST /orders"`
  (a path served once keeps its plain key).
- **Two files with the same MCP tool, resource or prompt name stop startup** (and fail a reload), with an
  error naming both files. Before, both loaded and `tools/call` ran whichever came first.
- **Config-service 401s are JSON** `{"error": "Unauthorized: missing or invalid config-service token ..."}` on
  every route (most answered plain text). A `flapi_*` MCP tool without a valid token answers JSON-RPC `-32001`
  naming `X-Config-Token` (it was `-32603 "Tool execution failed: ..."`).
- **`template/expand` and MCP `_dryRun` show the SQL that runs:** a typed `{{ params.x }}` is a `?` with its value
  in the new `bindings` list, instead of the value pasted in. `_dryRun` `parameters` are typed (`3`, not `"3"`)
  and `structuredContent.rows` is `[]`.
- **Metrics:** MCP role denials no longer count as tool `failures` or `server_errors` (they are `rejected`).
  Percentiles are interpolated inside finer buckets (ten per decade) instead of reporting the bucket's upper
  bound, so p50/p95/p99 values change. `/health` `caches.ready` no longer counts stale caches (see
  `caches.stale`).
- **`flapi cache refresh`** exits `2` (not `1`) for an argument it does not know.
- **`flapii config validate`** checks the `template.path` of `--config` (or `./flapi.yaml`) instead of
  `examples/sqls`, and fails when the config cannot be read.

Config service, MCP and CLI:

- **Two endpoint files with the same `url-path` and `method` stop startup**, with an error naming both files.
  Before, both loaded and only one answered. A reload that would create such a pair fails (400, with the
  reason), and creating an endpoint that a YAML file in the templates directory already declares - loaded or
  not - answers 409 instead of writing a second copy.
- **Created endpoint files get readable names:** `/made-by-cli` is `made-by-cli.yaml` and `/a/b` is `a_b.yaml`
  (REST `POST .../endpoints` and MCP `flapi_create_endpoint`); before, the slug escaping gave
  `made~1by~1cli.yaml`. A name that is taken answers 409.
- **MCP `flapi_update_endpoint` persists:** the change is merged into the endpoint's YAML like REST `PUT`, and
  takes any endpoint key (`config: {...}` or top-level keys). Before, it read only `method` and
  `template_source` and changed memory only. Changing the method is now refused, as with `PUT`.

- **Endpoints created or updated through the config service are written to disk.** `POST
  /api/v1/_config/endpoints` writes `<template dir>/<slug>.yaml` and answers 201 (409 if it exists, 400 on a
  parse error). `PUT` merges the change into the YAML file (comments in it are lost); changing `url-path`,
  `method` or the name answers 400, and endpoints that use `{{include}}` answer 409. `DELETE` removes the YAML
  file. Error bodies are JSON `{"error": ...}`.
- **A reload of an endpoint whose YAML is now invalid answers 400** with the reason; 404 means the endpoint is
  unknown. Reload errors carry an `error` field like create/update/delete.
- **A partial `PUT /api/v1/_config/endpoints/{slug}`** (`{"template-source": "x.sql"}`) merges onto the
  endpoint; it answered 500 `Missing required field in endpoint config`. A body that cannot be read is 400.
- **`PUT /api/v1/_config/project` and `DELETE /config` answer 405** with `Allow: GET`.
- **`GET /api/v1/_config/schema?tables=true`** returns tables only (no views), each with `name`, `schema`,
  `database`, `type`, `connection` and `qualified_name`; filter with `?connection=`. Failures answer 500.
- **MCP:** `tools/list`, `resources/list` and `prompts/list` follow endpoint changes without a restart; a
  pagination cursor from before a change is rejected.
- **Metrics:** a 503 because a cache is not ready is counted as `status.503_cache` and
  `requests.cache_unavailable`, no longer as a 5xx or a server error.
- **`flapii` option renames:** `flapii schema get --connection` (no `-c`), `flapii schema get --table`
  (no `-t`), `flapii cache update --ttl` (no `-t`). `flapii cache refresh --force` is deprecated and is a normal
  refresh; use `--full` for a full rebuild.
- **`flapii --json-style` re-cases only configuration documents** (endpoints, MCP tools/resources/prompts, cache
  config, project, templates list). Data such as metrics, cache audit, schema, health, template expansion and
  validation is printed exactly as the server sends it, in snake_case (`duration_ms`, `rows_affected`).
- **MCP `tools/call` failures no longer return the error text.** A SQL or template failure is a result with
  `isError: true` and `Tool execution failed on the server. The details are in the server log under request_id
  req-...`, like a REST 500. It used to return the database error, the SQL and absolute template paths. Read the
  detail from the server log. Bad arguments keep their specific messages.
- **MCP with `mcp.auth`: refused credentials answer `The credentials sent were not accepted.`** (still HTTP 401,
  `-32000`) instead of `Authentication required for method: ...`, which is now only sent when no credentials
  were sent. Clients that matched the old text for a bad or expired token must be updated.
- **The Claude Code plugin's MCP server sends the config token as `X-Config-Token`**, and `Authorization:
  Bearer ${FLAPI_MCP_TOKEN}` for `mcp.auth`. Set `FLAPI_MCP_TOKEN` if your server has `mcp.auth` on; before, the
  config token went in `Authorization` and every call was a 401 once `mcp.auth` was enabled.
- **Write responses list `rows_affected` first, then `data`, whose rows keep the `RETURNING` column order.**
  An MCP write result always has `data` (`[]` when no row came back). Clients that parse JSON are unaffected.
- **Metrics latency is measured from the start of the request** and so includes authentication (a Basic-auth
  endpoint's PBKDF2 check), rate limiting and middleware rejections. Latencies are rounded to three decimals.
  Numbers for authenticated endpoints are higher than before because they are now what clients see.
- **`--validate-config` lists every invalid endpoint or template file** instead of stopping at the first, and
  warns about an undeclared `{{ params.x }}` in an endpoint template (a name not in its `request:` fields, its
  `url-path` `:names`, or `limit`/`offset`). The warning does not change the exit code.

### Fixed: observability gaps and two product gaps (#215, #217)

- MCP JSON-RPC responses carry `X-Request-Id`; the access-log `Request:` line carries `request_id`.
- Async MCP tasks report end-to-end (submit to finish) latency as `protocol: "mcp_task"` series in the metrics.
- Swagger UI at `/doc` no longer loads from unpkg.com: swagger-ui-dist 5.18.2 is built into the binary and served
  from `/doc/assets/` (about 1.8 MB larger binary).
- Config-service `PUT` keeps the comments of an endpoint YAML file.

### Added: compaction on demand, `--method` for one-shot refreshes

- **`flapii cache compact [path]`** / **`POST /api/v1/_config/cache/compact`** run DuckLake compaction now, for
  the whole catalog or one endpoint's cache table, whatever `compaction.schedule` says. The answer is
  `{status, duration_ms, files_rewritten?}`; 409 on a read-only replica. A scheduled run now logs
  `DuckLake compaction is due ...` when it starts.
- **`flapi cache refresh --method <M>`** picks one of several cached endpoints on a path.

### Added: checking cache templates before the refresh does

- **`POST .../template/expand?target=cache&validate_only=1`** checks the cache populate template without running
  it: rendered with the live watermark, split into statements, parse errors reported per statement, the
  bare-SELECT rule applied, and every statement bound in a transaction that is rolled back (a `CREATE TABLE ...
  AS` bootstrap is created there `WITH NO DATA`, so the MERGE after it is checked before the first build).
  `MERGE INTOO`, `SELEC oops;`, an unknown column and a bare SELECT used to pass every check and fail at the
  next refresh. Answer: `{valid, statements, errors[{statement, type, kind, message}], warnings, expanded}`.
- **`GET /api/v1/_config/project` has `bundled`**: `true` when the server runs from a packed binary.
- **The Claude Code hook validates, not just renders:** cache templates with the new check, serving templates
  with `validate_only` also with a sample value for each typed request field (so SQL inside `{{#params.x}}` is
  checked), naming the parameter sets it checked. A cache not built yet is a note ("run `flapii cache refresh
  <path>`"); a packed server or one serving another `template.path` is not reloaded, and the hook says so.
  `FLAPI_CONFIG` now also covers files under its `template.path` outside the config's directory.
- **`flapii` says a request timed out** ("request timed out after Ns; the server may still be working (a refresh
  keeps running); raise --timeout and check `flapii cache audit <path>`") instead of "Cannot reach flAPI ... Is
  it running?".

### Added: reload by file, and safer MCP resources

- **`POST /api/v1/_config/endpoints/reload-file {"file": "<yaml>"}`** (`flapii endpoints reload --file <yaml>`)
  reloads every endpoint a YAML declares and LOADS a hand-written file the server has not loaded yet - a new
  endpoint goes live without a restart. `flapii endpoints create -f` of a file already in `template.path`
  answers 409 and points to it. The Claude Code plugin hook uses it, then renders the affected templates and
  reports render/SQL errors, a rejected token, `SECURITY WARNINGS`, and "restart the server" for `flapi.yaml`.
- **`mcp-resource.response`** (`max-rows`, `redact-columns`).
- **`?method=`** on every config-service endpoint route, `--method` on the `flapii` commands.
- **`GET /api/v1/_config/cache/audit?table=schema.table`** (`flapii cache audit --table`): one cache table's
  refreshes, whichever endpoint sharing it triggered them; `flapii cache audit -o table` shows the message.
- **A dropped cursor watermark is reported:** when the watermark does not look like `cursor.type`, the refresh
  answer has a `warning`, the audit row's message says so, and `template/expand?target=cache` returns
  `watermark_dropped_reason`.
- **Metrics:** a `rejected` count per series (REST 401/403 now attributed to their route; MCP role denials);
  `flapii metrics -o table` shows p50 and rejected. `/health` reports `caches.stale`; a cache's
  `data_age_seconds` falls back to the newest snapshot after a restart.

### Added: Claude Code plugin

flAPI now ships a Claude Code plugin:

```
/plugin marketplace add DataZooDE/flapi
/plugin install flapi@flapi
```

- Ten skills cover setup, REST endpoints, MCP tools, authentication, cache design, cache refresh-time tuning,
  testing, live configuration, deployment and operations.
- Seven slash commands: `/flapi:init`, `/flapi:new-endpoint`, `/flapi:add-cache`, `/flapi:auth-setup`,
  `/flapi:deploy-check`, `/flapi:diagnose` and `/flapi:metrics`.
- An MCP server entry for your running flAPI.
- A hook that validates your config after every edit and hot-reloads the affected endpoints.

See [docs/guides/claude-code.md](docs/guides/claude-code.md).

### Added: runtime metrics

`GET /api/v1/_config/metrics`, the new `flapii metrics` command and the new `flapi_get_metrics` MCP tool now
report:

- request counts, status classes and latency (p50/p95/p99/max) for every endpoint and MCP tool
- 401/403/429 rejection counts
- for every cache: readiness, last refresh mode/status/duration/rows, and the age of the data served

Narrow to one endpoint with `?endpoint=/path`.

### Added: authentication, requests and caching

- `auth.type: jwt` is an alias of `bearer`; a global `auth:` block accepts `roles`.
- OIDC presets take `tenant-id` (or `tenant`), `realm` and `domain`, which fill `{tenant}`, `{realm}` and
  `{domain}` in the issuer URL; Keycloak's default issuer is `https://{domain}/realms/{realm}`. Provider
  `azure`, `azure-ad` and `entra` are aliases of `microsoft`.
- Validators: `enum` accepts `values` or `allowedValues`, `string` accepts `pattern` or `regex`.
- Log level `warn` is accepted as `warning` (`--log-level`, `FLAPI_LOG_LEVEL`, `log-level:`, and at runtime
  `PUT /api/v1/_config/log-level` / `flapii config log-level set warn`).
- `ducklake.compaction.schedule` without `compaction.enabled: true` logs that it has no effect.
- `project-description` is optional.
- Startup warns about keys that have no effect: a request field's `field-type`, `server-name`, and a
  connection's `log-queries`, `log-parameters` and `allow`.
- The security audit warns `CORS_WILDCARD_WITH_AUTH` when authentication is on and `cors.allow-origins` is
  empty or `*`, and `AUTH_BCRYPT_UNSUPPORTED` for bcrypt hashes, which cannot log in. Its advice for passwords
  is a PBKDF2 hash: `passlib.hash.pbkdf2_sha256.using(rounds=600000).hash(...)`.
- Full cache rebuild: `flapii cache refresh <path> --full`, `flapi cache refresh --full`,
  `POST .../cache/refresh?full=true`.
- `flapi cache refresh` reports `duration_ms` and `rows_affected` per cache (and `"full": true` with `--full`),
  refreshes a table shared by several endpoints once, and compacts when compaction is due.
- Startup logs `Cache scheduler on: N scheduled cache(s), checked every X s`, warns when a schedule is shorter
  than the scan interval, and logs `flAPI server listening on http://<host>:<port> (REST API; MCP at
  .../mcp/jsonrpc)` once the port is bound.
- `GET /api/v1/_config/schema?connections=true` lists the configured connections.
- Config-service endpoint JSON shows `auth.roles` and whether the auth block is `inherited`.
- MCP tool schemas carry typed `default` values.
- `flapii` reads `FLAPI_URL` when `FLAPI_BASE_URL` is not set; `flapii cache audit -o table` prints a table;
  `flapii metrics -o table` shows cache-not-ready 503s separately.

### Added: cache retention, replicas and packaging

- Drop an obsolete cache table (one left behind by a changed `schema:`/`table:`) with
  `flapii cache drop-table [schema.]table` or `POST /api/v1/_config/cache/drop-table {"schema","table"}`.
  Its snapshots are expired and its files deleted. A table an endpoint still uses is refused with 409.
- `ducklake.compaction.delete-threshold`: the share of deleted rows above which compaction rewrites a data
  file (DuckLake's default, 0.95, is rarely reached by merge caches).
- `GET /api/v1/_config/cache/audit?sync_type=compaction` (any `sync_type`, combinable with `?table=`) shows
  compaction history. Startup logs when compaction is next due.
- `GET /api/v1/_config/project` shows `ducklake.warmup_effective` (`never` on a read-only replica).
- A read-only replica serves a cache as soon as the writer has built it. It checks again at most every 5 s on
  requests, and on every scheduler scan. Before, a replica started before the build answered 503
  `cache_not_built` until restarted.
- `flapi pack --exclude <glob>` (repeatable). The configured DuckLake catalog (`metadata-path`, its `.wal`,
  `data-path`) and `*.ducklake`, `*.ducklake.wal`, `*.ducklake.files` are never bundled.
- `embed://<path>` works outside a packed binary: it resolves against the configuration directory, so a
  config written for `flapi pack` also runs from its source tree.
- A missing local DuckLake metadata directory or `data-path` is created. A read-only replica that finds one
  missing stops and says so.

### Fixed

- **Two concurrent refreshes could invalidate the database until restart.** Populate templates that
  `DETACH DATABASE IF EXISTS src; ATTACH ... AS src (READ_ONLY)` (the recipe the caching skill taught) ran in
  parallel for different tables and collided on the alias: `INTERNAL Error: Database name src was already used
  by a different database for this meta transaction`, after which every request failed with
  `database has been invalidated` while `/health` stayed 200. Refreshes are serialised per catalog, and an
  invalidation from any cause now makes the health probes fail (see Upgrade notes).
- **The cache readiness registry follows endpoint changes.** A reload that changed `cache.table` made the GET a
  500 (it is 503 `cache_not_built` until built, or served when the table exists), and the old table's entry -
  including a `failed` one - stayed in `/health` (`caches.total` too) until restart.
- **`?target=cache&validate_only=1` executes a template's `ATTACH ... (READ_ONLY)`**, so statements reading the
  attached source are checked instead of failing with `schema src does not exist`; it detaches what it
  attached. A writable `ATTACH` is not executed and is reported `not_checked`.
- **A cached `GET /orders` beside an uncached `POST /orders`:** `flapi cache refresh --endpoint /orders` picked
  the POST and failed with `has no cache` (exit 2), and the config-service `/cache/*` routes answered
  `Ambiguous`. Only endpoints with a cache count now.
- **Time travel to a snapshot from before the cache's schema existed** answers 404 `snapshot_not_found`, not 500.
- **Read-only replicas:** a cache the writer has not built says so (`the writer has not built it yet`), not
  `warm-up skipped`; a replica that cannot attach its catalog says to start the writer first, and
  `DuckLake catalog attached read-only` is logged only after the attach succeeded.
- **`reload-file` of a deleted endpoint YAML** unloads the endpoints it declared (200 `"status":"removed"`)
  instead of answering 404 and leaving the routes loaded.
- **`drop-table`** applies the retention of live tables whose snapshots still pin the dropped table's files right
  away and names, in `pinned_by`, the endpoint whose policy keeps the rest.
- **Catalog-wide compaction audit rows** have an empty `cache_schema` (it was `main`).

- **SECURITY: Basic auth re-derived PBKDF2 (~165 ms of CPU) on every request,** wrong passwords included, so
  a flood of bad passwords was a cheap denial of service. A successful check is now remembered for 5 minutes
  (bounded to 10 000 entries, keyed by an HMAC under a per-process random key; no password is stored); a wrong
  password is never remembered; a changed hash or any auth (re)load invalidates it. At most half the cores
  derive PBKDF2 at once, and a request that waits more than 2 s for a slot is refused. Repeat requests with
  the right password drop from ~165 ms to ~1.5 ms.
- **Logs:** every Crow `Response: ...` access line has its `request_id` (only 500s had one), and with
  `log-format: json` the startup lines written while the configuration is read are JSON as well.
- **MCP `structuredContent.rows`** keep the SELECT column order (the keys were scrambled), and doubles keep
  their shortest form.
- **`flapii --version`** prints the package version. **`-q/--quiet`** shows no spinners or progress lines but
  still reports a failure, in one line (it used to print the spinner's "Failed" line and hide the reason). An
  error body that only repeats the message is no longer printed a second time.
- **Claude Code hook:** it checks without the token (`GET /doc.yaml`'s project name) that `FLAPI_URL` is this
  project's server before sending the token there; says "restart the server first" when `flapi.yaml`'s
  `ducklake`/`auth`/`mcp` settings changed since the server started; unloads an endpoint YAML a shell command
  deleted (`rm`, `git rm`, `mv`) via `reload-file`, and says so when the server cannot; gives no sample value
  to a parameter used inside `AT (...)`; never reports a failing sample run as "cache not built"; and says a
  dropped watermark means the next refresh would load the full source.
- **SECURITY: DuckLake catalog credentials were shown in clear.** A Postgres or MySQL `metadata-path` with
  `password=...` (including a resolved `{{env.X}}`) appeared in `GET /api/v1/_config/project` and in startup
  attach errors. Passwords, secrets, tokens and URL `user:password@` parts are now `<redacted>`.
- **Packed binaries:** `flapi cache refresh` and `flapi extensions install` read the bundled `flapi.yaml`.
  They used to fail with "Could not open file: flapi.yaml". The one-shot refresh resolves `embed://` paths too.
- **Retention with a same-named table in another schema** works for each table. Before, retention was off for
  both.
- **GC and retention report what they could not expire.** Snapshots that are shared with another table are
  logged as a WARNING and noted in the audit message.
- **Lock conflicts on a DuckDB-file catalog explain themselves:** stop the other process, or use a
  Postgres/SQLite/MySQL catalog for a writer with replicas. The one-shot refresh no longer prints a spurious
  "Failed to detach DuckLake catalog".
- **`--validate-config` with `template.path: ../sqls`** no longer reports path traversal or missing files
  for templates that exist. It warns only when a template is outside `template.path`.
- **`GET /api/v1/_config/metrics?endpoint=/x`** lists the endpoint's cache before its first refresh, and every
  cache row names its `endpoint`.

- **CORS preflights:** with `cors.allow-origins` set, an `OPTIONS` preflight from an allowed origin answered
  204 without any `Access-Control-Allow-*` header (REST paths and `/mcp/jsonrpc`), so browsers blocked every
  authenticated or JSON cross-origin call. Preflights now get `Access-Control-Allow-Origin`, `-Methods`,
  `-Headers` (the requested headers, unless `cors.allow-headers` is set; `Authorization`, `Content-Type`,
  `X-Config-Token` and `Mcp-Session-Id` by name when auth is on and none were requested), `-Max-Age: 600` and
  `Vary: Origin`. Responses that echo an allowed origin also carry `Vary: Origin`.
- **Auth config:** keys the chosen auth type never reads (`users:` under `bearer`, `jwt-secret:` under
  `basic`, ...) in an endpoint block, the top-level block or `mcp.auth` are named in a load warning; one
  scheme applies per block. The audit line of a 401 names the endpoint's scheme instead of always `basic`.
- **Security audit:** endpoint locations include the method (`endpoint GET /orders`), an empty
  `mcp.auth.jwt-secret` is reported as `AUTH_EMPTY_JWT_SECRET` at startup, and that warning says what happens:
  authentication fails closed and every token is refused.
- **Config service, shared url-paths:** reloading the `POST /orders` file replaced `GET /orders` and answered
  200 with the change not applied; `DELETE` of one removed both; `PUT` updated the first. Each now acts on the
  endpoint named by slug + `?method=` (reload: on that endpoint's file).
- **`resources/list` no longer lists uri-templated resources** (their `flapi://<name>` could not be read); they
  are in `resources/templates/list`. An empty resource result is `"[]"`, not `-32603`.
- **`POST .../endpoints/by-template`** also finds endpoints whose cache `template-file` is that file
  (`role: cache.template-file`).
- **`flapii`:** a `:` in a path (`/products/:id`) is sent literally (the server also decodes `%3A`); a 401 after
  sending a token says the token was rejected; `endpoints list -o table` shows `REST+Tool` and the connection of
  MCP-only tools and resources.
- **`flapii project init`:** include comments use `../common/...` (resolved from `sqls/`), the scaffolded
  `auth.yaml` has `enabled: true`, `.gitignore` no longer ignores `data/`, and `sample.sql` has no `LIMIT 100`
  (it broke pagination totals).
- **`flapi cache refresh`** accepts `--log-level` and `--no-telemetry` after the subcommand.
- **Messages:** a failing connection `init` at startup names the connection (`connection 'x' init: ...`); the
  description scanner names the matched phrase and its offset; the cursor-watermark warning says "an integer";
  the GCS credentials warning appears only when the configuration uses `gs://` or GCS; a rejected
  config-service token is logged.
- **MCP `tools/list`** no longer advertises `"minimum":0,"maximum":0` on every `number` parameter; bounds
  appear only when set. MCP `initialize` reports the build version instead of `0.3.0`.
- **Pagination:** `limit=0` returns the count with an empty `next` (it pointed at itself), and `?format=csv`
  is accepted under `request-fields-validation: true`.
- **Writes:** with `returns-data: true`, a `RETURNING` that matched no row answers `"data": []` instead of
  omitting `data`; a missing required field is listed once in `errors`, not twice.
- **`/doc.yaml`** types parameters from their validators (with bounds and typed defaults), documents body
  fields in the request body, adds `limit`/`offset`/`format`, the 400 response and the `X-Total-Count`,
  `X-Offset`, `X-Limit`, `X-Next` headers, and uses the endpoint's new `description:` key (or its MCP tool
  description) instead of "Description not available". An undescribable result no longer emits
  `properties: ~`.
- **Cached reads on a `serialize-access` connection run in parallel.** A read served from the DuckLake cache
  (endpoint with `cache.template-file`) no longer waits for the source connection's lock (16 clients:
  93 -> 283 requests/s in our measurement, the same as with `serialize-access: false`).

- **`flapi --version`** and `server.version` in `GET /api/v1/_config/health` show the real build version.
- **Passwords:** PBKDF2 hashes are no longer flagged by the security audit, and passlib's own encoding as well
  as standard and URL-safe base64 are accepted.
- **Config service:** creating an endpoint keeps its `mcp-tool`, validators and `rate-limit`; `endpoints
  validate` uses the real parser and says what to pass when no YAML was given.
- **MCP config tools** (`flapi_*`) accept the same tokens as the REST config service and read `X-Config-Token`
  first, then `Authorization: Bearer`. Startup warns about config tokens shorter than 16 characters.
- **`flapii` errors** show the status and the server's reason (`[400] Bad Request: ...`) followed by the JSON
  body, instead of only the status text.
- **Metrics:** percentiles never exceed the observed maximum.
- **Caching:** a table shared by several endpoints is warmed once; SIGTERM interrupts running populate
  scripts so shutdown takes seconds, and no refresh starts while shutting down; a failed warm-up over an
  existing table keeps serving it (state `stale`); a failed first build is logged once; audit `event_id` values
  are UUIDs; compaction's last run survives restarts.
- **`flapi pack`:** `--validate-config` on a packed binary no longer warns about a missing `sqls/sqls`
  directory.
- **`flapii project init`** creates the `data/sample.csv` its sample endpoint reads.
- **`async-after-ms`** is read as a whole number of milliseconds.
- **Cache audit log:** every refresh now records its real start and end time, `duration_ms`, `rows_affected`
  and the `snapshot_id` it produced. Before, start and end were the same instant and the other three were
  always empty, so refresh time could not be measured.
- **`duckdb.default_order: DESC` with DuckLake:** every commit into the DuckLake catalog failed
  (`Failed to flush changes into DuckLake: Could not convert string '...' to INT64`), so no cache was built and
  no audit event was recorded. DuckLake reads a table's columns with a bare `ORDER BY` on its own connection,
  and the global DESC reversed them. With DuckLake enabled, `default_order` is now applied per flAPI connection.
- **Global `rate_limit:`:** `rate_limit: {enabled: false}` stopped startup with `bad conversion`, because `max`
  and `interval` were required even with the limiter off. They now default to 100 and 60, as on endpoints.
  A malformed value names its key.

## v26.10.31 — security hardening, and serverless startup without data or network on the boot path

### Upgrade notes — these changes can turn a request or a config that worked into an error

- **Validator:** a request value that contains a single quote **and** an SQL-looking character or word is
  rejected with HTTP 400, even at a bound `{{ }}` site. This covers `( ) = < > + * / % & | ;`, `or`, `and`,
  `like` and similar, e.g. `O'Brien (retired)`. A lone apostrophe (`O'Brien`) is fine.
  Opt out per field with `preventSqlInjection: false`.
- **Validator:** `number`, `float`, `double`, `boolean`, `integer` and `bool` fields are now actually
  validated, so non-numeric or non-boolean values get HTTP 400.
- **Pagination:** `?limit=` and `?offset=` must be plain non-negative integers (HTTP 400 otherwise).
- **Cached endpoints:** `?cacheTable=`, `?cacheSchema=` and `?cacheCatalog=` in a request are ignored. The
  configured values are always used.
- **Rate limits:** limits are now matched by path **and** method, and counted per route rather than per literal
  path, so `/items/:id` shares one bucket. Limits that were silently bypassed now apply.
- **`cache.schedule`:** an invalid schedule (`5x`, `0s`, more than 10 years) stops startup instead of crashing
  the server later.
- **`flapi pack`:** symlinks are refused. The secret deny list is case-insensitive and covers `.env.*`, so
  `.env.example` is refused too (use `--allow-secrets`). A damaged bundle stops startup.
- **Config service:** template paths that resolve outside the templates directory can no longer be read or
  written through the management APIs (HTTP 400).
- **CORS:** an `Origin` not in `cors.allow-origins` gets no CORS headers (previously `*`).

Everything new for serverless deployments is opt-in; every default is unchanged.

### Docker

New `ghcr.io/datazoode/flapi:<version>-runtime` image with `core_functions`, every default DuckDB extension and
`bigquery` pre-installed, starting offline (`FLAPI_EXTENSIONS_OFFLINE=1`). `:latest` stays the slim image.

### Added: serverless startup without data or network on the boot path

Running flAPI on scale-to-zero platforms (Cloud Run, App Runner) loaded every cache from its source on every
instance start, and installed DuckDB extensions over the network on every boot. Each of these is now optional:

- `ducklake.warmup: always | if-empty | never` (default `always`, unchanged), per-endpoint `cache.warmup`, and the
  overrides `--cache-warmup <mode>` / `--no-cache-warmup` / `FLAPI_CACHE_WARMUP`. A skipped cache whose table
  does not exist is *unbuilt*: its endpoint answers `503 cache_not_built`, and `/health` answers `200 degraded`
  with an `unbuilt` list.
- `ducklake.read-only: true` attaches the catalog `READ_ONLY` for replicas of a catalog another process writes:
  no warm-up, no scheduled refresh, retention or compaction, and `409` from the config service's cache mutations.
- `flapi cache refresh --config … (--endpoint /path … | --all)` refreshes once and exits (0 ok, 1 a refresh
  failed, 2 usage), so the writer can be a job instead of an always-on server.
- `duckdb.default-extensions: [...]` replaces the hard-coded extension list; `flapi extensions install` pre-installs
  a config's extensions (defaults and every `INSTALL` in `connections.*.init`) at build time;
  `FLAPI_EXTENSION_DIRECTORY` points startup at them, and `FLAPI_EXTENSIONS_OFFLINE=1` turns a missing extension
  into a startup error instead of a download.
- `flapi pack --with-extensions` (or `--extensions-dir <dir>`) embeds the config's DuckDB extensions in the
  bundle. The packed binary extracts them once to a content-addressed directory and starts offline, so a
  single file runs with no network.
- A `ghcr.io/datazoode/flapi:<version>-runtime` image with `core_functions`, every default extension and
  `bigquery` pre-installed.
- Found on the way: flAPI's DuckDB loads no extension statically, so even `core_functions` was downloaded on the
  first query of every fresh container. `flapi extensions install` always includes it, and offline startup
  checks it explicitly.
- New guide: [docs/guides/serverless.md](docs/guides/serverless.md).

### Security: request-validator hardening

Found by a review of the server and reproduced against a real binary:

- The SQL-injection screen was bypassed with whitespace: `x'<newline>OR 2>1<newline>OR n = 'x` passed it and,
  spliced into `WHERE n = '{{{ params.name }}}'`, returned every row. The screen now collapses whitespace and
  inspects the whole value whenever it contains a single quote. It is still a heuristic: **use `{{ params.x }}`
  (bound) for request values**; the docs now say so (CONFIG_REFERENCE 5.9).
- `number`, `float`, `double`, `boolean` (and the aliases `integer`, `bool`) were treated as safe-to-skip-the-screen
  but were not validated at all, so `0 OR true` reached a `{{{ params.amount }}}` site. They are validated now.
- A request could override a cached endpoint's server-owned `cacheTable` / `cacheSchema` / `cacheCatalog` with
  `?cacheTable=...` and make the template read another table. Those values are now always the configured ones.

**Behaviour change:** a value containing a single quote **and** an SQL-looking character or word
(`( ) = < > + * / % & | ;`, `or`, `and`, `like`, ...) is rejected (HTTP 400), even for a bound `{{ }}` site -
e.g. `O'Brien (retired)`. A lone apostrophe (`O'Brien`) is fine. Set `preventSqlInjection: false` on a field you
use only through `{{ }}` if you need such values.

### Security: three rate-limit bypasses

Found by a review of the server and reproduced against a real binary:

- The limiter looked the endpoint up by path only, so a limited `POST /items` was never limited when
  `GET /items` had no limit (and the reverse). It now looks up by path **and method**.
- The bucket key included the literal request path, so `/items/:id` got a fresh bucket for every id. The
  bucket is now per route.
- With `rate-limit.key: user` or `user-or-ip` the bucket was a hash of the raw `Authorization` header before
  it was verified, so rotating invalid credentials gave a fresh bucket on every request (unlimited password
  guessing). Every such request now also counts against its client IP with a ceiling of 10x the per-user
  limit.

### Fixed: an invalid `cache.schedule` crashed the server; a failed refresh took a working cache offline

Found by a review of the server and reproduced against a real binary:

- `cache.schedule: 5x` (or `0s`, or a value that overflowed such as `30000d`) was accepted at startup and then
  terminated the whole process about two scans later. Schedules are now validated when the configuration
  loads (and by the cache API, HTTP 400): `<positive number>[s|m|h|d]`, at most 10 years. The scheduler
  also contains any exception per endpoint, and a failing scheduled refresh waits one interval instead of
  retrying on every scan against a source that is down.
- A failed refresh of an already-built cache turned the endpoint into HTTP 503 although the last snapshot was
  fine. It now keeps serving that snapshot; `/health` reports it as `degraded` with a `stale` list (HTTP 200).
  A cache that never built is still 503.
- The raw DuckDB error (SQL, file paths) was returned to anonymous callers in the 503 body (`detail`) and by
  `/health` (`error`). Both are removed; the error is in the server log.

### Security: `flapi pack` and `unpack` are safe against hostile input

Found by a review of the server and reproduced against a real binary:

- `flapi unpack` wrote a bundle entry named `../escaped.txt` outside `--to` (and followed symlinks already in
  the destination). Entries that are absolute, contain `..` or backslashes are now refused.
- `flapi pack` followed symlinks, so a link to a file outside the input tree was bundled. Symlinks are now
  refused.
- The secret deny list was case-sensitive: `.ENV`, `SECRETS/token`, `server.PEM` were bundled. It is now
  case-insensitive and also covers `.env.local`-style files (so `.env.example` is refused too; use
  `--allow-secrets` for those).
- A small bundle could inflate without limit. Reading a bundle is now capped at 1 GiB decompressed and
  100000 entries (`FLAPI_BUNDLE_MAX_MIB`, `FLAPI_BUNDLE_MAX_ENTRIES`), enforced while inflating.
- A damaged bundle made `info`/`unpack` abort, and a binary with a damaged bundle silently ran the
  `flapi.yaml` next to it instead. `info`/`unpack` now report an error, and a damaged bundle stops startup.

### Security: PUT of an endpoint's own configuration no longer strips its protections; pagination is validated

Found by a review of the server and reproduced against a real binary:

- `GET /api/v1/_config/endpoints/{slug}` followed by `PUT` of the returned JSON dropped every request
  validator (so `amount=500` was accepted on an endpoint that limited it to 100), dropped the endpoint's
  auth users (leaving authentication enabled with nobody able to pass, or a config that no longer matched the
  one served), and re-enabled a cache the endpoint had disabled. `PUT` now applies the JSON on top of the
  endpoint it replaces, so anything the JSON omits is kept, and `GET` now returns every validator field
  (`allowedValues`, `min`/`max` for dates and times, `min-length`/`max-length`, `preventSqlInjection`) so the
  round trip is lossless.
- `?limit=` and `?offset=` were parsed with a numeric prefix, so `limit=1;SELECT 1` or `limit=1 OR 1=1`
  reached the SQL text of `LIMIT {{params.limit}}`. They must now be plain non-negative integers (HTTP 400
  otherwise) and the template receives the normalised numbers.

### Security: the config service could write files outside the templates directory; CORS allowlist; token echo

Found by a review of the server and reproduced against a real binary:

- A config-token holder could create an endpoint whose `template-source` (or cache `template-file`) was an
  absolute path or `../…`, then read or overwrite that file through the template routes (REST, and MCP
  `flapi_update_endpoint`). Template paths used through the management APIs must now resolve - symlinks
  included - to a file inside the templates directory; anything else is rejected with HTTP 400. **Behaviour
  change:** an endpoint whose YAML points its template outside the templates directory can no longer have
  that template read or written through the config service (edit the file directly).
  Nested relative sources such as `reports/daily.sql` work, and their directories are created.
- A request from an `Origin` that is not in `cors.allow-origins` still received
  `Access-Control-Allow-Origin: *`. It now gets no CORS headers.
- With the config service enabled the server printed its bearer token to stdout even when you supplied it.
  A supplied token is no longer echoed; a generated one (the only place you can learn it) still is.

### Fixed: the VS Code endpoint tester no longer persists credentials

An `Authorization` header, API key, token or password typed into the endpoint tester's headers or parameters
(or into the workspace default headers) was saved in the workspace state file. Credential-looking names are
now saved with an empty value: the open panel still sends the real value, and after the panel is reopened
the name is restored empty for you to re-enter. Request history was already redacted.

### Fixed: VS Code "New endpoint" creates the endpoint on the server

- The explorer's "New endpoint" command used to write `examples/sqls/<name>.yaml` and `.sql` into your local
  workspace, which is only right when the server happens to read that folder. It now creates the endpoint on
  the flAPI server itself: it is live immediately, its YAML and SQL are saved in the server's templates
  directory (so it survives a restart), and nothing is written locally.
- You now pick the connection from the ones the server has configured, instead of a hard-coded `default`
  that usually did not exist. Errors (unknown connection, rejected path) show the server's message.
- The endpoint YAML is built by the server from the values you typed, never assembled from text in the
  extension, so a hostile name or path cannot inject YAML or escape the templates directory.
- The new endpoint's files are no longer opened in an editor afterwards; find it in the flAPI explorer.

### Fixed: the VS Code extension and the CLI share one ConfigService transport

- The extension's YAML validator and reload used a second HTTP client that ignored `flapi.insecure`, had no
  retry policy and reported errors differently from the rest of the extension. All ConfigService calls now go
  through the same client the CLI uses: the token (sent as `X-Config-Token` and `Authorization`), TLS
  verification, timeout and retries behave identically everywhere, and reads are retried while writes (POST,
  PUT, DELETE) never are.
- New settings `flapi.timeout` (seconds, default 30), `flapi.retries` (default 3) and `flapi.insecure`
  (default off) are declared in the extension, and changing `flapi.serverUrl` or any of them now takes effect
  for every view and the YAML validator without reloading the window.
- The endpoint tester honours `flapi.timeout` and `flapi.insecure` too (self-signed HTTPS servers work, a
  hung server no longer hangs the tester). It talks to your generated REST routes and never receives the
  config-service token.
- `FlapiApiClient` (`@flapi/shared`) accepts `baseUrl`/`authToken`/`timeout`/`retries`/`verifyTls` and still
  accepts `baseURL`/`token`/`debug`. Its `timeout` is now in seconds, like `flapi.timeout` and `--timeout`.

### Fixed: VS Code "Validate SQL Template" now validates the SQL in the editor

It validated the saved endpoint YAML and then said the SQL template was valid, so broken or unsaved SQL
passed. The command now sends the editor's current text to the server, which renders the Mustache and
EXPLAINs the result against the endpoint's connection (nothing is saved) and reports the real error.
`POST .../template/expand?validate_only=1` accepts an optional `template` field for this.

### Fixed: VS Code extension security and correctness (from a review, each reproduced against a real server)

- The extension logged the full request headers, including the config-service token, whenever the token
  changed. It no longer logs credentials, and request headers it does log (and keeps in history) have
  `Authorization`, API-key and similar values redacted.
- The token is stored in VS Code SecretStorage instead of the workspace state file; an older stored token is
  migrated and removed.
- The endpoint and SQL tester webviews rendered server-supplied column names, errors and the configured
  url-path/method as HTML, so a hostile name ran script in the webview. They now render text, and both pages
  carry a Content-Security-Policy with a nonce (inline `onclick` handlers were replaced).
- Explorer open/delete/rename commands refuse a server-supplied path that resolves outside the templates
  directory.
- Saving or reloading an endpoint YAML reloaded the wrong slug (taken from the file name rather than the
  url-path / MCP name) and always reported success. It now reloads the endpoint the server knows and reports
  the real result.
- `POST /api/v1/_config/endpoints/by-template` now finds endpoints created through the API.
- Removed `EndpointEditorProvider`, a custom editor that was never registered.
- The extension's tests now include a real-server suite (`npm run test:integration`, run in CI and by
  `make cli-integration-test`).

### Fixed: more CLI behaviour that did not match the server

- `flapii` no longer repeats a write whose response was lost (`endpoints create`, cache refresh, delete,
  ...). Only reads are retried, and HTTP 429/5xx on reads are now actually retried (they never were).
  The CLI and the VS Code extension now share one HTTP client instead of two drifting copies.
- `flapii cache update` sends settings the server reads: `--schedule`, `--table`, `--schema`, `--enabled`.
  `--ttl`, `--max-size` and `--strategy` were never read by the server and now fail with a message.
  `PUT .../cache` no longer fails (500) when `enabled` is omitted.
- `flapii endpoints wizard` produced a payload and a YAML file the server could not read. It now creates a
  working endpoint with its SQL template (`--output-file` writes the YAML and a `.sql` next to it).
- Connection failures, a missing or wrong token, and a server with the config service off now say what to do.

### Fixed: the CLI listed only one MCP entity, printed its token, and misreported templates

Found by a review of the CLI and VS Code extension, then reproduced against a real server:

- `GET /api/v1/_config/endpoints` keyed MCP tools, resources and prompts by their (empty) url-path, so
  only the last one survived. They are now keyed by name, and `flapii mcp tools|resources|prompts list`
  see all of them.
- `flapii config show` printed the config-service token. It now shows `<redacted>`.
- `flapii templates get` printed `[object Object]`, and `flapii templates test` reported every working
  template as invalid. Both now match what the server returns.
- The CLI's own integration suite could not start (it did not enable the config service) and some of its
  tests passed without testing anything. It now runs against a real server on a temporary copy of
  `examples/`, and runs in CI (`make cli-integration-test`).

## v26.10.2 — safer configuration: env variables are enforced, dead keys are reported, and the docs match the code

### Fixed: `flapii project init` scaffolded a configuration flAPI does not read

The generated `flapi.yaml` used `description:` and a top-level `template-source:` (the real keys are
`project-description` and `template: {path: ...}`), so the sample endpoint was never loaded. The
sample endpoint also lacked `method`, the auth template used unsupported `${VAR}` references, and the
rate-limit template used keys flAPI never reads. The scaffold now generates the real keys, with an
`environment-whitelist` for the variables its templates mention, and passes `--validate-config`.

### Fixed: `flapi_delete_endpoint` now removes the endpoint's YAML file

Since `flapi_create_endpoint` persists its endpoint, deleting one only from memory made it return on
the next start or reload. Delete now also removes the endpoint's YAML file (only that file, only
from inside the templates directory); the SQL template is left in place. This applies to
hand-written endpoint files as well.

### Fixed: `flapi_create_endpoint` created an endpoint that could not be given a template

The create tool ignored `template-source` (it read an undeclared `template_source`), kept the
endpoint in memory only, and left `flapi_update_template` writing to the templates *directory* and
`flapi_reload_endpoint` finding nothing on disk. It now declares `path`, `method`, `template-source`
and `connection`, validates them, writes the endpoint YAML and a placeholder SQL file under the
templates directory, and the endpoint serves, reloads and survives a restart. A `template-source`
outside the templates directory is rejected.

### Fixed: `{{ env.NAME }}` (with spaces) in a YAML file stayed literal

In YAML configuration files only the exact `{{env.NAME}}` was substituted; `{{ env.NAME }}` and
`{{env.NAME }}` stayed as literal text with no error. They are now substituted and checked against
`template.environment-whitelist` like the exact form. The triple-brace SQL form `{{{ env.NAME }}}`
is not touched. `${NAME}` is still not supported.

### Fixed: every shipped example validates; include/comment handling is consistent

- `examples/flapi-test.yaml` now validates: it declares a stand-in `sap-abap-trial` connection for
  the two SAP endpoints that name it (the real one stays excluded for test isolation).
- An `{{include}}` on a `#` line inside a `|` block scalar is now treated as content, matching how
  `{{env.NAME}}` has always been handled there; full-line comments are still skipped.
- Environment-whitelist patterns are compiled once instead of per lookup, and an invalid pattern is
  rejected when the configuration loads.

### Fixed: documentation showed configuration that flAPI does not read

- TLS is configured under `enforce-https` (`enabled`, `ssl-cert-file`, `ssl-key-file`). The reference,
  the security guide and the shipped example documented a top-level `https:` block with underscore
  keys; flAPI ignores that and serves plain HTTP. There is no plain-HTTP redirect listener.
- About thirty examples used `${NAME}` for environment variables. Only `{{env.NAME}}` is substituted
  (and must match `template.environment-whitelist`); `${NAME}` stayed literal text, so a password
  written that way was the string `${NAME}`.

### Fixed: secrets from `{{env.NAME}}` no longer leak through the config dump, and the environment listing works with patterns

- When a configuration failed to load, the debug dump of the parsed config printed every
  credential-named value (`password`, `token`, ...) verbatim, including ones read from the
  environment. Those values now print as `<redacted>`.
- The YAML parser no longer keeps the values of substituted variables after a parse.
- `GET /api/v1/_config/environment-variables` treated whitelist entries as variable names, so a
  pattern such as `^DB_.*` listed nothing. It now lists the variables that actually exist and match
  the whitelist (plus plain-name entries, reported as unavailable when unset).

### Added: unknown top-level configuration keys are reported

A top-level key in `flapi.yaml` that flAPI does not read now logs a warning at startup (and under
`--validate-config`) instead of being silently ignored, with a hint for common near-misses such as
`rate-limit` (per-endpoint) vs `rate_limit` (global) and `https` vs `enforce-https`. It is a warning,
never an error, so a working configuration keeps starting. `version:` stays allowed as metadata.

### Fixed: `{{env.NAME}}` in an `{{include}}`d file was never substituted or checked

A password in a shared file such as `auth.yaml` stayed the literal text `{{env.DB_PASSWORD}}`, and
an unlisted variable there raised no error. Included files now follow the same substitution and
`template.environment-whitelist` rules as the file that includes them. Because an unset variable
substitutes as empty, an `auth.users` entry with an empty username or password now stops startup.
The shipped customers example uses demo credentials instead of unset `CUSTOMER_API_*` variables.

### Fixed: `--validate-config` rejected MCP prompt files; `flapi pack` now validates first

- `--validate-config` reported an MCP prompt file (`mcp-prompt:`) as invalid because it demanded a
  `template-source` and a connection that prompts do not use. Prompts now validate, and the shipped
  `examples/flapi.yaml` passes.
- `flapi pack` now validates `<in>/flapi.yaml` before writing anything. An invalid config is refused
  with the validation errors and no bundle is created, instead of producing a binary that fails at
  startup on the deploy target. Pass `--skip-validation` to pack anyway.

### Fixed: `environment-whitelist` was documented as required for YAML files, and never enforced

`{{env.NAME}}` in a YAML configuration file — the main config, endpoint files, included files — is
documented as requiring `NAME` to match a pattern in `template.environment-whitelist`. It did not.
The whitelist was only ever read for SQL templates; for YAML files the variable was substituted
whether or not it was listed, and whether or not there was a whitelist at all. The shipped S3, GCS
and Azure examples made it worse: they put `environment-whitelist:` at the **top level**, where nothing
reads it either, so anyone copying them believed they had restricted which variables a config could read.

The whitelist is now enforced for the main config and endpoint files, with the same rule SQL templates
already had:

- **A variable that matches no pattern stops flAPI at startup**, with an error naming every such
  variable and the key to add it to. It is never left in the file as literal text — a literal
  `{{env.DB_PASSWORD}}` would silently become the password.
- **An empty or missing whitelist allows no variables.** (For YAML files it used to mean "allow
  everything".)
- **A top-level `environment-whitelist:` is an error**, naming `template.environment-whitelist`.
- `env.NAME` conditions in `{{include ... if env.NAME}}` obey the whitelist too. They could read
  whether any variable was set, whitelisted or not.

**Before upgrading, check any config that uses `{{env.NAME}}` in YAML:**

```yaml
template:
  path: ./sqls
  environment-whitelist:        # under template:, not at the top level
    - '^DB_.*'                  # regular expressions, matched against the WHOLE name
    - '^API_KEY$'
```

`flapi --validate-config` reports a missing entry, so a CI check catches it before a deploy. Patterns
match the whole variable name (`^DB_` alone would match only a variable called `DB_`) and are
**case-sensitive**, since `AWS_REGION` and `aws_region` are different variables.

Content pulled in with `{{include}}` is not scanned — a `{{env.NAME}}` inside an included file is left as
literal text, exactly as before (see #165).

A full-line comment can mention `{{env.NAME}}` without being checked. A comment after a value
(`key: value # see {{env.X}}`) is still checked, so word those without the braces, and so is a `#` line
inside a block scalar (`key: |`), which is text rather than a comment. The whitelist itself must be in the
main config file, not in a file it includes.

Also corrected: the `${NAME}` shell form, which several docs and the Azure example used, was never
supported — it is left as literal text. The Azure example now uses `{{env.NAME}}`. And the step that
substitutes variables no longer writes each variable's *value* to the debug log, only its name. (A value can
still appear if you put a secret into a field flAPI itself logs, such as `project-name`.)

### Changed: a `server:` block is now an error, not silently ignored

flAPI has no `server:` block. Setting the port or host under one —

```yaml
server:
  port: 9000
  host: 127.0.0.1
```

— never did anything: the server stayed on port 8080, bound to `0.0.0.0`, with no message. The
block appeared in the configuration docs and in the BigQuery-procedure example, so it was easy to
copy. Because a wrong port sends traffic to the wrong place, flAPI now **refuses to start** with a
config that contains a `server:` block (in any form, including an empty one), and says what to use:

```
flAPI has no `server:` block; it was never read, so anything set under it had no effect.
Configure the HTTP server with the top-level `http-port` and `http-host` keys, or with
--port/-p and --host, or the FLAPI_PORT and FLAPI_HOST environment variables.
```

**If your config has a `server:` block, replace it before upgrading:**

```yaml
http-port: 9000
http-host: 127.0.0.1
```

`flapi --validate-config` reports it too, so a CI check catches it before a deploy. Removing the
block changes nothing about how flAPI behaves, since no value under `server:` has ever had any
effect — but a deployment whose config still carries one will **not start** until it is removed.

### Fixed: three `mcp.*` settings did nothing, and now say so

`mcp.port`, `mcp.host` and `mcp.allow-list-changed-notifications` were documented but never took
effect. MCP is served by the same server as the REST API — on the HTTP port and host, at
`/mcp/jsonrpc` — and flAPI has no server-to-client notification transport, so it always advertises
`listChanged: false`. Anyone who set `mcp.port: 8081` and pointed an MCP client at 8081 got
"connection refused".

flAPI now logs a warning naming each of these keys when it is set, and what to use instead. The
configuration still loads; nothing about how flAPI runs changes. They are gone from the
documentation and the example configurations.

### Changed: the Docker `:latest` tag is now the latest release

`ghcr.io/datazoode/flapi:latest` was pushed by every CI run, including pull requests, so
`docker run ghcr.io/datazoode/flapi` pulled whatever change had most recently been built — often
unmerged and unreviewed. `:latest` is now pushed only by a release, and only after that release's
tests pass. Each release is also tagged with its version (`:v26.09.26` and so on), and `:main`
tracks the main branch for anyone who wants the newest merged code.

## v26.09.26 — a failed start exits cleanly, and a clean stop releases the cache

### Fixed: a startup failure now exits non-zero instead of dumping core, or lying

Two startup failures were reported wrongly, and both matter to whatever supervises flAPI.

A database that cannot be opened — most often the DuckLake or DuckDB cache file still locked by
the previous process during a restart — used to **abort**: exit `134` (SIGABRT) with a core dump
of the ~77 MB binary. A crash-looping container filled its disk with core dumps of a recoverable,
operator-fixable condition. It now logs the reason and exits `1`:

```
[error] Database initialization error: Error creating database, Details: Failed to attach
        DuckLake catalog: ... Could not set lock on file ".../cache.ducklake":
        Conflicting lock is held in .../flapi (PID 3534350) by user jr.
[error] flAPI cannot start until the database can be opened.
```

Other shapes that land here: an unreadable `duckdb.db_path`, a cache file on a volume that has
not mounted yet, bad attach credentials.

A **bind failure now exits `1` as well**, where it used to exit `0`. `the server could not start:
bind: Address already in use` was logged and the process then reported success, so systemd would
not restart it and a Kubernetes container went `Completed` rather than `CrashLoopBackOff` — the
reason visible only to whoever read the logs.

Genuinely unexpected exceptions still abort, which is what the handler is for.

### Fixed: a clean shutdown now releases DuckLake and flushes the database

On `SIGTERM`, flAPI is meant to detach the DuckLake catalog and checkpoint the database before
exiting, so the next process can take the cache lock straight away and starts from a flushed file.
That work used to run in a place where it could not be relied on: it never appeared in a real
shutdown log, and on a failed startup the same code crashed. It now runs while flAPI is still
fully alive — after in-flight requests have drained — and says so:

```
[info] Received SIGTERM, shutting down...
[info] Detached DuckLake catalog: cache
[info] Checkpointed database
```

Long-running MCP tasks still executing at `SIGTERM` do not delay this: they are interrupted,
and the process exits promptly.

### Fixed: `heartbeat.params` now reaches the template

An endpoint's `heartbeat:` block has always accepted `params`, and the configuration reference
documented them, but they were never passed on — every heartbeat ran the template with no
parameters. They now arrive exactly like request parameters:

```yaml
heartbeat:
  enabled: true
  params:
    status: active       # the warm-up renders {{params.status}} as "active"
```

**If you set `heartbeat.params`, your heartbeats will start using them.** A heartbeat still runs
the endpoint's full request path — template, validators and query — whether or not the endpoint
is cached.

## v26.09.23 — a slow query no longer blocks everything else, and `auth.*` works over MCP

### Added: a slow query no longer blocks other requests

Crow runs a request handler on the io thread that owns its connection, so one slow query held
that thread for the whole query and every other connection assigned to it waited — including
`GET /health/live`, which is how a stalled instance is supposed to report itself.

Measured on one ~6.5s query with 40 concurrent probes and a single io thread:

| | probes blocked | stall reported | worst probe |
|---|---|---|---|
| before | 40 / 40 | 0 / 40 | 5.04s |
| after | 0 / 40 | 40 / 40 | 0.005s |

Handlers now run on a bounded worker pool and the response is completed back on the connection's
own thread. The queue is bounded on purpose: when it is full the request is answered `503` rather
than queued behind work the client stopped waiting for. Set `FLAPI_DISABLE_HANDLER_OFFLOAD=1` to
go back to the old behaviour.

`SIGTERM` is now handled properly alongside it: in-flight requests are drained before the process
exits, and queued-but-not-started work is abandoned after a bounded budget so a backlog cannot
hold a container open. A query that is *already executing* is still waited for — flAPI does not
cancel it — so a genuinely hung backend can still delay shutdown until your platform's grace
period expires. Bounding that needs query cancellation and is not in this release.

### Fixed: `auth.*` was empty in every MCP template

The documented multi-tenant pattern

```sql
{{#auth.username}}WHERE tenant = '{{ auth.username }}'{{/auth.username}}
```

worked over REST and rendered **nothing** over MCP, because the MCP path never injected the
authenticated caller's identity. In that conditional form the filter disappeared entirely and an
authenticated caller received **every tenant's rows**; written unconditionally it compared against
`''` and returned none. Both were silent.

`auth.username`, `auth.roles`, `auth.type`, `auth.email` and `auth.authenticated` now render the
same on REST, MCP `tools/call` and MCP `resources/read`. A caller still cannot declare its own
identity — `__auth_*` sent as an argument is discarded on every surface.

**If you have an MCP endpoint whose template filters on `auth.*`, its results will change.** That
is the point: it was returning too much or too little before.

### Fixed: cache retention destroyed other endpoints' history

Every cached endpoint shares one DuckLake catalog, but `keep-last-snapshots` and
`max-snapshot-age` are configured per endpoint — and the expiry ran catalog-wide. One endpoint
reaching its retention limit expired **every other endpoint's snapshots**, discarding their
time-travel history and resetting their incremental watermark. `flapii cache gc` did the same, and
against a hardcoded one-day cutoff that ignored your configuration entirely.

Retention now only expires snapshots belonging to its own cache table, and never one that another
table shares — so an endpoint may keep more history than you asked for, and will say so in the
log, rather than deleting a neighbour's data. It also always keeps the newest snapshot, which is
where the incremental watermark is read from, and applies `keep-last-snapshots` across the table's
whole history rather than across the already-expired subset when both keys are set.

Retention is disabled, with a warning, for a cache table whose name is ambiguous — two schemas
holding the same table name — because DuckLake's catalog exposes no schema name to tell them
apart and expiring the wrong one would delete another endpoint's data.

### Fixed: incremental refresh dropped rows written while it ran

`{{cache.previousSnapshotTimestamp}}` was the instant the previous refresh *committed*. That
refresh read your source earlier than that, so any row written in between was never read by it —
and was then excluded by the next refresh too. On a continuously written source, **every refresh
permanently dropped the rows written during its own execution**, and reported success.

With a `cursor:` configured, the watermark is now `max(<cursor column>)` over the rows actually
cached. Rows above it are exactly the rows not yet loaded.

Two consequences worth knowing:

- The value is the cursor's own type, not a timestamp. `WHERE updated_at > TIMESTAMP '{{...}}'`
  stays right for a timestamp cursor; an integer or string cursor needs quoting to match.
- `{{cache.snapshotTimestamp}}` is a *different* value (still the commit time) and must not be
  used as a watermark.

### Fixed: credentials came back from MCP dry runs and errors

A `_dryRun` tool call returns the rendered SQL, and MCP is unauthenticated by default. Connection
passwords, whitelisted environment variables (`{{{ env.API_KEY }}}`) and credential-valued request
defaults were all returned verbatim — and so were the ones a database error quoted back, which is
a path no `_dryRun` flag is needed to reach.

All three sources are redacted now, in previews and in error messages, on **every** surface that
returns one: REST responses, MCP `tools/call` and MCP `resources/read` all scrub the same set.
(REST was leaking the same way, through the same mechanism, for any endpoint without an `auth:`
block.) Where a value is too short to replace without corrupting the surrounding SQL, the preview
is withheld and says so rather than being silently mangled.

### Fixed: MCP config tools that reported success without doing anything

Eleven `flapi_*` config tools returned confident results for work they never performed:

| Tool | What it returned |
|---|---|
| `flapi_expand_template` | a hardcoded `SELECT * FROM data WHERE 1=1` |
| `flapi_test_template` | "Template test passed", for a query it never ran |
| `flapi_update_template` | success, with the file on disk untouched |
| `flapi_refresh_cache` | "Cache refresh has been scheduled" — nothing was scheduled |
| `flapi_run_cache_gc` | "Garbage collection triggered" |
| `flapi_refresh_schema` | "schema_refreshed" |
| `flapi_get_cache_audit` | **invented audit records** |
| `flapi_get_cache_status` | three fields of static YAML, while promising snapshot history |
| `flapi_get_environment` | an empty list, always |
| `flapi_get_filesystem` | an empty tree |
| `flapi_get_schema` | `"tables": null` |

Every one of them had a working REST handler that the adapter was constructing and then
discarding. They now call it, so the MCP and REST surfaces cannot disagree — an agent asking
whether a cache refreshed gets the real answer, and one asking to expand a template gets that
endpoint's own SQL.

`flapi_get_project_config` also reported version `1.0.0` for every build; it now returns the real
project configuration.

**Every `flapi_*` tool now requires the config-service token**, matching the REST route it
delegates to. Twelve of them were classified "read-only discovery" and left unauthenticated, which
was harmless only while their bodies returned hardcoded data — once they were wired to the real
handlers, three of them became unauthenticated disclosures: `flapi_get_environment` returns
environment *values*, `flapi_expand_template` returns rendered SQL, and `flapi_test_template`
*executes* any endpoint's template. If you call these tools, send
`Authorization: Bearer <config-service-token>`; with no config service configured they now decline,
which is correct.

`flapi_test_template` also runs the endpoint's own validators before executing, so it is no longer
a way around the constraints the endpoint itself enforces. `flapi_run_cache_gc` requires `path` —
its "all endpoints" form never worked — and every tool now declares its arguments in `tools/list`,
so an agent can discover them instead of guessing and getting an error.

### Fixed: smaller disclosures on the same theme

- **`GET /api/v1/_config/health` is the one config-service route without a token**, and it
  published every storage backend's path and its raw error text. A path can carry an inline
  credential or a signature. Unauthenticated callers now get status, scheme and latency; the
  detail needs the token.
- A **connection property literally named `pw`, `key`, `pass` or `sas`** is now recognised as a
  credential. Recognition is whole-key, so `passenger_count`, `sort_key` and `keyword` are still
  ordinary columns and are not redacted out of your SQL.
- flAPI **warns at startup** (`CREDENTIAL_TOO_SHORT_TO_REDACT`) when a configured credential is
  under four characters. Such a value cannot be scrubbed without corrupting the SQL around it, so
  error detail and dry-run previews are withheld for every endpoint using that connection — with
  nothing in the response to say why. Now you are told once, at startup, and can lengthen it.
- The incremental **cursor watermark is validated against its declared type** before the next
  refresh interpolates it into SQL. A cached value that does not look like its `cursor.type` makes
  the refresh fall back to a full load and log why, rather than being pasted into a query.
- Cache retention **fails closed**: if the catalog cannot be read, nothing is expired. It
  previously proceeded on a partial answer.

### Fixed: a failed start no longer hangs

If flAPI could not bind its port — most often because something else already holds it — the
process hung instead of exiting, and ignored `SIGTERM` from that state, so a supervisor had to
`SIGKILL` it. It now exits promptly, non-zero, saying `the server could not start: bind: Address
already in use`.

### Changed: one config-service name per endpoint

The config service addresses an endpoint by a slug, and the encoding is now injective — `/a-b` and
`/a/b` no longer collide. `/customers/` is `-customers-`; a literal `-` is `~1` and a literal `~`
is `~0`.

**The CLI and the VSCode extension must be updated together with the server.** An older `flapii`
sends the previous encoding and every config command will `404`. A percent-encoded url-path is no
longer accepted as a second name for the same endpoint.

## v26.09.20 — SQLite attachments no longer wedge, and readiness notices a stalled request

### Fixed: a concurrent read and write wedged a SQLite attachment

A `GET` issued at the same moment as a `POST` against a DuckDB SQLite attachment deadlocked the
two against each other. **Two requests were enough**, and it reproduced every time. The
attachment could then stop answering for good while the process, DuckDB, `/health/live` and every
other connection stayed perfectly healthy — so the instance kept accepting traffic it could no
longer serve, with nothing in the logs to say why.

Serialising only the writes does not help: the pair that wedges is a reader and a writer. flAPI
now runs **one query at a time** on any connection whose `init` contains an
`ATTACH ... (TYPE sqlite)`. Nothing else is affected — parquet, BigQuery, Postgres and DuckLake
traffic still runs fully in parallel, because the lock is per connection and only taken for
connections that need it.

Measured: 19 of 20 concurrent read+write pairs failed before, 20 of 20 pass now.

The cost is real — concurrent requests to a SQLite-backed endpoint queue rather than overlap — so
you can override the detection in either direction:

```yaml
connections:
  my-backend:
    serialize-access: true      # an unrecognised backend with the same problem

  read-only-snapshot:
    serialize-access: false     # a SQLite attachment you have measured and want parallel
```

### Added: readiness fails while a request is stalled

```yaml
stall-timeout-s: 60   # 0 disables
```

`GET /health` returns `503 {"status":"stalled"}` once the oldest in-flight request outlives the
budget, with `requests.in_flight` and `requests.oldest_ms` in the body. This measures the symptom
rather than probing each backend — a probe against a wedged connection blocks too, leaking a
thread per health check — so it catches any stall, whatever caused it.

The default is deliberately high: flAPI's answer to a genuinely long query is the MCP Tasks
extension, so a *synchronous* request still running after a minute is pathological rather than
slow. **Liveness deliberately still passes** — the process is fine, and killing it rather than
draining it turns a stall into an outage.

### Known limitation

A handler runs on the Crow io thread that owns its connection, so a slow synchronous query blocks
other connections landing on that thread — `GET /health` included. Readiness can therefore be
blocked by the very stall it reports: with one ~5 s query in flight, 39 of 40 health requests
answered `503 stalled` in under a millisecond and one waited out the query. A hung probe is still
treated as a failed probe by Kubernetes and Cloud Run, so the instance does leave rotation — but
by timeout rather than by the documented response. Tracked in
[#120](https://github.com/DataZooDE/flapi/issues/120).

## v26.09.19 — Blocking span export for scale-to-zero, and a documentation correction

### Fixed: `tracing.endpoint` must be the full trace URL

The examples published with v26.09.18 used `endpoint: http://localhost:4318`. **That does not
work.** opentelemetry-cpp uses an overridden URL verbatim and does not append `/v1/traces`, so
flAPI posted to `/` and any real collector rejected it — silently, because
`OtlpHttpExporter::Export` discards its own result and reports success regardless. Anyone who
copied the documented configuration got no traces and no signal that anything was wrong.

Use `endpoint: http://localhost:4318/v1/traces`.

### Added: opt-in blocking span export

For request-billed, scale-to-zero platforms (Cloud Run, App Runner), where CPU is throttled the
moment the response is sent and the background export thread may never be scheduled:

```yaml
tracing:
  flush:
    mode: on_response
    blocking_timeout_ms: 300     # required for otlp_http; 1-1000
```

Off unless you set the budget — there is no safe default for how much latency you will trade for
telemetry. Adds ~3 ms against a healthy collector, is bounded exactly at the budget against a
hanging one, and does not grow with concurrency. Against a hanging collector throughput degrades
to roughly `workers / blocking_timeout_ms`.

### Added

- **`spans_submitted`** in `GET /api/v1/_config/metrics`. `spans_dropped` cannot detect a
  rejecting collector — the SDK drops silently on a full queue and reports export failures as
  success — so `submitted - exported` is the honest view of queue loss. For `otlp_http`, the flAPI
  log (`[OTLP TRACE HTTP Exporter] ERROR`) remains the only reliable failure signal.

### Fixed

- **MCP spans reported `http.route: <unmatched>`**, putting every agent call in the same bucket as
  a vulnerability scanner's 404s. Now `/mcp/jsonrpc`.
- **The `SIGTERM` force-flush budget was hardcoded at 2 s**, so tuning `flush.timeout_ms` had no
  effect on the path that matters most under a short shutdown grace. Now configurable and
  range-checked.
- **SQLite lock contention** (partial fix for #116): lock errors are retried with bounded backoff
  on both read and write paths, and exhausted retries return `503` with `Retry-After` rather than
  `500` — contention is not a fault in the request. The rollback path no longer runs `ROLLBACK`
  when no transaction is active, which had compounded every real error with a spurious one.

### Known limitation

Concurrent mixed read/write traffic against a **SQLite-backed** endpoint can still wedge that
attachment permanently, while `/health/live` continues to return 200 and other connections keep
serving. A restart clears it and no data is lost. Root-caused and tracked in
[#116](https://github.com/DataZooDE/flapi/issues/116).

## v26.09.18 — OpenTelemetry observability, and health checks during cache warmup

### Observability and tracing

flAPI now emits OpenTelemetry traces for every request, correlated with the audit log and the
application log through one shared request identity. **Off by default**, and when enabled it
exports no customer data unless an endpoint is explicitly opted in. See
[docs/OBSERVABILITY.md](docs/OBSERVABILITY.md).

- **MCP trace context (SEP-414).** flAPI now honours the unprefixed `traceparent`, `tracestate` and
  `baggage` keys inside `params._meta`, so a tool call joins the agent's existing trace instead of
  starting a disconnected one. `_meta` wins over the HTTP header: behind a gateway the HTTP hop
  carries the gateway's span, not the agent's. Parsed even when tracing is disabled, so the ids
  still reach the audit and application logs. This closes a conformance gap — flAPI advertised MCP
  `2026-07-28` while discarding the trace context conforming clients already sent.
- **One SERVER span per request on every route**, created in the first middleware. A
  handler-level span would miss every 401, 403, 429, CORS preflight and 404 — the requests most
  likely to be complained about.
- **Inner spans** for template rendering, DuckDB execution (both the plain and prepared paths) and
  flAPI's own outbound OIDC/JWKS calls.
- **`X-Request-Id` on every response**, always server-minted; an inbound one is never honoured.
  The same id appears in the audit log and in every application log line for that request, with
  `trace_id`/`span_id` alongside when tracing is on. `X-Trace-Id` is returned when a span exists.
- **REST audit coverage.** The audit log previously covered MCP only, while the documentation
  claimed otherwise. It now covers every request on every route, including ones rejected before the
  handler; denials are never suppressed.
- **`server.log_level` is now read** from YAML, with precedence CLI > env > YAML > default. It had
  appeared in three example configs without being honoured.
- **`GET /api/v1/_config/metrics`** implemented, bearer-gated. It had been in the OpenAPI document
  with no route behind it.
- **Capture tiers** (`off` / `metadata` / `payload`, default `metadata`), with a global `off`
  beating any per-endpoint opt-in. Filled paths, query strings, header values and credentials are
  never exported at any tier, and error status is an enumerated `error.type` rather than an
  exception message. Redaction is shared between the audit log and the span path so the two cannot
  drift; built-in credential stems match as substrings, the operator's `audit.redact` list matches
  whole keys.
- **DuckDB execution profiling** (`tracing.db_profiling: off | summary | detailed`) attaches the
  metrics behind `EXPLAIN ANALYZE` to the database span, read through DuckDB's C API. A fixed
  allowlist: `QUERY_NAME` is the SQL text and `EXTRA_INFO` is the rendered filter predicate, so
  neither is ever requested. Gated on the span actually recording, so a 1% sampling ratio pays 1%
  of the cost.
- **`FLAPI_WITH_TRACING=OFF`** builds no-op twins and links no OpenTelemetry symbol. Request ids,
  log correlation, REST audit coverage and the metrics route all still work — they are not tracing
  features. A mixed-macro build fails to link rather than corrupting silently.

### Correctness fixes made along the way

- **Copy-on-write endpoint table.** `getEndpointForPathAndMethod` returned a raw pointer into a
  `std::vector` that config reload and the config-service routes cleared and re-`push_back`-ed with
  no lock against in-flight requests — a use-after-free. Reads now pin an immutable snapshot
  through `EndpointRef`.
- **Route regexes are compiled once** at config load instead of on every call.
- **One `FlapiApp` alias** for the Crow middleware tuple, enforced by CI. A second literal
  `crow::App<...>` spelling silently creates a second, unconfigured middleware tuple.

### Build

- `opentelemetry-cpp` 1.24.0 via an in-repo vcpkg overlay port. The pinned baseline's 1.17.0 does
  not compile — 136 of its headers use fixed-width integer types without including `<cstdint>`.
- **The project now builds at C++20**, because abseil (via opentelemetry-cpp) exports a
  `cxx_std_20` requirement. The bundled DuckDB is deliberately built at C++17 inside the same
  binary: C++20 removed `std::uncaught_exception()`, which DuckDB still calls behind a
  `__cplusplus` guard that MSVC defeats. CMake saves and restores `CMAKE_CXX_STANDARD` around it.
- `opentelemetry-cpp` links `PRIVATE`, and no header under `src/include/` includes an
  OpenTelemetry header, so protobuf and abseil stay out of ~80 translation units.

### Documentation

- Restructured around tasks: a new index at [docs/README.md](docs/README.md), eight guides under
  `docs/guides/`, and the `docs/spec/` tree brought back in line with the code.
- Roughly 15,000 lines of unlinked material that contradicted the live reference were removed;
  completed plans and shipped design notes moved to `docs/archive/`.

### Health checks during cache warmup

- The HTTP listener now opens before cache warmup completes, so platforms can connect during long
  startup cache builds.
- Added always-on `GET /health/live` for liveness and `GET /health` for readiness. `/health`
  returns `503` while caches are still starting or degraded.
- Cache-enabled data endpoints now return `503` with `Retry-After: 5` while their cache is starting
  or failed, instead of risking a `200` response from a half-built cache.
- Cache warmup failures are captured per endpoint and surfaced in health output; warmup continues
  to later caches.
- Concurrent refreshes for the same DuckLake cache table are suppressed so scheduler refreshes do
  not collide with startup warmup.

### Build correctness

- `CROW_ENABLE_COMPRESSION` is now defined once for every translation unit via CMake instead of by
  three headers. The macro adds a member to `crow::response`, so a translation unit that reached
  `<crow.h>` through a different include order saw a different layout — an ODR violation that made
  `response::is_completed()` read an unrelated byte across the library/test boundary.
- The unit tests that use the `#define private public` access hack now include `<crow.h>` before it,
  so crow is never parsed with rewritten access specifiers.

## v26.08.31 — MCP 2026-07-28 dual-era support and the Tasks extension

- **MCP revision `2026-07-28`, served dual-era.** The modern stateless path and the legacy
  `initialize`/session path are served from the same endpoint, chosen per request by whether the
  client sent the modern `_meta` block. Existing clients keep working unchanged; newer ones get the
  stateless path automatically. flAPI had never really trusted the session — every `tools/call`
  already re-authenticated from the HTTP request — so dropping it lost nothing and made the server
  genuinely stateless behind a load balancer.
- **Tasks extension** for long-running analytical queries: a query that would outlive a proxy or
  client timeout becomes a durable, pollable task rather than a dropped connection.
- Typed tool schemas and structured results, OAuth discovery, per-tool RBAC, shadow/dry-run,
  response shaping, per-tool rate limiting, and a prompt-injection hygiene scanner.
- Cursor pagination, parameterised resource templates (`flapi://customers/{id}`), and
  `x-mcp-header` for per-tenant edge routing.
- Closed a gap where method authorization could be skipped by omitting the session header.
- **DuckDB bumped to v1.5.5.**

## v26.08.07 — Feedback banner and issue links on errors

- An interactive start prints a small banner once a day pointing at the issue tracker; under a
  container or systemd there is no terminal, so it never prints and the startup log line carries
  the pointer instead. Silence it with `DATAZOO_NO_BANNER=1`.
- Every JSON error response carries a `report_issue` link, so a user hitting a problem has
  somewhere to send it without hunting for the repository.

## v26.07.17 — Self-packaging, 12-factor configuration, and REST type coverage

### Self-packaging (single-binary deploy)

- `flapi pack` folds a whole config tree — YAML, SQL templates, small data files — into the binary
  itself, so `scp flapi-prod user@host` becomes the deploy. `info` inspects a bundle and `unpack`
  extracts it.
- A ZIP is appended after the executable, or on macOS written into a reserved `__FLAPI/__bundle`
  Mach-O segment allocated at link time, so the output stays **notarisable**. Fat/universal macOS
  binaries supported.
- `EmbeddedArchiveFileProvider` serves config and templates from the bundle, and an
  `embed://` DuckDB filesystem lets `read_csv('embed://data/cities.csv')` reach the same bytes from
  SQL.
- Reproducible: set `SOURCE_DATE_EPOCH` and the output is bit-identical across runs.
- Secrets are refused at pack time by default (`*.env`, `secrets/*`, `*.pem`, `*.key`).

### Configuration

- 12-factor environment variables: `FLAPI_CONFIG`, `FLAPI_LOG_LEVEL`, `FLAPI_PORT`, `FLAPI_HOST`.
- The baked-in version is derived from the git tag rather than hardcoded.

### REST serialization correctness

A sweep of DuckDB types that were previously serialized wrongly or not at all (#89):

- Native `LIST`/`STRUCT`/`ARRAY`/`UNION` columns serialized per row.
- `MAP` columns as `{key: value}` rather than a positional pair array.
- `UUID`, `HUGEINT`, `BLOB` and `BIT` corrected.
- `VARINT`/`BIGNUM`, `GEOMETRY` and `VARIANT` now serialized rather than dropped.

### Other

- Client parity restored between the `flapii` CLI, the VS Code extension and the server (#75–#77).
- PostHog telemetry moved to the shared schema v2.
- Wheel license metadata corrected to BUSL-1.1, and the MCP registry marker added.
- MCP logging capability emitted as an empty object rather than `null`.

## v26.05.18 — Prepared-statement coverage swept across every code path

Follow-up to v26.05.17. After v26.05.17 shipped, an internal audit found that the prepared-statement path was only wired into the GET endpoint executor — POST/PUT/PATCH writes and the Arrow-streaming endpoint still rendered Mustache templates as strings. This release closes that gap.

### Coverage extension

- **POST/PUT/PATCH writes** now take the prepared path. `executeWrite` calls the rewriter first, splits the rewritten SQL into statements (quote-aware), distributes the binding plan across statements by counting `?` placeholders per statement, and prepares + binds + executes each one. Multi-statement INSERT…;SELECT…RETURNING templates keep working — each statement is its own prepared statement with the right slice of the binding plan.
- **Arrow streaming (`executeQueryRaw`)** now routes through the prepared path with the same fall-back-to-string behaviour when the binding plan is empty.
- **`countSqlPlaceholders` helper** in `src/sql_utils.cpp` — quote-aware `?` counter (skips placeholders inside `'…'`, `"…"`, `$tag$…$tag$`) used by the write-path distributor. Covered by 6 new Catch2 cases.

### Coverage extension — tests

- **Read-path corpus extended from 37 → 99 parameterised payloads** (`test/integration/test_sql_injection_corpus.py`). Adds endpoints for `double`, `boolean`, `date`, `time`, `uuid`, `enum`, `email` so every validator type is exercised end-to-end. Plus a `/lookup-int-paged` endpoint that proves pagination + prepared bindings work together (count + paginated wraps both bind correctly).
- **New write-path corpus** (`test/integration/test_sql_injection_write_corpus.py`, 19 cases). Fires the classic injection payloads at a `POST /widgets/` endpoint and asserts the payload lands as a literal string column value, never as a side-effect that drops the table or smuggles extra rows. Includes a multi-statement INSERT-then-SELECT-RETURNING endpoint to exercise binding-plan slicing.

### Validator hardening (defense in depth)

- `validateDate` and `validateTime` now demand the entire input string be consumed — `2024-03-15' OR 1=1` no longer parses to `2024-03-15` and silently drops the suffix. Same fix as `validateInt` in v26.05.17.

### HTTP status correctness

- New `flapi::BadRequestError` exception class. `QueryExecutor::executeWithBindings` throws it on bind-conversion failure (caller supplied an invalid value for a typed param); `RequestHandler` catches it and returns **HTTP 400** with a JSON body, instead of the previous `500 Internal Server Error`. Server-side prepare/execute failures still return 500 (they're not client errors).

### Tests

- **586 C++ unit assertions** (Catch2; +6 for `countSqlPlaceholders`).
- **483 integration tests passing** (+81 from the corpus extensions). 21 skip in environments without the relevant fixtures.

---

## v26.05.17 — Security roadmap (Waves 0–3) + BSL relicense

**Headline:** in-product MCP + general security hardening — per-tool RBAC, shadow/dry-run, response shaping, description hygiene, prepared-statement SQL-injection defense, PBKDF2 password hashing, audit log, per-user rate limit, CORS allowlist, TLS wire-up, startup auditor. Simple things stay simple — every new control is opt-in via single-line YAML.

### License

- **Apache 2.0 → BSL v1.1** ([e1b465e](https://github.com/DataZooDE/flapi/commit/e1b465e)). The Business Source License is source-available; non-production use is permitted without a commercial agreement. The Change License (MPL 2.0) takes effect five years after first publication of each version. See [`LICENSE`](./LICENSE) for the full text.

### MCP hardening — Wave 2 (#24)

- **Per-tool RBAC** ([8886cd2](https://github.com/DataZooDE/flapi/commit/8886cd2), #27). `mcp-tool.allowed-roles: [admin, analyst]` in the endpoint YAML restricts a tool to JWT/OIDC principals carrying one of those roles. **Deny-by-default**: when `mcp.auth.enabled: true`, every tool MUST declare `allowed-roles` — a tool without one refuses every call. Endpoints with `mcp.auth.enabled` unset keep working role-free for `flapii project init` demos.
- **Dry-run / shadow mode** ([385f793](https://github.com/DataZooDE/flapi/commit/385f793), #29). Pass `"_dryRun": true` in `tools/call` arguments. flAPI runs validators + template expansion + EXPLAIN and returns the rendered SQL + plan as JSON, but never executes the query. The same controls that gate a real call (RBAC, rate limit) gate a dry-run too.
- **Tool-description hygiene scanner** ([63a1af7](https://github.com/DataZooDE/flapi/commit/63a1af7), #28). At config-load time, descriptions are scanned for control characters, JSON-breakout patterns, and known role-override phrases ("ignore previous instructions"). Strict-mode opt-in via `mcp.strict-descriptions: true` — refuses to start when any tool fails the scan.
- **Per-tool response shaping** ([9c9cd55](https://github.com/DataZooDE/flapi/commit/9c9cd55), #30). New `mcp-tool.response` block: `max-rows` caps the result-set size, `redact-columns: [...]` replaces listed columns with a redaction sentinel, `sample: true` returns only summary metadata (`row_count`, `columns`, `sampled: true`).
- **Per-tool rate limit** ([0a7d69c](https://github.com/DataZooDE/flapi/commit/0a7d69c), #34). New `mcp-tool.rate-limit: { enabled, max, interval }` keyed on the authenticated principal (with an anonymous fallback bucket per tool).

### General security wins — Wave 1 (#23)

- **PBKDF2-SHA256 password hashing** ([db87b8e](https://github.com/DataZooDE/flapi/commit/db87b8e), #36). `auth.users[*].password` accepts the MCF string `$pbkdf2-sha256$<iter>$<b64-salt>$<b64-hash>` (OpenSSL `PKCS5_PBKDF2_HMAC` with 600 k iterations, 16-byte salt, 32-byte key — OWASP 2023 minimum). Compatible with Python `passlib` and any other PBKDF2-SHA256 generator. Plaintext and 32-char-hex MD5 hashes still verify, but the startup auditor emits a deprecation warning.
- **Config-driven CORS allowlist** ([f1a6751](https://github.com/DataZooDE/flapi/commit/f1a6751), #32). The legacy wildcard `Access-Control-Allow-Origin: *` is gone — default is same-origin only. Opt into specific origins via `cors.allow-origins: [...]`. `flapii project init` still ships `["*"]` so first-run demos work; the auditor warns when `*` meets `auth.enabled: true`.
- **JSONL request audit log** ([1c762d4](https://github.com/DataZooDE/flapi/commit/1c762d4), #31). `audit: { enabled, sink: stdout|file, path, redact: [...] }` emits one JSON line per request (REST and MCP) with principal, method/target, params (redacted per config), status, row count, latency. Off by default, one-line to enable.
- **Per-user rate limit** ([b44c92d](https://github.com/DataZooDE/flapi/commit/b44c92d), #33). New `rate-limit.key: ip | user | user-or-ip`. The default stays `ip` for backward compatibility; `user-or-ip` is the recommended setting for share-NAT scenarios where many users share a single egress IP.
- **TLS in embedded server** ([e38c715](https://github.com/DataZooDE/flapi/commit/e38c715), #35). The `HTTPSConfig` struct is now wired into Crow's `ssl_file()` chain. Reverse-proxy termination is still recommended for production, but direct TLS is supported for self-contained deployments.

### SQL-injection defense — Wave 3 (#25)

- **Prepared-statement path for typed scalar params** ([8bf073d](https://github.com/DataZooDE/flapi/commit/8bf073d) + [ca16217](https://github.com/DataZooDE/flapi/commit/ca16217), #37). `{{ params.X }}` (double-brace) references on fields with typed validators (`int`, `double`, `boolean`, `date`, `time`, `uuid`, `enum`, `email`, `string`) are now rewritten to `?` and bound via `duckdb_bind_*`. The value travels as a primitive, not text — SQL injection becomes structurally impossible for those sites. Triple-brace `{{{ params.X }}}` is unchanged (for `LIKE` patterns and other text-mode use sites). The integer validator was also tightened: `1; DROP TABLE` no longer slips through as `1`.
- **W3.3: SQL-keyword regex demotion** ([ca16217](https://github.com/DataZooDE/flapi/commit/ca16217)). For numeric/temporal bindable fields, the historic keyword regex is demoted to a debug-level log line — the prepared bind is the hard defense, and the regex's false positives (`latitude=1.111`) are gone. Varchar-classified fields keep the regex because flAPI templates routinely embed them via triple-brace.
- **37-payload integration corpus** at `test/integration/test_sql_injection_corpus.py` — every classic injection pattern (UNION, OR 1=1, comment-evasion, xkcd 327) returns zero rows; legitimate values still match.

### Honest defaults & honest docs — Wave 0 (#22)

- **Startup security auditor** ([655d61f](https://github.com/DataZooDE/flapi/commit/655d61f), #26). At boot, flAPI scans the loaded config and emits structured warnings for: plaintext passwords, MD5 passwords, MCP exposed without auth on a non-loopback bind, and CORS wildcard combined with `auth.enabled`.
- **Documentation correctness**. The misleading claim that `{{{ }}}` "prevents SQL injection" is gone from `docs/CONFIG_REFERENCE.md`. The actual layered defense (validators → prepared bind → regex fallback for triple-brace and untyped fields) is documented in [§ 9 SQL Templates](docs/CONFIG_REFERENCE.md).

### Fixes

- **Windows release link** ([4619687](https://github.com/DataZooDE/flapi/commit/4619687)). `mcp_authorization_policy.hpp` forward-declared `EndpointConfig` as `class` while the actual type is `struct`; MSVC encodes that keyword into mangled symbols, so the call site and definition emitted different names. Fixed by aligning the forward decl.
- **Auth-context param leak** ([4619687](https://github.com/DataZooDE/flapi/commit/4619687)). `RequestValidator::validateRequestFields` rejected every authenticated write request as containing five phantom unknown fields (`__auth_username` / `_email` / `_roles` / `_type` / `_authenticated`). The reserved `__auth_*` prefix is now silently skipped.
- **Release linker fix** ([1116f25](https://github.com/DataZooDE/flapi/commit/1116f25)). Explicit `safeGet<int>` template instantiation in `config_manager.cpp` for cross-TU release linking (debug inlined; release with `-Wl,--no-undefined` exposed the missing definition).
- **Cross-platform smoke tests in CI** ([2f25366](https://github.com/DataZooDE/flapi/commit/2f25366), [822ea3e](https://github.com/DataZooDE/flapi/commit/822ea3e), [b3c9744](https://github.com/DataZooDE/flapi/commit/b3c9744)). Each platform binary (linux-amd64, linux-arm64, macos-arm64, windows-amd64) is now booted in CI before release; the four smoke jobs gate `create-release`.
- **Auth template variable names** ([8b2b8d8](https://github.com/DataZooDE/flapi/commit/8b2b8d8)). Doc fix: it's `auth.username`, not `context.auth.username`.

### Tests

- **580 C++ unit assertions** (Catch2).
- **402 integration tests passing** (37 of them parameterised SQL-injection payloads). 21 skip in environments without the relevant fixtures (AWS Secrets Manager, OIDC issuers).

---

For earlier history see `git log` or the GitHub release notes for prior tags (`v26.04.22` and below).
