#pragma once

#include "coroutine.h"

#include <functional>
#include <iostream>
#include <thread>

class Task {
public:
    Task(const Task&) = delete;
    Task(Task&&) = default;

    template <typename P>
    explicit Task(Coroutine<P> coroutine)
        : poisoned_(false)
        , func_([coroutine] () mutable
            {
                assert(coroutine);
                coroutine.promise().taskCompleted();
                coroutine.resume();
            })
    {
        coroutine.promise().assignTask(this);
    }

    static Task poison() {
        Task task;
        task.poisoned_ = true;
        return task;
    }

    bool poisoned() { return poisoned_; }
    void operator()()
    {
        func_();
        cancel();
    }

    void cancel()
    {
        std::function<void()> empty = []{};
        func_.swap(empty);
    }

private:
    Task() = default;

    bool poisoned_ = false;
    std::function<void()> func_;
};

