#include "proxy/proxy_server.hpp"

#include <csignal>
#include <charconv>
#include <iostream>
#include <string_view>

namespace {

bool parseNumber(const char* text, std::size_t maximum, std::size_t& value) {
    const std::string_view input(text);
    if (input.empty()) return false;
    const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
    return result.ec == std::errc{} && result.ptr == input.data() + input.size() &&
           value >= 1 && value <= maximum;
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

    std::size_t configuredPort = 8080;
    std::size_t workerCount = proxy::ProxyServer::kDefaultWorkerCount;
    std::size_t queueSize = proxy::ProxyServer::kDefaultQueueSize;
    if (argc > 4 || (argc > 1 && !parseNumber(argv[1], 65535, configuredPort)) ||
        (argc > 2 && !parseNumber(argv[2], 256, workerCount)) ||
        (argc > 3 && !parseNumber(argv[3], 4096, queueSize))) {
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
