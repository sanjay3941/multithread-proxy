#include "proxy/proxy_server.hpp"

#include "proxy/client_handler.hpp"

#include <algorithm>
#include <iostream>

namespace proxy {

ProxyServer::ProxyServer(std::uint16_t port, std::string bindAddress)
    : port_(port), bindAddress_(std::move(bindAddress)) {}

ProxyServer::~ProxyServer() {
    listener_.close();
    std::lock_guard<std::mutex> lock(workersMutex_);
    for (auto& worker : workers_) {
        if (worker.thread.joinable()) worker.thread.join();
    }
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
    std::cout << "Proxy listening on " << bindAddress_ << ':' << port_ << '\n';
    for (;;) {
        try {
            auto client = listener_.accept();
            auto done = std::make_shared<std::atomic_bool>(false);
            Worker worker{
                std::thread([connection = std::move(client), done]() mutable {
                ClientHandler(std::move(connection)).run();
                done->store(true);
            }),
                done};
            {
                std::lock_guard<std::mutex> lock(workersMutex_);
                workers_.push_back(std::move(worker));
                reapFinishedWorkers();
            }
        } catch (const std::exception& error) {
            std::cerr << "accept loop: " << error.what() << '\n';
        }
    }
}

} // namespace proxy
