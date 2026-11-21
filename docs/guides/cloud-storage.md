# Reading from cloud storage

**Goal:** query data in S3, GCS or Azure, and optionally load flAPI's own
configuration from there too.

There are two separate things you might want, and they are configured
differently.

---

## 1. Query data in a bucket

Point a connection at a cloud URL. DuckDB's `httpfs` extension loads
automatically.

```yaml
# flapi.yaml
connections:
  events:
    properties:
      path: 's3://my-bucket/events/*.parquet'
```

```sql
SELECT * FROM read_parquet('{{{ conn.path }}}')
WHERE event_date >= '{{{ params.since }}}'
```

S3 and Azure credentials come from the environment (`AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY`, `AWS_REGION`; `AZURE_STORAGE_CONNECTION_STRING` or
`AZURE_STORAGE_ACCOUNT` + `AZURE_STORAGE_KEY`), never from the config file: flAPI
sets the matching DuckDB settings at startup.

**GCS is different.** flAPI does not configure GCS from `GOOGLE_APPLICATION_CREDENTIALS`
or `GOOGLE_CLOUD_PROJECT` (it only logs that they are set), and DuckDB's `httpfs`
does not use a service-account file. It needs an HMAC key (Cloud Storage →
Settings → Interoperability) in a DuckDB secret, for example in the connection's
`init`, with the values taken from whitelisted environment variables:

```yaml
connections:
  events:
    init: |
      INSTALL httpfs;
      LOAD httpfs;
      CREATE OR REPLACE SECRET gcs_hmac (
        TYPE gcs,
        KEY_ID '{{env.GCS_HMAC_KEY_ID}}',
        SECRET '{{env.GCS_HMAC_SECRET}}'
      );
    properties:
      path: 'gs://my-bucket/events/*.parquet'
```

Glob patterns work, and DuckDB pushes filters down, so a partitioned layout is
read selectively rather than wholesale.

## 2. Load the configuration itself from a bucket

Useful when there is no filesystem to mount — Lambda, Cloud Run, Azure Functions
— or when a bucket is your GitOps target.

```bash
flapi --config s3://my-bucket/configs/flapi.yaml
flapi --config gs://my-bucket/configs/flapi.yaml
flapi --config https://raw.githubusercontent.com/myorg/configs/main/flapi.yaml
```

SQL templates are resolved relative to the config, so the whole tree can live in
the bucket. A `template.path` of `s3://`, `gs://`, `az://` or `https://` works the same
way; plain `https://` has no directory listing, so add a `manifest.txt` (one endpoint YAML
per line) at the root of `template.path`. See
[CONFIG_REFERENCE](../CONFIG_REFERENCE.md#remote-template-paths).

> **Air-gapped hosts:** a remote `--config` is read before the config exists, so `duckdb.extension_directory`
> in that file does not apply to this first read. Set `FLAPI_EXTENSION_DIRECTORY` (httpfs pre-installed there)
> and `FLAPI_EXTENSIONS_OFFLINE=1` in the environment instead.

> The alternative for immutable deployments is to bake the config tree into the
> binary instead: see [self-packaging](./self-packaging.md). Choose cloud config
> when you want to change configuration without redeploying, and self-packaging
> when you want the artifact to be complete and fixed.

## Changing things in a bucket

With config and templates in a bucket, "deploying" is uploading files. What takes effect when:

| You change | Takes effect |
|---|---|
| a `.sql` template | within one `storage.cache.ttl` (300 s by default); set `storage.cache.enabled: false` for the next request, at the cost of a round trip to the store per render |
| an endpoint `.yaml` (validators, cache, auth, ...) | after a **restart** |
| a new endpoint (`.yaml` + `.sql`) | after a restart; on plain http(s) also add the `.yaml` to `manifest.txt`, which is the only source of endpoints there |
| `flapi.yaml` itself | after a restart |

A template you upload is live as soon as it is read: check it first with
`flapi -c <config> --validate-config`, ideally as a CI gate before the upload. The config service and the
`flapi_*` MCP tools cannot create, edit, delete or reload endpoint files in a bucket (they answer 501 and say
so); they can read them.

## Running it in production

- **Startup time.** flAPI reads every endpoint file before it listens, one request at a time: about 1 listing
  request plus 4 per endpoint (`HEAD` + `GET` for the YAML and for the SQL). The HTTP port stays closed until the
  load finishes, so `/health/live` refuses connections meanwhile. Size a Kubernetes `startupProbe` as roughly
  `4 x endpoints x store round-trip` (200 endpoints at 50 ms is about 40 s) and remember that N replicas starting
  together send N times that many requests to the store.
- **Store outages.** A template read that fails after its cache TTL serves the last good copy for up to 15 s per
  retry (and logs a warning) instead of failing the request; with no cached copy the request is a 500 whose log
  line carries the store's status and the redacted URL. `/health` does not probe the store.
- **Credentials** are read from the environment at startup only: short-lived STS tokens are not refreshed, so
  restart (or use a role the SDK chain refreshes) before they expire.
- **Offline hosts.** Set `FLAPI_EXTENSIONS_OFFLINE=1` and pre-populate `FLAPI_EXTENSION_DIRECTORY` with the
  extensions flAPI loads (httpfs for remote reads, plus the others in `duckdb.default-extensions`; trim that list
  for offline images). `azure` is loaded only when an `az://` path is read.
- **CI gate.** `flapi -c <config> --validate-config` exits 0/1 (human text on stdout, details on stderr) and
  needs the bucket credentials, which makes it a real pre-deploy check of what is in the bucket.
- **`flapi pack`** does not bundle remote templates: it warns, and the packed binary still reads them from the
  bucket at runtime.

## Who can write the bucket

Whoever can write the template bucket can run arbitrary SQL as the flAPI process: templates and endpoint YAML are
code. flAPI does not verify an integrity checksum or signature of remote files. Give the bucket (or the prefix)
**read-only** credentials to flAPI and **write** access to as few people as the code itself would have, enable
object versioning, and scope the credentials to the one prefix. S3 credentials from the environment are stored as
a redacted DuckDB secret, so a template cannot read them back, but SQL can still use them to read whatever they
allow.

## Full details

Authentication per provider, IAM policies, endpoint overrides for S3-compatible
stores, caching behaviour and troubleshooting are all in
**[CLOUD_STORAGE_GUIDE.md](../CLOUD_STORAGE_GUIDE.md)**.

## How it works

Remote paths are dispatched through an `IFileProvider` abstraction to DuckDB's
virtual filesystem.

- [spec/DESIGN_DECISIONS.md](../spec/DESIGN_DECISIONS.md) — the filesystem abstraction
- [archive/flapi-10-fs-abstraction.md](../archive/flapi-10-fs-abstraction.md) — the original design note (historical)
