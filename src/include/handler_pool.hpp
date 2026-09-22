#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace flapi {

/**
 * Worker threads that run request handlers off Crow's io threads.
 *
 * Crow runs a handler on the io thread that owns its connection, so a slow
 * synchronous query holds that thread for the whole query and every other
 * connection assigned to it waits - including GET /health, which is how a
 * stalled instance is supposed to report itself (#120). Measured on one
 * ~5s query with 40 concurrent probes: with a single io thread, 40/40 probes
 * blocked and 0/40 reported the stall.
 *
 * Handing the work to this pool frees the io thread immediately; the response
 * is completed back on the owning io_service, which is the thread Crow expects
 * to touch the connection's buffers.
 *
 * BOUNDED on purpose. An unbounded queue turns a slow backend into unbounded
 * memory growth and unbounded latency - the request that finally gets served
 * is one the client stopped waiting for long ago. When the queue is full,
 * submit() fails and the caller answers 503, which is the honest response to
 * "more work than this instance can take".
 */
class HandlerPool {
public:
    HandlerPool(std::size_t threads, std::size_t max_queued);
    ~HandlerPool();

    HandlerPool(const HandlerPool&) = delete;
    HandlerPool& operator=(const HandlerPool&) = delete;

    /// Queue a job. Returns false when the queue is full or the pool is
    /// stopping, in which case the caller MUST answer the request itself -
    /// the job will never run.
    [[nodiscard]] bool submit(std::function<void()> job);

    /// Stop accepting work and join every worker. Jobs already queued are
    /// allowed to finish, so a connection waiting on one is not abandoned.
    void shutdown();

    std::size_t queued() const;
    std::size_t threads() const { return workers_.size(); }

    /// Jobs refused because the queue was full, since startup.
    std::uint64_t rejected() const { return rejected_.load(std::memory_order_relaxed); }

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    std::size_t max_queued_;
    bool stopping_ = false;
    std::atomic<std::uint64_t> rejected_{0};
};

}  // namespace flapi
