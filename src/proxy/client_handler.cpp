#include "proxy/client_handler.hpp"

#include "http/http_parser.hpp"
#include "logging/logger.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace proxy {
namespace {

constexpr std::size_t bufferSize = 16 * 1024;

std::pair<std::string, std::string> readHeaders(
    network::Socket& socket, const std::shared_ptr<logging::ConnectionStats>& stats) {
    std::string data;
    std::vector<char> buffer(bufferSize);
    while (data.find("\r\n\r\n") == std::string::npos) {
        const auto count = socket.receive(buffer.data(), buffer.size());
        if (count == 0) throw std::runtime_error("client disconnected before request");
        if (count < 0) throw std::runtime_error("client receive failed");
        data.append(buffer.data(), static_cast<std::size_t>(count));
        stats->bytesReceived.fetch_add(static_cast<std::uint64_t>(count));
        if (data.size() > 1024 * 1024) throw std::runtime_error("HTTP headers too large");
    }
    const auto end = data.find("\r\n\r\n") + 4;
    return {data.substr(0, end), data.substr(end)};
}

void relay(network::Socket& source, network::Socket& destination,
           const std::shared_ptr<logging::ConnectionStats>& stats, int* responseStatus = nullptr,
           const char* disconnectEvent = nullptr) {
    std::vector<char> buffer(bufferSize);
    for (;;) {
        const auto count = source.receive(buffer.data(), buffer.size());
        if (count <= 0) {
            if (disconnectEvent != nullptr) {
                logging::Logger::instance().log(logging::LogLevel::INFO,
                    "[" + stats->connectionId + "] STATUS=" + disconnectEvent);
            }
            break;
        }
        stats->bytesReceived.fetch_add(static_cast<std::uint64_t>(count));
        try {
            destination.sendAll(buffer.data(), static_cast<std::size_t>(count));
            stats->bytesSent.fetch_add(static_cast<std::uint64_t>(count));
            if (responseStatus != nullptr && *responseStatus == 0) {
                const std::string response(buffer.data(), static_cast<std::size_t>(count));
                const auto lineEnd = response.find("\r\n");
                if (lineEnd != std::string::npos) {
                    std::istringstream statusLine(response.substr(0, lineEnd));
                    std::string version;
                    statusLine >> version >> *responseStatus;
                }
            }
        } catch (const std::exception& error) {
            logging::Logger::instance().log(logging::LogLevel::ERROR,
                "[" + stats->connectionId + "] STATUS=FAILED ERROR=FORWARDING_ERROR MESSAGE=" +
                error.what());
            break;
        }
    }
    destination.shutdown();
}

} // namespace

ClientHandler::ClientHandler(network::Socket client,
                             std::shared_ptr<logging::ConnectionStats> stats)
    : client_(std::move(client)), stats_(std::move(stats)) {}

void ClientHandler::run() noexcept {
    bool successful = false;
    try {
        stats_->threadId = [] {
            std::ostringstream output;
            output << std::this_thread::get_id();
            return output.str();
        }();
        const auto request = readHeaders(client_, stats_);
        logging::Logger::instance().log(logging::LogLevel::INFO,
            "[" + stats_->connectionId + "] CLIENT=" + stats_->clientIp + ":" +
            std::to_string(stats_->clientPort) + " STATUS=REQUEST_RECEIVED");
        const auto parsed = http::HttpParser::parseRequest(request.first);
        stats_->method = parsed.method;
        stats_->targetHost = parsed.host;
        stats_->targetPort = parsed.port;
        stats_->protocol = parsed.method == "CONNECT" ? "HTTPS" : "HTTP";
        logging::Logger::instance().log(logging::LogLevel::INFO,
            "[" + stats_->connectionId + "] PROTOCOL=" + stats_->protocol +
            " METHOD=" + parsed.method + " TARGET=" + parsed.host + ":" + parsed.port +
            " PATH=" + parsed.path + " STATUS=PARSED");
        if (parsed.method == "CONNECT") {
            handleConnect(request.first, request.second, parsed);
        } else {
            handleHttp(request.first, request.second, parsed);
        }
        successful = true;
    } catch (const std::invalid_argument& error) {
        stats_->status = "FAILED";
        stats_->errorCode = "INVALID_REQUEST";
        stats_->errorMessage = error.what();
        stats_->httpStatus = 400;
        logging::Logger::instance().log(logging::LogLevel::ERROR,
            "[" + stats_->connectionId + "] PROTOCOL=" + stats_->protocol +
            " STATUS=FAILED HTTP_STATUS=400 ERROR=INVALID_REQUEST MESSAGE=" + error.what());
        try {
            const std::string response = "HTTP/1.1 400 Bad Request\r\n"
                                         "Connection: close\r\nContent-Length: 0\r\n\r\n";
            client_.sendAll(response.data(), response.size());
            stats_->bytesSent.fetch_add(response.size());
        } catch (...) {
        }
    } catch (const std::exception& error) {
        stats_->status = "FAILED";
        stats_->errorCode = "UPSTREAM_CONNECTION_FAILED";
        stats_->errorMessage = error.what();
        stats_->httpStatus = 502;
        logging::Logger::instance().log(logging::LogLevel::ERROR,
            "[" + stats_->connectionId + "] PROTOCOL=" + stats_->protocol +
            " STATUS=FAILED HTTP_STATUS=502 ERROR=UPSTREAM_CONNECTION_FAILED MESSAGE=" + error.what());
        try {
            const std::string response = "HTTP/1.1 502 Bad Gateway\r\n"
                                         "Connection: close\r\nContent-Length: 0\r\n\r\n";
            client_.sendAll(response.data(), response.size());
            stats_->bytesSent.fetch_add(response.size());
        } catch (...) {
        }
    }
    stats_->status = successful ? "COMPLETED" : "FAILED";
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stats_->startTime).count();
    logging::Logger::instance().log(successful ? logging::LogLevel::INFO : logging::LogLevel::ERROR,
        "[" + stats_->connectionId + "] PROTOCOL=" + stats_->protocol +
        " STATUS=" + stats_->status + " DURATION=" + std::to_string(duration / 1000.0) +
        "s BYTES_RECEIVED=" + std::to_string(stats_->bytesReceived.load()) +
        " BYTES_SENT=" + std::to_string(stats_->bytesSent.load()));
    logging::ProxyStatistics::instance().finish(stats_, successful);
}

void ClientHandler::handleHttp(const std::string& requestHead, const std::string& buffered,
                               const http::HttpRequest& parsed) {
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "[" + stats_->connectionId + "] TARGET=" + parsed.host + ":" + parsed.port +
        " STATUS=UPSTREAM_CONNECTING");
    auto remote = network::Socket::connectTo(parsed.host, parsed.port);
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "[" + stats_->connectionId + "] TARGET=" + parsed.host + ":" + parsed.port +
        " STATUS=UPSTREAM_CONNECTED");

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
    stats_->bytesSent.fetch_add(serialized.size());

    const auto contentLength = parsed.headers.find("content-length");
    std::size_t remaining = contentLength == parsed.headers.end()
                                ? 0
                                : static_cast<std::size_t>(std::stoull(contentLength->second));
    if (!buffered.empty()) {
        const auto initial = std::min(remaining, buffered.size());
        remote.sendAll(buffered.data(), initial);
        stats_->bytesSent.fetch_add(initial);
        remaining -= initial;
    }
    std::vector<char> buffer(bufferSize);
    while (remaining > 0) {
        const auto count = client_.receive(buffer.data(), std::min(buffer.size(), remaining));
        if (count <= 0) throw std::runtime_error("client disconnected during request body");
        stats_->bytesReceived.fetch_add(static_cast<std::uint64_t>(count));
        remote.sendAll(buffer.data(), static_cast<std::size_t>(count));
        stats_->bytesSent.fetch_add(static_cast<std::size_t>(count));
        remaining -= static_cast<std::size_t>(count);
    }
    relay(remote, client_, stats_, &stats_->httpStatus, "UPSTREAM_DISCONNECTED");
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "[" + stats_->connectionId + "] STATUS=HTTP_RESPONSE_RECEIVED HTTP_STATUS=" +
        std::to_string(stats_->httpStatus));
}

void ClientHandler::handleConnect(const std::string&, const std::string& buffered,
                                  const http::HttpRequest& parsed) {
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "[" + stats_->connectionId + "] TARGET=" + parsed.host + ":" + parsed.port +
        " STATUS=UPSTREAM_CONNECTING");
    auto remote = network::Socket::connectTo(parsed.host, parsed.port);
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "[" + stats_->connectionId + "] TARGET=" + parsed.host + ":" + parsed.port +
        " STATUS=UPSTREAM_CONNECTED");
    const std::string established = "HTTP/1.1 200 Connection Established\r\n\r\n";
    client_.sendAll(established.data(), established.size());
    stats_->bytesSent.fetch_add(established.size());
    if (!buffered.empty()) {
        remote.sendAll(buffered.data(), buffered.size());
        stats_->bytesSent.fetch_add(buffered.size());
    }
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "[" + stats_->connectionId + "] TARGET=" + parsed.host + ":" + parsed.port +
        " STATUS=TUNNEL_ESTABLISHED");

    std::thread clientToServer([this, &remote] {
        relay(client_, remote, stats_, nullptr, "CLIENT_DISCONNECTED");
    });
    std::thread serverToClient([this, &remote] {
        relay(remote, client_, stats_, nullptr, "UPSTREAM_DISCONNECTED");
    });
    clientToServer.join();
    remote.shutdown();
    serverToClient.join();
}

} // namespace proxy
