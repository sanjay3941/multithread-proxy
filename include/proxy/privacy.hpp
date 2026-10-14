#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace proxy {

// One policy shared by all clients of this proxy instance.
class Privacy {
public:
    static Privacy& instance();
    static std::string normalizeHost(std::string host, bool rule = false);
    static std::string quote(const std::string& text);
    std::string check(const std::string& host);
    void edit(const std::string& action, const std::string& list, const std::string& domain);
    void setEnabled(const std::string& category, bool enabled);
    std::string json() const;
    void cacheHit(std::size_t bytes);
    void connected(std::uint64_t milliseconds);
    bool detailedLogs() const { return detailedLogs_; }

private:
    Privacy();
    static bool matches(const std::set<std::string>& rules, const std::string& host);
    void save(const std::set<std::string>& blocks, const std::set<std::string>& allows,
              bool ads, bool trackers) const;
    mutable std::mutex mutex_;
    std::map<std::string, std::set<std::string>> defaults_;
    std::set<std::string> blocks_, allows_;
    bool ads_ = true, trackers_ = true, detailedLogs_ = false;
    std::string statePath_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> adsBlocked_{0}, trackersBlocked_{0}, customBlocked_{0};
    std::atomic<std::uint64_t> cacheHits_{0}, bytesSaved_{0}, connectMs_{0}, connects_{0};
};
} // namespace proxy
