// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/HostFunction.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/ValueCheck.hpp>

namespace isoline::runtime
{

namespace
{
    /// The host failure of @p kind for @p mismatch.
    /// @param kind Whose values they were.
    /// @param mismatch The first value not of its type; nullopt for an address the store does not hold.
    /// @return The failure, as an Abrupt.
    [[nodiscard]] Abrupt mismatchFailure(HostFailureKind kind, std::optional<ValueMismatch> mismatch) noexcept
    {
        return HostFailure { .kind = kind, .code = 0, .mismatch = mismatch };
    }

    /// Whether @p values are of @p types, as a host failure of @p kind when they are not.
    [[nodiscard]] std::expected<void, Abrupt> checkHostValues(
        Store const& store,
        HostFailureKind kind,
        std::span<Value const> values,
        std::span<binary::ValueType const> types) noexcept
    {
        return checkValues(store, values, types).transform_error([kind](ValueMismatch mismatch) {
            return mismatchFailure(kind, mismatch);
        });
    }

    /// Whether a call of @p function with @p args and room for @p resultCount results may enter it.
    /// @return Nothing, or the HostFailure{ArgumentMismatch} it is refused with.
    [[nodiscard]] std::expected<void, Abrupt> checkCall(Store const& store,
                                                        FuncAddr function,
                                                        std::span<Value const> args,
                                                        std::size_t resultCount) noexcept
    {
        if (function >= store.functionCount())
            return std::unexpected { mismatchFailure(HostFailureKind::ArgumentMismatch, std::nullopt) };
        auto const& type = store.functionType(function);
        return checkHostValues(store, HostFailureKind::ArgumentMismatch, args, type.params)
            .and_then([&]() -> std::expected<void, Abrupt> {
                if (resultCount == type.results.size())
                    return {};
                return std::unexpected { mismatchFailure(
                    HostFailureKind::ArgumentMismatch,
                    ValueMismatch {
                        .kind = MismatchKind::Count,
                        .index = static_cast<uint32_t>(std::min(resultCount, type.results.size())) }) };
            });
    }
} // namespace

HostContext::HostContext(Store& store,
                         ICodeExecutor& executor,
                         std::optional<uint32_t> callerInstance) noexcept:
    _store { &store }, _executor { &executor }, _callerInstance { callerInstance }
{
}

Store const& HostContext::store() const noexcept
{
    return *_store;
}

std::optional<uint32_t> HostContext::callerInstance() const noexcept
{
    return _callerInstance;
}

std::expected<void, Abrupt> HostContext::invoke(FuncAddr function,
                                                std::span<Value const> args,
                                                std::span<Value> results) noexcept
{
    return invokeChecked(*_store, *_executor, function, args, results).outcome;
}

std::expected<ExnAddr, Abrupt> HostContext::addException(TagAddr tag, std::vector<Value> payload) noexcept
{
    if (tag >= _store->tagCount())
        return std::unexpected { mismatchFailure(HostFailureKind::ArgumentMismatch, std::nullopt) };
    auto const& params = _store->type(_store->tag(tag).typeId).params;
    return checkHostValues(*_store, HostFailureKind::ArgumentMismatch, payload, params)
        .and_then([&]() -> std::expected<ExnAddr, Abrupt> {
            return _store->addException(tag, std::move(payload)).transform_error([](Trap trap) {
                return Abrupt { trap };
            });
        });
}

void HostContext::markEscaped(Value const& value) noexcept
{
    _store->markEscaped(value);
}

std::optional<std::span<std::byte>> HostContext::callerMemory(uint32_t index) noexcept
{
    if (!_callerInstance)
        return std::nullopt;
    auto const& memories = _store->moduleInstance(*_callerInstance).memories;
    if (index >= memories.size())
        return std::nullopt;
    return _store->memory(memories[index]).bytes();
}

InvocationResult invokeChecked(Store& store,
                               ICodeExecutor& executor,
                               FuncAddr function,
                               std::span<Value const> args,
                               std::span<Value> results) noexcept
{
    if (auto const checked = checkCall(store, function, args, results.size()); !checked)
        return InvocationResult { .entry = Entry::Refused, .outcome = checked, .refusedGrowth = {} };
    // Past the nesting limit the invocation counts as entered and traps, as a call too deep does.
    if (auto const nesting = store.checkInvocationNesting(); !nesting)
        return InvocationResult { .entry = Entry::Entered,
                                  .outcome = std::unexpected { Abrupt { nesting.error() } },
                                  .refusedGrowth = {} };
    auto const invocation = InvocationScope { store };
    auto const scope = RefusalScope { store };
    // Initializers run in order, so the record is read after the invocation, while the scope holds it.
    return InvocationResult { .entry = Entry::Entered,
                              .outcome = executor.invoke(store, function, args, results),
                              .refusedGrowth = scope.record() };
}

std::expected<void, Abrupt> checkHostOutcome(Store const& store,
                                             binary::FuncType const& type,
                                             std::expected<void, Abrupt> outcome,
                                             std::span<Value const> results) noexcept
{
    if (!outcome)
    {
        // An exception it throws must be one the store holds, or a handler would read a forged one.
        auto const* thrown = std::get_if<Thrown>(&outcome.error());
        if (thrown && !store.isLiveException(thrown->exception))
            return std::unexpected { mismatchFailure(HostFailureKind::ResultMismatch, std::nullopt) };
        return outcome;
    }
    return checkHostValues(store, HostFailureKind::ResultMismatch, results, type.results);
}

} // namespace isoline::runtime
