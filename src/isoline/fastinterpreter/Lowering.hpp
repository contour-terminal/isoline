// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <limits>
#include <memory>

#include <isoline/binary/Module.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/LoweringArena.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::fastinterpreter
{

/// The most abstract operand stack entries lowering touches per decoded body unit (bodyUnits()),
/// amortized over the function: LoweringStats::work is at most this many times its bodyUnits.
///
/// Work is counted per entry: its push, its pop and its materialization, each once. An instruction pushes
/// at most one entry (local.tee pops one and pushes it again, a push of its own), and every pop, the
/// results return and end pop included, and every materialization is of an entry some instruction
/// pushed: 1 + 1 + 1. Laying out a frame, O(params + local runs + results) per function, is not counted:
/// local runs are body units, and the params and results are bounded by the type section's limits
/// (binary::DecodeLimits::maxParams, maxResults).
inline constexpr auto WorkPerBodyUnit = uint64_t { 3 };

/// The most slots a frame may have: slot operands are 32 bits wide.
inline constexpr auto MaxFrameSlots = uint64_t { std::numeric_limits<uint32_t>::max() };

/// The codeIndex of a LoweringError refused outside any function: lowering the module's own tables.
inline constexpr auto NoFunction = std::numeric_limits<uint32_t>::max();

/// Why a module was not lowered.
enum class LoweringFailure : uint8_t
{
    /// The module needs more than the compile budget, which is the module's to answer for.
    Budget,
    /// The host had no memory to give, which is not the module's to answer for.
    HostMemory,
    /// A function's frame needs more slots than MaxFrameSlots, which only decode limits raised past
    /// their defaults admit.
    FrameSlots,
};

/// How a module is lowered: fixed for its code, which records both.
struct LoweringConfig
{
    runtime::ExecutionMode mode = runtime::ExecutionMode::Production; ///< The execution mode.
    Dispatch dispatch = Dispatch::Switch; ///< The dispatch the code is for; one of availableDispatches().
};

/// Why a module was not lowered, and where.
struct LoweringError
{
    /// The function lowering was refused at, by its index in the code section, or NoFunction.
    uint32_t codeIndex = 0;
    /// What the refused reservation or allocation asked for, in bytes; for FrameSlots, the slots.
    uint64_t requested = 0;
    /// The arena's budget, the compile budget; for FrameSlots, MaxFrameSlots.
    uint64_t limit = 0;
    LoweringFailure reason = LoweringFailure::Budget; ///< Why.

    /// Whether both record the same refusal.
    [[nodiscard]] bool operator==(LoweringError const&) const = default;
};

/// The most code words lowering emits for one function: MaxWordsPerBodyUnit per body unit, and
/// FunctionOverheadWords.
/// @param function A decoded function.
/// @return The bound, in code words.
[[nodiscard]] constexpr uint64_t codeBoundWords(binary::Function const& function) noexcept
{
    return (bodyUnits(function) * MaxWordsPerBodyUnit) + FunctionOverheadWords;
}

/// What lowering reserves from its arena before it lowers one of @p module's functions, and refuses the
/// module with when the budget cannot hold it: the function's code bound and its local-initialization
/// runs, and with the first function the records of all of them.
///
/// The temporaries of lowering (the abstract operand stack, the forwarding chains, the slots of the
/// locals) are not reserved, but charged as they are allocated, so a refusal still comes at the first
/// allocation past the budget.
/// @param module A decoded module.
/// @param codeIndex The function's index in the code section (checked).
/// @return The bytes.
[[nodiscard]] uint64_t loweringReservation(binary::Module const& module, uint32_t codeIndex);

/// Lowers every function of @p module into slot operations, once, for every instance of it in every store.
///
/// Each function is reserved its loweringReservation() before it is lowered, and every allocation is
/// charged to @p arena, so the module is refused at the first byte past the arena's budget, never after its
/// peak. The pass is iterative and linear: at most MaxWordsPerBodyUnit code words and WorkPerBodyUnit
/// entries of work per body unit (LoweredModule::stats()). A function whose frame needs more than
/// MaxFrameSlots slots is refused before anything is reserved for it.
///
/// Precondition (checked): @p arena is not null; @p config's dispatch is available.
/// @param module The validated module; the lowered module refers to nothing of it.
/// @param config The execution mode and dispatch to lower for.
/// @param arena The arena every container of the code allocates from; the lowered module keeps it.
/// @return The lowered module, or why and where it was refused.
[[nodiscard]] std::expected<std::shared_ptr<LoweredModule const>, LoweringError> lower(
    validate::ValidatedModule const& module,
    LoweringConfig const& config,
    std::shared_ptr<ILoweringArena> arena);

} // namespace isoline::fastinterpreter
