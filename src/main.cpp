#include "proxy/proxy_server.hpp"

#include <charconv>
#include <csignal>
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

// ---------------------------------------------------------------------------
// SIGINT handling
//
// g_shutdown is written only from the signal handler and read only from the
// accept loop in ProxyServer::start().  volatile sig_atomic_t satisfies the
// POSIX requirement for signal-handler-safe shared state.
// ---------------------------------------------------------------------------

volatile sig_atomic_t g_shutdown = 0;

void handleSigint(int) noexcept {
    g_shutdown = 1;
    // Restore the default handler so that a second Ctrl+C kills the process
    // immediately rather than re-entering this handler during shutdown.
    // signal() is async-signal-safe per POSIX.
    std::signal(SIGINT, SIG_DFL);
}

} // namespace

int main(int argc, char* argv[]) {
    // Ignore SIGPIPE so that writes to a closed socket throw instead of
    // terminating the process.
    std::signal(SIGPIPE, SIG_IGN);

    // Install SIGINT handler via sigaction.  SA_RESTART is deliberately NOT
    // set: a signal delivered while accept() is blocking causes EINTR rather
    // than an automatic restart, so the accept loop exits promptly and the
    // existing ProxyServer::stop() path runs in full.
    struct sigaction sa{};
    sa.sa_handler = handleSigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   // no SA_RESTART
    sigaction(SIGINT, &sa, nullptr);

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
        proxy::ProxyServer server(static_cast<std::uint16_t>(configuredPort),
                                  "0.0.0.0", workerCount, queueSize);
        // Pass the shutdown flag so start() can log a human-readable message
        // and distinguish a graceful shutdown from a real accept() failure.
        server.start(&g_shutdown);
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
    // ProxyServer destructor runs here: stop(), drain(), join, print stats.
    // Static destructors run after main() returns: Cache::~Cache() joins the
    // housekeeping janitor thread.
}
