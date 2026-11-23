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
COPY --from=build ${TARGETARCH}/flapi /app/flapi

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
# runtime: the same image with every default DuckDB extension (plus bigquery)
# pre-installed, so a
# container never downloads one at startup (scale-to-zero platforms, egress-less
# networks). Built with `--target runtime`; published as :<tag>-runtime.
# Installed by the binary that ships, so the directory matches its DuckDB
# version and platform (under QEMU the arm64 stage fetches arm64 builds).
# ---------------------------------------------------------------------------
FROM slim AS runtime
ENV FLAPI_EXTENSION_DIRECTORY=/opt/flapi/extensions
# bigquery is a community extension, published per DuckDB version some time after a DuckDB
# release: best effort, so the image still builds before it exists for this DuckDB version.
# core_functions, parquet, json and icu are built into flapi and need no install.
RUN /app/flapi extensions install --dir /opt/flapi/extensions \
        httpfs ducklake fts postgres sqlite \
    && (/app/flapi extensions install --dir /opt/flapi/extensions bigquery@community \
        || echo "bigquery@community is not available for this DuckDB version; image built without it")
LABEL org.opencontainers.image.description="flAPI with every default DuckDB extension (plus bigquery when published for its DuckDB version) pre-installed."

# The default target stays the slim image.
FROM slim
