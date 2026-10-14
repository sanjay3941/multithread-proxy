#include "proxy/work_queue.hpp"

namespace proxy {

WorkQueue::WorkQueue(std::size_t maxSize) noexcept : maxSize_(maxSize) {}

bool WorkQueue::push(WorkItem item) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopped_ || queue_.size() >= maxSize_) {
        return false;   // item destructs here, closing the socket
    }
    queue_.push(std::move(item));
    lock.unlock();
    cv_.notify_one();
    return true;
}

bool WorkQueue::pop(WorkItem& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });
    if (stopped_ || queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop();
    return true;
}

void WorkQueue::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    cv_.notify_all();
}

void WorkQueue::drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Each WorkItem destructs here, closing its socket via ~Socket().
    while (!queue_.empty()) {
        logging::ProxyStatistics::instance().finish(queue_.front().stats, false);
        queue_.pop();
    }
}

} // namespace proxy
