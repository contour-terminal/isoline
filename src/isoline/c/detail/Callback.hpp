// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <utility>

#include <isoline/c/export.h>

namespace isoline::c::detail
{

/// A function of the embedder's that the C API calls back: a pointer of the convention every isoline_
/// callback type and finalizer parameter is declared with, ISOLINE_CALL (export.h).
template <typename Result, typename... Arguments>
using CallbackFunction = Result(ISOLINE_CALL*)(Arguments...);

/// The trampoline through which the C API calls an embedder's callback: the function and the context it
/// was registered with, called as the C ABI requires, the context first.
///
/// The function is the embedder's C, so no C++ exception can leave it; one that does anyway ends the
/// process here, at the boundary, rather than unwinding through Isoline.
template <typename Result, typename... Arguments>
class Callback
{
  public:
    /// The callback's C type: what a header's `Result (ISOLINE_CALL* )(void* context, Arguments...)`
    /// typedef names.
    using Function = CallbackFunction<Result, void*, Arguments...>;

    /// @param function The embedder's function; not null (the C function that takes it refuses NULL).
    /// @param context Passed to @p function as its first argument, as the embedder gave it.
    constexpr Callback(Function function, void* context) noexcept:
        _function { function }, _context { context }
    {
    }

    /// Calls the embedder's function with the context and @p arguments.
    /// @return What the function returns.
    auto operator()(Arguments... arguments) const noexcept -> Result
    {
        return _function(_context, arguments...);
    }

  private:
    Function _function;
    void* _context;
};

/// An embedder's finalizer of a context it handed the C API: called exactly once, with that context,
/// when whatever holds it ends. No finalizer (a NULL finalizer parameter, which wasm.h allows) is no call.
class Finalizer
{
  public:
    /// The finalizer's C type: `void (ISOLINE_CALL* finalizer)(void*)`, every finalizer parameter's.
    using Function = CallbackFunction<void, void*>;

    /// @param function The embedder's finalizer, or null for none.
    /// @param context What @p function is called with.
    constexpr Finalizer(Function function, void* context) noexcept:
        _function { function }, _context { context }
    {
    }

    Finalizer(Finalizer const&) = delete;
    auto operator=(Finalizer const&) -> Finalizer& = delete;

    /// Takes over @p other's finalizer; @p other then has none.
    constexpr Finalizer(Finalizer&& other) noexcept:
        _function { std::exchange(other._function, nullptr) },
        _context { std::exchange(other._context, nullptr) }
    {
    }

    /// Runs this finalizer, then takes over @p other's; @p other then has none.
    auto operator=(Finalizer&& other) noexcept -> Finalizer&
    {
        if (this != &other)
        {
            run();
            _function = std::exchange(other._function, nullptr);
            _context = std::exchange(other._context, nullptr);
        }
        return *this;
    }

    /// Runs the finalizer, if there is one.
    ~Finalizer() { run(); }

  private:
    /// Calls the finalizer once and leaves none.
    void run() noexcept
    {
        if (auto const function = std::exchange(_function, nullptr))
            function(_context);
    }

    Function _function;
    void* _context;
};

} // namespace isoline::c::detail
