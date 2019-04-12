#pragma once

#include "executer.h"
#include "coroutine.h"
#include "task.h"

#include <experimental/coroutine>
#include <memory>
#include <variant>
#include <mutex>
#include <condition_variable>
#include <list>
#include <set>

#include <boost/stacktrace.hpp>
#include <unordered_map>
#include <sstream>

class Executer;

template <typename T> class Future;
template <typename T> class Promise;
template <typename T> class Awaiter;

template <typename T>
using CoroutineHandle = Coroutine<Promise<T>>;

template <typename T>
struct DestroyCoroutine {
    using pointer = CoroutineHandle<T>;

    void operator()(CoroutineHandle<T> coroutine)
    {
        assert(coroutine);
        coroutine.destroy();
    }
};

template <typename T>
using CoroutineHolder = std::unique_ptr<CoroutineHandle<T>, DestroyCoroutine<T>>;

std::set<void*> promises_;
std::unordered_map<void*, std::string> removers_;

template <typename T>
class Promise {
    using Subscription = std::list<std::function<void()>>;

public:
    Promise()
    {
        assert(!promises_.count(this));
        promises_.emplace(this);
    }

    ~Promise()
    {
        if (assignedTask_) {
            assignedTask_->cancel();
        }

        assert(promises_.count(this));
        promises_.erase(this);

        std::ostringstream ss;
        ss << boost::stacktrace::stacktrace();
        removers_[this] = ss.str();
    }

    /// Coroutine support
    Future<T> get_return_object()
    {
        coroutine_ = CoroutineHandle<T>::from_promise(*this);
        return Future<T>(std::move(coroutine_));
    }

    auto return_value(T value)
    {
        {
            std::lock_guard lock(mtx_);
            assert(std::holds_alternative<std::monostate>(result_));
            result_ = std::move(value);
        }

        cv_.notify_one();
        //notifySubscribers();
    }

    auto unhandled_exception()
    {
        {
            std::lock_guard lock(mtx_);
            assert(std::holds_alternative<std::monostate>(result_));
            result_ = std::current_exception();
        }

        cv_.notify_one();
        //notifySubscribers();
    }

    auto initial_suspend() { return std::experimental::suspend_always(); }

    struct Awaiter {
        Promise<T>* thiz;

        bool await_ready() { return false; }

        template <typename Z>
        void await_suspend(CoroutineHandle<Z>)
        {
            assert(promises_.count(thiz));

            std::lock_guard lock(thiz->mtx_);
            thiz->notifySubscribers();
            thiz->finished_ = true;
        }

        void await_resume() {}
    };

    auto final_suspend()
    {
        if (!promises_.count(this)) {
            std::cerr << boost::stacktrace::stacktrace();

            std::cerr << "\n\nremover:\n" << removers_[this] << "\n\n";

            assert(false);
        }
        return Awaiter{this};
        //return std::experimental::suspend_always();
    }

    ///////////////////////////////////////////////////////////////////////////

    T result()
    {
        std::unique_lock lock(mtx_);
        cv_.wait(lock, [&] { return finished_; });

        if (auto v = std::get_if<T>(&result_))  { return std::move(*v); }
        std::rethrow_exception(std::get<std::exception_ptr>(result_));
    }

    Executer* executer() { return executer_; }

    void setExecuter(Executer* executer)
    {
        assert(!executer_);
        executer_ = executer;
    }

    void schedule()
    {
        assert(executer_);
        executer_->schedule(Task(coroutine_));
    }

    template <typename F, typename... Args>
    void subscribe(F&& f, Args&&... args)
    {
        std::lock_guard lock(mtx_);
        std::visit([&] (auto&& value) {
            using S = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<S, std::monostate>) {
                subscribers_.emplace_back(std::forward<F>(f), std::forward<Args>(args)...);
            } else {
                f(std::forward<Args>(args)...);
            }
        }, result_);
    }

    void assignTask(Task* task)
    {
        assert(!assignedTask_ && task);
        assignedTask_ = task;
    }

    void taskCompleted()
    {
        assert(assignedTask_);
        assignedTask_ = nullptr;
    }

private:
    void notifySubscribers()
    {
        //std::lock_guard lock(mtx_);
        for (auto&& sub: subscribers_) { sub(); }
    }

    std::variant<std::monostate, T, std::exception_ptr> result_;

    CoroutineHandle<T> coroutine_;

    std::mutex mtx_;
    std::condition_variable cv_;
    bool finished_ = false;

    Subscription subscribers_;

    Executer* executer_ = nullptr;
    Task* assignedTask_ = nullptr;
};

template <typename T>
class Future {
    friend class Promise<T>;
    friend class Executer;

public:
    using promise_type = Promise<T>;

    Future(const Future&) = delete;
    Future(Future&&) noexcept = default;

    T get()
    {
        assert(coroutine_);

        T result = coroutine_.get().promise().result();
        coroutine_.reset();
        return result;
    }

    auto operator co_await()
    {
        assert(coroutine_);
        return Awaiter(std::move(coroutine_));
    }

    bool valid() const { return coroutine_ != nullptr; }

private:
    CoroutineHandle<T> coroutine()
    {
        assert(coroutine_);
        return coroutine_.get();
    }

    explicit Future(CoroutineHandle<T> coroutine): coroutine_(coroutine)
    {}

    CoroutineHolder<T> coroutine_;
};

template <typename T>
class Awaiter {
public:
    explicit Awaiter(CoroutineHolder<T> coroutine)
        : coroutine_(std::move(coroutine))
    {}

    bool await_ready() { return false; }

    T await_resume()
    {
        T result = coroutine_.get().promise().result();
        coroutine_.reset();
        return result;
    }

    template <typename Z>
    void await_suspend(CoroutineHandle<Z> caller)
    {
        assert(coroutine_);
        assert(caller.promise().executer());

        coroutine_.get().promise().subscribe([caller] {
            caller.promise().schedule();
        });

        if (!coroutine_.get().promise().executer()) {
            auto executer = caller.promise().executer();

            coroutine_.get().promise().setExecuter(executer);
            executer->schedule(Task(coroutine_.get()));
        }
    }

private:
    CoroutineHolder<T> coroutine_;
};
