#pragma once

#include "network/socket.hpp"

#include <string>

namespace proxy {

class ClientHandler {
public:
    explicit ClientHandler(network::Socket client);
    void run() noexcept;

private:
    void handleHttp(const std::string& requestHead, const std::string& buffered);
    void handleConnect(const std::string& requestHead, const std::string& buffered);
    network::Socket client_;
};

} // namespace proxy
