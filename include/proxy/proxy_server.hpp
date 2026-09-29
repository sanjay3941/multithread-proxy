#pragma once

#include "network/socket.hpp"
#include "proxy/work_queue.hpp"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace proxy {

/// HTTP/HTTPS forward proxy backed by a fixed-size worker thread pool.
///
/// A bounded queue decouples the accept loop from the workers.  When the
/// queue is full the new connection is logged and closed immediately so
/// the accept loop never blocks.  Workers are started in the constructor
/// and joined in the destructor; active connections are allowed to finish
/// naturally before the join.
class ProxyServer {
public:
    static constexpr std::size_t kDefaultWorkerCount = 4;
    static constexpr std::size_t kDefaultQueueSize   = 16;

    /// \param port          TCP port to listen on.
    /// \param bindAddress   IPv4 address to bind (default "0.0.0.0").
    /// \param workerCount   Fixed number of worker threads (default 4).
    /// \param queueSize     Maximum pending connections in the queue (default 16).
    explicit ProxyServer(std::uint16_t port,
                         std::string   bindAddress = "0.0.0.0",
                         std::size_t   workerCount = kDefaultWorkerCount,
                         std::size_t   queueSize   = kDefaultQueueSize);
    ~ProxyServer();

    ProxyServer(const ProxyServer&)            = delete;
    ProxyServer& operator=(const ProxyServer&) = delete;

    /// Runs the accept loop.  Blocks until accept() throws (e.g. when the
    /// listener is closed or SIGINT interrupts the call with EINTR).
    ///
    /// \param shutdownFlag  Optional pointer to a volatile sig_atomic_t set by
    ///                      a signal handler.  When non-null and non-zero after
    ///                      an accept() failure, the failure is treated as a
    ///                      graceful shutdown request rather than an error.
    void start(const volatile sig_atomic_t* shutdownFlag = nullptr);
    void printStatistics() const;

private:
    void workerLoop();
    void stop();

    std::uint16_t            port_;
    std::string              bindAddress_;
    std::size_t              workerCount_;
    network::Socket          listener_;
    WorkQueue                queue_;
    std::vector<std::thread> workers_;
};

} // namespace proxy
