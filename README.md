# Multithreaded HTTP/HTTPS Forward Proxy

A multithreaded HTTP/HTTPS forward proxy server implemented in C++17 using POSIX TCP sockets.

The proxy accepts client connections, processes HTTP requests, forwards traffic to destination servers, and supports HTTPS through the HTTP `CONNECT` tunneling mechanism.

The project was developed and tested on Linux/Kali Linux and has been successfully tested with both local clients and an external smartphone connected through the same network.

## Features

- HTTP forward proxy
- HTTPS CONNECT tunneling
- Multithreaded client handling using `std::thread`
- POSIX TCP socket programming
- Custom RAII-based socket wrapper
- HTTP request parsing
- HTTP header processing
- HTTP request forwarding
- HTTP response forwarding
- HTTPS bidirectional TCP forwarding
- IPv4 support
- External device support
- Client and upstream connection error handling
- CMake-based build system
- TLS traffic forwarding without decryption

## How It Works

The proxy listens for incoming TCP connections on a configurable port.

When a client connects, the server creates a dedicated worker thread to handle that connection.

For HTTP requests, the proxy:

1. Accepts the client connection.
2. Reads the HTTP request.
3. Parses the request method, host, port, path, HTTP version, and headers.
4. Converts proxy-style absolute URLs into origin-form requests when required.
5. Removes proxy-specific headers.
6. Establishes a connection with the destination server.
7. Forwards the request to the destination server.
8. Receives the response.
9. Sends the response back to the client.
10. Closes the connection.

For HTTPS requests, the client first sends an HTTP `CONNECT` request.

For example:

```http
CONNECT example.com:443 HTTP/1.1
Host: example.com:443
```

The proxy establishes a TCP connection to the requested destination and responds with:

```http
HTTP/1.1 200 Connection Established
```

After this, the proxy forwards encrypted TLS traffic between the client and destination server.

The proxy does not decrypt or inspect the HTTPS application data.

## Multithreading

The proxy uses a thread-per-client architecture.

Each accepted client connection is handled by a separate `std::thread`.

HTTPS connections additionally use separate forwarding threads for bidirectional data transfer between the client and destination server.

The implementation avoids sharing request state between client workers, allowing multiple connections to be processed concurrently.

Socket resources are managed using RAII to ensure that file descriptors are properly released.

## Project Structure

```text
multithread-proxy/
│
├── include/
│   ├── http/
│   │   └── http_parser.hpp
│   ├── network/
│   │   └── socket.hpp
│   └── proxy/
│       ├── client_handler.hpp
│       └── proxy_server.hpp
│
├── src/
│   ├── http/
│   │   └── http_parser.cpp
│   ├── network/
│   │   └── socket.cpp
│   ├── proxy/
│   │   ├── client_handler.cpp
│   │   └── proxy_server.cpp
│   └── main.cpp
│
├── CMakeLists.txt
├── .gitignore
└── README.md
```

## Requirements

The project currently targets Linux/POSIX environments.

Required:

- Linux
- C++17 compatible compiler
- CMake
- POSIX sockets
- pthread support

On Debian-based Linux distributions such as Kali Linux:

```bash
sudo apt update
sudo apt install g++ cmake
```

## Building

Clone the repository:

```bash
git clone git@github.com:sanjay3941/multithread-proxy.git
```

Enter the project directory:

```bash
cd multithread-proxy
```

Create a build directory:

```bash
mkdir build
cd build
```

Generate the build files:

```bash
cmake ..
```

Build the project:

```bash
make -j
```

The executable will be generated as:

```text
build/cpp_proxy
```

## Running

Run the proxy with the default port:

```bash
./cpp_proxy
```

The default listening port is:

```text
8080
```

A custom port can also be specified:

```bash
./cpp_proxy 3128
```

The proxy listens on:

```text
0.0.0.0:8080
```

This allows clients on the local network to connect to the proxy.

## Testing With curl

HTTPS traffic can be tested using:

```bash
curl -4 -v -x http://127.0.0.1:8080 https://google.com
```

A successful connection should show the `CONNECT` request:

```text
CONNECT google.com:443 HTTP/1.1
```

followed by:

```text
HTTP/1.1 200 Connection Established
```

The TLS handshake then occurs through the established tunnel.

## External Device Testing

The proxy can also be tested using a smartphone or another device connected to the same network.

Configure the device's manual proxy settings using the IP address of the Linux machine:

```text
Proxy Host: <Kali-IP>
Proxy Port: 8080
```

For example:

```text
Proxy Host: 10.12.57.110
Proxy Port: 8080
```

After configuring the proxy, HTTPS websites can be accessed through the C++ proxy.

Multiple browser connections can be observed simultaneously on the proxy server.

## Network Traffic Analysis

Wireshark can be used to inspect traffic between the external client and the proxy.

A useful display filter is:

```text
tcp.port == 8080
```

Traffic from a specific client can be filtered using:

```text
ip.addr == <client-ip>
```

For HTTPS connections, Wireshark can show information such as:

- Client and proxy IP addresses
- TCP connections
- TCP ports
- TCP packet flow
- TLS handshake
- TLS version
- TLS application data
- Server Name Indication (SNI), when available

The HTTPS application data remains encrypted because the proxy implements tunneling rather than TLS interception.

## Error Handling

The proxy returns appropriate HTTP error responses for common failures.

### 400 Bad Request

Returned when the client sends a malformed or invalid request that cannot be parsed or validated.

### 502 Bad Gateway

Returned when the proxy cannot establish or communicate with the upstream destination server.

## Socket Management

The project includes a custom `network::Socket` abstraction.

The socket wrapper follows RAII principles so that socket file descriptors are automatically closed when their owning objects are destroyed.

The socket abstraction is move-only, allowing ownership to be transferred without accidentally duplicating socket ownership.

## HTTP Processing

The HTTP parser extracts:

- Request method
- Host
- Port
- Path
- HTTP version
- Headers

Proxy-style requests such as:

```http
GET http://example.com/index.html HTTP/1.1
Host: example.com
```

are converted into origin-form requests before being forwarded:

```http
GET /index.html HTTP/1.1
Host: example.com
```

Proxy-specific headers are removed before forwarding the request upstream.

## HTTPS Processing

HTTPS is handled using the standard HTTP `CONNECT` mechanism.

The proxy establishes a TCP connection to the requested destination and then forwards data in both directions.

The proxy does not perform TLS interception or man-in-the-middle decryption.

Therefore, the proxy does not directly inspect:

- HTTPS page contents
- HTTP paths inside the TLS connection
- Cookies
- Passwords
- Encrypted application data

This implementation focuses on transparent HTTPS tunneling.

## Current Limitations

The current implementation has the following limitations:

- HTTP connections handle one request/response before closing.
- Chunked HTTP request-body forwarding is not implemented.
- IPv6 is not currently supported.
- No proxy authentication.
- No domain filtering.
- No caching.
- No rate limiting.
- No traffic dashboard.
- No TLS interception.
- No graceful signal-based shutdown.
- Thread-per-client architecture is not optimized for very large numbers of simultaneous connections.


## Technologies

- C++17
- POSIX TCP Sockets
- `std::thread`
- CMake
- Linux
- HTTP/1.1
- HTTPS CONNECT
- TLS
- Wireshark
- Git
- GitHub


## Repository

GitHub repository:

https://github.com/sanjay3941/multithread-proxy

## Author

**Sanjay Viswaq**

B.Tech Computer Science and Engineering  
Cybersecurity Specialization
