#!/usr/bin/env bash
# Starts the forward proxy (private, port 8080) and the wstunnel server (public, $PORT).
# TUNNEL_SECRET must be set as an environment variable in the Render dashboard.
set -u

: "${TUNNEL_SECRET:?Set TUNNEL_SECRET in the Render environment variables}"
PORT="${PORT:-10000}"

echo "[start] launching proxy on :8080 (workers=8, queue=32)"
./cpp_proxy 8080 8 32 &

echo "[start] launching wstunnel server on :${PORT}"
# TLS is terminated by Render, so the server speaks plain ws:// here.
# --restrict-to means tunnel clients can ONLY reach the proxy, nothing else.
# --restrict-http-upgrade-path-prefix means only clients that know the secret can connect.
wstunnel server \
  --restrict-http-upgrade-path-prefix "$TUNNEL_SECRET" \
  --restrict-to 127.0.0.1:8080 \
  "ws://0.0.0.0:${PORT}" &

# If either process exits, stop everything so Render restarts the service.
wait -n
echo "[start] a process exited; shutting down" >&2
kill 0 2>/dev/null
exit 1
