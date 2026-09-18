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
LIMIT 1000
```

Credentials come from the environment (`AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY`, `AWS_REGION`, and the Google/Azure equivalents), never
from the config file. Glob patterns work, and DuckDB pushes filters down, so a
partitioned layout is read selectively rather than wholesale.

## 2. Load the configuration itself from a bucket

Useful when there is no filesystem to mount — Lambda, Cloud Run, Azure Functions
— or when a bucket is your GitOps target.

```bash
flapi --config s3://my-bucket/configs/flapi.yaml
flapi --config gs://my-bucket/configs/flapi.yaml
flapi --config https://raw.githubusercontent.com/myorg/configs/main/flapi.yaml
```

SQL templates are resolved relative to the config, so the whole tree can live in
the bucket.

> The alternative for immutable deployments is to bake the config tree into the
> binary instead: see [self-packaging](./self-packaging.md). Choose cloud config
> when you want to change configuration without redeploying, and self-packaging
> when you want the artifact to be complete and fixed.

## Full details

Authentication per provider, IAM policies, endpoint overrides for S3-compatible
stores, caching behaviour and troubleshooting are all in
**[CLOUD_STORAGE_GUIDE.md](../CLOUD_STORAGE_GUIDE.md)**.

## How it works

Remote paths are dispatched through an `IFileProvider` abstraction to DuckDB's
virtual filesystem.

- [spec/DESIGN_DECISIONS.md](../spec/DESIGN_DECISIONS.md) — the filesystem abstraction
- [archive/flapi-10-fs-abstraction.md](../archive/flapi-10-fs-abstraction.md) — the original design note (historical)
