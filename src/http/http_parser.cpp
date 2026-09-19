#include "http/http_parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <stdexcept>

namespace http {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? "" : value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

void parseAuthority(const std::string& authority, std::string& host, std::string& port,
                    const std::string& defaultPort) {
    if (authority.empty()) {
        throw std::invalid_argument("missing host");
    }
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) throw std::invalid_argument("invalid IPv6 host");
        host = authority.substr(1, close - 1);
        port = close + 1 < authority.size() && authority[close + 1] == ':'
                   ? authority.substr(close + 2)
                   : defaultPort;
    } else {
        const auto colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(':') == colon) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;
            port = defaultPort;
        }
    }
    if (host.empty() || port.empty()) throw std::invalid_argument("invalid authority");

    unsigned int portNumber = 0;
    const auto* begin = port.data();
    const auto* end = begin + port.size();
    const auto result = std::from_chars(begin, end, portNumber);
    if (result.ec != std::errc{} || result.ptr != end || portNumber == 0 || portNumber > 65535) {
        throw std::invalid_argument("invalid port");
    }
}

} // namespace

HttpRequest HttpParser::parseRequest(const std::string& requestHead) {
    std::istringstream stream(requestHead);
    std::string line;
    if (!std::getline(stream, line)) throw std::invalid_argument("empty HTTP request");
    line = trim(line);
    std::istringstream requestLine(line);
    HttpRequest request;
    if (!(requestLine >> request.method >> request.target >> request.version) ||
        request.version.rfind("HTTP/", 0) != 0) {
        throw std::invalid_argument("invalid HTTP request line");
    }
    for (std::string header; std::getline(stream, header);) {
        header = trim(header);
        if (header.empty()) break;
        const auto separator = header.find(':');
        if (separator == std::string::npos) throw std::invalid_argument("invalid HTTP header");
        request.headers[lower(trim(header.substr(0, separator)))] =
            trim(header.substr(separator + 1));
    }

    if (request.method == "CONNECT") {
        parseAuthority(request.target, request.host, request.port, "443");
        request.path = request.target;
        return request;
    }

    const auto hostHeader = request.headers.find("host");
    std::string authority;
    if (request.target.rfind("http://", 0) == 0) {
        const auto start = 7;
        const auto slash = request.target.find('/', start);
        authority = request.target.substr(start, slash == std::string::npos
                                                   ? std::string::npos
                                                   : slash - start);
        request.path = slash == std::string::npos ? "/" : request.target.substr(slash);
    } else {
        if (hostHeader == request.headers.end()) throw std::invalid_argument("missing Host header");
        authority = hostHeader->second;
        request.path = request.target.empty() ? "/" : request.target;
    }
    parseAuthority(authority, request.host, request.port, "80");
    return request;
}

} // namespace http
