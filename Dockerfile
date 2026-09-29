# syntax=docker/dockerfile:1

# ==============================================================================
# Stage 1: Build the C++ Forward Proxy and Download wstunnel
# ==============================================================================
FROM ubuntu:24.04 AS builder

ARG WSTUNNEL_VERSION=10.7.1

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ca-certificates \
    curl \
 && rm -rf /var/lib/apt/lists/*

# Download pinned wstunnel binary
RUN curl -fsSL "https://github.com/erebe/wstunnel/releases/download/v${WSTUNNEL_VERSION}/wstunnel_${WSTUNNEL_VERSION}_linux_amd64.tar.gz" \
    | tar -xz -C /tmp \
 && chmod +x /tmp/wstunnel

# Build cpp_proxy from source using CMake
WORKDIR /src
COPY CMakeLists.txt ./
COPY include/ include/
COPY src/ src/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build --parallel

# ==============================================================================
# Stage 2: Production Runtime Image
# ==============================================================================
FROM ubuntu:24.04

# Runtime requirements: ca-certificates for outbound TLS, bash for supervisor
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    bash \
 && rm -rf /var/lib/apt/lists/*

# Create unprivileged application user
RUN useradd --system --create-home --home-dir /app appuser

WORKDIR /app

# Copy binaries from builder stage
COPY --from=builder /tmp/wstunnel /usr/local/bin/wstunnel
COPY --from=builder /src/build/cpp_proxy /app/cpp_proxy
COPY start.sh /app/start.sh

# Set executable permissions and create logging directory
RUN chmod +x /usr/local/bin/wstunnel /app/cpp_proxy /app/start.sh \
 && mkdir -p /app/logs \
 && chown -R appuser:appuser /app

USER appuser

# Render supplies the public web service port through $PORT.
ENV PORT=10000
EXPOSE 10000

ENTRYPOINT ["/app/start.sh"]
