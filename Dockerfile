

# -----------------------------------------------------------------------------

# flAPI build stage
FROM debian:bookworm-slim AS flapi_builder

# Install build dependencies
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    curl \
    zip \
    unzip \
    tar \
    pkg-config \
    ca-certificates \
    && apt-get clean -y && rm -rf /var/lib/apt/lists/*

# Set up working directory
WORKDIR /app

# Copy the source code
COPY . .

# Bootstrap vcpkg
RUN ./vcpkg/bootstrap-vcpkg.sh

# Build the project
RUN make clean && make release

# -----------------------------------------------------------------------------

# DuckDB-BigQuery build stage
FROM debian:bookworm-slim AS duckdb_bigquery_builder

# Install build dependencies
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \
    build-essential \
    cmake \
    flex \
    bison \
    ninja-build \
    git \
    curl \
    zip \
    unzip \
    tar \
    pkg-config \
    ca-certificates \
    && apt-get clean -y && rm -rf /var/lib/apt/lists/*

# Set up working directory
WORKDIR /duckdb-bigquery

# Clone the duckdb-bigquery repository
RUN git clone --recursive https://github.com/hafenkran/duckdb-bigquery.git . \
    && git clone https://github.com/Microsoft/vcpkg.git \
    && ./vcpkg/bootstrap-vcpkg.sh \
    && git checkout develop

# Build duckdb-bigquery
RUN make clean \
    && VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake make release

# -----------------------------------------------------------------------------

# Runtime stage
FROM debian:bookworm-slim

# Install runtime dependencies
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \
    libstdc++6 \
    ca-certificates \
    libubsan1 \
    libasan6 \
    && apt-get clean -y && rm -rf /var/lib/apt/lists/*

# Set up working directory
WORKDIR /app

# Copy the built executable from the flapi_builder stage
COPY --from=flapi_builder /app/build/Release/flapi /app/flapi

# Copy the built duckdb-bigquery extension from the duckdb_bigquery_builder stage
COPY --from=duckdb_bigquery_builder /duckdb-bigquery/build/release/extension/bigquery/bigquery.duckdb_extension /app/bigquery.duckdb_extension
COPY --from=duckdb_bigquery_builder /duckdb-bigquery/build/release/duckdb /app/duckdb

# Copy any necessary configuration files or resources
COPY examples/flapi_local_bq.yaml /app/config/flapi.yaml
COPY examples/sqls/publicis.yaml /app/config/sqls/publicis.yaml
COPY examples/sqls/publicis_cache.sql /app/config/sqls/publicis_cache.sql
COPY examples/sqls/publicis.sql /app/config/sqls/publicis.sql

# Expose the port your application listens on
EXPOSE 8080

# Set the entrypoint to run your application
ENTRYPOINT ["/app/flapi", \
            "--port", "8080", \
            "--config", "/app/config/flapi.yaml"]
