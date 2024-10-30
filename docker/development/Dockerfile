FROM cgr.dev/chainguard/glibc-dynamic:latest

WORKDIR /app
COPY ./build/Release/flapi /app/flapi

ENTRYPOINT ["/app/flapi"]
