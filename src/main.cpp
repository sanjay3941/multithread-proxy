#include "proxy/proxy_server.hpp"

#include <csignal>
#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    const int configuredPort = argc > 1 ? std::atoi(argv[1]) : 8080;
    if (configuredPort < 1 || configuredPort > 65535) {
        std::cerr << "Usage: " << argv[0] << " [port]\n";
        return 2;
    }
    try {
        proxy::ProxyServer server(static_cast<std::uint16_t>(configuredPort));
        server.start();
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
