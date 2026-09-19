#pragma once

#include <map>
#include <string>

namespace http {

struct HttpRequest {
    std::string method;
    std::string host;
    std::string port;
    std::string target;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
};

class HttpParser {
public:
    static HttpRequest parseRequest(const std::string& requestHead);
};

} // namespace http
