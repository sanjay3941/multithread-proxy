#pragma once
#include "network/socket.hpp"
#include "http/http_parser.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace proxy {
class Dashboard {
public:
    Dashboard(std::uint16_t port, std::uint16_t proxyPort, std::vector<std::string> bindAddresses);
    ~Dashboard();
    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;
    std::string url() const;
    // The instance outlives ProxyServer and its workers.
    static bool serveProxy(network::Socket& client, const http::HttpRequest& parsed,
                           const std::string& head, const std::string& buffered);
private:
    static Dashboard* active_;
    void handle(network::Socket& client, std::string request = {});
    void run();
    void serve(network::Socket& client, std::string request);
    std::uint16_t port_, proxyPort_;
    std::vector<std::string> bindAddresses_;
    network::Socket listener_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
} // namespace proxy
