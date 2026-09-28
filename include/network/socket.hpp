#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <string>
#include <stdexcept>
#include <utility>

namespace network {

class TimeoutError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(int descriptor) noexcept;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    static Socket createTcp();
    /// Connects to host:port. If connectTimeoutSeconds > 0, the TCP handshake
    /// is bounded by that many seconds; throws on timeout or refusal.
    static Socket connectTo(const std::string& host, const std::string& port,
                            int connectTimeoutSeconds = 0);

    void bindAndListen(const std::string& host, std::uint16_t port,
                       int backlog = 128);
    Socket accept() const;
    std::pair<std::string, std::uint16_t> peerAddress() const;

    std::size_t sendAll(const void* data, std::size_t length) const;
    ssize_t receive(void* buffer, std::size_t length) const;

    /// Sets SO_RCVTIMEO: receive() will return -1/EAGAIN after this many seconds.
    void setReceiveTimeout(int seconds);
    /// Sets SO_SNDTIMEO: sendAll() will throw after this many seconds of blocking.
    void setSendTimeout(int seconds);

    void shutdown();
    void close() noexcept;
    bool isOpen() const noexcept;
    int nativeHandle() const noexcept;

private:
    int descriptor_ = -1;
};

} // namespace network
