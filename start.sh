#!/usr/bin/env bash
# ==============================================================================
# Container Entrypoint: start.sh
# Supervises the internal C++ forward proxy and the public wstunnel server.
# ==============================================================================
set -u

echo "=================================================="
echo " Starting Multithreaded Proxy Container Services "
echo "=================================================="

# 1. Validate required environment configuration
if [ -z "${TUNNEL_SECRET:-}" ]; then
    echo "[start.sh] [FATAL] TUNNEL_SECRET environment variable is not set." >&2
    echo "[start.sh] Please configure TUNNEL_SECRET in your Render dashboard environment variables." >&2
    exit 1
fi

PUBLIC_PORT="${PORT:-10000}"
INTERNAL_PROXY_PORT="${INTERNAL_PORT:-18080}"
WORKER_COUNT="${PROXY_WORKERS:-8}"
QUEUE_SIZE="${PROXY_QUEUE:-32}"

# 2. Locate and validate required executables
PROXY_BIN="/app/cpp_proxy"
WSTUNNEL_BIN="/usr/local/bin/wstunnel"

if [ ! -x "$PROXY_BIN" ]; then
    if [ -x "./cpp_proxy" ]; then
        PROXY_BIN="./cpp_proxy"
    elif [ -x "./build/cpp_proxy" ]; then
        PROXY_BIN="./build/cpp_proxy"
    else
        echo "[start.sh] [FATAL] cpp_proxy binary not found or not executable at $PROXY_BIN" >&2
        exit 1
    fi
fi

if ! command -v "$WSTUNNEL_BIN" >/dev/null 2>&1; then
    if command -v wstunnel >/dev/null 2>&1; then
        WSTUNNEL_BIN="$(command -v wstunnel)"
    elif [ -x "/tmp/wstunnel" ]; then
        WSTUNNEL_BIN="/tmp/wstunnel"
    else
        echo "[start.sh] [FATAL] wstunnel executable not found at $WSTUNNEL_BIN" >&2
        exit 1
    fi
fi

# 3. Ensure runtime log directory exists
mkdir -p logs

PROXY_PID=""
WSTUNNEL_PID=""
SHUTTING_DOWN=false

# 4. Graceful shutdown handler
cleanup() {
    if [ "$SHUTTING_DOWN" = true ]; then
        return
    fi
    SHUTTING_DOWN=true

    echo ""
    echo "[start.sh] Shutdown signal received. Initiating graceful shutdown..."

    # Send SIGINT to cpp_proxy (invokes its graceful shutdown: drains queue, joins workers, prints statistics)
    if [ -n "$PROXY_PID" ] && kill -0 "$PROXY_PID" 2>/dev/null; then
        echo "[start.sh] Sending SIGINT to cpp_proxy (PID ${PROXY_PID})..."
        kill -SIGINT "$PROXY_PID" 2>/dev/null || true
    fi

    # Send SIGTERM to wstunnel server
    if [ -n "$WSTUNNEL_PID" ] && kill -0 "$WSTUNNEL_PID" 2>/dev/null; then
        echo "[start.sh] Sending SIGTERM to wstunnel server (PID ${WSTUNNEL_PID})..."
        kill -SIGTERM "$WSTUNNEL_PID" 2>/dev/null || true
    fi

    # Wait for both processes to complete
    if [ -n "$PROXY_PID" ]; then
        wait "$PROXY_PID" 2>/dev/null || true
        echo "[start.sh] cpp_proxy stopped cleanly."
    fi
    if [ -n "$WSTUNNEL_PID" ]; then
        wait "$WSTUNNEL_PID" 2>/dev/null || true
        echo "[start.sh] wstunnel server stopped."
    fi

    echo "[start.sh] All container processes terminated."
}

trap cleanup SIGTERM SIGINT

# 5. Start internal C++ HTTP/HTTPS Forward Proxy
echo "[start.sh] Launching C++ proxy on internal port 127.0.0.1:${INTERNAL_PROXY_PORT} (workers=${WORKER_COUNT}, queue=${QUEUE_SIZE})..."
"$PROXY_BIN" "$INTERNAL_PROXY_PORT" "$WORKER_COUNT" "$QUEUE_SIZE" &
PROXY_PID=$!

sleep 0.5
if ! kill -0 "$PROXY_PID" 2>/dev/null; then
    echo "[start.sh] [FATAL] cpp_proxy failed to start on port ${INTERNAL_PROXY_PORT}." >&2
    exit 1
fi
echo "[start.sh] cpp_proxy is running (PID ${PROXY_PID})."

# 6. Start wstunnel Server
# Render terminates external TLS (HTTPS/WSS). wstunnel server listens for cleartext WebSocket
# on 0.0.0.0:$PUBLIC_PORT and strictly restricts destination forwarding to the internal proxy.
echo "[start.sh] Launching wstunnel server on 0.0.0.0:${PUBLIC_PORT} (forwarding to 127.0.0.1:${INTERNAL_PROXY_PORT})..."
"$WSTUNNEL_BIN" server \
    --restrict-to "127.0.0.1:${INTERNAL_PROXY_PORT}" \
    --restrict-http-upgrade-path-prefix "$TUNNEL_SECRET" \
    "ws://0.0.0.0:${PUBLIC_PORT}" &
WSTUNNEL_PID=$!

sleep 0.5
if ! kill -0 "$WSTUNNEL_PID" 2>/dev/null; then
    echo "[start.sh] [FATAL] wstunnel server failed to start on port ${PUBLIC_PORT}." >&2
    kill -SIGINT "$PROXY_PID" 2>/dev/null || true
    exit 1
fi
echo "[start.sh] wstunnel server is running (PID ${WSTUNNEL_PID})."
echo "[start.sh] Ready to accept authenticated WebSocket connections via Render."

# 7. Supervise child processes
EXIT_CODE=0
wait -n || EXIT_CODE=$?

echo "[start.sh] A managed service exited with status ${EXIT_CODE}. Shutting down remaining services..."
cleanup
exit "$EXIT_CODE"
