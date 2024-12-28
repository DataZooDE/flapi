FROM ubuntu:24.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    libc6 \
    libstdc++6 \
    libubsan1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the appropriate binary based on architecture
COPY --from=build flapi /app/flapi

# Ensure executable permissions
RUN chmod +x /app/flapi

ENTRYPOINT ["/app/flapi"]
