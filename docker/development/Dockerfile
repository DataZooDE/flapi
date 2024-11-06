FROM ubuntu:oracular-20241009

RUN apt-get update && apt-get install -y --no-install-recommends \
    libc6 \
    libstdc++6 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY ./build/Release/flapi /app/flapi

ENTRYPOINT ["/app/flapi"]
