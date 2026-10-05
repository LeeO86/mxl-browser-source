// SPDX-License-Identifier: MIT
// From mxl-multiviewer (src/util/taskpool.hpp, MIT, same author).
#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mbs::util
{
// A fixed set of worker threads that runs a batch of tasks and waits for all of them. The CPU
// compositor started one thread per tile per frame (std::async) before; here the converter
// splits a frame into bands of rows.
class TaskPool
{
public:
    explicit TaskPool(unsigned threads)
    {
        threads = std::max(1u, threads);
        for (unsigned i = 0; i < threads; ++i)
        {
            workers_.emplace_back([this] { loop(); });
        }
    }

    ~TaskPool()
    {
        {
            std::lock_guard lock{mutex_};
            stopping_ = true;
        }
        wake_.notify_all();
        for (auto& worker : workers_)
        {
            worker.join();
        }
    }

    TaskPool(TaskPool const&) = delete;
    TaskPool& operator=(TaskPool const&) = delete;

    // Runs every task (the calling thread helps) and returns when all have finished.
    void run(std::vector<std::function<void()>> const& tasks)
    {
        {
            std::lock_guard lock{mutex_};
            tasks_ = &tasks;
            next_ = 0;
            pending_ = tasks.size();
        }
        wake_.notify_all();
        work();
        std::unique_lock lock{mutex_};
        done_.wait(lock, [this] { return pending_ == 0; });
        tasks_ = nullptr;
    }

private:
    // Takes and runs tasks until none is left; false when there was nothing to take.
    bool work()
    {
        bool any = false;
        for (;;)
        {
            std::function<void()> const* task = nullptr;
            {
                std::lock_guard lock{mutex_};
                if (tasks_ == nullptr || next_ >= tasks_->size())
                {
                    return any;
                }
                task = &(*tasks_)[next_++];
            }
            (*task)();
            any = true;
            std::lock_guard lock{mutex_};
            if (--pending_ == 0)
            {
                done_.notify_all();
            }
        }
    }

    void loop()
    {
        for (;;)
        {
            {
                std::unique_lock lock{mutex_};
                wake_.wait(lock, [this] { return stopping_ || (tasks_ != nullptr && next_ < tasks_->size()); });
                if (stopping_)
                {
                    return;
                }
            }
            work();
        }
    }

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::vector<std::function<void()>> const* tasks_ = nullptr;
    std::size_t next_ = 0;
    std::size_t pending_ = 0;
    bool stopping_ = false;
};
} // namespace mbs::util
