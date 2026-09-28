#pragma once

#include "logging/statistics.hpp"
#include "network/socket.hpp"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <queue>

namespace proxy {

/// A single unit of work passed from the accept loop to a worker thread.
struct WorkItem {
    network::Socket                          socket;
    std::shared_ptr<logging::ConnectionStats> stats;
};

/// Thread-safe, bounded, blocking queue.
///
/// push() enqueues an item if there is room and the queue has not been
/// stopped; it returns false (without touching the item) otherwise.
/// pop() blocks until an item is available or the queue is stopped;
/// it returns false when stopped and empty.
/// stop() wakes all blocked pop() callers and prevents future push() calls.
/// drain() closes and discards sockets that were queued but not yet picked up.
class WorkQueue {
public:
    explicit WorkQueue(std::size_t maxSize) noexcept;

    bool push(WorkItem item);
    bool pop(WorkItem& out);

    void stop();
    void drain();

private:
    const std::size_t        maxSize_;
    std::queue<WorkItem>     queue_;
    std::mutex               mutex_;
    std::condition_variable  cv_;
    bool                     stopped_ = false;
};

} // namespace proxy
