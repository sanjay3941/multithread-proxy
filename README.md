# Multithreaded Privacy Proxy

A C++17 HTTP/HTTPS forward proxy for Linux with domain blocking, a local dashboard, a fixed worker pool, and an in-memory HTTP cache. The dashboard, **Proxy Server**, shows real activity and lets you manage domain rules without editing files.

HTTPS remains encrypted between the browser and destination. Blocking uses the destination hostname; the proxy does not intercept TLS or install certificates.

## Build

Requires Linux/POSIX sockets, a C++17 compiler with filesystem support, CMake 3.16+, pthreads, and glibc 2.34+ (for closing inherited descriptors in the hostname helper). Kali satisfies these requirements. Python 3 is needed for tests. On Kali/Debian, install missing build tools with `sudo apt install build-essential cmake`.

```bash
git clone https://github.com/sanjay3941/multithread-proxy.git
cd multithread-proxy
cmake -S . -B build
cmake --build build --parallel
```

The dashboard and starter domain list are embedded at compile time. Rebuild after changing `web/` or `config/default_domains.txt`. No Node.js or separate web server is required.

## Start the proxy and dashboard

From the build directory, run just:

```bash
./cpp_proxy
```

This starts the proxy on **127.0.0.1:8080 and every active eth2 IPv4 address on port 8080**, sharing **16 workers** and a **64-connection queue**. The direct dashboard listens only on **127.0.0.1:8081**. From the repository root, use `./build/cpp_proxy` instead. No access key, login, or extra startup setting is needed. Other adapters are not bound automatically.

The dashboard includes a **Proxy configuration** panel showing the listening IP addresses, proxy port, local dashboard URL, and device setup instructions. eth2 addresses are discovered at startup; restart after DHCP changes. If eth2 is unavailable, startup prints a notice and the proxy remains usable locally. If a selected address/port cannot be bound, startup fails instead of silently presenting a partially available service.

### Access from a phone or another computer

Use the eth2 IP displayed in the dashboard as the device proxy server, with port 8080. Devices must be able to reach that interface; Wi-Fi client isolation can prevent access even on the same network. Connected clients also have access to the shared rule controls.

1. Connect to a network that can reach the configured proxy interface.
2. Configure the device/browser's HTTP proxy using Kali's reachable IP address and port **8080**.
3. Open **http://proxy.internal** in that browser, explicitly including `http://`.

The proxy recognizes this exact reserved hostname and serves the dashboard itself. It does not resolve the name through DNS or fetch it from the internet. Page assets, live statistics, and rule edits all use the same proxy connection. No hosts-file entry, separate dashboard port exposure, or SSH tunnel is needed for a device that can already reach the proxy.

The browser must actually use the configured proxy. HTTPS-only upgrades, apps that ignore Wi-Fi proxy settings, or bypass rules for this hostname will prevent access. This is an HTTP dashboard; `https://proxy.internal` is not supported. A phone must be able to reach the VM's network address: a host-only VM adapter is generally accessible only from the host computer, so use an appropriate reachable VM network for phone access.

On Kali itself, open **http://127.0.0.1:8081** directly, with no browser proxy configuration. This is the easiest presentation view. `http://proxy.internal` still requires using the proxy.

If the presentation browser runs on the Windows host rather than inside Kali, localhost refers to Windows. Forward the dashboard once with `ssh -N -L 127.0.0.1:8081:127.0.0.1:8081 kali`, then open the same local URL in Windows. No browser proxy setting is needed for this direct dashboard view.

For Windows access when using an SSH tunnel, forward the proxy port:

```powershell
ssh -N -L 127.0.0.1:8080:127.0.0.1:8080 kali
```

Configure the Windows browser's proxy to `127.0.0.1:8080`, then open `http://proxy.internal`.

**Shared prototype:** anyone who can use this proxy can view the dashboard and modify the universal ruleset. There is no user isolation or authentication. Keep it on a trusted network, not a public internet-facing port. Ordinary cross-site forms and scripts are still prevented from changing rules through Host/Origin checks and a same-origin request header; that header is not a secret or a login mechanism.

## Configuration

```text
./build/cpp_proxy [port [workers [queue]]]
```

| Setting | Default | Meaning |
| --- | --- | --- |
| CLI port | 8080 | Proxy port, 1–65535. |
| CLI workers | 16 | Worker threads, 1–256. |
| CLI queue | 64 | Waiting connections, 1–4096. |
| `PROXY_BIND_ADDRESS` | Localhost + eth2 | Optional single-address override replacing automatic discovery. Normally leave unset. |
| `PROXY_DASHBOARD_PORT` | `8081` | Local dashboard port; 0 disables only the separate listener. The friendly address still works through the proxy. |
| `PROXY_STATE_DIR` | `data` | Persistent rule directory; relative to working directory unless absolute. |
| `PROXY_DETAILED_LOGS` | Off | Set to `1` for detailed console and disk logs. |
| `PROXY_CACHE_SIMULATE` | Off | Set to `1` to report cache eligibility without serving or coalescing cached responses. |

Environment variables remain optional overrides. If you previously exported `PROXY_BIND_ADDRESS`, unset it to restore automatic localhost + eth2 listeners. `PROXY_ADMIN_TOKEN` is no longer used. If 8081 is occupied, stop the older instance or override the local dashboard port. The friendly address uses port 80 in its URL, but all its traffic travels through the configured proxy port; no listener on port 80 is required.

Start from the same directory each time so the default `data/privacy.rules` location stays consistent. Starting from `build/` and the repository root uses different state files unless `PROXY_STATE_DIR` is set to an absolute directory.

## Domain rules

Under **Domain rules**, enter a hostname such as `example.com` and choose **Always block** or **Allow exception**. Search and remove entries in the saved list. Advertising and tracker protection have separate toggles.

- Matching ignores case and an optional final dot.
- `example.com` includes its subdomains, but not `notexample.com`.
- Allow exceptions override every block rule, including parent-domain rules.
- Custom blocks take precedence over built-in categories for counting.
- URLs, paths, ports, IP addresses, and wildcards are rejected. Use punycode for international names. `localhost` is accepted for local testing.
- `proxy.internal` is reserved for management and cannot be added as a custom rule.
- There is a combined limit of 2,000 custom entries.
- Rules are shared by everyone using this proxy instance.
- Changes apply to new requests, before DNS, upstream connections, or cached responses. Existing CONNECT tunnels finish normally; reconnect to apply new rules.

Settings persist in `data/privacy.rules`. Saves use a temporary file and atomic rename. Failed saves leave the previous live policy unchanged. Newly created rule files are private to the operating-system user. Stop the proxy before manual edits. Invalid state files fail startup instead of silently losing rules. Separate instances should use separate state directories.

### Included starter list

This is a small, auditable starter list, not comprehensive ad-blocking coverage. Every included domain is visible in the dashboard.

| Category | Domains | Source |
| --- | --- | --- |
| Advertising | `doubleclick.net`, `googlesyndication.com`, `googleadservices.com` | [Google advertising technologies](https://policies.google.com/technologies/ads) |
| Tracking | `google-analytics.com` | [Google Analytics network domains](https://support.google.com/analytics/answer/12017362) |
| Tracking | `clarity.ms` | [Microsoft Clarity CSP documentation](https://learn.microsoft.com/en-us/clarity/setup-and-installation/clarity-csp) |

Rules are bundled rather than automatically downloaded. Add custom domains as needed. Blocking shared domains can break functionality; allow exceptions provide a way to recover.

## Dashboard metrics

| Metric | Meaning |
| --- | --- |
| Ads / trackers / custom blocks | Denied HTTP requests or CONNECT attempts, not individual ads or unique trackers. |
| Open or queued connections | Accepted connections that have not finished. |
| Completed successfully | Handled without a proxy processing/transport error. An origin HTTP error can still be successfully forwarded. |
| Failed connections | Timeouts, invalid requests, upstream errors, and queue rejections. Intentional blocks are separate. |
| Average origin connection | Successful DNS + TCP setup time this session. Excludes cache hits/blocks; not Wi-Fi strength, bandwidth, or page-load time. |
| Served from memory | Cached response bytes sent to clients, including headers, and cache/coalescing hit counts. Not measured total network savings. |

Counters reset on restart. Dashboard requests are excluded from browsing totals and never enter the response cache. The page refreshes every 2.5 seconds while visible and stores no browsing-history feed. “Proxy reachable” means the management endpoint responds, not that internet connectivity has been independently verified. A dash means there are no samples yet. No background internet probes are sent.

## Statistics by client IP

The **Devices** table splits browsing totals by the source IP observed on each accepted proxy socket. It shows active and total connections, completed connections, failures, blocks split into advertising/tracker/custom categories, finished-connection traffic I/O, and last activity. Dashboard requests are excluded. Direct dashboard viewers do not appear as proxy clients.

The table is a per-IP view, not authenticated device identification. NAT, SSH tunnels, or other shared source addresses combine multiple devices into one row; DHCP address changes create new rows. Blocking rules remain universal. Up to 256 source IPs are retained per process session, including temporary dashboard-only entries. Additional clients still work and contribute to global statistics; finished connections omitted from the IP split are counted below the table. Device data is in memory only and resets on restart.

All users with dashboard access can see the client IPs and aggregate counters. Destination URLs or browsing history are not listed. Traffic I/O is the sum of reads and writes on both sides of completed connections and can count the same payload twice. It is not download speed or unique data consumption; active tunnel byte totals appear when the tunnel finishes.

### Best-effort names

One background resolver thread attempts a network hostname once for each newly tracked IP. Loopback clients use the proxy machine's hostname. Other clients use `/usr/bin/getent hosts IP`, honoring the system's configured name services, with a 1.2-second process deadline. No shell is involved; helper output is validated, inherited sockets are closed, and timed-out helpers are killed and reaped. Missing `getent`, failed lookups, or timeouts display **Hostname unavailable**. Requests never wait for a lookup.

This lookup may issue reverse DNS/name-service queries through Kali's configured resolver. A returned name is a network label, not a verified owner, device model, or the friendly name shown in a device's settings. Many Wi-Fi networks do not publish usable client names. There is no network scan or attempt to bypass device privacy settings. Thread usage includes one additional resolver thread; worker and queue defaults remain 16 and 64.

## HTTP forwarding and caching

The handler checks policy, then cache eligibility or opens an upstream connection. Absolute request targets become origin-form paths, and the outgoing Host header is derived from the destination. Each client connection handles one HTTP request and response, using `Connection: close`.

The proxy removes incoming `Proxy-Authorization`, `Forwarded`, `X-Forwarded-For`, `X-Real-IP`, `Via`, and hop-by-hop request headers. It preserves application cookies and authorization. Duplicate request headers, chunked request bodies, invalid Content-Length, and Connection options that remove routing/framing headers are rejected.

On the third eligible GET for a full HTTP URL within five minutes, an eligible response can be cached. Concurrent popular misses wait up to 15 seconds for one fetch, falling back to forwarding when coalescing fails.

Only complete `200 OK` responses with Content-Length and explicit `Cache-Control: public, max-age=N` are stored. Requests with credentials, cookies, ranges, conditions, bodies, or cache-bypass directives are excluded. Responses containing private/no-store/no-cache, Set-Cookie, Vary, Transfer-Encoding, or Age are excluded. Unsafe methods invalidate the matching URL.

Freshness is capped at ten minutes. Entries expire after ten idle minutes, checked once per minute. Limits are 1 MiB per response, 16 MiB overall, and 4,096 tracked URLs. Least recently used entries are evicted when needed. Cache hits include Age. Cache memory is shared across users of the instance and cleared on restart. HTTPS contents cannot be cached.

## Concurrency, timeouts, and shutdown

Each worker handles successive HTTP connections or a bidirectional CONNECT tunnel using `poll()`. A tunnel occupies one worker throughout its lifetime. The larger default pool reduces queueing from simultaneous tunnels; it does not increase internet bandwidth or remove DNS latency. A full queue closes the new socket immediately and records a failure.

Timeouts are defined in `include/proxy/timeouts.hpp`:

| Operation | Default |
| --- | --- |
| Complete client headers | 30-second absolute deadline |
| Upstream TCP connect | 10 seconds per candidate address; DNS is separate |
| HTTP upstream read | 60 seconds per idle read |
| HTTPS tunnel idle | 120 seconds, reset by traffic in either direction |
| Socket write | 60 seconds per blocked write |

Request-body reads retain the 30-second receive timeout. Active transfers can outlast individual waits. Before a response starts, errors return 400, 408, 502, or 504; policy blocks return 403. After a response/tunnel starts, failures close it without injecting another HTTP response.

SIGINT and SIGTERM request graceful shutdown. The accept loop checks the flag at least every 200 ms, including when another thread receives the signal. Queued sockets close and workers finish active connections before joining. Active tunnels can delay shutdown. A second signal of the same type uses the default handler for immediate termination. Dashboard and cache housekeeping threads are joined during shutdown.

## Logging and privacy boundaries

Default console logs retain operational events and counts but replace client addresses, destinations, and URL paths with `[redacted]`. No request log file is written. Existing logs from older runs are not deleted.

```bash
PROXY_DETAILED_LOGS=1 ./build/cpp_proxy
```

Detailed logging includes client addresses, destinations and HTTP paths, and appends to `logs/proxy.log`, with one rotated archive around the 1 MiB threshold. Treat these files as browsing data. CONNECT destination names can be logged; encrypted HTTPS paths/content remain unavailable.

Domain blocking does not provide anonymity or complete tracking prevention. First-party tracking, fingerprinting, direct-IP requests, domain aliases, and traffic bypassing the proxy are outside the filter. Clients can deliberately evade domain rules; this is not network-enforced access control. HTTPS multiplexing within an allowed tunnel cannot be inspected. There is no cosmetic ad removal, TLS interception, or DNS encryption. Destinations see the proxy host's outward IP, and DNS uses the host resolver.

The dashboard has no access key and is available through the proxy to all connected users. It validates Host/Origin, allows no CORS access, and uses only local assets. The forwarding port allows arbitrary destination ports, including private destinations. This project is intended for personal use and learning, not public multi-user hosting.

## Tests

```bash
python3 tests/test_devices.py build/cpp_proxy
python3 tests/test_listeners.py build/cpp_proxy
python3 tests/test_dashboard.py build/cpp_proxy
python3 tests/test_privacy.py build/cpp_proxy
python3 tests/test_worker_pool.py build/cpp_proxy
python3 tests/test_cache.py build/cpp_proxy
python3 tests/test_timeouts.py build/cpp_proxy
```

Tests use temporary directories and local fixtures; no root or external servers are required. The timeout suite takes a few minutes. Existing regression suites enable detailed logging to inspect events. The privacy suite covers API access checks, live rules, cache blocking, persistence, failed saves, header filtering, counters, minimal logs, and shutdown. The device suite uses two loopback source IPs to verify separate counters, blocking categories, active connections, failures, traffic, dashboard exclusion, and local names. The listener suite checks direct dashboard access, localhost/eth2 routing, reported configuration, and absence of listeners on unrelated interfaces. The dashboard suite also checks the friendly hostname and eight simultaneous tunnels with the default pool.

Manual blocking test, without contacting an external server:

```bash
curl --noproxy "" -i -x http://127.0.0.1:8080 http://doubleclick.net/
```

With advertising protection enabled and no allow exception, expect 403 and an increased ad-block count.

## Project layout

| Path | Responsibility |
| --- | --- |
| `src/main.cpp` | Configuration, startup and signals. |
| `src/proxy/` | Worker pool, handler, cache, policy and dashboard server. |
| `src/http/` | Request parsing. |
| `src/network/` | RAII sockets and connection setup. |
| `src/logging/` | Logs and aggregate statistics. |
| `include/` | C++ headers. |
| `web/` | Dashboard HTML, CSS and JavaScript. |
| `config/default_domains.txt` | Bundled domains and sources. |
| `tests/` | Integration and regression tests. |
| `data/` | Runtime settings, ignored by Git. |

Remaining limitations include one HTTP request per connection, incomplete HTTP/1.1 support, an IPv4-only listener, blocking DNS resolution, and no proxy authentication or rate limiting. Outbound connections support IPv4/IPv6.

## Author

Sanjay Viswaq
B.Tech Computer Science and Engineering, Cybersecurity Specialization

[Repository](https://github.com/sanjay3941/multithread-proxy)
