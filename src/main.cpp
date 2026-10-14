#include "proxy/proxy_server.hpp"
#include "proxy/dashboard.hpp"
#include "proxy/privacy.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <algorithm>
#include <vector>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

// Discover eth2 at startup; never expose the proxy on unrelated adapters.
std::vector<std::string> proxyAddresses() {
    if (const char* configured = std::getenv("PROXY_BIND_ADDRESS")) return {configured};
    std::vector<std::string> addresses{"127.0.0.1"};
    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) throw std::runtime_error("Cannot enumerate network interfaces.");
    for (auto* entry = interfaces; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
            std::string(entry->ifa_name) != "eth2" || !(entry->ifa_flags & IFF_UP)) continue;
        char text[INET_ADDRSTRLEN];
        const auto* address = reinterpret_cast<sockaddr_in*>(entry->ifa_addr);
        if (::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) &&
            std::find(addresses.begin(), addresses.end(), text) == addresses.end()) addresses.emplace_back(text);
    }
    ::freeifaddrs(interfaces);
    if (addresses.size() == 1) std::cerr << "eth2 has no active IPv4 address; proxy is available locally only.\n";
    return addresses;
}

bool parseNumber(const char* text, std::size_t maximum, std::size_t& value) {
    const std::string_view input(text);
    if (input.empty()) return false;
    const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
    return result.ec == std::errc{} && result.ptr == input.data() + input.size() &&
           value >= 1 && value <= maximum;
}

// ---------------------------------------------------------------------------
// SIGINT handling
//
// g_shutdown is written only from the signal handler and read only from the
// accept loop in ProxyServer::start().  volatile sig_atomic_t satisfies the
// POSIX requirement for signal-handler-safe shared state.
// ---------------------------------------------------------------------------

volatile sig_atomic_t g_shutdown = 0;

void handleSigint(int signal) noexcept {
    g_shutdown = 1;
    // Restore the default handler so that a second Ctrl+C kills the process
    // immediately rather than re-entering this handler during shutdown.
    // signal() is async-signal-safe per POSIX.
    std::signal(signal, SIG_DFL);
}

} // namespace

int main(int argc, char* argv[]) {
    // Ignore SIGPIPE so that writes to a closed socket throw instead of
    // terminating the process.
    std::signal(SIGPIPE, SIG_IGN);

    // The accept loop polls this flag even when a worker receives the signal.
    struct sigaction sa{};
    sa.sa_handler = handleSigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   // no SA_RESTART
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::size_t configuredPort = 8080;
    std::size_t workerCount    = proxy::ProxyServer::kDefaultWorkerCount;
    std::size_t queueSize      = proxy::ProxyServer::kDefaultQueueSize;
    if (argc > 4 ||
        (argc > 1 && !parseNumber(argv[1], 65535, configuredPort)) ||
        (argc > 2 && !parseNumber(argv[2],   256, workerCount))    ||
        (argc > 3 && !parseNumber(argv[3],  4096, queueSize))) {
        std::cerr << "Usage: " << argv[0] << " [port [workers [queue]]]\n";
        return 2;
    }

    try {
        proxy::Privacy::instance();
        const auto bindAddresses = proxyAddresses();
        std::size_t dashboardPort = 8081;
        const char* configuredDashboard = std::getenv("PROXY_DASHBOARD_PORT");
        if (configuredDashboard && std::string(configuredDashboard) == "0") dashboardPort = 0;
        if (configuredDashboard && std::string(configuredDashboard) != "0" &&
            !parseNumber(configuredDashboard, 65535, dashboardPort))
            throw std::invalid_argument("PROXY_DASHBOARD_PORT must be 0 (disabled) or 1-65535.");
        proxy::Dashboard dashboard(static_cast<std::uint16_t>(dashboardPort),
                                   static_cast<std::uint16_t>(configuredPort), bindAddresses);
        std::cout << "Proxy Server dashboard: http://proxy.internal (through this proxy)" << std::endl;
        if (dashboardPort) std::cout << "Local dashboard: " << dashboard.url() << std::endl;
        proxy::ProxyServer server(static_cast<std::uint16_t>(configuredPort),
                                  bindAddresses, workerCount, queueSize);
        // Pass the shutdown flag so start() can log a human-readable message
        // and distinguish a graceful shutdown from a real accept() failure.
        server.start(&g_shutdown);
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
    // ProxyServer and Dashboard destructors have stopped and joined their threads.
    // Static destructors run after main() returns: Cache::~Cache() joins the
    // housekeeping janitor thread.
}
