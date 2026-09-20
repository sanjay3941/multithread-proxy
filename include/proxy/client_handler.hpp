#pragma once

#include "network/socket.hpp"
#include "logging/statistics.hpp"
#include "http/http_parser.hpp"

#include <memory>
#include <string>

namespace proxy {

class ClientHandler {
public:
    ClientHandler(network::Socket client, std::shared_ptr<logging::ConnectionStats> stats);
    void run() noexcept;

private:
    void handleHttp(const std::string& requestHead, const std::string& buffered,
                    const http::HttpRequest& parsed);
    void handleConnect(const std::string& requestHead, const std::string& buffered,
                       const http::HttpRequest& parsed);
    network::Socket client_;
    std::shared_ptr<logging::ConnectionStats> stats_;
};

} // namespace proxy
