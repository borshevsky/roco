#pragma once

#include <experimental/coroutine>

template <typename P>
using Coroutine = std::experimental::coroutine_handle<P>;
