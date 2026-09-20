#pragma once

#include "network/socket.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace proxy {

class ProxyServer {
public:
    explicit ProxyServer(std::uint16_t port, std::string bindAddress = "0.0.0.0");
    ~ProxyServer();

    ProxyServer(const ProxyServer&) = delete;
    ProxyServer& operator=(const ProxyServer&) = delete;
    void start();
    void printStatistics() const;

private:
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic_bool> done;
    };

    void reapFinishedWorkers();
    std::uint16_t port_;
    std::string bindAddress_;
    network::Socket listener_;
    std::mutex workersMutex_;
    std::vector<Worker> workers_;
};

} // namespace proxy
