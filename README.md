# Multithreaded HTTP/HTTPS Forward Proxy

A C++17 forward proxy for Linux and POSIX environments, implemented with TCP sockets and `std::thread`. It forwards HTTP requests and supports HTTPS through HTTP `CONNECT` tunnels. HTTPS traffic remains encrypted between the client and destination server.

## Features

- HTTP request parsing and forwarding, including absolute-URL to origin-form conversion.
- Request-body forwarding based on `Content-Length`.
- HTTPS tunneling with bidirectional TCP forwarding.
- A fixed worker thread pool with a bounded, thread-safe connection queue.
- An in-memory cache for popular, explicitly public HTTP responses.
- Move-only RAII socket ownership and automatic descriptor cleanup.
- Connection lifecycle logging to the console and a log file.
- Per-connection byte counts, duration, destination, and status tracking.
- Aggregate connection statistics maintained with atomic counters.
- CMake build configuration with compiler warnings enabled on UNIX.

## Requirements

- Linux or a compatible POSIX environment.
- A C++17 compiler and standard library with filesystem support.
- CMake 3.16 or later.
- POSIX sockets and pthread support.

On Debian-based systems, including Kali Linux:

```bash
sudo apt update
sudo apt install build-essential cmake
```

## Build

```bash
git clone https://github.com/sanjay3941/multithread-proxy.git
cd multithread-proxy
cmake -S . -B build
cmake --build build --parallel
```

The executable is `build/cpp_proxy`.

## Run

From the repository root:

```bash
./build/cpp_proxy
```

The default listening address is `0.0.0.0:8080`. To select another port:

```bash
./build/cpp_proxy 3128
```

The command-line interface accepts up to three positional arguments:

```text
./build/cpp_proxy [port [workers [queue]]]
```

| Argument | Default | Description |
| --- | --- | --- |
| port | 8080 | TCP port to listen on (1–65535). |
| workers | 4 | Number of worker threads (1–256). |
| queue | 16 | Maximum connections in the work queue (1–4096). |

Arguments must be positive decimal integers in these ranges. Invalid values exit with usage status 2 before the server starts.

The bind address is always `0.0.0.0` (all IPv4 interfaces).

The listener accepts IPv4 connections on all local interfaces by default. Use the proxy on a trusted network: authentication, destination restrictions, and rate limiting are not implemented.

## Request Processing

### HTTP

For each client connection, the proxy:

1. Reads the request headers and parses the method, destination, port, path, version, and headers.
2. Opens a TCP connection to the destination server.
3. Converts an absolute request target such as `http://example.com/index.html` to `/index.html`.
4. Removes the incoming `Connection` and `Proxy-Connection` headers and adds `Connection: close`.
5. Forwards the request body when `Content-Length` is provided.
6. Relays the upstream response to the client and closes the connection.

Each HTTP connection handles one request and response. Persistent client connections and chunked request-body forwarding are not implemented.

### HTTP Cache

The proxy observes requests for each full HTTP URL (host, port, path, and query). On the third request for the same URL within five minutes, it can capture an eligible upstream response. Later requests can be served from memory while that response is fresh. Concurrent requests for a popular URL wait briefly for one in-progress fetch, avoiding duplicate origin requests when that response can be stored. If it cannot be stored or the wait exceeds 15 seconds, those requests fetch normally.

Only complete `200 OK` responses with `Content-Length` and explicit `Cache-Control: public, max-age=N` are stored. Requests with credentials, cookies, ranges, conditions, request bodies, or cache bypass directives are forwarded without caching. Responses with `private`, `no-store`, `no-cache`, `Set-Cookie`, `Vary`, `Transfer-Encoding`, or `Age` are not stored. The cache also rejects incomplete and oversized responses. Unsafe methods such as POST invalidate a cached response for the matching URL.

Entries expire according to `max-age`, capped at ten minutes, and are removed after ten minutes without access. A housekeeping thread checks for expired and idle entries once a minute. Memory is limited to 1 MiB per response and 16 MiB overall; least recently used entries are evicted when necessary. Demand tracking is limited to 4,096 URLs. Cache hits include an `Age` response header. Decisions such as `MISS`, `HIT`, `COALESCED`, `STORED`, and `BYPASS` appear in `logs/proxy.log` under the connection ID.

To measure which popular responses would qualify without serving cached responses, start the proxy in simulation mode:

```bash
PROXY_CACHE_SIMULATE=1 ./build/cpp_proxy
```

Simulation mode logs `WOULD_STORE` for eligible responses. It still forwards every request to the origin and does not coalesce requests. The cache is in memory only and is empty after a restart.

### HTTPS

Clients establish an HTTPS tunnel using a request such as:

```http
CONNECT example.com:443 HTTP/1.1
Host: example.com:443
```

After connecting to the destination, the proxy sends:

```http
HTTP/1.1 200 Connection Established
```

The client worker uses `poll()` to relay traffic in both directions and remains occupied until the tunnel closes. The client and destination perform their TLS handshake through this tunnel. The proxy does not terminate TLS or decrypt application data, so HTTPS paths, cookies, and response contents are not available to its HTTP parser or logger.

HTTPS tunnel contents cannot be cached by this proxy.

## Concurrency and Socket Management

`ProxyServer` maintains a fixed pool of worker threads and a bounded queue of accepted connections.

| Parameter | Default | CLI position | Description |
| --- | --- | --- | --- |
| Worker count | 4 | argv[2] | OS threads that run `ClientHandler::run()`. |
| Queue capacity | 16 | argv[3] | Maximum sockets waiting to be picked up. |

**Rejection.** When the queue is full the new socket is closed immediately, a `STATUS=REJECTED REASON=QUEUE_FULL` warning is logged, and the connection is counted as failed. The accept loop is never blocked.

**Worker reuse.** After finishing a connection a worker loops back and calls `WorkQueue::pop()`, blocking until the next item arrives. The same OS thread handles many successive connections. The `THREAD=` field in every log line makes this visible: the same thread ID appears across multiple `REQ-*` entries.

**HTTPS tunnel occupancy.** `handleConnect()` uses `poll()` to watch both sockets from the client worker. That worker is occupied for the full lifetime of the tunnel. The 120-second idle timeout resets when traffic flows in either direction; an active tunnel can last longer than the idle window, so plan the pool size accordingly.

**Shutdown and signal handling.** The SIGINT handler sets a shutdown flag and restores the default handler. When SIGINT interrupts the main thread's blocked `accept()` call, `ProxyServer::start()` logs the shutdown request and returns. Its destructor then:

1. Closes the listener, stops the queue, and closes queued connections.
2. Joins the workers after their active connections finish or time out.
3. Prints aggregate statistics.

When `main()` returns, the cache's housekeeping thread is stopped and joined. A second Ctrl+C uses the restored default handler and terminates the process immediately. The first signal does not forcibly interrupt active connections. Because SIGINT is not directed specifically to the main thread, a signal handled by a worker may leave `accept()` blocked until another event interrupts it.

`network::Socket` owns a socket descriptor, closes it on destruction, and supports move semantics. Its send operation handles partial writes, and its receive operation retries interrupted system calls. The program ignores `SIGPIPE` and uses `MSG_NOSIGNAL` when sending data.

The listener is IPv4-only. Outbound hostname resolution uses `AF_UNSPEC`, allowing attempts to connect to either IPv4 or IPv6 destination addresses when available.

## Timeouts

Timeout values are defined in `include/proxy/timeouts.hpp`:

| Operation | Default | Behavior |
| --- | --- | --- |
| Client headers | 30 seconds | Total deadline, including clients that send headers slowly. |
| Upstream TCP connection | 10 seconds | Applies to each candidate address; DNS resolution is separate. |
| HTTP upstream read | 60 seconds | Maximum wait for the next response data. |
| HTTPS tunnel idle | 120 seconds | Resets when traffic is forwarded in either direction. |
| Socket write | 60 seconds | Bounds a blocked write to either peer. |

HTTP request-body reads retain the 30-second client receive timeout. Read and write timeouts bound individual waits, not the total duration of an active transfer. Kernel socket timeouts may expire slightly later than their configured value.

## Logging and Statistics

### Connection Logs

The logger writes to standard output and appends to `logs/proxy.log`, relative to the process working directory. Running the executable from the repository root keeps logs in the repository's `logs` directory.

Log entries include a timestamp, severity, and thread ID. Connection events use identifiers such as `REQ-000001` to associate messages with a request. Depending on the event, entries include:

- Client address and port.
- HTTP method, protocol classification, destination, and request path.
- Upstream connection and tunnel establishment status.
- Disconnect events and forwarding errors.
- HTTP response status, when captured.
- Connection duration and byte counts.

HTTPS destination information is taken from the `CONNECT` request; encrypted HTTP request details are not logged. Log writes are serialized with a mutex. File-open failures do not stop console logging, and log rotation is not implemented.

### Aggregate Statistics

`ProxyStatistics` tracks total and active connections, HTTP and HTTPS connection counts, successful and failed connections, transferred bytes, and accumulated duration. Aggregate byte and duration totals are updated when a connection finishes.

Byte counts combine both sides of a proxied connection: received bytes include reads from clients and upstream servers, and sent bytes include writes to both. These are application-level I/O counts, not a measurement of unique payload bytes or total network traffic.

`ProxyServer::printStatistics()` formats and logs the summary. The destructor calls it after all workers have been joined, so the summary is printed at the end of both a normal exit and a graceful Ctrl+C shutdown. There is no periodic reporting or traffic dashboard.

## Manual Testing

Start the proxy in one terminal, then run these commands in another:

```bash
# HTTP forwarding
curl --noproxy "" -v -x http://127.0.0.1:8080 http://example.com/

# HTTPS tunneling
curl --noproxy "" -v -x http://127.0.0.1:8080 https://example.com/
```

For HTTPS, curl should report a successful `CONNECT` response before negotiating TLS with the destination.

Run the local timeout regression checks with Python 3 after building:

```bash
python3 tests/test_timeouts.py build/cpp_proxy
```

Run the focused worker-pool check with:

```bash
python3 tests/test_worker_pool.py build/cpp_proxy
```

Run the HTTP cache integration checks with:

```bash
python3 tests/test_cache.py build/cpp_proxy
```

The checks use loopback fixtures, the configured timeout values, and a temporary log directory. They take a few minutes and do not require root access or external servers.

To test from another device on a reachable network, configure its manual HTTP proxy with the Linux machine's reachable IPv4 address and the selected listening port. The device must support the proxy setting, and the host firewall must allow that connection.

For packet inspection in Wireshark, filter on the selected port, for example:

```text
tcp.port == 8080
```

Packet captures can show TCP connections, the initial `CONNECT` exchange, and TLS records. HTTPS application contents remain encrypted.

## Docker + Render Deployment

This section describes deploying the proxy to [Render](https://render.com) using a containerized `wstunnel` transport bridge.

### Architecture

Render Web Services expose HTTP/HTTPS and WebSocket endpoints; they do not route arbitrary inbound raw TCP traffic to containers. To host the native C++ TCP forward proxy on Render without altering its socket architecture, `wstunnel` is used strictly as an encrypted L4 WebSocket transport layer.

```text
Local Windows Machine:
  [ Browser / curl ]
          |
          | TCP (http://127.0.0.1:8080)
          v
  [ wstunnel Client (v10.7.1) ]
          |
          | WSS (WebSocket over TLS / Port 443)
          v
Render Cloud:
  [ Render Web Service Edge / TLS Termination ]
          |
          | Cleartext WebSocket ($PORT)
          v
  [ Docker Container ]
    +-------------------------------------------------------------+
    |                                                             |
    |  [ wstunnel Server (v10.7.1) ]                              |
    |         | (Loopback TCP forwarding)                         |
    |         v                                                   |
    |  [ cpp_proxy (Internal Port 18080) ]                        |
    |         |                                                   |
    +---------|---------------------------------------------------+
              | Outbound TCP (HTTP / HTTPS CONNECT)
              v
       [ Internet Origin Servers ]
```

#### Core Design Guarantees

- **`wstunnel` is strictly a transport bridge:** It encapsulates raw TCP stream bytes inside WebSocket frames over TLS and decapsulates them inside the container. It does not parse or modify HTTP/HTTPS proxy payloads.
- **`cpp_proxy` remains the active forward proxy:** All HTTP parsing, validation, in-memory caching, request forwarding, statistics tracking, and HTTPS `CONNECT` tunnels are handled natively by `cpp_proxy`.
- **Zero TLS termination by proxy:** For HTTPS `CONNECT` tunnels, end-to-end TLS encryption between client and origin destination is completely preserved.
- **Zero public TCP exposure:** `cpp_proxy` listens exclusively on `127.0.0.1:18080` inside the container. It cannot be accessed directly from the public internet.
- **Strong tunnel authentication:** Access to the `wstunnel` server is protected by `TUNNEL_SECRET` using path-prefix restriction. Unauthenticated connections are rejected immediately with `HTTP 400 Bad Request`.
- **Destination lockdown:** The `wstunnel` server is locked with `--restrict-to 127.0.0.1:18080`, preventing attackers from using the tunnel to relay traffic to arbitrary internal or external hosts.

---

### Render Web Service Configuration

Configure a new Web Service in the Render Dashboard:

| Setting | Value |
| --- | --- |
| **Language / Environment** | `Docker` |
| **Repository** | Your Git repository |
| **Branch** | `main` |
| **Dockerfile Path** | `./Dockerfile` |
| **Docker Context** | `.` |
| **Instance Type** | Free or Starter |

#### Required Environment Variables

Set the following in **Environment** > **Environment Variables** in the Render Dashboard:

| Variable | Required | Default | Description |
| --- | --- | --- | --- |
| `TUNNEL_SECRET` | **Yes** | *None* | Shared secret string for WebSocket authentication. Must be kept confidential and never committed to version control. |
| `PORT` | Auto-injected | `10000` | Public port assigned by Render. The `wstunnel` server binds to `0.0.0.0:$PORT`. |
| `INTERNAL_PORT` | No | `18080` | Internal loopback TCP port on which `cpp_proxy` listens. |
| `PROXY_WORKERS` | No | `8` | Number of worker threads for the `cpp_proxy` thread pool. |
| `PROXY_QUEUE` | No | `32` | Maximum pending connection queue depth for `cpp_proxy`. |

Generate a strong random secret using:
```bash
openssl rand -hex 24
```

---

### Container Process Supervision (`start.sh`)

The container entrypoint `/app/start.sh` concurrently launches and supervises both processes:
1. Validates that `TUNNEL_SECRET` is present; exits with an informative error message if missing.
2. Starts `cpp_proxy` bound to `127.0.0.1:$INTERNAL_PORT`.
3. Starts `wstunnel server` on `0.0.0.0:$PORT` with `--restrict-to 127.0.0.1:$INTERNAL_PORT` and `--restrict-http-upgrade-path-prefix "$TUNNEL_SECRET"`.
4. Traps `SIGTERM` and `SIGINT`:
   - Sends `SIGINT` to `cpp_proxy` to trigger its graceful shutdown sequence (draining the queue, finishing active transfers within timeouts, joining workers, and logging aggregate statistics).
   - Sends `SIGTERM` to `wstunnel server`.
   - Waits for both processes to terminate cleanly before container exit.
5. Monitors both child processes; if either process fails, `start.sh` terminates the remaining process and exits with the corresponding failure code so Render can restart the container.

---

### Local Windows Client Setup

#### 1. Download wstunnel for Windows

Download the pinned `v10.7.1` release for Windows:
- Release archive: `wstunnel_10.7.1_windows_amd64.tar.gz`
- URL: `https://github.com/erebe/wstunnel/releases/download/v10.7.1/wstunnel_10.7.1_windows_amd64.tar.gz`

Extract `wstunnel.exe` to a local directory (for example, `C:\Tools\wstunnel\`).

#### 2. Start the Windows wstunnel Client

Open PowerShell or Command Prompt and run:

```powershell
wstunnel.exe client `
  -L "tcp://127.0.0.1:8080:127.0.0.1:18080" `
  -P "YOUR_TUNNEL_SECRET" `
  "wss://your-service-name.onrender.com"
```

*Command breakdown:*
- `-L tcp://127.0.0.1:8080:127.0.0.1:18080`: Listens locally on `127.0.0.1:8080` and routes incoming TCP connections through the tunnel to `127.0.0.1:18080` on the Render container.
- `-P YOUR_TUNNEL_SECRET`: Specifies the secret HTTP upgrade path prefix required by the server's `--restrict-http-upgrade-path-prefix` configuration.
- `wss://your-service-name.onrender.com`: Secure WebSocket endpoint terminating TLS at Render's edge.

Once connected, your local machine exposes an HTTP/HTTPS forward proxy at `http://127.0.0.1:8080`.

---

### Verification and Testing Procedures

Run these tests from another terminal on the local Windows machine while the `wstunnel` client is running:

#### 1. HTTP Forwarding Test

```bash
curl --noproxy "" -v -x http://127.0.0.1:8080 http://example.com/
```
*Expected behavior:* The client receives `HTTP/1.1 200 OK`. `cpp_proxy` logs:
`[REQ-XXXXXX] PROTOCOL=HTTP METHOD=GET TARGET=example.com:80 ... STATUS=COMPLETED`

#### 2. HTTPS CONNECT Tunneling Test

```bash
curl --noproxy "" -v -x http://127.0.0.1:8080 https://example.com/
```
*Expected behavior:* Curl initiates `CONNECT example.com:443 HTTP/1.1`. `cpp_proxy` replies with `HTTP/1.1 200 Connection Established`. Curl performs TLS negotiation directly with `example.com` through the tunnel and receives the HTTPS response.

#### 3. Egress IP Verification

```bash
curl --noproxy "" -x http://127.0.0.1:8080 https://ifconfig.me
```
*Expected behavior:* Returns the public egress IPv4 address of the Render container host, confirming the request originated from the cloud environment rather than the local client.

#### 4. Security Verification Tests

Verify that the proxy is inaccessible without proper credentials:

- **Direct unauthenticated HTTP access:**
  ```bash
  curl -i https://your-service-name.onrender.com/
  ```
  *Expected behavior:* `wstunnel` rejects the request with `HTTP/1.1 400 Bad Request` (`Invalid protocol request`). `cpp_proxy` is not contacted.

- **Incorrect tunnel secret:**
  Run the client with an invalid secret:
  ```powershell
  wstunnel.exe client -L "tcp://127.0.0.1:8080:127.0.0.1:18080" -P "wrong_secret" "wss://your-service-name.onrender.com"
  ```
  *Expected behavior:* The WebSocket upgrade handshake fails with `Invalid status code: 400`. No tunnel is established.

- **Destination restriction check:**
  Attempting to route to any host other than `127.0.0.1:18080` (e.g. `-L tcp://127.0.0.1:8080:google.com:443`) is rejected by the server's `--restrict-to` policy.

---

### Expected Server Logs

In the Render service dashboard logs, expected operational output includes:

```text
==================================================
 Starting Multithreaded Proxy Container Services 
==================================================
[start.sh] Launching C++ proxy on internal port 127.0.0.1:18080 (workers=8, queue=32)...
[YYYY-MM-DD HH:MM:SS.mmm] [INFO] [THREAD=...] POOL_STARTED WORKERS=8 QUEUE_CAPACITY=32
[YYYY-MM-DD HH:MM:SS.mmm] [INFO] [THREAD=...] PROXY_LISTENING ADDRESS=0.0.0.0:18080
[start.sh] cpp_proxy is running (PID ...).
[start.sh] Launching wstunnel server on 0.0.0.0:10000 (forwarding to 127.0.0.1:18080)...
INFO wstunnel::tunnel::server::server: Starting wstunnel server listening on 0.0.0.0:10000
[start.sh] wstunnel server is running (PID ...).
[start.sh] Ready to accept authenticated WebSocket connections via Render.
INFO wstunnel::tunnel::server::server: Tunnel accepted due to matched restriction: Allow path prefix <SECRET>
[YYYY-MM-DD HH:MM:SS.mmm] [INFO] [THREAD=...] [REQ-000001] CLIENT=127.0.0.1:... STATUS=ACCEPTED
[YYYY-MM-DD HH:MM:SS.mmm] [INFO] [THREAD=...] [REQ-000001] PROTOCOL=HTTP METHOD=GET TARGET=example.com:80 ... STATUS=COMPLETED
```

On container shutdown (redeploy or service stop):
```text
[start.sh] Shutdown signal received. Initiating graceful shutdown...
[start.sh] Sending SIGINT to cpp_proxy (PID ...)...
[start.sh] Sending SIGTERM to wstunnel server (PID ...)...
[YYYY-MM-DD HH:MM:SS.mmm] [INFO] [THREAD=...] SIGINT received, initiating graceful shutdown
[YYYY-MM-DD HH:MM:SS.mmm] [INFO] [THREAD=...] ========== PROXY STATISTICS ==========
Total Connections      : ...
...
[start.sh] cpp_proxy stopped cleanly.
[start.sh] wstunnel server stopped.
[start.sh] All container processes terminated.
```

---

### Known Deployment Limitations

- **Render Free Tier Spin-down:** Free tier web services spin down after 15 minutes of inactivity. When a new connection arrives, container cold-start takes approximately 30–50 seconds.
- **Worker Occupancy during HTTPS Tunnels:** Each active HTTPS `CONNECT` tunnel occupies one worker thread in `cpp_proxy` for the tunnel's lifetime (governed by `HTTPS_TUNNEL_IDLE_TIMEOUT = 120s`). Size `PROXY_WORKERS` appropriately if expecting many concurrent persistent tunnels.
- **WebSocket Transport Overhead:** Encapsulating TCP streams into WebSockets introduces minor latency and framing overhead compared to direct TCP.
- **TCP Only:** The proxy architecture handles TCP-based protocols (HTTP/HTTPS); UDP-based protocols (e.g. QUIC / HTTP/3) are not tunneled by the HTTP forward proxy.

## Error Handling

Before a response begins, the client handler returns `408 Request Timeout` for client header/body timeouts, `504 Gateway Timeout` for upstream timeouts, `400 Bad Request` for invalid arguments, and `502 Bad Gateway` for other caught connection errors. Send failures while returning error responses are suppressed.

Read, write, and tunnel idle timeouts are logged as failures and counted as failed connections. Once an HTTP response or CONNECT tunnel has started, a failure closes the connection without injecting another HTTP response into the stream.

## Project Structure

```text
multithread-proxy/
|-- CMakeLists.txt
|-- Dockerfile
|-- .dockerignore
|-- start.sh
|-- README.md
|-- include/
|   |-- http/http_parser.hpp
|   |-- logging/logger.hpp
|   |-- logging/statistics.hpp
|   |-- network/socket.hpp
|   |-- proxy/client_handler.hpp
|   |-- proxy/cache.hpp
|   |-- proxy/proxy_server.hpp
|   |-- proxy/timeouts.hpp
|   `-- proxy/work_queue.hpp
|-- src/
|   |-- main.cpp
|   |-- http/http_parser.cpp
|   |-- logging/logger.cpp
|   |-- logging/statistics.cpp
|   |-- network/socket.cpp
|   |-- proxy/client_handler.cpp
|   |-- proxy/cache.cpp
|   |-- proxy/proxy_server.cpp
|   `-- proxy/work_queue.cpp
|-- build/                    # Generated build files and executable
|-- tests/
|   |-- test_timeouts.py       # Local timeout regression checks
|   |-- test_worker_pool.py    # Queue limit, recovery, and worker reuse
|   `-- test_cache.py          # Cache eligibility, expiry, and coalescing
`-- logs/                     # Runtime logs when started from the repository root
```

## Current Limitations

- One HTTP request and response per client connection.
- No chunked request-body forwarding or complete HTTP/1.1 compatibility.
- No IPv6 listener, although outbound connections can use IPv6.
- No proxy authentication, destination filtering, or rate limiting.
- Cache scope is limited to simple, explicitly public HTTP responses; it does not cache HTTPS tunnels or persist across restarts.
- Fixed worker pool (default 4 workers, 16-slot queue); HTTPS tunnels each occupy one worker for their full lifetime.
- DNS resolution is not covered by the TCP connection timeout.
- SIGINT shutdown may wait if a worker receives the signal while the main thread remains blocked in `accept()`.
- No periodic statistics reporting, TLS interception, log rotation, or traffic dashboard.

## Author

Sanjay Viswaq
B.Tech Computer Science and Engineering, Cybersecurity Specialization

Repository: [sanjay3941/multithread-proxy](https://github.com/sanjay3941/multithread-proxy)
