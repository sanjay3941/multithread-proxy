#include "proxy/proxy_server.hpp"

#include "proxy/client_handler.hpp"
#include "logging/logger.hpp"
#include "logging/statistics.hpp"

#include <algorithm>

namespace proxy {

ProxyServer::ProxyServer(std::uint16_t port, std::string bindAddress)
    : port_(port), bindAddress_(std::move(bindAddress)) {}

ProxyServer::~ProxyServer() {
    listener_.close();
    std::lock_guard<std::mutex> lock(workersMutex_);
    for (auto& worker : workers_) {
        if (worker.thread.joinable()) worker.thread.join();
    }
    printStatistics();
}

void ProxyServer::printStatistics() const {
    logging::Logger::instance().log(logging::LogLevel::INFO,
                                    logging::ProxyStatistics::instance().summary());
}

void ProxyServer::reapFinishedWorkers() {
    auto worker = workers_.begin();
    while (worker != workers_.end()) {
        if (worker->done->load()) {
            if (worker->thread.joinable()) worker->thread.join();
            worker = workers_.erase(worker);
        } else {
            ++worker;
        }
    }
}

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
            auto done = std::make_shared<std::atomic_bool>(false);
            Worker worker{
                std::thread([connection = std::move(client), stats, done]() mutable {
                ClientHandler(std::move(connection), stats).run();
                done->store(true);
            }),
                done};
            {
                std::lock_guard<std::mutex> lock(workersMutex_);
                workers_.push_back(std::move(worker));
                reapFinishedWorkers();
            }
        } catch (const std::exception& error) {
            logging::Logger::instance().log(logging::LogLevel::ERROR,
                                            std::string("ACCEPT_FAILURE ERROR=") + error.what());
        }
    }
}

} // namespace proxy
