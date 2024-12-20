FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libc6 \
    libstdc++6 \
    libubsan1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY build/release/flapi.amd64 /app/flapi.amd64
COPY build/release/flapi.arm64 /app/flapi.arm64

# Use architecture-specific binary based on target platform
ARG TARGETARCH
RUN if [ "$TARGETARCH" = "amd64" ]; then \
        cp /app/flapi.amd64 /app/flapi; \
    elif [ "$TARGETARCH" = "arm64" ]; then \
        cp /app/flapi.arm64 /app/flapi; \
    fi \
    && chmod +x /app/flapi \
    && rm /app/flapi.* 

ENTRYPOINT ["/app/flapi"]
