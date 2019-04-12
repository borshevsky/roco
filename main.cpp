#include "executer.h"
#include "future.h"

#include <iostream>

Future<int> f1()
{
    co_return 1;
}

Future<std::string> f2()
{
    co_return "abc";
}

Future<std::string> f3(Executer& next)
{
    auto value = co_await next.apply(f1);
    auto str = co_await f2();

    co_return str + "/" + std::to_string(value);
}

Future<int> fib(size_t n)
{
    if (n == 1) { co_return 1; }
    if (n == 2) { co_return 1; }

    size_t n1 = co_await fib(n - 1);
    size_t n2 = co_await fib(n - 2);
    co_return n1 + n2;
}

int main(int argc, char** argv)
{
    int size = std::atoi(argv[1]);

    ThreadPool tp1(size), tp2(1);
    //auto value = tp1.apply(f3, tp2);
    //std::cerr << "value: " << value.get() << "\n";

    auto fib20 = tp1.apply(fib, 20);
    fib20.get();
    assert(tp1.empty());
    //std::cerr << fib10.get() << "\n";
}