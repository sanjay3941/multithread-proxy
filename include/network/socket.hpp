#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <string>
#include <utility>

namespace network {

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
    static Socket connectTo(const std::string& host, const std::string& port);

    void bindAndListen(const std::string& host, std::uint16_t port,
                       int backlog = 128);
    Socket accept() const;
    std::pair<std::string, std::uint16_t> peerAddress() const;

    std::size_t sendAll(const void* data, std::size_t length) const;
    ssize_t receive(void* buffer, std::size_t length) const;
    void shutdown();
    void close() noexcept;
    bool isOpen() const noexcept;
    int nativeHandle() const noexcept;

private:
    int descriptor_ = -1;
};

} // namespace network
