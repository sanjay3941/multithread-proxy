#include "network/socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace network {
namespace {

std::runtime_error systemError(const char* operation) {
    return std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

} // namespace

Socket::Socket(int descriptor) noexcept : descriptor_(descriptor) {}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : descriptor_(other.descriptor_) {
    other.descriptor_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = other.descriptor_;
        other.descriptor_ = -1;
    }
    return *this;
}

Socket Socket::createTcp() {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        throw systemError("socket");
    }
    return Socket(descriptor);
}

Socket Socket::connectTo(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const int result = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
    if (result != 0) {
        throw std::runtime_error("getaddrinfo: " + std::string(gai_strerror(result)));
    }

    int connected = -1;
    int lastError = ECONNREFUSED;
    for (addrinfo* address = results; address != nullptr; address = address->ai_next) {
        connected = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (connected < 0) {
            lastError = errno;
            continue;
        }
        if (::connect(connected, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        lastError = errno;
        ::close(connected);
        connected = -1;
    }
    ::freeaddrinfo(results);
    if (connected < 0) {
        errno = lastError;
        throw systemError("connect");
    }
    return Socket(connected);
}

void Socket::bindAndListen(const std::string& host, std::uint16_t port, int backlog) {
    if (descriptor_ < 0) {
        throw std::logic_error("bindAndListen on closed socket");
    }
    int reuse = 1;
    if (::setsockopt(descriptor_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        throw systemError("setsockopt");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("listen host must be an IPv4 address");
    }
    if (::bind(descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        throw systemError("bind");
    }
    if (::listen(descriptor_, backlog) < 0) {
        throw systemError("listen");
    }
}

Socket Socket::accept() const {
    const int client = ::accept(descriptor_, nullptr, nullptr);
    if (client < 0) {
        throw systemError("accept");
    }
    return Socket(client);
}

std::size_t Socket::sendAll(const void* data, std::size_t length) const {
    const auto* bytes = static_cast<const char*>(data);
    std::size_t sent = 0;
    while (sent < length) {
        const ssize_t count = ::send(descriptor_, bytes + sent, length - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw systemError("send");
        }
        sent += static_cast<std::size_t>(count);
    }
    return sent;
}

ssize_t Socket::receive(void* buffer, std::size_t length) const {
    ssize_t count;
    do {
        count = ::recv(descriptor_, buffer, length, 0);
    } while (count < 0 && errno == EINTR);
    return count;
}

void Socket::shutdown() {
    if (descriptor_ >= 0) {
        ::shutdown(descriptor_, SHUT_RDWR);
    }
}

void Socket::close() noexcept {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}

bool Socket::isOpen() const noexcept {
    return descriptor_ >= 0;
}

int Socket::nativeHandle() const noexcept {
    return descriptor_;
}

} // namespace network
