#include "handler_pool.hpp"

#include <crow/logging.h>

#include <utility>

namespace flapi {

HandlerPool::HandlerPool(std::size_t threads, std::size_t max_queued)
    : max_queued_(max_queued) {
    if (threads == 0) {
        threads = 1;
    }
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { run(); });
    }
}

HandlerPool::~HandlerPool() {
    shutdown();
}

bool HandlerPool::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        if (max_queued_ > 0 && jobs_.size() >= max_queued_) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void HandlerPool::shutdown(std::chrono::milliseconds drain_budget) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        drain_deadline_ = std::chrono::steady_clock::now() + drain_budget;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

std::size_t HandlerPool::queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

void HandlerPool::run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            // Drain on shutdown rather than dropping: a queued job owns a
            // connection that is waiting for a response, and abandoning it
            // leaves the client hanging until its own timeout.
            if (jobs_.empty()) {
                return;
            }
            // ...but not forever. Past the drain deadline the remaining queue
            // is abandoned, so one slow job cannot keep the process alive
            // until the platform SIGKILLs it mid-write.
            if (stopping_ && std::chrono::steady_clock::now() >= drain_deadline_) {
                const auto abandoned = jobs_.size();
                jobs_.clear();
                lock.unlock();
                CROW_LOG_WARNING << "shutdown drain budget expired; abandoned "
                                 << abandoned << " queued request(s)";
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            // A handler that throws must not take the worker - or the process -
            // with it. The job itself is responsible for answering its request;
            // by the time we see this, it has not.
            CROW_LOG_ERROR << "handler pool job threw: " << e.what();
        } catch (...) {
            CROW_LOG_ERROR << "handler pool job threw a non-standard exception";
        }
    }
}

}  // namespace flapi
