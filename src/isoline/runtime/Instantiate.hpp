// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <variant>

#include <isoline/binary/Module.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::runtime
{

/// Why an import could not be bound.
///
/// The enumerators are the rows of LinkErrorTable, in the same order; adding an error is adding an
/// enumerator here and a row there.
enum class LinkErrorCode : uint8_t
{
    UnknownImport,          ///< No external value was supplied for it.
    IncompatibleImportType, ///< The value supplied is of another kind, or its type does not match.
};

/// The last LinkErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastLinkErrorCode = LinkErrorCode::IncompatibleImportType;

/// One link error: its code and the message the spec test suite expects for it.
struct LinkErrorRow
{
    LinkErrorCode code;           ///< The error; equal to the row's index.
    std::string_view specMessage; ///< The spec suite's message, which assert_unlinkable matches.
};

/// Every link error, one row each, in LinkErrorCode order.
inline constexpr auto LinkErrorTable = std::to_array<LinkErrorRow>({
    { .code = LinkErrorCode::UnknownImport, .specMessage = "unknown import" },
    { .code = LinkErrorCode::IncompatibleImportType, .specMessage = "incompatible import type" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: LinkErrorTable)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastLinkErrorCode) + 1;
    }(),
    "LinkErrorTable must hold one row per LinkErrorCode, at the index of its enumerator");

/// The message the spec test suite expects for @p code.
/// @param code The error to describe.
/// @return Its row's message in LinkErrorTable.
[[nodiscard]] constexpr std::string_view specMessage(LinkErrorCode code) noexcept
{
    return LinkErrorTable[static_cast<std::size_t>(code)].specMessage;
}

/// The message an allocation failure is reported with. The spec suite has none: the spec's
/// interpreter never runs out, so this is Isoline's own.
/// @return "out of memory".
[[nodiscard]] constexpr std::string_view specMessage(AllocationFailure const& /*failure*/) noexcept
{
    return "out of memory";
}

/// A failure to bind the imports: what went wrong, and at which import.
struct LinkError
{
    /// Builds an error; both fields are required, so one that forgets its code does not compile.
    /// @param code What went wrong.
    /// @param importIndex The import it is reported at.
    constexpr LinkError(LinkErrorCode code, uint32_t importIndex) noexcept:
        code { code }, importIndex { importIndex }
    {
    }

    LinkErrorCode code;   ///< What went wrong.
    uint32_t importIndex; ///< The index of the import in the module's import section.

    bool operator==(LinkError const&) const = default;
};

/// Why instantiation failed: an import did not bind, the instance would exceed one of the store's
/// limits, the store could not allocate a memory or table, initialization trapped, or the start
/// function threw an exception it did not catch.
using InstantiationError = std::variant<LinkError, LimitExceeded, AllocationFailure, Trap, Thrown>;

/// Executes code for instantiation: constant expressions, and the start function.
///
/// Implemented by the interpreter; instantiation only decides what runs and in which order.
class ICodeExecutor
{
  public:
    virtual ~ICodeExecutor() = default;

    /// Invokes a function.
    /// @param store The store holding it.
    /// @param function Its address.
    /// @param args Its arguments, matching its parameter types.
    /// @param results Receives its results; sized to its result types.
    /// @return Nothing, or how execution ended abruptly: the trap, or the exception it threw and no
    ///         handler caught.
    [[nodiscard]] virtual std::expected<void, Abrupt> invoke(Store& store,
                                                             FuncAddr function,
                                                             std::span<Value const> args,
                                                             std::span<Value> results) = 0;

    /// Evaluates a constant expression of a module instance.
    /// @param store The store holding the instance.
    /// @param moduleInstance The instance whose globals and functions the expression may name.
    /// @param expression The expression; valid for that instance's module.
    /// @return Its value, or the trap evaluation ended with.
    [[nodiscard]] virtual std::expected<Value, Trap> evaluateConst(Store& store,
                                                                   uint32_t moduleInstance,
                                                                   binary::ConstExpr const& expression) = 0;
};

/// Instantiates a module, as the WebAssembly 3.0 spec's instantiate does (Execution, Modules).
///
/// It checks the imports (their count, kinds and types, limits by subtyping), allocates the
/// module's functions, tags, tables, memories and globals (evaluating the globals' initializers in order),
/// evaluates the element segments, then initializes the tables and memories in segment order, as
/// table.init and memory.init followed by elem.drop and data.drop would; declarative element
/// segments are dropped too. The first segment that is out of bounds traps; the segments before it
/// stay written, and the instance stays in the store. Last, it runs the start function.
///
/// Before anything is allocated or interned, the instance and everything it defines (its memories and
/// tables, by count and by their minimum bytes and elements; the types the store does not hold yet;
/// its tags) are checked against what is left of the store's limits (Store::checkInstanceDemand), so
/// a limit refusal (LimitExceeded) leaves no instance, memory, table, type or tag behind; imported
/// entities claim nothing. Tables and memories are allocated before the instance is added to the
/// store, so an AllocationFailure leaves no instance behind (the tables and memories allocated before
/// the failing one stay in the store, unreferenced). A trap from a global's initializer, an element
/// segment, or later, leaves the instance in the store, as the spec's partial instantiation does.
///
/// The instance keeps @p module, and its module is @p module's, sharing its ownership. Every external
/// value in @p imports names an entity of @p store (checked), and there are no more of them than the
/// module has imports (checked).
/// @param store The store to allocate in.
/// @param executor Evaluates constant expressions and runs the start function.
/// @param module The validated module (checked: not null).
/// @param imports The external values its imports bind to, in import order.
/// @return The address of the new module instance, or why instantiation failed.
[[nodiscard]] std::expected<uint32_t, InstantiationError> instantiate(
    Store& store,
    ICodeExecutor& executor,
    std::shared_ptr<validate::ValidatedModule const> module,
    std::span<ExternValue const> imports);

} // namespace isoline::runtime
