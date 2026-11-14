---
description: Pre-deployment check for a flAPI project - validation, secrets, packaging, cache warm-up and replica settings, health probes
argument-hint: "[target: docker | binary | cloud-run | kubernetes | pypi]"
---

Run a deployment readiness check for this flAPI project. Target: $ARGUMENTS

Use the `flapi-deploy` skill (and `flapi-operate` for probes, `flapi-cache-design`
for cache warm-up choices, `flapi-auth` for exposure).

Work through this list and report each item as PASS / FIX / N/A with evidence:

1. `flapi -c flapi.yaml --validate-config` is clean (no errors; warnings explained).
2. No secrets in the tree: no literal passwords/tokens in YAML or SQL; every
   `{{env.X}}` matched by `template.environment-whitelist`; `.env`, `*.pem`, `*.key`
   not shipped (`flapi pack` refuses them by default - do not add `--allow-secrets`).
3. Auth: every endpoint that exposes non-public data has `auth:`; the config service
   is either off in production or protected by a strong `--config-service-token`.
4. Caches: warm-up mode fits the target (`if-empty`/`never` + a prebuilt cache for
   serverless; `always` only if startup time allows); replicas use
   `ducklake.read-only: true` on a shared catalog; retention is set.
5. Artifact: build it for the target (`flapi pack --in . --out <name>
   [--with-extensions]` for a single binary, the Docker image, ...) and run it from
   another directory to prove it is self-contained (`<name> info`).
6. Probes: `/health/live` for liveness, `/health` for readiness (503 until caches
   are ready); confirm both answer on the started artifact.
7. Observability: log level, `log-format: json` for log shipping, tracing if
   required.
Finish with the exact commands to ship it.
