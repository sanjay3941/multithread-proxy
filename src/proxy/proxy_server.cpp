#include "proxy/proxy_server.hpp"

#include "proxy/client_handler.hpp"
#include "logging/logger.hpp"
#include "logging/statistics.hpp"

namespace proxy {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

ProxyServer::ProxyServer(std::uint16_t port, std::string bindAddress,
                         std::size_t workerCount, std::size_t queueSize)
    : port_(port)
    , bindAddress_(std::move(bindAddress))
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
void ProxyServer::stop() {
    listener_.close();
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
// HTTPS CONNECT note: handleConnect() spawns two relay threads internally
// and blocks the worker until both join (i.e. until the tunnel closes).
// A single HTTPS session therefore occupies one worker for its entire
// lifetime.  This is unchanged from the previous design.
void ProxyServer::workerLoop() {
    WorkItem item;
    while (queue_.pop(item)) {
        ClientHandler(std::move(item.socket), item.stats).run();
    }
}

// ---------------------------------------------------------------------------
// Accept loop
// ---------------------------------------------------------------------------

void ProxyServer::start() {
    listener_ = network::Socket::createTcp();
    listener_.bindAndListen(bindAddress_, port_);
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "PROXY_LISTENING ADDRESS=" + bindAddress_ + ":" + std::to_string(port_));

    for (;;) {
        try {
            auto client = listener_.accept();
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
        } catch (const std::exception& error) {
            // accept() throws when the listener fd is closed (during stop())
            // or on a transient OS error.  Either way we exit the loop.
            logging::Logger::instance().log(logging::LogLevel::ERROR,
                std::string("ACCEPT_FAILURE ERROR=") + error.what());
            break;
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
