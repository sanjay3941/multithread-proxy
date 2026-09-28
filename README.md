# Multithreaded HTTP/HTTPS Forward Proxy

A C++17 forward proxy for Linux and POSIX environments, implemented with TCP sockets and `std::thread`. It forwards HTTP requests and supports HTTPS through HTTP `CONNECT` tunnels. HTTPS traffic remains encrypted between the client and destination server.

## Features

- HTTP request parsing and forwarding, including absolute-URL to origin-form conversion.
- Request-body forwarding based on `Content-Length`.
- HTTPS tunneling with bidirectional TCP forwarding.
- A fixed worker thread pool with a bounded, thread-safe connection queue.
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

The client worker uses `poll()` to forward traffic in both directions and remains occupied until the tunnel closes. The client and destination perform their TLS handshake through this tunnel. The proxy does not terminate TLS or decrypt application data, so HTTPS paths, cookies, and response contents are not available to its HTTP parser or logger.

## Concurrency and Socket Management

`ProxyServer` maintains a fixed pool of worker threads and a bounded queue of accepted connections.

| Parameter | Default | CLI position | Description |
| --- | --- | --- | --- |
| Worker count | 4 | argv[2] | OS threads that run `ClientHandler::run()`. |
| Queue capacity | 16 | argv[3] | Maximum sockets waiting to be picked up. |

**Rejection.** When the queue is full the new socket is closed immediately, a `STATUS=REJECTED REASON=QUEUE_FULL` warning is logged, and the connection is counted as failed. The accept loop is never blocked.

**Worker reuse.** After finishing a connection a worker loops back and calls `WorkQueue::pop()`, blocking until the next item arrives. The same OS thread handles many successive connections. The `THREAD=` field in every log line makes this visible: the same thread ID appears across multiple `REQ-*` entries.

**HTTPS tunnel occupancy.** `handleConnect()` uses `poll()` for bidirectional forwarding within the client worker. A tunnel occupies one worker for its lifetime. The 120-second idle timeout resets when traffic flows in either direction; an active tunnel can last longer, so plan pool size accordingly.

**Shutdown.** When `ProxyServer` destructs after `start()` returns, the following sequence runs:

1. The listener socket is closed so any blocked `accept()` fails immediately.
2. `WorkQueue::stop()` sets a stopped flag and wakes all idle workers.
3. `WorkQueue::drain()` closes every socket still waiting in the queue.
4. All worker threads are joined; workers mid-`ClientHandler::run()` finish their current connection (bounded by timeouts) before the join completes.
5. Aggregate statistics are printed.

Normal termination with Ctrl+C sends SIGINT, which kills the process without running the destructor. A signal handler is not included in this change.

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

`ProxyServer::printStatistics()` formats and logs the summary, and the server destructor calls it after joining workers. There is currently no periodic reporting, dashboard, or graceful signal handler. Normal termination with Ctrl+C does not invoke that destructor, so it should not be expected to print a final summary.

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

The checks use loopback fixtures, the configured timeout values, and a temporary log directory. They take a few minutes and do not require root access or external servers.

To test from another device on a reachable network, configure its manual HTTP proxy with the Linux machine's reachable IPv4 address and the selected listening port. The device must support the proxy setting, and the host firewall must allow that connection.

For packet inspection in Wireshark, filter on the selected port, for example:

```text
tcp.port == 8080
```

Packet captures can show TCP connections, the initial `CONNECT` exchange, and TLS records. HTTPS application contents remain encrypted.

## Error Handling

Before a response begins, the client handler returns `408 Request Timeout` for client header/body timeouts, `504 Gateway Timeout` for upstream timeouts, `400 Bad Request` for invalid arguments, and `502 Bad Gateway` for other caught connection errors. Send failures while returning error responses are suppressed.

Read, write, and tunnel idle timeouts are logged as failures and counted as failed connections. Once an HTTP response or CONNECT tunnel has started, a failure closes the connection without injecting another HTTP response into the stream.

## Project Structure

```text
multithread-proxy/
|-- CMakeLists.txt
|-- README.md
|-- include/
|   |-- http/http_parser.hpp
|   |-- logging/logger.hpp
|   |-- logging/statistics.hpp
|   |-- network/socket.hpp
|   |-- proxy/client_handler.hpp
|   |-- proxy/proxy_server.hpp
|   `-- proxy/timeouts.hpp
|-- src/
|   |-- main.cpp
|   |-- http/http_parser.cpp
|   |-- logging/logger.cpp
|   |-- logging/statistics.cpp
|   |-- network/socket.cpp
|   |-- proxy/client_handler.cpp
|   |-- proxy/proxy_server.cpp
|   `-- proxy/work_queue.cpp
|-- build/                    # Generated build files and executable
|-- tests/
|   |-- test_timeouts.py       # Local timeout regression checks
|   `-- test_worker_pool.py    # Queue limit, recovery, and worker reuse
`-- logs/                     # Runtime logs when started from the repository root
```

## Current Limitations

- One HTTP request and response per client connection.
- No chunked request-body forwarding or complete HTTP/1.1 compatibility.
- No IPv6 listener, although outbound connections can use IPv6.
- No proxy authentication, destination filtering, caching, or rate limiting.
- Fixed worker pool (default 4 workers, 16-slot queue); HTTPS tunnels each occupy one worker for their full lifetime.
- DNS resolution is not covered by the TCP connection timeout.
- No graceful signal-based shutdown or periodic statistics reporting.
- No TLS interception, log rotation, or traffic dashboard.

## Author

Sanjay Viswaq
B.Tech Computer Science and Engineering, Cybersecurity Specialization

Repository: [sanjay3941/multithread-proxy](https://github.com/sanjay3941/multithread-proxy)
