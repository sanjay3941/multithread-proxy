#include "proxy/proxy_server.hpp"

#include "proxy/client_handler.hpp"
#include "logging/logger.hpp"
#include "logging/statistics.hpp"
#include <cerrno>
#include <fcntl.h>
#include <poll.h>

namespace proxy {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

ProxyServer::ProxyServer(std::uint16_t port, std::vector<std::string> bindAddresses,
                         std::size_t workerCount, std::size_t queueSize)
    : port_(port)
    , bindAddresses_(std::move(bindAddresses))
    , workerCount_(workerCount)
    , queue_(queueSize) {
    workers_.reserve(workerCount_);
    for (std::size_t i = 0; i < workerCount_; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "POOL_STARTED WORKERS=" + std::to_string(workerCount_) +
        " QUEUE_CAPACITY=" + std::to_string(queueSize));
}

ProxyServer::~ProxyServer() {
    stop();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

// stop() is called from the destructor after start() has returned.
// Sequence:
//   1. Close the listener so that any concurrent accept() fails immediately.
//   2. Signal the queue so that idle workers wake and exit their loop.
//   3. Drain the queue: close every socket that was waiting but not yet served.
//   4. Join all workers.  Workers that are mid-ClientHandler::run() will finish
//      their current connection (bounded by the configured timeouts) before
//      joining, so active connections are not forcibly interrupted.
//   5. Print aggregate statistics.
//
// After stop() returns, main() returns and static destructors run.
// Cache::~Cache() is called during static destruction; it sets stopping_,
// signals the housekeeping CV, and joins the janitor thread.
void ProxyServer::stop() {
    listeners_.clear();
    queue_.stop();
    queue_.drain();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    printStatistics();
}

// ---------------------------------------------------------------------------
// Worker loop
// ---------------------------------------------------------------------------

// Each worker blocks on queue_.pop().  When a WorkItem is available the
// worker constructs a ClientHandler on its stack and calls run().
// The same OS thread handles successive connections — worker reuse is
// visible in the logs through repeated THREAD= values across different
// REQ-* entries.
//
// HTTPS CONNECT forwards both directions with poll() in this worker.
// A tunnel occupies one worker until the connection closes or the idle timeout fires.
void ProxyServer::workerLoop() {
    WorkItem item;
    while (queue_.pop(item)) {
        ClientHandler(std::move(item.socket), item.stats).run();
    }
}

// ---------------------------------------------------------------------------
// Accept loop
// ---------------------------------------------------------------------------

// Poll periodically so shutdown does not depend on which thread receives
// SIGINT or SIGTERM. Nonblocking accept also handles readiness races.
void ProxyServer::start(const volatile sig_atomic_t* shutdownFlag) {
    std::vector<pollfd> ready;
    for (const auto& address : bindAddresses_) {
        auto listener = network::Socket::createTcp();
        listener.bindAndListen(address, port_);
        const int flags = ::fcntl(listener.nativeHandle(), F_GETFL, 0);
        if (flags < 0 || ::fcntl(listener.nativeHandle(), F_SETFL, flags | O_NONBLOCK) < 0)
            throw std::runtime_error("Unable to configure listener.");
        ready.push_back({listener.nativeHandle(), POLLIN, 0});
        listeners_.push_back(std::move(listener));
    }
    for (const auto& address : bindAddresses_)
        logging::Logger::instance().log(logging::LogLevel::INFO,
            "PROXY_LISTENING ADDRESS=" + address + ":" + std::to_string(port_));

    for (;;) {
        if (shutdownFlag && *shutdownFlag) {
            logging::Logger::instance().log(logging::LogLevel::INFO,
                "Shutdown requested, initiating graceful shutdown");
            break;
        }
        const int available = ::poll(ready.data(), ready.size(), 200);
        if (available == 0 || (available < 0 && errno == EINTR)) continue;
        if (available < 0) throw std::runtime_error("Listener poll failed.");
        for (std::size_t i = 0; i < ready.size(); ++i) {
            if (!(ready[i].revents & POLLIN)) continue;
            try {
                auto client = listeners_[i].accept();
                const auto peer = client.peerAddress();
                auto stats = logging::ProxyStatistics::instance().begin(peer.first, peer.second);
                logging::Logger::instance().log(logging::LogLevel::INFO,
                    "[" + stats->connectionId + "] CLIENT=" + peer.first + ":" +
                    std::to_string(peer.second) + " STATUS=ACCEPTED");

                // Attempt to hand off to a worker via the queue.
                // push() takes the WorkItem by value; if it returns false the
                // local WorkItem destructs here, closing the client socket.
                if (!queue_.push(WorkItem{std::move(client), stats})) {
                    logging::Logger::instance().log(logging::LogLevel::WARN,
                        "[" + stats->connectionId + "] CLIENT=" + peer.first + ":" +
                        std::to_string(peer.second) +
                        " STATUS=REJECTED REASON=QUEUE_FULL");
                    logging::ProxyStatistics::instance().finish(stats, /*successful=*/false);
                }
            } catch (const std::exception&) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                throw;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void ProxyServer::printStatistics() const {
    logging::Logger::instance().log(logging::LogLevel::INFO,
        logging::ProxyStatistics::instance().summary());
}

} // namespace proxy
