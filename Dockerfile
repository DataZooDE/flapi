FROM ubuntu:24.04 AS slim

# Install runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    libc6 \
    libstdc++6 \
    libubsan1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

ARG TARGETARCH
# The binary and, when the build context has it, extensions/<duckdb version>/<platform>/
# beside it (#245): flapi loads its default DuckDB extensions from /app/extensions, so the
# image starts without downloading anything. CI copies the directory its platform job
# verified; `make docker` fills it with `flapi extensions install --defaults`.
COPY --from=build ${TARGETARCH}/ /app/

# Ensure executable permissions
RUN chmod +x /app/flapi

# Add labels
LABEL org.opencontainers.image.title="flAPI"
LABEL org.opencontainers.image.description="flAPI is a powerful service that automatically generates read-only APIs for datasets by utilizing SQL templates. Built on top of DuckDB and leveraging its SQL engine and extension ecosystem, flAPI offers a seamless way to connect to various data sources and expose them as RESTful APIs."
LABEL org.opencontainers.image.version="0.2.0"
LABEL org.opencontainers.image.authors="DataZoo GmbH <hello@datazoo.com>"
LABEL org.opencontainers.image.url="https://github.com/datazoo/flapi"
LABEL org.opencontainers.image.source="https://github.com/datazoo/flapi"
LABEL org.opencontainers.image.licenses="BUSL-1.1"

ENTRYPOINT ["/app/flapi"]

# ---------------------------------------------------------------------------
# runtime: the same image plus the bigquery community extension, added to
# /app/extensions. Built with `--target runtime`; published as
# :<tag>-runtime. The default extensions are in the default image already (#245),
# so this target no longer sets FLAPI_EXTENSION_DIRECTORY. Installed by the binary
# that ships, so it matches its DuckDB version and platform (under QEMU the arm64
# stage fetches the arm64 build).
# ---------------------------------------------------------------------------
FROM slim AS runtime
# bigquery is a community extension, published per DuckDB version some time after a DuckDB
# release. The build fails without it (#251): a -runtime image without bigquery is not one.
# On a DuckDB bump that it does not support yet, hold the bump or the runtime target back.
RUN /app/flapi extensions install --dir /app/extensions bigquery@community
LABEL org.opencontainers.image.description="flAPI with its default DuckDB extensions plus bigquery."

# The default target is the image with the default extensions.
FROM slim
