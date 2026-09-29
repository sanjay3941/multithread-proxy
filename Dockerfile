# syntax=docker/dockerfile:1
# ---- Stage 1: build the proxy ----
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --parallel 1

# ---- Stage 2: runtime image (proxy + wstunnel) ----
FROM ubuntu:24.04
ARG WSTUNNEL_VERSION=10.7.1
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && curl -fsSL "https://github.com/erebe/wstunnel/releases/download/v${WSTUNNEL_VERSION}/wstunnel_${WSTUNNEL_VERSION}_linux_amd64.tar.gz" \
    | tar -xz -C /usr/local/bin wstunnel \
 && chmod +x /usr/local/bin/wstunnel \
 && apt-get purge -y curl \
 && apt-get autoremove -y \
 && rm -rf /var/lib/apt/lists/*

RUN useradd --system --create-home --home-dir /app appuser
WORKDIR /app
COPY --from=build /src/build/cpp_proxy /app/cpp_proxy
COPY start.sh /app/start.sh
RUN chmod +x /app/start.sh \
 && mkdir -p /app/logs \
 && chown -R appuser:appuser /app
USER appuser

# Render sets $PORT; only that port is public. The proxy stays on 127.0.0.1:8080 side.
CMD ["/app/start.sh"]
