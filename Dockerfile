FROM ubuntu:24.04

# Install runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    libc6 \
    libstdc++6 \
    libubsan1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# build context is set in the github `build-push-action`
COPY --from=build flapi /app/flapi

# Ensure executable permissions
RUN chmod +x /app/flapi && \
    # Verify binary
    file /app/flapi && \
    # Test execution
    /app/flapi --version

ENTRYPOINT ["/app/flapi"]
