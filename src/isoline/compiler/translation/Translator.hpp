// SPDX-License-Identifier: Apache-2.0
#pragma once

// Translation from a validated WebAssembly function body to the compiler's SSA intermediate
// representation: control flow, locals, numerics, calls, globals, the execution budget's polls and the
// context values. One pass over the decoded body, with explicit stacks and no recursion; every cap of
// CompileLimits a hostile body could push past is refused with its CompileError.

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/CompileLimits.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/runtime/ExecutionMode.hpp>

namespace isoline::validate
{
class ValidatedModule;
} // namespace isoline::validate

namespace isoline::compiler::translation
{

/// What translating any function of a module looks up in the module's index spaces, built once per
/// module: an imported function's or global's entry would otherwise cost a scan of the imports per
/// function, quadratic over a module of many imports and many functions.
class ModuleIndex
{
  public:
    /// The index of @p module, allocated from @p memory.
    /// @param module The module; outlives the index.
    /// @param memory What the index allocates from.
    ModuleIndex(binary::Module const& module, std::pmr::memory_resource& memory);

    /// How many functions the module imports: the first indices of its function index space.
    [[nodiscard]] uint32_t importedFunctionCount() const noexcept
    {
        return static_cast<uint32_t>(_importedFunctionTypes.size());
    }

    /// The type index of function @p function of the function index space (Require: within it).
    [[nodiscard]] uint32_t functionType(uint32_t function) const noexcept;

    /// The type of global @p global of the global index space (Require: within it).
    [[nodiscard]] binary::GlobalType const& globalType(uint32_t global) const noexcept;

    /// How many memories the module has, imported and defined.
    [[nodiscard]] uint32_t memoryCount() const noexcept
    {
        return static_cast<uint32_t>(_importedMemories.size() + _module->memories.size());
    }

    /// The type of memory @p memory of the memory index space (Require: within it).
    [[nodiscard]] binary::MemoryType const& memoryType(uint32_t memory) const noexcept;

    /// How many tables the module imports: the first indices of its table index space.
    [[nodiscard]] uint32_t importedTableCount() const noexcept
    {
        return static_cast<uint32_t>(_importedTables.size());
    }

    /// The type of table @p table of the table index space (Require: within it).
    [[nodiscard]] binary::TableType const& tableType(uint32_t table) const noexcept;

  private:
    binary::Module const* _module;
    std::pmr::vector<uint32_t> _importedFunctionTypes;
    std::pmr::vector<binary::GlobalType const*> _importedGlobals;
    std::pmr::vector<binary::MemoryType const*> _importedMemories;
    std::pmr::vector<binary::TableType const*> _importedTables;
};

/// What translate() translates, and within which caps.
struct TranslationInput
{
    /// The module, validated. Translation refuses a deferred feature where it meets one (checkOpcode(),
    /// checkValueType()), so it needs no checkModule() first.
    validate::ValidatedModule const& module;
    ModuleIndex const& index;    ///< The module's index, built once for all its functions.
    uint32_t codeIndex;          ///< The function, by its index in the code section.
    runtime::ExecutionMode mode; ///< Deterministic canonicalizes every NaN result.
    CompileLimits const& limits; ///< The caps.
    /// Polled before the first instruction and every 65'536 IR instructions after, at a WebAssembly
    /// instruction boundary.
    CompileCancellation const& cancellation;
};

/// Translates one function into SSA.
///
/// The entry block's parameters are the WebAssembly parameters. It first polls at FunctionEntry
/// (pollStored), then enters the frame (enterFrame: the call depth, the stack charge, then the native
/// check), then loads the context values (the step countdown, and memory 0's base and bound when the
/// module has a memory), then gives each declared local its zero, a reference local the null of its own
/// heap. Locals are SSA values: local.get, local.set and local.tee emit nothing. A loop header takes the
/// loop's parameters, the locals the loop assigns (assignedLocalsPerLoop()) and the context values. A
/// forward edge passes its target's results, the locals modified on its path since the target's entry,
/// and the context values; the end's merge takes a parameter only where its edges' values differ, a
/// local an edge did not pass counting as its entry value. Branch arguments are charged against
/// CompileLimits::maxBranchArguments. Every branch
/// to a loop goes through a block of its own that polls at LoopBackEdge, so br_if polls only when taken.
/// Every exit stores the countdown and leaves the frame before returning; every call stores it before
/// and reloads the context values after; a trap site stores it when it changed since. Integer division
/// and trapping truncation check explicitly with trapIf; Deterministic mode canonicalizes the NaN results
/// of NumericRuleTable's rows. Dead code emits nothing; a branch hint makes the unlikely successor cold.
///
/// A load or store is checkMemory(index, base, bound) [memory, offset, size] and a load or store of the
/// guarded address it returns; the index keeps the memory's address type (intermediate/Guards.hpp). A
/// 64-bit memory's access whose offset + size alone passes 2^64 - 1 traps OutOfBoundsMemoryAccess after
/// its operands. Memory 0's base and bound are the context values, another memory's memoryBase and
/// memoryBound, loaded once per block and again after a call or stub. memory.size is the bound shifted by
/// the page size's log2. memory.grow, the bulk operations, the drops, table.grow and table.fill, and
/// table.get and table.set of an imported table call their runtime stub (StubCallTable), which reloads
/// the context values after; a 32-bit memory's or table's size or grow result is narrowed by
/// i32.wrap_i64. A defined table's table.get and table.set are inline: checkTable
/// (OutOfBoundsTableAccess), then loadTableElement or storeTableElement. call_indirect is checkTable
/// (UndefinedElement), loadTableElement, trapIfSubject of a null element (UninitializedElement),
/// checkDispatch, the type compare (IndirectCallTypeMismatch), then callIndirect of the dispatch row.
/// @param input The function and its caps.
/// @param memory The compile's arena, which the function and every temporary allocate from. A refusal of
///               it throws std::bad_alloc, which the compile's boundary turns into its error.
/// @return The function, well-formed (intermediate::verify()); or FunctionTooLarge, TooManyInstructions,
///         TooManyBlockParameters, TooManyBranchArguments, PoolTooLarge, UnsupportedFeature or Cancelled,
///         naming the function.
[[nodiscard]] std::expected<intermediate::Function, CompileError> translate(
    TranslationInput const& input, std::pmr::memory_resource& memory);

} // namespace isoline::compiler::translation
