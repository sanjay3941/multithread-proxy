#include "proxy/cache.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <ctime>
#include <sstream>
#include <string_view>

namespace proxy {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t");
    const auto last = value.find_last_not_of(" \t");
    return first == std::string::npos ? "" : value.substr(first, last - first + 1);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool parseNumber(std::string_view text, std::size_t& number) {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

} // namespace

Cache& Cache::instance() {
    static Cache cache;
    return cache;
}

Cache::Cache() : janitor_([this] {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        housekeeping_.wait_for(lock, std::chrono::minutes(1));
        prune(Clock::now());
    }
}) {}

Cache::~Cache() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    housekeeping_.notify_one();
    janitor_.join();
}

void Cache::prune(Clock::time_point now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now >= it->second.expires ||
            now - it->second.lastAccess >= std::chrono::minutes(10)) {
            bytes_ -= it->second.response.size();
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = demand_.begin(); it != demand_.end();) {
        auto& times = it->second;
        while (!times.empty() && now - times.front() >= std::chrono::minutes(5)) {
            times.pop_front();
        }
        if (times.empty()) it = demand_.erase(it);
        else ++it;
    }
}

bool Cache::findFresh(const std::string& key, Clock::time_point now, std::string& response) {
    const auto it = entries_.find(key);
    if (it == entries_.end() || now >= it->second.expires ||
        now - it->second.lastAccess >= std::chrono::minutes(10)) return false;
    it->second.lastAccess = now;
    const auto& stored = it->second.response;
    const auto headerEnd = stored.find("\r\n\r\n");
    const auto age = it->second.initialAge +
        std::chrono::duration_cast<std::chrono::seconds>(now - it->second.stored).count();
    const auto header = stored.substr(0, headerEnd);
    const auto hasConnection = lower(header).find("\r\nconnection:") != std::string::npos;
    response = header + "\r\nAge: " + std::to_string(age) +
        (hasConnection ? "" : "\r\nConnection: close") + stored.substr(headerEnd);
    return true;
}

int Cache::eligibleLifetime(const std::string& response, int& initialAge) {
    initialAge = 0;
    if (response.size() > maxEntryBytes) return 0;
    const auto end = response.find("\r\n\r\n");
    if (end == std::string::npos || end > 64 * 1024) return 0;
    const auto lineEnd = response.find("\r\n");
    if (lineEnd == std::string::npos || lineEnd >= end) return 0;
    const std::string status = response.substr(0, lineEnd);
    if (status.rfind("HTTP/1.1 200 ", 0) != 0 && status.rfind("HTTP/1.0 200 ", 0) != 0) {
        return 0;
    }
    std::map<std::string, std::string> headers;
    for (std::size_t pos = lineEnd + 2; pos < end;) {
        const auto next = response.find("\r\n", pos);
        if (next == std::string::npos || next > end) return 0;
        const auto colon = response.find(':', pos);
        if (colon == std::string::npos || colon >= next) return 0;
        const auto name = lower(trim(response.substr(pos, colon - pos)));
        if (!headers.emplace(name, trim(response.substr(colon + 1, next - colon - 1))).second) {
            return 0; // Ambiguous duplicate fields are not cached.
        }
        pos = next + 2;
    }
    if (headers.count("set-cookie") || headers.count("vary") ||
        headers.count("transfer-encoding") || headers.count("age")) return 0;
    const auto connection = headers.find("connection");
    if (connection != headers.end() && lower(connection->second) != "close") return 0;
    const auto length = headers.find("content-length");
    const auto control = headers.find("cache-control");
    if (length == headers.end() || control == headers.end()) return 0;
    std::size_t bodyLength = 0;
    if (!parseNumber(length->second, bodyLength) || bodyLength > maxEntryBytes ||
        bodyLength != response.size() - (end + 4)) return 0;

    bool isPublic = false;
    std::size_t maxAge = 0;
    bool hasMaxAge = false;
    std::istringstream directives(lower(control->second));
    for (std::string directive; std::getline(directives, directive, ',');) {
        directive = trim(directive);
        if (directive == "no-store" || directive == "no-cache" ||
            directive == "private") return 0;
        if (directive == "public") isPublic = true;
        if (directive.rfind("max-age=", 0) == 0) {
            if (hasMaxAge || !parseNumber(std::string_view(directive).substr(8), maxAge)) return 0;
            hasMaxAge = true;
        }
    }
    if (!isPublic || !hasMaxAge || maxAge == 0) return 0;

    // A response whose Date is already old must not gain a fresh lifetime.
    const auto date = headers.find("date");
    if (date != headers.end()) {
        std::tm parsed{};
        const auto* text = date->second.c_str();
        const auto* end = strptime(text, "%a, %d %b %Y %H:%M:%S GMT", &parsed);
        if (end == nullptr || *end != '\0') return 0;
        const auto issued = timegm(&parsed);
        if (issued == static_cast<std::time_t>(-1)) return 0;
        const auto apparentAge = std::max<std::time_t>(0, std::time(nullptr) - issued);
        if (static_cast<std::size_t>(apparentAge) >= maxAge) return 0;
        initialAge = static_cast<int>(std::min<std::size_t>(apparentAge, 2147483647));
        maxAge -= static_cast<std::size_t>(apparentAge);
    }
    return static_cast<int>(std::min<std::size_t>(maxAge, 600));
}

Cache::Lookup Cache::lookup(const std::string& key, bool simulation) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();
    prune(now);
    std::string response;
    if (!simulation && findFresh(key, now, response)) {
        return {Action::Hit, std::move(response), nullptr};
    }
    if (demand_.find(key) == demand_.end() && demand_.size() >= maxDemandKeys) {
        const auto oldest = std::min_element(demand_.begin(), demand_.end(),
            [](const auto& a, const auto& b) {
                return a.second.back() < b.second.back();
            });
        demand_.erase(oldest);
    }
    auto& times = demand_[key];
    times.push_back(now);
    if (times.size() > 3) times.pop_front();
    if (times.size() < 3) return {Action::Cold, {}, nullptr};
    if (simulation) return {Action::Simulate, {}, nullptr};
    const auto existing = flights_.find(key);
    if (existing != flights_.end()) return {Action::Wait, {}, existing->second};
    auto flight = std::make_shared<Flight>();
    flights_[key] = flight;
    return {Action::Fetch, {}, flight};
}

bool Cache::wait(const std::string& key, const std::shared_ptr<Flight>& flight,
                 std::string& response) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!flight->ready.wait_for(lock, std::chrono::seconds(15), [&] { return flight->done; })) {
        return false;
    }
    return findFresh(key, Clock::now(), response);
}

bool Cache::store(const std::string& key, std::string response, int lifetimeSeconds,
                  int initialAge, const std::shared_ptr<Flight>& flight) {
    if (response.size() > maxEntryBytes || lifetimeSeconds <= 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (flight && flight->invalidated) return false;
    const auto now = Clock::now();
    prune(now);
    const auto previous = entries_.find(key);
    if (previous != entries_.end()) {
        bytes_ -= previous->second.response.size();
        entries_.erase(previous);
    }
    while (bytes_ + response.size() > maxTotalBytes && !entries_.empty()) {
        const auto oldest = std::min_element(entries_.begin(), entries_.end(),
            [](const auto& a, const auto& b) {
                return a.second.lastAccess < b.second.lastAccess;
            });
        bytes_ -= oldest->second.response.size();
        entries_.erase(oldest);
    }
    bytes_ += response.size();
    entries_.emplace(key, Entry{std::move(response), now,
        now + std::chrono::seconds(std::min(lifetimeSeconds, 600)), now, initialAge});
    return true;
}

void Cache::invalidate(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = entries_.find(key);
    if (entry != entries_.end()) {
        bytes_ -= entry->second.response.size();
        entries_.erase(entry);
    }
    const auto flight = flights_.find(key);
    if (flight != flights_.end()) flight->second->invalidated = true;
}

void Cache::complete(const std::string& key, const std::shared_ptr<Flight>& flight) {
    if (!flight) return;
    std::lock_guard<std::mutex> lock(mutex_);
    flight->done = true;
    const auto it = flights_.find(key);
    if (it != flights_.end() && it->second == flight) flights_.erase(it);
    flight->ready.notify_all();
}

} // namespace proxy
