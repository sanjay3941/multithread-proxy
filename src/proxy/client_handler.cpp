#include "proxy/client_handler.hpp"

#include "http/http_parser.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace proxy {
namespace {

constexpr std::size_t bufferSize = 16 * 1024;

std::pair<std::string, std::string> readHeaders(network::Socket& socket) {
    std::string data;
    std::vector<char> buffer(bufferSize);
    while (data.find("\r\n\r\n") == std::string::npos) {
        const auto count = socket.receive(buffer.data(), buffer.size());
        if (count == 0) throw std::runtime_error("client disconnected before request");
        if (count < 0) throw std::runtime_error("client receive failed");
        data.append(buffer.data(), static_cast<std::size_t>(count));
        if (data.size() > 1024 * 1024) throw std::runtime_error("HTTP headers too large");
    }
    const auto end = data.find("\r\n\r\n") + 4;
    return {data.substr(0, end), data.substr(end)};
}

void relay(network::Socket& source, network::Socket& destination) {
    std::vector<char> buffer(bufferSize);
    for (;;) {
        const auto count = source.receive(buffer.data(), buffer.size());
        if (count <= 0) break;
        try {
            destination.sendAll(buffer.data(), static_cast<std::size_t>(count));
        } catch (const std::exception&) {
            break;
        }
    }
    destination.shutdown();
}

} // namespace

ClientHandler::ClientHandler(network::Socket client) : client_(std::move(client)) {}

void ClientHandler::run() noexcept {
    try {
        const auto request = readHeaders(client_);
        const auto parsed = http::HttpParser::parseRequest(request.first);
        if (parsed.method == "CONNECT") {
            handleConnect(request.first, request.second);
        } else {
            handleHttp(request.first, request.second);
        }
    } catch (const std::exception& error) {
        std::cerr << "client handler: " << error.what() << '\n';
        try {
            const std::string response = "HTTP/1.1 400 Bad Request\r\n"
                                         "Connection: close\r\nContent-Length: 0\r\n\r\n";
            client_.sendAll(response.data(), response.size());
        } catch (...) {
        }
    }
}

void ClientHandler::handleHttp(const std::string& requestHead, const std::string& buffered) {
    const auto parsed = http::HttpParser::parseRequest(requestHead);
    auto remote = network::Socket::connectTo(parsed.host, parsed.port);

    std::istringstream input(requestHead);
    std::string line;
    std::getline(input, line);
    std::ostringstream outgoing;
    outgoing << parsed.method << ' ' << parsed.path << ' ' << parsed.version << "\r\n";
    while (std::getline(input, line)) {
        if (line == "\r" || line.empty()) break;
        const auto separator = line.find(':');
        if (separator == std::string::npos) continue;
        const auto name = line.substr(0, separator);
        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerName == "proxy-connection" || lowerName == "connection") continue;
        outgoing << line << "\r\n";
    }
    outgoing << "Connection: close\r\n\r\n";
    const auto serialized = outgoing.str();
    remote.sendAll(serialized.data(), serialized.size());

    const auto contentLength = parsed.headers.find("content-length");
    std::size_t remaining = contentLength == parsed.headers.end()
                                ? 0
                                : static_cast<std::size_t>(std::stoull(contentLength->second));
    if (!buffered.empty()) {
        const auto initial = std::min(remaining, buffered.size());
        remote.sendAll(buffered.data(), initial);
        remaining -= initial;
    }
    std::vector<char> buffer(bufferSize);
    while (remaining > 0) {
        const auto count = client_.receive(buffer.data(), std::min(buffer.size(), remaining));
        if (count <= 0) throw std::runtime_error("client disconnected during request body");
        remote.sendAll(buffer.data(), static_cast<std::size_t>(count));
        remaining -= static_cast<std::size_t>(count);
    }
    relay(remote, client_);
}

void ClientHandler::handleConnect(const std::string& requestHead, const std::string& buffered) {
    const auto parsed = http::HttpParser::parseRequest(requestHead);
    auto remote = network::Socket::connectTo(parsed.host, parsed.port);
    const std::string established = "HTTP/1.1 200 Connection Established\r\n\r\n";
    client_.sendAll(established.data(), established.size());
    if (!buffered.empty()) {
        remote.sendAll(buffered.data(), buffered.size());
    }

    std::thread clientToServer([this, &remote] { relay(client_, remote); });
    std::thread serverToClient([this, &remote] { relay(remote, client_); });
    clientToServer.join();
    remote.shutdown();
    serverToClient.join();
}

} // namespace proxy
