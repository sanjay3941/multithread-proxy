#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace proxy {

// Shared, bounded cache for explicitly public HTTP responses. All network I/O
// happens outside this class's mutex.
class Cache {
public:
    struct Flight {
        std::condition_variable ready;
        bool done = false;
        bool invalidated = false;
    };
    enum class Action { Hit, Cold, Fetch, Wait, Simulate };
    struct Lookup {
        Action action;
        std::string response;
        std::shared_ptr<Flight> flight;
    };

    static Cache& instance();
    Lookup lookup(const std::string& key, bool simulation);
    bool wait(const std::string& key, const std::shared_ptr<Flight>& flight,
              std::string& response);
    bool store(const std::string& key, std::string response, int lifetimeSeconds,
               int initialAge, const std::shared_ptr<Flight>& flight);
    void complete(const std::string& key, const std::shared_ptr<Flight>& flight);
    void invalidate(const std::string& key);
    static int eligibleLifetime(const std::string& response, int& initialAge);

    static constexpr std::size_t maxEntryBytes = 1024 * 1024;
    static constexpr std::size_t maxTotalBytes = 16 * 1024 * 1024;
    static constexpr std::size_t maxDemandKeys = 4096;

private:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        std::string response;
        Clock::time_point stored;
        Clock::time_point expires;
        Clock::time_point lastAccess;
        int initialAge;
    };

    Cache();
    ~Cache();
    void prune(Clock::time_point now);
    bool findFresh(const std::string& key, Clock::time_point now, std::string& response);
    std::map<std::string, Entry> entries_;
    std::map<std::string, std::deque<Clock::time_point>> demand_;
    std::map<std::string, std::shared_ptr<Flight>> flights_;
    std::size_t bytes_ = 0;
    std::mutex mutex_;
    std::condition_variable housekeeping_;
    bool stopping_ = false;
    std::thread janitor_;
};

} // namespace proxy
