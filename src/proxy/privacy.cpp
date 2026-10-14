#include "proxy/privacy.hpp"
#include "dashboard_assets.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace proxy {
namespace {
constexpr std::size_t maxRules = 2000;
std::string array(const std::set<std::string>& values) {
    std::string result = "[";
    for (const auto& v : values) {
        if (result.size() > 1) result += ',';
        result += Privacy::quote(v);
    }
    return result + ']';
}
}

Privacy& Privacy::instance() { static Privacy policy; return policy; }

std::string Privacy::quote(const std::string& text) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}

std::string Privacy::normalizeHost(std::string host, bool rule) {
    if (rule) {
        const auto first = host.find_first_not_of(" \t\r\n");
        const auto last = host.find_last_not_of(" \t\r\n");
        host = first == std::string::npos ? "" : host.substr(first, last - first + 1);
    }
    std::transform(host.begin(), host.end(), host.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!host.empty() && host.back() == '.') host.pop_back();
    unsigned char address[16];
    if (inet_pton(AF_INET, host.c_str(), address) == 1 ||
        inet_pton(AF_INET6, host.c_str(), address) == 1) {
        if (rule) throw std::invalid_argument("Enter a domain name, not an IP address.");
        return host;
    }
    if (host.empty() || host.size() > 253) throw std::invalid_argument("Invalid domain name.");
    std::istringstream labels(host);
    for (std::string label; std::getline(labels, label, '.');) {
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            throw std::invalid_argument("Invalid domain name.");
        for (unsigned char c : label) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                throw std::invalid_argument("Use a domain only, without a URL, path, port or wildcard. Use punycode for international domains.");
        }
    }
    if (host.back() == '.' || (rule && host.find('.') == std::string::npos && host != "localhost"))
        throw std::invalid_argument("Enter a full domain, such as example.com.");
    return host;
}

Privacy::Privacy() {
    const char* detail = std::getenv("PROXY_DETAILED_LOGS");
    detailedLogs_ = detail && std::string(detail) == "1";
    std::istringstream bundled(resources::domains);
    for (std::string line; std::getline(bundled, line);) {
        if (line.empty() || line.front() == '#') continue;
        std::istringstream row(line);
        std::string category, domain;
        if (row >> category >> domain) defaults_[category].insert(normalizeHost(domain, true));
    }
    const char* directory = std::getenv("PROXY_STATE_DIR");
    const std::filesystem::path stateDir = directory ? directory : "data";
    if (std::filesystem::create_directories(stateDir)) ::chmod(stateDir.c_str(), 0700);
    statePath_ = (stateDir / "privacy.rules").string();
    if (!std::filesystem::exists(statePath_)) return;
    if (std::filesystem::file_size(statePath_) > 512 * 1024)
        throw std::runtime_error("Privacy rules file is too large.");
    std::ifstream input(statePath_);
    if (!input) throw std::runtime_error("Cannot read privacy rules file.");
    for (std::string line; std::getline(input, line);) {
        if (line.empty() || line.front() == '#') continue;
        std::istringstream row(line);
        std::string kind, value, extra;
        if (!(row >> kind >> value) || (row >> extra)) throw std::runtime_error("Invalid privacy rules file.");
        if (kind == "block") blocks_.insert(normalizeHost(value, true));
        else if (kind == "allow") allows_.insert(normalizeHost(value, true));
        else if ((kind == "ads" || kind == "trackers") && (value == "0" || value == "1"))
            (kind == "ads" ? ads_ : trackers_) = value == "1";
        else throw std::runtime_error("Invalid privacy rules file.");
        if (blocks_.size() + allows_.size() > maxRules) throw std::runtime_error("Too many custom rules.");
    }
}

bool Privacy::matches(const std::set<std::string>& rules, const std::string& host) {
    for (std::size_t start = 0;;) {
        if (rules.count(host.substr(start))) return true;
        const auto dot = host.find('.', start);
        if (dot == std::string::npos) return false;
        start = dot + 1;
    }
}

std::string Privacy::check(const std::string& rawHost) {
    const auto host = normalizeHost(rawHost);
    std::lock_guard<std::mutex> lock(mutex_);
    if (matches(allows_, host)) return "";
    if (matches(blocks_, host)) { ++customBlocked_; return "custom"; }
    if (ads_ && matches(defaults_.at("ads"), host)) { ++adsBlocked_; return "ads"; }
    if (trackers_ && matches(defaults_.at("trackers"), host)) { ++trackersBlocked_; return "trackers"; }
    return "";
}

void Privacy::save(const std::set<std::string>& blocks, const std::set<std::string>& allows,
                   bool ads, bool trackers) const {
    std::ostringstream output;
    output << "# Managed by the local privacy dashboard. Stop the proxy before manual edits.\n"
           << "ads " << ads << "\ntrackers " << trackers << '\n';
    for (const auto& d : blocks) output << "block " << d << '\n';
    for (const auto& d : allows) output << "allow " << d << '\n';
    std::string pattern = statePath_ + ".XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) throw std::runtime_error("Cannot save rules. Check the state directory permissions.");
    const auto data = output.str();
    std::size_t written = 0;
    bool good = true;
    while (written < data.size()) {
        const auto count = ::write(fd, data.data() + written, data.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { good = false; break; }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(fd) != 0) good = false;
    if (::close(fd) != 0) good = false;
    if (!good || ::rename(name.data(), statePath_.c_str()) != 0) {
        ::unlink(name.data());
        throw std::runtime_error("Could not save rules; the previous policy is still active.");
    }
}

void Privacy::edit(const std::string& action, const std::string& list, const std::string& domain) {
    const auto normalized = normalizeHost(domain, true);
    if (normalized == "proxy.internal") throw std::invalid_argument("proxy.internal is reserved for the dashboard.");
    if ((action != "add" && action != "remove") || (list != "block" && list != "allow"))
        throw std::invalid_argument("Unknown rule action.");
    std::lock_guard<std::mutex> lock(mutex_);
    auto blocks = blocks_, allows = allows_;
    auto& selected = list == "block" ? blocks : allows;
    if (action == "add") selected.insert(normalized); else selected.erase(normalized);
    if (blocks.size() + allows.size() > maxRules) throw std::invalid_argument("The limit is 2,000 custom rules.");
    save(blocks, allows, ads_, trackers_);
    blocks_.swap(blocks); allows_.swap(allows);
}

void Privacy::setEnabled(const std::string& category, bool enabled) {
    if (category != "ads" && category != "trackers") throw std::invalid_argument("Unknown protection category.");
    std::lock_guard<std::mutex> lock(mutex_);
    const bool ads = category == "ads" ? enabled : ads_;
    const bool trackers = category == "trackers" ? enabled : trackers_;
    save(blocks_, allows_, ads, trackers);
    ads_ = ads; trackers_ = trackers;
}

void Privacy::cacheHit(std::size_t bytes) { ++cacheHits_; bytesSaved_ += bytes; }
void Privacy::connected(std::uint64_t milliseconds) { connectMs_ += milliseconds; ++connects_; }

std::string Privacy::json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream out;
    const auto count = connects_.load();
    out << "{\"adsEnabled\":" << (ads_ ? "true" : "false")
        << ",\"trackersEnabled\":" << (trackers_ ? "true" : "false")
        << ",\"detailedLogs\":" << (detailedLogs_ ? "true" : "false")
        << ",\"adsBlocked\":" << adsBlocked_.load()
        << ",\"trackersBlocked\":" << trackersBlocked_.load()
        << ",\"customBlocked\":" << customBlocked_.load()
        << ",\"cacheHits\":" << cacheHits_.load() << ",\"bytesSaved\":" << bytesSaved_.load()
        << ",\"originConnections\":" << count << ",\"connectMs\":";
    if (count) out << static_cast<double>(connectMs_.load()) / count; else out << "null";
    out << ",\"uptime\":" << std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started_).count()
        << ",\"blocklist\":" << array(blocks_) << ",\"allowlist\":" << array(allows_)
        << ",\"defaultAds\":" << array(defaults_.at("ads"))
        << ",\"defaultTrackers\":" << array(defaults_.at("trackers")) << '}';
    return out.str();
}
} // namespace proxy
