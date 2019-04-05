#include <iostream>
#include <experimental/coroutine>
#include <queue>
#include <thread>
#include <sstream>
#include <csignal>

std::vector<std::string>* logs;

void addLog(std::string s) {
    static std::mutex M;
    std::lock_guard l(M);

    logs->push_back(s);
}

void dumpLogs()
{
    for (auto&& l: *logs) {
        std::cerr << l << "\n";
    }
}

struct NewLineStream {
    NewLineStream() = default;

    NewLineStream(const NewLineStream& other) = default;
    NewLineStream(NewLineStream&&) = default;

    template <typename V>
    NewLineStream& operator << (V&& v)
    {
        origin_ << v;
        return *this;
    }

    ~NewLineStream()
    {
        addLog(origin_.str());
    }

    std::ostringstream origin_;
};

template <typename... T>
NewLineStream log(T... tag)
{
    NewLineStream ret;
    ret << "## ";
    int _[sizeof...(T)] = { (ret << tag, 0)... };
    ret << ": ";
    return ret;
}

template <typename T>
class Executer;

template <typename T>
class Future {
public:
    struct promise_type {
        promise_type(): v(std::make_shared<T>())
        {
            static std::atomic<int> ID = 0;
            id = ID++;

        }
        ~promise_type()
        {
            log(id) << "~promise";
        }

        auto set_executer(Executer<T>* executer)
        {
            assert(executer_ == nullptr);
            executer_ = executer;
        }

        auto get_return_object()
        {
            coro_ = Handle::from_promise(*this);
            return Future<T>(coro_, v);
        }
        auto initial_suspend()
        {
            //executer_->apply(coro_);
            return std::experimental::coroutines_v1::suspend_always();
        }
        auto return_value(T v)
        {
            *this->v = v;
        }

        auto final_suspend()
        {
            log(id) << "i'm finally suspended!";
            return std::experimental::coroutines_v1::suspend_always();
        }

        auto unhandled_exception()
        {
            std::terminate();
        }

        int id = 0;
        std::shared_ptr<T> v;
        std::experimental::coroutine_handle<promise_type> coro_;
        Executer<T>* executer_ = nullptr;
    };

    using Handle = std::experimental::coroutine_handle<promise_type>;

    Future(Handle coro, std::shared_ptr<T> v): coro_(coro), v_(std::move(v))
    {
    }

    T get()
    {
        while (*v_ == T());
        return *v_;
    }

    Handle handle() { return coro_; }

    operator Handle() { return coro_; }

    auto operator co_await()
    {
        struct Awaiter {
            Awaiter(Future* thiz): thiz(thiz)
            {
            }

            auto await_ready()
            {
                return false;
            }

            auto await_resume()
            {
                return *thiz->coro_.promise().v;
            }

            auto await_suspend(std::experimental::coroutine_handle<promise_type> caller)
            {
                assert(caller.promise().executer_);

                log("Awaiter") << "Await. Caller: " << caller.promise().id << ", thiz: " << thiz->coro_.promise().id;

                if (!thiz->coro_.promise().executer_) {
                    thiz->coro_.promise().set_executer(caller.promise().executer_);
                    thiz->coro_.promise().executer_->apply1(thiz->coro_);
                }

                caller.promise().executer_->apply1(caller, [c = thiz->coro_] {
                    log("check") << c.promise().id << " done? " << std::boolalpha << c.done();
                    return c.done();
                });
            }

            Future* thiz;
        };

        return Awaiter(this);
    }

    Executer<T>* executer_;
    Handle coro_;
    std::shared_ptr<T> v_;
};

template <typename T>
class Executer {
public:
    explicit Executer(std::string tag)
        : finished_(false)
        , tag_(std::move(tag))
        //, worker_(&Executer::worker, this)
    {
        for (size_t i = 0; i < 4; ++i) {
            workers_.emplace_back(&Executer::worker, this);
        }
    }

    ~Executer() {
        finished_ = true;
        cv_.notify_all();

        for (auto& w: workers_) { w.join(); }
        //worker_.join();
    }

    template <typename F, typename... Args>
    Future<T> apply(F&& f, Args&&... args)
    {
        static_assert(std::is_same_v<decltype(f(args...)), Future<T>>);
        return apply(f(args...));
    }

    Future<T> apply(Future<T> future)
    {
        auto coro = future.coro_;
        coro.promise().set_executer(this);
        apply1(coro);
        return future;
    }

    void apply1(
        std::experimental::coroutine_handle<typename Future<T>::promise_type> coro,
        std::function<bool()> ready = [] { return true; }
    ) {
        log(tag()) << "apply coro " << coro.promise().id;
        assert(coro.promise().executer_ != nullptr);
        {
            std::lock_guard lock(mtx_);
            tasks_.push({coro, ready});
        }

        cv_.notify_one();
    }

    void worker()
    {
        while (!finished_) {
            std::unique_lock lock(mtx_);
            cv_.wait(lock, [this] { return !tasks_.empty() || finished_; });
            if (finished_) { return; }
            auto task = std::move(tasks_.front());
            tasks_.pop();

            if (!task.ready()) {
                //log(tag()) << "Task with coro " << task.coro.promise().id << " not ready yet, rescheduled\n";
                tasks_.push(task);
                continue;
            }

            log(tag()) << "Fetched " << task.coro.promise().id << " for execution";
            lock.unlock();
            bool done = task.coro.done();

            if (done) {
                log(tag()) << "Coro " << task.coro.promise().id << " is already done";
                continue;
            }

            log(tag()) << "Coro " << task.coro.promise().id << " is ready for resume";
            task.coro.resume();
            log(tag()) << "Coro " << task.coro.promise().id << " completed: " << std::boolalpha << task.coro.done();
        }
    }

    std::string tag()
    {
        std::ostringstream ss;
        ss << tag_;
        return ss.str();
    }

    struct Executable {
        std::experimental::coroutine_handle<typename Future<T>::promise_type> coro;
        std::function<bool()> ready;
    };

    std::queue<Executable> tasks_;

    std::atomic<bool> finished_;
    std::string tag_;
    std::mutex mtx_;
    std::condition_variable cv_;
    //std::thread worker_;
    std::vector<std::thread> workers_;

};

Future<int> f2()
{
    co_return 2;
}

Future<int> f1(Executer<int>& next)
{
    log("f1") << "Start";
    int v = co_await next.apply(f2);
    log("f1") << "v = " << v;
    int v2 = co_await f2();
    log("f1") << "v2 = " << v2;
    co_return v2 + 2;
}

Future<int> fib(size_t n)
{
    log("fib") << "fib(" << n << ")\n";

    assert(n > 0);

    if (n == 1) { co_return 1; }
    if (n == 2) { co_return 1; }

    log("fib") << "Coro " << n << " awaiting for " << n - 1 << " & " << n - 2 << "\n";
    co_return co_await fib(n - 1) + co_await fib(n - 2);
}

int main()
{
    std::vector<std::string> actlogs;
    logs = &actlogs;

    signal(SIGABRT, [] (int n) { dumpLogs(); exit(n); });
    signal(SIGSTOP, [] (int n) { dumpLogs(); exit(n); });
    signal(SIGKILL, [] (int n) { dumpLogs(); exit(n); });

    Executer<int> e1("1"), e2("2");
    auto res = e1.apply(f1, e2);
    auto val = res.get();
    assert(val == 4);
    assert(e1.apply(fib, 10).get() == 55);
    assert(e2.apply(fib, 11).get() == 89);

    std::cout << e1.apply(fib, 10).get() << "\n";

    //dumpLogs();
    return 0;
}