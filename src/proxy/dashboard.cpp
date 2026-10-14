#include "proxy/dashboard.hpp"
#include "proxy/privacy.hpp"
#include "logging/statistics.hpp"
#include "dashboard_assets.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <sstream>
#include <stdexcept>

namespace proxy {
namespace {
using Clock = std::chrono::steady_clock;
std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r");
    const auto end = value.find_last_not_of(" \t\r");
    return begin == std::string::npos ? "" : value.substr(begin, end - begin + 1);
}
void respond(network::Socket& socket, int status, const std::string& type, const std::string& body) {
    const std::string reason = status == 200 ? "OK" : status == 403 ? "Forbidden" :
        status == 404 ? "Not Found" : status == 500 ? "Internal Server Error" : "Bad Request";
    const std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
        "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff"
        "\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer"
        "\r\nContent-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self'; "
        "connect-src 'self'; img-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'\r\n\r\n";
    socket.sendAll(head.data(), head.size());
    socket.sendAll(body.data(), body.size());
}
std::string decode(const std::string& value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') out += ' ';
        else if (value[i] == '%') {
            if (i + 2 >= value.size()) throw std::invalid_argument("Invalid form encoding.");
            unsigned int ch = 0;
            const auto parsed = std::from_chars(value.data() + i + 1, value.data() + i + 3, ch, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + i + 3 || ch == 0)
                throw std::invalid_argument("Invalid form encoding.");
            out += static_cast<char>(ch); i += 2;
        } else out += value[i];
    }
    return out;
}
std::map<std::string, std::string> form(const std::string& body) {
    std::map<std::string, std::string> fields;
    std::istringstream stream(body);
    for (std::string entry; std::getline(stream, entry, '&');) {
        const auto equal = entry.find('=');
        if (equal == std::string::npos || !fields.emplace(decode(entry.substr(0, equal)),
            decode(entry.substr(equal + 1))).second) throw std::invalid_argument("Invalid form fields.");
    }
    return fields;
}
}

Dashboard* Dashboard::active_ = nullptr;

Dashboard::Dashboard(std::uint16_t port, std::uint16_t proxyPort, std::vector<std::string> bindAddresses)
    : port_(port), proxyPort_(proxyPort), bindAddresses_(std::move(bindAddresses)) {
    if (!port_) { active_ = this; return; }
    if (port_ == proxyPort_) throw std::invalid_argument("Dashboard and proxy ports must differ.");
    listener_ = network::Socket::createTcp();
    listener_.bindAndListen("127.0.0.1", port_);
    const int flags = ::fcntl(listener_.nativeHandle(), F_GETFL, 0);
    if (flags < 0 || ::fcntl(listener_.nativeHandle(), F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error("Cannot configure dashboard listener.");
    thread_ = std::thread([this] { run(); });
    active_ = this;
}

Dashboard::~Dashboard() {
    active_ = nullptr;
    stopping_ = true;
    if (thread_.joinable()) thread_.join();
}
std::string Dashboard::url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/";
}
void Dashboard::run() {
    while (!stopping_) {
        pollfd ready{listener_.nativeHandle(), POLLIN, 0};
        if (::poll(&ready, 1, 200) <= 0) continue;
        try {
            auto client = listener_.accept();
            client.setReceiveTimeout(2); client.setSendTimeout(2);
            handle(client);
        } catch (const std::exception&) { /* Client disconnects do not stop management. */ }
    }
}

bool Dashboard::serveProxy(network::Socket& client, const http::HttpRequest& parsed,
                           const std::string& head, const std::string& buffered) {
    if (!active_ || Privacy::normalizeHost(parsed.host) != "proxy.internal") return false;
    if (parsed.method == "CONNECT" || parsed.port != "80") {
        respond(client, 400, "text/plain", "Open http://proxy.internal using plain HTTP.\n");
        return true;
    }
    const auto lineEnd = head.find("\r\n");
    active_->handle(client, parsed.method + " " + parsed.path + " " + parsed.version +
                    head.substr(lineEnd) + buffered);
    return true;
}

void Dashboard::handle(network::Socket& client, std::string request) {
    client.setSendTimeout(2);
    try { serve(client, std::move(request)); }
    catch (const std::invalid_argument& error) {
        respond(client, 400, "application/json", "{\"error\":" + Privacy::quote(error.what()) + "}");
    } catch (const std::exception&) {
        respond(client, 500, "application/json", "{\"error\":\"Unable to complete the request. Check state directory permissions.\"}");
    }
}

void Dashboard::serve(network::Socket& client, std::string request) {
    if (request.size() > 16384) throw std::invalid_argument("Request too large.");
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    auto receive = [&] {
        if (stopping_) throw std::invalid_argument("Dashboard is stopping.");
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        pollfd ready{client.nativeHandle(), POLLIN, 0};
        if (remaining <= 0 || ::poll(&ready, 1, static_cast<int>(remaining)) <= 0)
            throw std::invalid_argument("Request timed out.");
        char buffer[4096];
        const auto count = client.receive(buffer, sizeof(buffer));
        if (count <= 0) throw std::invalid_argument("Incomplete request.");
        request.append(buffer, static_cast<std::size_t>(count));
        if (request.size() > 16384) throw std::invalid_argument("Request too large.");
    };
    while (request.find("\r\n\r\n") == std::string::npos) receive();
    const auto end = request.find("\r\n\r\n") + 4;
    std::istringstream head(request.substr(0, end));
    std::string method, path, version, line, extra;
    std::getline(head, line);
    std::istringstream first(line);
    if (!(first >> method >> path >> version) || (first >> extra) ||
        (version != "HTTP/1.1" && version != "HTTP/1.0")) throw std::invalid_argument("Invalid request.");
    std::map<std::string, std::string> headers;
    while (std::getline(head, line) && line != "\r") {
        const auto colon = line.find(':');
        if (colon == std::string::npos || !headers.emplace(lower(trim(line.substr(0, colon))),
            trim(line.substr(colon + 1))).second) throw std::invalid_argument("Invalid headers.");
    }
    const auto host = lower(headers["host"]);
    const auto suffix = ":" + std::to_string(port_);
    if ((host != "proxy.internal" && host != "proxy.internal:80" &&
         host != "127.0.0.1" + suffix && host != "localhost" + suffix) ||
        (headers.count("origin") && headers["origin"] != "http://" + host &&
         !(host == "proxy.internal:80" && headers["origin"] == "http://proxy.internal")) ||
        (headers.count("sec-fetch-site") && headers["sec-fetch-site"] == "cross-site")) {
        respond(client, 403, "application/json", "{\"error\":\"Use http://proxy.internal through your proxy or the local dashboard address.\"}"); return;
    }
    if (headers.count("transfer-encoding")) throw std::invalid_argument("Chunked requests are unsupported.");
    if (method == "GET" && path == "/") { respond(client, 200, "text/html; charset=utf-8", resources::html); return; }
    if (method == "GET" && path == "/app.js") { respond(client, 200, "text/javascript; charset=utf-8", resources::js); return; }
    if (method == "GET" && path == "/style.css") { respond(client, 200, "text/css; charset=utf-8", resources::css); return; }
    if (method == "GET" && path == "/api/status") {
        std::string addresses = "[";
        for (const auto& address : bindAddresses_) {
            if (addresses.size() > 1) addresses += ',';
            addresses += Privacy::quote(address);
        }
        addresses += ']';
        const std::string payload = "{\"privacy\":" + Privacy::instance().json() +
            ",\"connections\":" + logging::ProxyStatistics::instance().json() +
            ",\"devices\":" + logging::ProxyStatistics::instance().devicesJson() +
            ",\"proxyPort\":" + std::to_string(proxyPort_) +
            ",\"bindAddress\":" + Privacy::quote(bindAddresses_.front()) +
            ",\"bindAddresses\":" + addresses +
            ",\"dashboardPort\":" + std::to_string(port_) + "}";
        respond(client, 200, "application/json", payload); return;
    }
    if (method != "POST" || (path != "/api/rules" && path != "/api/settings")) {
        respond(client, 404, "application/json", "{\"error\":\"Unknown endpoint.\"}"); return;
    }
    if (headers["x-proxy-dashboard"] != "1") {
        respond(client, 403, "application/json", "{\"error\":\"Submit changes from the dashboard.\"}"); return;
    }
    if (headers["content-type"].rfind("application/x-www-form-urlencoded", 0) != 0)
        throw std::invalid_argument("Expected a form body.");
    const auto length = headers["content-length"];
    std::size_t size = 0;
    const auto parsed = std::from_chars(length.data(), length.data() + length.size(), size);
    if (length.empty() || parsed.ec != std::errc{} || parsed.ptr != length.data() + length.size() || size > 4096)
        throw std::invalid_argument("Invalid body length.");
    while (request.size() < end + size) receive();
    const auto fields = form(request.substr(end, size));
    try {
        if (path == "/api/rules") Privacy::instance().edit(fields.at("action"), fields.at("list"), fields.at("domain"));
        else {
            const auto value = fields.at("enabled");
            if (value != "0" && value != "1") throw std::invalid_argument("Invalid setting.");
            Privacy::instance().setEnabled(fields.at("category"), value == "1");
        }
    } catch (const std::out_of_range&) { throw std::invalid_argument("Missing form fields."); }
    respond(client, 200, "application/json", "{\"saved\":true}");
}
} // namespace proxy
