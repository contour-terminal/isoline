// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace isoline::c::detail
{

/// Runs the body of a C API function, which no C++ exception may leave.
///
/// Every wasm_ definition consists of exactly one call of this (wasm.h declares its functions without
/// noexcept, so their definitions cannot carry it; this does). The one exception a body may raise is
/// std::bad_alloc, when host memory runs out: it becomes the value @p onHostMemoryExhausted gives,
/// which reports the out-of-memory outcome the function's contract names (NULL, false, or the immortal
/// HOST_MEMORY_EXHAUSTED error or trap). Any other exception is a defect in Isoline and ends the
/// process, as an exception leaving a noexcept function does.
///
/// @param body The function's work, called once with no arguments.
/// @param onHostMemoryExhausted Gives the function's result when @p body runs out of host memory.
/// @return What @p body returns, or what @p onHostMemoryExhausted returns.
template <typename Body, typename OnHostMemoryExhausted>
    requires std::is_invocable_v<Body&&>
             && std::is_convertible_v<std::invoke_result_t<OnHostMemoryExhausted&>,
                                      std::invoke_result_t<Body&&>>
auto boundary(Body&& body, OnHostMemoryExhausted onHostMemoryExhausted) noexcept
    -> std::invoke_result_t<Body&&>
{
    try
    {
        return std::invoke(std::forward<Body>(body));
    }
    catch (std::bad_alloc const&)
    {
        return onHostMemoryExhausted();
    }
}

} // namespace isoline::c::detail
