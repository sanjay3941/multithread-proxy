#include "proxy/proxy_server.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

    const int configuredPort = argc > 1 ? std::atoi(argv[1]) : 8080;
    if (configuredPort < 1 || configuredPort > 65535) {
        std::cerr << "Usage: " << argv[0] << " [port [workers [queue]]]\n";
        return 2;
    }

    const auto workerCount = static_cast<std::size_t>(
        argc > 2 ? std::atoi(argv[2]) : static_cast<int>(proxy::ProxyServer::kDefaultWorkerCount));
    const auto queueSize = static_cast<std::size_t>(
        argc > 3 ? std::atoi(argv[3]) : static_cast<int>(proxy::ProxyServer::kDefaultQueueSize));

    if (workerCount < 1 || queueSize < 1) {
        std::cerr << "Usage: " << argv[0] << " [port [workers [queue]]]\n";
        return 2;
    }

    try {
        proxy::ProxyServer server(static_cast<std::uint16_t>(configuredPort),
                                  "0.0.0.0", workerCount, queueSize);
        server.start();
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
