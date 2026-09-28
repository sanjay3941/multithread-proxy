#include "proxy/client_handler.hpp"

#include "http/http_parser.hpp"
#include "logging/logger.hpp"
#include "proxy/timeouts.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace proxy {
namespace {

constexpr std::size_t bufferSize = 16 * 1024;

using Clock = std::chrono::steady_clock;

class RequestTimeout : public network::TimeoutError {
public:
    using network::TimeoutError::TimeoutError;
};

bool timedOut() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
}

// Retain the same deadline when a signal interrupts poll().
bool waitForData(pollfd* sockets, nfds_t count, Clock::time_point deadline) {
    for (;;) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now()).count();
        if (remaining <= 0) return false;
        const int result = ::poll(sockets, count, static_cast<int>(remaining));
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) throw std::runtime_error("socket poll failed");
        if (result == 0) continue;
        return true;
    }
}

std::pair<std::string, std::string> readHeaders(
    network::Socket& socket, const std::shared_ptr<logging::ConnectionStats>& stats) {
    std::string data;
    std::vector<char> buffer(bufferSize);
    const auto deadline = Clock::now() + std::chrono::seconds(timeouts::CLIENT_HEADER_TIMEOUT);
    while (data.find("\r\n\r\n") == std::string::npos) {
        pollfd ready{socket.nativeHandle(), POLLIN, 0};
        if (!waitForData(&ready, 1, deadline)) {
            throw RequestTimeout("client header deadline exceeded");
        }
        const auto count = socket.receive(buffer.data(), buffer.size());
        if (count == 0) throw std::runtime_error("client disconnected before request");
        if (count < 0) {
            if (timedOut()) throw RequestTimeout("client header read timed out");
            throw std::runtime_error("client receive failed");
        }
        data.append(buffer.data(), static_cast<std::size_t>(count));
        stats->bytesReceived.fetch_add(static_cast<std::uint64_t>(count));
        if (data.size() > 1024 * 1024) throw std::runtime_error("HTTP headers too large");
    }
    const auto end = data.find("\r\n\r\n") + 4;
    return {data.substr(0, end), data.substr(end)};
}

void relay(network::Socket& source, network::Socket& destination,
           const std::shared_ptr<logging::ConnectionStats>& stats, bool& responseStarted) {
    std::vector<char> buffer(bufferSize);
    for (;;) {
        const auto count = source.receive(buffer.data(), buffer.size());
        if (count < 0) {
            if (timedOut()) throw network::TimeoutError("upstream read timed out");
            throw std::runtime_error("upstream receive failed");
        }
        if (count == 0) {
            if (!responseStarted) throw std::runtime_error("upstream closed before response");
            logging::Logger::instance().log(logging::LogLevel::INFO,
                "[" + stats->connectionId + "] STATUS=UPSTREAM_DISCONNECTED");
            break;
        }
        stats->bytesReceived.fetch_add(static_cast<std::uint64_t>(count));
        responseStarted = true;
        destination.sendAll(buffer.data(), static_cast<std::size_t>(count));
        stats->bytesSent.fetch_add(static_cast<std::uint64_t>(count));
        if (stats->httpStatus == 0) {
            const std::string response(buffer.data(), static_cast<std::size_t>(count));
            const auto lineEnd = response.find("\r\n");
            if (lineEnd != std::string::npos) {
                std::istringstream statusLine(response.substr(0, lineEnd));
                std::string version;
                statusLine >> version >> stats->httpStatus;
            }
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
    const auto fail = [this](int status, const char* reason, const char* code,
                             const std::exception& error) {
        stats_->status = "FAILED";
        stats_->errorCode = code;
        stats_->errorMessage = error.what();
        if (!responseStarted_) stats_->httpStatus = status;
        logging::Logger::instance().log(logging::LogLevel::ERROR,
            "[" + stats_->connectionId + "] PROTOCOL=" + stats_->protocol +
            " STATUS=FAILED HTTP_STATUS=" + std::to_string(stats_->httpStatus) +
            " ERROR=" + code + " MESSAGE=" + error.what());
        // An HTTP response or CONNECT tunnel already in progress cannot receive
        // a second HTTP status line. Close it after recording the failure.
        if (!responseStarted_) {
            try {
                const std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                    "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                client_.sendAll(response.data(), response.size());
                stats_->bytesSent.fetch_add(response.size());
            } catch (...) {}
        }
    };
    try {
        stats_->threadId = [] {
            std::ostringstream output;
            output << std::this_thread::get_id();
            return output.str();
        }();

        // Header reading also enforces an absolute deadline; this receive
        // timeout remains in effect for subsequent HTTP request-body reads.
        client_.setReceiveTimeout(timeouts::CLIENT_HEADER_TIMEOUT);
        client_.setSendTimeout(timeouts::SOCKET_WRITE_TIMEOUT);

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
    } catch (const RequestTimeout& error) {
        fail(408, "Request Timeout", "CLIENT_TIMEOUT", error);
    } catch (const network::TimeoutError& error) {
        fail(504, "Gateway Timeout", "TIMEOUT", error);
    } catch (const std::invalid_argument& error) {
        fail(400, "Bad Request", "INVALID_REQUEST", error);
    } catch (const std::exception& error) {
        fail(502, "Bad Gateway", "CONNECTION_FAILED", error);
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

    // Connect to upstream with a bounded timeout.
    auto remote = network::Socket::connectTo(parsed.host, parsed.port,
                                             timeouts::UPSTREAM_CONNECT_TIMEOUT);

    // Bound how long we wait for the upstream server to respond.
    remote.setReceiveTimeout(timeouts::UPSTREAM_READ_TIMEOUT);
    remote.setSendTimeout(timeouts::SOCKET_WRITE_TIMEOUT);

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
        if (count < 0 && timedOut()) throw RequestTimeout("client request body timed out");
        if (count <= 0) throw std::runtime_error("client disconnected during request body");
        stats_->bytesReceived.fetch_add(static_cast<std::uint64_t>(count));
        remote.sendAll(buffer.data(), static_cast<std::size_t>(count));
        stats_->bytesSent.fetch_add(static_cast<std::size_t>(count));
        remaining -= static_cast<std::size_t>(count);
    }
    relay(remote, client_, stats_, responseStarted_);
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "[" + stats_->connectionId + "] STATUS=HTTP_RESPONSE_RECEIVED HTTP_STATUS=" +
        std::to_string(stats_->httpStatus));
}

void ClientHandler::handleConnect(const std::string&, const std::string& buffered,
                                  const http::HttpRequest& parsed) {
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "[" + stats_->connectionId + "] TARGET=" + parsed.host + ":" + parsed.port +
        " STATUS=UPSTREAM_CONNECTING");

    // Connect to upstream with a bounded timeout.
    auto remote = network::Socket::connectTo(parsed.host, parsed.port,
                                             timeouts::UPSTREAM_CONNECT_TIMEOUT);

    remote.setSendTimeout(timeouts::SOCKET_WRITE_TIMEOUT);
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "[" + stats_->connectionId + "] TARGET=" + parsed.host + ":" + parsed.port +
        " STATUS=UPSTREAM_CONNECTED");
    const std::string established = "HTTP/1.1 200 Connection Established\r\n\r\n";
    responseStarted_ = true;
    stats_->httpStatus = 200;
    client_.sendAll(established.data(), established.size());
    stats_->bytesSent.fetch_add(established.size());
    if (!buffered.empty()) {
        remote.sendAll(buffered.data(), buffered.size());
        stats_->bytesSent.fetch_add(buffered.size());
    }
    logging::Logger::instance().log(logging::LogLevel::INFO,
        "[" + stats_->connectionId + "] TARGET=" + parsed.host + ":" + parsed.port +
        " STATUS=TUNNEL_ESTABLISHED");

    // One worker watches both directions so activity on either side keeps the
    // tunnel alive. Blocking sends have their own bounded timeout.
    pollfd sockets[] = {{client_.nativeHandle(), POLLIN, 0},
                        {remote.nativeHandle(), POLLIN, 0}};
    std::vector<char> buffer(bufferSize);
    auto deadline = Clock::now() + std::chrono::seconds(timeouts::HTTPS_TUNNEL_IDLE_TIMEOUT);
    for (;;) {
        if (!waitForData(sockets, 2, deadline)) {
            throw network::TimeoutError("tunnel idle timeout");
        }
        for (int i = 0; i < 2; ++i) {
            if (sockets[i].revents == 0) continue;
            auto& source = i == 0 ? client_ : remote;
            auto& destination = i == 0 ? remote : client_;
            const auto count = source.receive(buffer.data(), buffer.size());
            if (count < 0) throw std::runtime_error("tunnel receive failed");
            if (count == 0) {
                logging::Logger::instance().log(logging::LogLevel::INFO,
                    "[" + stats_->connectionId + "] STATUS=" +
                    (i == 0 ? "CLIENT_DISCONNECTED" : "UPSTREAM_DISCONNECTED"));
                return;
            }
            stats_->bytesReceived.fetch_add(static_cast<std::uint64_t>(count));
            destination.sendAll(buffer.data(), static_cast<std::size_t>(count));
            stats_->bytesSent.fetch_add(static_cast<std::uint64_t>(count));
            deadline = Clock::now() + std::chrono::seconds(timeouts::HTTPS_TUNNEL_IDLE_TIMEOUT);
        }
    }
}

} // namespace proxy
