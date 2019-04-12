#pragma once

#include "task.h"

#include <utility>
#include <vector>
#include <thread>
#include <queue>

class Executer {
public:
    template <typename F, typename... Args>
    auto apply(F&& f, Args&&... args) -> decltype(f(args...))
    {
        auto future = f(std::forward<Args>(args)...);
        auto coroutine = future.coroutine();
        coroutine.promise().setExecuter(this);
        schedule(Task(coroutine));
        return future;
    }

    virtual void schedule(Task&& task) = 0;
};

class BlockingQueue {
public:
    void push(Task&& task)
    {
        {
            std::lock_guard l(mtx_);
            q_.push(std::move(task));
        }

        cv_.notify_one();
    }

    Task pop()
    {
        std::unique_lock l(mtx_);
        cv_.wait(l, [&] { return !q_.empty(); });

        auto task = std::move(q_.front());
        q_.pop();

        return task;
    }

    bool empty()
    {
        std::lock_guard l(mtx_);
        return q_.empty();
    }

private:
    std::queue<Task> q_;

    std::mutex mtx_;
    std::condition_variable cv_;
};

class ThreadPool: public Executer {
public:
    explicit ThreadPool(size_t size)
    {
        for (size_t i = 0; i < size; ++i) {
            pollers_.emplace_back(&ThreadPool::poll, this);
        }
    }

    ~ThreadPool()
    {
        q_.push(Task::poison());
        for (auto& p: pollers_) { p.join(); }
    }

    bool empty() { return q_.empty(); }

private:
    void schedule(Task&& task) override
    {
        q_.push(std::move(task));
    }

    void poll()
    {
        while (true) {
            auto task = q_.pop();

            if (task.poisoned()) {
                q_.push(std::move(task));
                break;
            }

            task();
        }
    }

    BlockingQueue q_;
    std::vector<std::thread> pollers_;
};
