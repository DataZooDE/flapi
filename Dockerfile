FROM ubuntu:24.04 AS base

# Install runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    libc6 \
    libstdc++6 \
    libubsan1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Default target architecture
ARG TARGETARCH=amd64

# Copy prebuilt artifact based on architecture
COPY ./artifacts/flapi-linux-${TARGETARCH}/flapi /app/flapi

# Ensure executable permissions
RUN chmod +x /app/flapi

# Set entrypoint
ENTRYPOINT ["/app/flapi"]
