// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::runtime
{

/// What a host function reaches its store and the guest through, for the one call it is in.
///
/// The store is read-only: what a host writes (an exception, a call into the guest, the caller's
/// memory) goes through the checked members here, so that no value of the wrong type, and no
/// address the store does not hold, enters it. Values read through store() are borrowed for this
/// host frame.
class HostContext
{
  public:
    /// @param store The store the host function was called in; must outlive the context.
    /// @param executor What executes the guest code the host function invokes; must outlive it too.
    /// @param callerInstance The module instance whose code called the host function; nullopt when
    ///                       the embedder called it directly.
    HostContext(Store& store, ICodeExecutor& executor, std::optional<uint32_t> callerInstance) noexcept;

    /// A context belongs to the one host call it was made for: neither copied nor moved, so that no
    /// copy outlives the call and writes to the store outside a counted host call (HostCallScope).
    HostContext(HostContext const&) = delete;
    HostContext(HostContext&&) = delete;
    HostContext& operator=(HostContext const&) = delete;
    HostContext& operator=(HostContext&&) = delete;
    ~HostContext() = default;

    /// The store, read-only; what is read through it is borrowed for this host frame.
    /// @return The store.
    [[nodiscard]] Store const& store() const noexcept;

    /// The module instance whose code called the host function.
    /// @return Its address; nullopt when no Wasm code called it: the embedder called the host function
    ///         directly, or it is a module's start function, which instantiation invokes.
    [[nodiscard]] std::optional<uint32_t> callerInstance() const noexcept;

    /// Invokes a function of the store, as the embedder would (invokeChecked()): re-entering the guest.
    /// @param function Its address.
    /// @param args Its arguments.
    /// @param results Receives its results; one slot per result.
    /// @return Nothing, or how it ended abruptly; HostFailure{ArgumentMismatch} when the function does
    ///         not exist, @p args are not of its params' types or @p results has the wrong size.
    [[nodiscard]] std::expected<void, Abrupt> invoke(FuncAddr function,
                                                     std::span<Value const> args,
                                                     std::span<Value> results) noexcept;

    /// Allocates an exception the host function may throw by returning Thrown. Its address is
    /// borrowed for this host frame, as what is read through store() is: a host that keeps it longer
    /// escapes it (markEscaped()).
    /// @param tag The tag it is thrown with.
    /// @param payload The values it carries, of the tag's params' types.
    /// @return Its address; HostFailure{ArgumentMismatch} when the tag does not exist (no mismatch) or
    ///         the payload is not of its params' types (the mismatch); Trap{ExceptionLimitExceeded}
    ///         when the store's exceptions are at their limit.
    [[nodiscard]] std::expected<ExnAddr, Abrupt> addException(TagAddr tag,
                                                              std::vector<Value> payload) noexcept;

    /// Escapes the exception @p value refers to (Store::markEscaped()): what a host keeps past its
    /// frame, an exnref read through store() or returned by addException() or invoke(), it escapes,
    /// or a later sweep may reclaim it. Exnref arguments arrive escaped already. Any other value is
    /// left alone.
    /// @param value The value.
    void markEscaped(Value const& value) noexcept;

    /// The bytes of the caller's memory @p index, valid until the next grow or invoke: the host
    /// re-fetches them after either.
    /// @param index The memory's index in the caller instance's memory index space.
    /// @return Its bytes; nullopt without a caller instance, or when it has no memory @p index.
    [[nodiscard]] std::optional<std::span<std::byte>> callerMemory(uint32_t index) noexcept;

  private:
    Store* _store;
    ICodeExecutor* _executor;
    std::optional<uint32_t> _callerInstance;
};

/// Whether an invocation entered the guest.
enum class Entry : uint8_t
{
    Refused, ///< The invocation's own checks failed: no guest code ran.
    Entered, ///< The function ran, to completion or abruptly.
};

/// The outcome of invokeChecked().
struct InvocationResult
{
    Entry entry = Entry::Refused;        ///< Whether it entered the guest.
    std::expected<void, Abrupt> outcome; ///< Nothing, or how it ended abruptly or was refused.
    GrowthRecord refusedGrowth;          ///< The first refused grows of this invocation, its own.
};

/// The one checked entry into guest code: what the embedder, a host function (HostContext::invoke)
/// and every test harness call. It checks that the function exists, that @p args are of its params'
/// types (checkValues()) and that @p results has one slot per result, and only then invokes it within
/// an InvocationScope and a RefusalScope of its own: an outermost call starts a fresh step budget, a
/// nested one shares its caller's. A call that fails a check is HostFailure{ArgumentMismatch}, never a
/// failed precondition: no guest code runs. A call past StoreLimits::maxNestedInvocations enters and
/// traps with CallStackExhausted.
/// @param store The store holding the function.
/// @param executor What executes it.
/// @param function Its address.
/// @param args Its arguments.
/// @param results Receives its results.
/// @return Whether it entered, its outcome, and its refused grows.
[[nodiscard]] InvocationResult invokeChecked(Store& store,
                                             ICodeExecutor& executor,
                                             FuncAddr function,
                                             std::span<Value const> args,
                                             std::span<Value> results) noexcept;

/// Checks what a host function of @p type ended with: results of its result types (checkValues(),
/// their count included), or a Thrown of an exception the store holds (Store::isLiveException()).
/// @param store The store it was called in.
/// @param type Its type, in store form.
/// @param outcome What it returned.
/// @param results The results it left.
/// @return @p outcome when it is valid; HostFailure{ResultMismatch} with the first mismatch when a
///         result is not of its type, and without one when it threw an exception the store does not
///         hold.
[[nodiscard]] std::expected<void, Abrupt> checkHostOutcome(Store const& store,
                                                           binary::FuncType const& type,
                                                           std::expected<void, Abrupt> outcome,
                                                           std::span<Value const> results) noexcept;

/// A host function's implementation.
class IHostFunction
{
  public:
    virtual ~IHostFunction() = default;

    /// Runs the host function: reads @p args, writes @p results, or ends abruptly. It must not throw,
    /// and may be re-entered through @p context.
    ///
    /// @p results arrives holding the default value of each result type. What the function leaves
    /// there is checked (checkHostOutcome()): a result not of its type is a HostFailure{ResultMismatch}.
    /// A Thrown of an exception the store holds (one a guest function it invoked threw, or one it
    /// allocated with HostContext::addException) is thrown on in its Wasm caller, whose handlers may
    /// catch it; any other Abrupt passes through unchanged.
    /// @param context Its store and caller, for this call.
    /// @param args Its arguments, of its params' types.
    /// @param results Its results.
    /// @return Nothing, or how it ended abruptly.
    [[nodiscard]] virtual std::expected<void, Abrupt> call(HostContext& context,
                                                           std::span<Value const> args,
                                                           std::span<Value> results) noexcept = 0;
};

/// A callable that may implement a host function: invoked as IHostFunction::call is, and noexcept.
template <typename F>
concept HostCallable = std::is_nothrow_invocable_r_v<std::expected<void, Abrupt>,
                                                     F&,
                                                     HostContext&,
                                                     std::span<Value const>,
                                                     std::span<Value>>;

/// An IHostFunction that calls a HostCallable.
template <HostCallable F>
class CallableHostFunction final: public IHostFunction
{
  public:
    /// @param function The callable.
    explicit CallableHostFunction(F function): _function { std::move(function) } {}

    /// Calls the callable.
    [[nodiscard]] std::expected<void, Abrupt> call(HostContext& context,
                                                   std::span<Value const> args,
                                                   std::span<Value> results) noexcept override
    {
        return _function(context, args, results);
    }

  private:
    F _function;
};

/// Makes a host function of a noexcept callable.
/// @param function The callable.
/// @return The host function, to add to a store (Store::addHostFunction()).
/// @throws std::bad_alloc When the host has no memory for it; and whatever moving @p function throws.
template <HostCallable F>
[[nodiscard]] std::shared_ptr<IHostFunction> makeHostFunction(F function)
{
    return std::make_shared<CallableHostFunction<F>>(std::move(function));
}

} // namespace isoline::runtime
