// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <isoline/binary/Types.hpp>

namespace isoline::runtime
{

/// The resources a store caps, each at construction (StoreLimits) and each refused by name.
///
/// The enumerators are the rows of LimitTable, in the same order; adding a limit is adding an
/// enumerator here, a field to StoreLimits and a row there.
enum class Limit : uint8_t
{
    CallDepth,         ///< Wasm frames in progress at once; past it, CallStackExhausted.
    StackValues,       ///< The static costs of the frames in progress; past it, CallStackExhausted.
    NestedInvocations, ///< Invocations in progress at once, host-nested ones included.
    MemoryBytes,       ///< The bytes of every memory of the store together.
    TableElements,     ///< The elements of every table of the store together.
    Memories,          ///< The memories of the store.
    Tables,            ///< The tables of the store.
    Instances,         ///< The module instances of the store.
    Exceptions,        ///< The exceptions the store holds; past it, ExceptionLimitExceeded.
    ExceptionValues,   ///< The payload values of those exceptions together.
    Types,             ///< The interned function types, weighted 1 + params + results each.
    Tags,              ///< The tags of the store.
    Steps,             ///< The steps of the outermost invocation; past it, StepLimitExceeded.
};

/// The last Limit enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastLimit = Limit::Steps;

/// The limits a store keeps to, fixed at its construction: everything a guest may claim of the host's
/// stack and memory. Every count and size here caps the whole store, all its instances together, so
/// that a module of many memories or tables cannot multiply a per-entity cap.
struct StoreLimits
{
    /// How many Wasm frames may be live at once before a call traps with CallStackExhausted.
    ///
    /// The scope is part of the cross-engine contract: the depth counts the Wasm frames of every
    /// invocation of the store in progress, not of one. A host function that invokes Wasm again
    /// (HostContext::invoke(), or an instantiation's start function) nests within the depth of the call
    /// that reached it, so Wasm → host → Wasm recursion is cut at the same call in every engine. Host
    /// frames take no depth (maxNestedInvocations bounds them).
    uint32_t maxCallDepth = 10'000;

    /// The stack values the frames in progress may cost together, host-nested invocations' included,
    /// before a call traps with CallStackExhausted. The charge is counted in values, the same in every
    /// engine; what a value takes in bytes is each engine's own: interpreter::ValueBytes on the
    /// reference interpreter's stacks (a runtime::Value, its size the standard library's variant's), and on
    /// the fast interpreter's slot chunks at most fastinterpreter::SlotBytesPerValue, which
    /// docs/fast-interpreter.md's chunk bound multiplies by up to four. The default, 4 Mi values, is tens of
    /// MiB on the reference engine.
    ///
    /// A Wasm frame costs validate::ValidatedModule::frameCost(), params + locals + its most operands +
    /// its deepest label nesting + 1, and a host frame hostFrameCost(), params + results + 1: static
    /// costs, so the call that exhausts the stack is the same in every engine (the deterministic stack
    /// limit). The + 1 charges a frame that holds nothing, so that (func $r (call $r)) is not free.
    ///
    /// The order at a callee entry is part of that contract: first the FunctionEntry poll (the
    /// interrupt, then one step), then, for a Wasm callee, the depth (maxCallDepth), then this charge.
    /// So a call refused for its stack has cost its step, and a call that would exceed both the step
    /// budget and the stack traps with StepLimitExceeded. A tail call gives its caller's charge back
    /// before it charges the callee's.
    uint64_t maxStackValues = uint64_t { 1 } << 22;

    /// How many invocations may be in progress at once: the outermost and those host functions nest
    /// within it. Each nesting takes host stack (a few frames, kilobytes under sanitizers), so this
    /// keeps Wasm → host → Wasm recursion from overflowing it; one more traps with CallStackExhausted.
    /// 100 leaves a wide margin: under ASan an 8 MiB stack overflowed at about 240 nestings, and a
    /// release build's nesting takes 1–2 KB. Every HostContext::invoke() is one more, so a
    /// component-model call crossing N core instances through host adapters nests N + 1.
    uint32_t maxNestedInvocations = 100;

    /// The bytes all memories of the store may hold together.
    uint64_t maxMemoryBytes = uint64_t { 1 } << 32;

    /// The elements all tables of the store may hold together.
    uint64_t maxTableElements = 10'000'000;

    /// How many memories the store may hold.
    uint32_t maxMemories = 10'000;

    /// How many tables the store may hold.
    uint32_t maxTables = 100'000;

    /// How many module instances the store may hold.
    ///
    /// It counts instances, not their size: an instance's functions, globals, element items and data
    /// views are not charged, so a store may hold up to maxInstances times the largest module's
    /// footprint. That is acceptable while instantiation is embedder-driven, as it is: the embedder
    /// chooses which modules to instantiate, and each one's footprint is linear in its size (the
    /// memory-bounds suite checks it). Once guests can instantiate (the component model), this must charge
    /// size.
    uint32_t maxInstances = 10'000;

    /// How many exceptions the store may hold at once; a throw past it traps with ExceptionLimitExceeded.
    /// An allocation on a full arena first sweeps it (PollSite::ExceptionSweep) and reclaims what no
    /// root reaches; escaped exceptions are never reclaimed, so this bounds those a guest may pin.
    uint32_t maxExceptions = 65'536;

    /// How many payload values the exceptions together may hold; a throw past it traps with
    /// ExceptionLimitExceeded.
    uint64_t maxExceptionValues = uint64_t { 1 } << 20;

    /// The type units the store's interned function types may take together: each type, interned once
    /// however many modules use it, takes 1 + its params + its results, so a wide type costs what it
    /// holds.
    uint64_t maxTypes = 1'000'000;

    /// How many tags the store may hold.
    uint32_t maxTags = 1'000'000;

    /// How many steps the outermost invocation may take, the invocations nested in it included: one
    /// step per poll whose PollSiteTable row charges one (StepCharge::OneStep). Poll sites are
    /// Wasm-level places, so the same module, inputs and limits exhaust the budget at the same point
    /// in every engine, in both execution modes. Past it, StepLimitExceeded. Every outermost
    /// invocation, and every outermost instantiation, starts a fresh budget; an instantiation a host
    /// function makes is nested in the call that reached the host, and shares its budget. The default is
    /// no limit.
    uint64_t maxSteps = std::numeric_limits<uint64_t>::max();
};

/// The static stack cost of a host function's frame, charged against StoreLimits::maxStackValues
/// while it runs: params + results + 1.
/// @param type The host function's type.
/// @return Its cost in stack values.
[[nodiscard]] constexpr uint64_t hostFrameCost(binary::FuncType const& type) noexcept
{
    return uint64_t { type.params.size() } + type.results.size() + 1;
}

/// One row of LimitTable: a limit, its name and what it counts.
struct LimitRow
{
    Limit limit;            ///< The limit; equal to the row's index.
    std::string_view name;  ///< Its name in a refusal's message: "memory bytes".
    std::string_view unit;  ///< What it counts: "bytes".
    std::string_view field; ///< The StoreLimits member that sets it, which value reads: "maxMemoryBytes".
    uint64_t (*value)(StoreLimits const& limits) noexcept; ///< Reads its value from a StoreLimits.
};

/// Every limit, one row each, in Limit order.
inline constexpr auto LimitTable = std::to_array<LimitRow>({
    { .limit = Limit::CallDepth,
      .name = "call depth",
      .unit = "frames",
      .field = "maxCallDepth",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxCallDepth; } },
    { .limit = Limit::StackValues,
      .name = "stack values",
      .unit = "values",
      .field = "maxStackValues",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxStackValues; } },
    { .limit = Limit::NestedInvocations,
      .name = "nested invocations",
      .unit = "invocations",
      .field = "maxNestedInvocations",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxNestedInvocations; } },
    { .limit = Limit::MemoryBytes,
      .name = "memory bytes",
      .unit = "bytes",
      .field = "maxMemoryBytes",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxMemoryBytes; } },
    { .limit = Limit::TableElements,
      .name = "table elements",
      .unit = "elements",
      .field = "maxTableElements",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxTableElements; } },
    { .limit = Limit::Memories,
      .name = "memories",
      .unit = "memories",
      .field = "maxMemories",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxMemories; } },
    { .limit = Limit::Tables,
      .name = "tables",
      .unit = "tables",
      .field = "maxTables",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxTables; } },
    { .limit = Limit::Instances,
      .name = "instances",
      .unit = "module instances",
      .field = "maxInstances",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxInstances; } },
    { .limit = Limit::Exceptions,
      .name = "exceptions",
      .unit = "exceptions",
      .field = "maxExceptions",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxExceptions; } },
    { .limit = Limit::ExceptionValues,
      .name = "exception values",
      .unit = "values",
      .field = "maxExceptionValues",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxExceptionValues; } },
    { .limit = Limit::Types,
      .name = "types",
      .unit = "type units",
      .field = "maxTypes",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxTypes; } },
    { .limit = Limit::Tags,
      .name = "tags",
      .unit = "tags",
      .field = "maxTags",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxTags; } },
    { .limit = Limit::Steps,
      .name = "steps",
      .unit = "steps",
      .field = "maxSteps",
      .value = [](StoreLimits const& limits) noexcept -> uint64_t { return limits.maxSteps; } },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: LimitTable)
        {
            if (static_cast<std::size_t>(row.limit) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastLimit) + 1;
    }(),
    "LimitTable must hold one row per Limit, at the index of its enumerator");

/// The value @p limits gives @p limit.
/// @param limits The limits.
/// @param limit The limit to read.
/// @return Its value, widened to 64 bits.
[[nodiscard]] constexpr uint64_t limitValue(StoreLimits const& limits, Limit limit) noexcept
{
    return LimitTable[static_cast<std::size_t>(limit)].value(limits);
}

/// Why the store refused storage: to allocate a memory or table, or to grow one.
///
/// The enumerators are the rows of RefusalTable, in the same order.
enum class Refusal : uint8_t
{
    TypeMaximum, ///< Past the type's maximum, or what its address type can reach.
    Limit,       ///< Past the store's limit (StoreLimits) of all memories' bytes or all tables' elements.
    HostMemory,  ///< The host ran out of memory.
    Guard,       ///< The embedder's memory has too small a guard region.
    Embedder,    ///< The embedder's allocator refused it.
};

/// The last Refusal enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastRefusal = Refusal::Embedder;

/// One row of RefusalTable: a refusal and how it reads.
struct RefusalRow
{
    Refusal refusal;       ///< The refusal; equal to the row's index.
    std::string_view text; ///< How it reads: "above the store's limit".
};

/// Every refusal, one row each, in Refusal order.
inline constexpr auto RefusalTable = std::to_array<RefusalRow>({
    { .refusal = Refusal::TypeMaximum, .text = "above the type's maximum" },
    { .refusal = Refusal::Limit, .text = "above the store's limit" },
    { .refusal = Refusal::HostMemory, .text = "out of host memory" },
    { .refusal = Refusal::Guard, .text = "guard too small" },
    { .refusal = Refusal::Embedder, .text = "refused by the embedder" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: RefusalTable)
        {
            if (static_cast<std::size_t>(row.refusal) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastRefusal) + 1;
    }(),
    "RefusalTable must hold one row per Refusal, at the index of its enumerator");

/// How @p refusal reads.
/// @param refusal The refusal.
/// @return Its row's text in RefusalTable.
[[nodiscard]] constexpr std::string_view describe(Refusal refusal) noexcept
{
    return RefusalTable[static_cast<std::size_t>(refusal)].text;
}

/// A store limit an allocation or an instantiation would exceed; nothing was allocated for it.
struct LimitExceeded
{
    Limit limit; ///< The limit.

    /// Equal when both name the same limit.
    bool operator==(LimitExceeded const&) const = default;
};

/// How @p exceeded reads.
/// @param exceeded The limit exceeded.
/// @return "<name> limit exceeded", as "memory bytes limit exceeded".
[[nodiscard]] inline std::string describe(LimitExceeded const& exceeded)
{
    return std::format("{} limit exceeded", LimitTable[static_cast<std::size_t>(exceeded.limit)].name);
}

/// Why the store could not allocate a memory or a table, beyond a limit it caps.
struct AllocationFailure
{
    binary::ExternKind kind; ///< Memory or Table.
    Refusal reason;          ///< Why.

    /// Equal when both name the same kind and reason.
    bool operator==(AllocationFailure const&) const = default;
};

/// The first refused grow of a scope, per kind, for the embedder: a refused table.grow never masks a
/// later memory refusal.
struct GrowthRecord
{
    std::optional<Refusal> memory; ///< The first refused memory.grow's reason, if any.
    std::optional<Refusal> table;  ///< The first refused table.grow's reason, if any.

    /// Equal when both name the same refusals.
    bool operator==(GrowthRecord const&) const = default;
};

/// What one instantiation will claim of the store's limits beyond the instance itself: its defined
/// (not imported) memories and tables, their declared minimum sizes, the types it interns anew and
/// its tags.
struct InstanceDemand
{
    std::size_t memories = 0;   ///< The memories it defines.
    std::size_t tables = 0;     ///< The tables it defines.
    uint64_t memoryBytes = 0;   ///< Their minimum bytes together, saturated at 2^64 - 1.
    uint64_t tableElements = 0; ///< Their minimum elements together, saturated at 2^64 - 1.
    uint64_t typeUnits = 0;     ///< The type units of the types the store does not hold yet.
    std::size_t tags = 0;       ///< The tags it defines.
};

/// Why the store did not allocate a memory or a table: a limit it caps, or another refusal.
using AllocationError = std::variant<LimitExceeded, AllocationFailure>;

} // namespace isoline::runtime
