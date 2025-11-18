FROM ubuntu:24.04

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
LABEL org.opencontainers.image.licenses="Apache-2.0"

ENTRYPOINT ["/app/flapi"]
