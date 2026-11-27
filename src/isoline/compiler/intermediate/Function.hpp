// SPDX-License-Identifier: Apache-2.0
#pragma once

// The compiler's SSA intermediate representation: a function is blocks of instructions over dense value
// IDs, with block parameters in place of phi nodes and branch arguments on the branch. Operands, results,
// block parameters and immediates live in per-function pools that instructions and blocks name by range,
// so a call with a thousand arguments costs what a call with one does, per argument. Every container
// allocates from the function's memory resource: the compile's arena.

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/runtime/Trap.hpp>

namespace isoline::compiler::intermediate
{

/// An SSA value: a block parameter or an instruction's result, numbered densely from 0 in the order
/// they were created (compact() renumbers them in block order).
enum class ValueId : uint32_t
{
};

/// A block, numbered densely from 0 in the order they were created; block 0 is the entry, whose
/// parameters are the function's.
enum class BlockId : uint32_t
{
};

/// A run of values in a Function's operand pool: an instruction's operands or results, a block's
/// parameters.
struct OperandRange
{
    uint32_t begin = 0; ///< The pool index of the first value.
    uint32_t count = 0; ///< How many values.

    /// Whether both name the same run.
    [[nodiscard]] bool operator==(OperandRange const&) const = default;
};

/// A run of immediates in a Function's immediate pool.
struct ImmediateRange
{
    uint32_t begin = 0; ///< The pool index of the first immediate.
    uint32_t count = 0; ///< How many immediates.

    /// Whether both name the same run.
    [[nodiscard]] bool operator==(ImmediateRange const&) const = default;
};

/// Where a terminator may go: a block, and the arguments it passes to that block's parameters, as a
/// run of the terminator's operands.
struct BranchTarget
{
    BlockId block {};          ///< The target.
    OperandRange arguments {}; ///< Its arguments, in the operand pool.

    /// Whether both name the same target with the same arguments.
    [[nodiscard]] bool operator==(BranchTarget const&) const = default;
};

/// What Builder takes for a target: a block and the values passed to its parameters.
struct BranchTo
{
    BlockId block {};                      ///< The target.
    std::span<ValueId const> arguments {}; ///< Its arguments.
};

/// One instruction: an operation of Operations.def over runs of the function's pools.
struct Instruction
{
    Operation operation {};       ///< What it does; Removed once a pass removed it.
    Type type {};                 ///< Its first result's type; I32 when it has no result.
    OperandRange operands {};     ///< Its operands.
    ImmediateRange immediates {}; ///< Its immediates, laid out as its row's ImmediateSchema says.
    OperandRange results {};      ///< The values it defines.
};

/// Whether a block is expected to run: a cold block is laid out at the end of its innermost loop.
enum class Hotness : uint8_t
{
    Normal, ///< Expected to run.
    Cold,   ///< Expected not to: a branch hint's unlikely successor.
};

/// One block: its parameters, and its instructions in order, the last a terminator.
struct Block
{
    OperandRange parameters {};              ///< The values it defines on entry.
    std::pmr::vector<uint32_t> instructions; ///< Its instructions, as indices into the function's.
    Hotness hotness = Hotness::Normal;       ///< Whether it is expected to run.
};

/// The entry of a per-value, per-block or per-number array that names nothing: a value not numbered yet
/// (compact()), a block not reachable, in no loop or without a parent (Dominators). The one such value.
inline constexpr auto Unnumbered = std::numeric_limits<uint32_t>::max();

/// The most entries a Function's operand or immediate pool can hold: what a 32-bit pool index addresses.
/// Builder Requires it; a translation of guest input refuses before it (CompileLimits::maxPoolEntries).
inline constexpr auto MaxPoolEntries = uint64_t { std::numeric_limits<uint32_t>::max() };

/// A function of the intermediate representation. Movable, not copyable: a copy would leave the arena.
class Function
{
  public:
    /// An empty function, every container of which allocates from @p memory.
    /// @param memory The compile's arena; outlives the function.
    explicit Function(std::pmr::memory_resource& memory);

    Function(Function const&) = delete;
    Function& operator=(Function const&) = delete;
    Function(Function&&) noexcept = default;
    /// Takes @p other's contents. Both must allocate from the same resource (Require): between two, the
    /// containers would copy into this one's while memory() named the other's.
    Function& operator=(Function&& other) noexcept;
    ~Function() = default;

    /// The resource every container of the function allocates from.
    [[nodiscard]] std::pmr::memory_resource& memory() const noexcept { return *_memory; }

    /// How many blocks there are.
    [[nodiscard]] uint32_t blockCount() const noexcept { return static_cast<uint32_t>(_blocks.size()); }

    /// The block @p block.
    /// @param block A block of the function.
    [[nodiscard]] Block const& block(BlockId block) const noexcept;

    /// The block @p block, for a pass to change.
    /// @param block A block of the function.
    [[nodiscard]] Block& block(BlockId block) noexcept;

    /// How many instructions there are, removed ones included until compact().
    [[nodiscard]] uint32_t instructionCount() const noexcept
    {
        return static_cast<uint32_t>(_instructions.size());
    }

    /// The instruction at @p index.
    /// @param index An index below instructionCount().
    [[nodiscard]] Instruction const& instruction(uint32_t index) const noexcept;

    /// The instruction at @p index, for a pass to change.
    /// @param index An index below instructionCount().
    [[nodiscard]] Instruction& instruction(uint32_t index) noexcept;

    /// How many values there are.
    [[nodiscard]] uint32_t valueCount() const noexcept { return static_cast<uint32_t>(_valueTypes.size()); }

    /// The type of @p value.
    /// @param value A value of the function.
    [[nodiscard]] Type typeOf(ValueId value) const noexcept;

    /// The size of the operand pool, which every OperandRange of the function lies within.
    [[nodiscard]] std::size_t operandPoolSize() const noexcept { return _operands.size(); }

    /// The size of the immediate pool, which every ImmediateRange of the function lies within.
    [[nodiscard]] std::size_t immediatePoolSize() const noexcept { return _immediates.size(); }

    /// The operand pool entries the next Builder::addParameter() of @p block appends: none while its
    /// reserved room lasts, else the twice as large room its parameters move to.
    /// @param block A block of the function.
    [[nodiscard]] uint64_t parameterGrowth(BlockId block) const noexcept;

    /// The values of @p range.
    /// @param range A run within the operand pool.
    [[nodiscard]] std::span<ValueId const> operands(OperandRange range) const noexcept;

    /// The values of @p range, for a pass to change.
    /// @param range A run within the operand pool.
    [[nodiscard]] std::span<ValueId> operands(OperandRange range) noexcept;

    /// The immediates of @p range.
    /// @param range A run within the immediate pool.
    [[nodiscard]] std::span<uint64_t const> immediates(ImmediateRange range) const noexcept;

    /// The immediates of @p range, for a pass to change.
    /// @param range A run within the immediate pool.
    [[nodiscard]] std::span<uint64_t> immediates(ImmediateRange range) noexcept;

    /// Calls @p visit with each target of @p instruction, in its immediates' order: switch's fallback
    /// first. An instruction whose schema has no target fields has none.
    /// @param instruction A well-formed instruction of the function (verify()).
    /// @param visit Called as visit(BranchTarget).
    template <typename Visit>
    void forEachTarget(Instruction const& instruction, Visit const& visit) const;

    /// The targets of @p instruction, as forEachTarget() visits them.
    /// @param instruction A well-formed instruction of the function.
    /// @return Them, allocated from memory().
    [[nodiscard]] std::pmr::vector<BranchTarget> targets(Instruction const& instruction) const;

    /// The terminator of @p block: its last instruction that is not removed, when that is a terminator.
    /// @param block A block of the function whose instruction indices lie within it.
    /// @return Its index; none when the block does not end with a terminator.
    [[nodiscard]] std::optional<uint32_t> terminator(BlockId block) const noexcept;

    /// The successors of @p block: its terminator's targets' blocks, in their order, repeated when a
    /// terminator names one twice. None when it has no terminator().
    /// @param block A well-formed block of the function.
    /// @return Them, allocated from memory().
    [[nodiscard]] std::pmr::vector<BlockId> successors(BlockId block) const;

    /// Marks the instruction at @p index Removed; its block keeps listing it until compact().
    /// @param index An index below instructionCount().
    void remove(uint32_t index) noexcept;

    /// Removes the blocks @p removed, their instructions marked Removed, and renumbers the others densely
    /// in their order, every target updated. For a pass that dropped blocks no kept block branches to: the
    /// unreachable ones, or one merged into its predecessor. A value a removed block defined must have no
    /// use left in a kept one (compact() Requires it).
    /// @param removed The blocks, in ascending order, each once; never the entry (Require), and no kept
    ///                block's terminator may target one (Require).
    void removeBlocks(std::span<BlockId const> removed);

    /// Drops removed instructions and those no block lists, and renumbers what is left densely: values
    /// in block order (parameters, then each instruction's results), instructions in block order, every
    /// pool repacked. No instruction left may use a value whose definition was removed (Require).
    void compact();

  private:
    friend class Builder;

    std::pmr::memory_resource* _memory;
    std::pmr::vector<Block> _blocks;
    /// Per block, the pool slots reserved for its parameters, from parameters.begin on: addParameter()
    /// moves a block's parameters to the pool's end with twice the room when they fill it.
    std::pmr::vector<uint32_t> _parameterCapacity;
    std::pmr::vector<Instruction> _instructions;
    std::pmr::vector<ValueId> _operands;
    std::pmr::vector<uint64_t> _immediates;
    std::pmr::vector<Type> _valueTypes;
};

/// Appends blocks, block parameters and instructions to a Function. Leaves the checking to verify().
class Builder
{
  public:
    /// A builder of @p function, with no current block.
    /// @param function The function to append to; outlives the builder.
    explicit Builder(Function& function) noexcept;

    /// Appends an empty block.
    /// @return Its ID.
    [[nodiscard]] BlockId createBlock();

    /// Appends a parameter to @p block, which may already hold instructions.
    /// @param block A block of the function.
    /// @param type The parameter's type.
    /// @return The value it defines.
    [[nodiscard]] ValueId addParameter(BlockId block, Type type);

    /// Makes @p block the one instructions are appended to.
    /// @param block A block of the function.
    void switchTo(BlockId block) noexcept;

    /// Appends an instruction to the current block (switchTo(), Require).
    /// @param operation What it does.
    /// @param resultTypes The types of the values it defines.
    /// @param operands Its operands.
    /// @param immediates Its immediates, laid out as operation's row says.
    /// @return The values it defines, in the operand pool: valid until the function next changes.
    [[nodiscard]] std::span<ValueId const> emit(Operation operation,
                                                std::span<Type const> resultTypes,
                                                std::span<ValueId const> operands,
                                                std::span<uint64_t const> immediates = {});

    /// Ends the current block with a jump to @p target, passing @p arguments.
    void jump(BlockId target, std::span<ValueId const> arguments);

    /// Ends the current block with a branch on @p condition: to @p ifNonZero with @p ifNonZeroArguments
    /// when it is not zero, else to @p ifZero with @p ifZeroArguments.
    void branch(ValueId condition,
                BlockId ifNonZero,
                std::span<ValueId const> ifNonZeroArguments,
                BlockId ifZero,
                std::span<ValueId const> ifZeroArguments);

    /// Ends the current block with a switch on @p index: to targets[index] when the index is below
    /// targets.size(), else to @p fallback.
    void switchOn(ValueId index, std::span<BranchTo const> targets, BranchTo fallback);

    /// Replaces the operands of the instruction at @p index by @p operands, appended to the operand pool
    /// as a new run; the old run stays unused until compact(). For a pass or a translation that settles a
    /// terminator's arguments after emitting it, which also updates the argument counts it changes.
    /// @param index An instruction of the function.
    /// @param operands Its new operands.
    void replaceOperands(uint32_t index, std::span<ValueId const> operands);

    /// Replaces the immediates of the instruction at @p index by @p immediates, appended to the immediate
    /// pool as a new run; the old run stays unused until compact(). For a pass that rewrites an
    /// instruction into another operation, such as a folded one into its constant.
    /// @param index An instruction of the function.
    /// @param immediates Its new immediates.
    void replaceImmediates(uint32_t index, std::span<uint64_t const> immediates);

    /// Ends the current block by returning @p values.
    void ret(std::span<ValueId const> values);

    /// Ends the current block with a trap of @p code.
    void trap(runtime::TrapCode code);

  private:
    /// Appends an instruction whose operands are @p operandParts, concatenated.
    std::span<ValueId const> append(Operation operation,
                                    std::span<Type const> resultTypes,
                                    std::span<std::span<ValueId const> const> operandParts,
                                    std::span<uint64_t const> immediates);

    /// Appends a value of @p type.
    ValueId createValue(Type type);

    Function* _function;
    std::optional<BlockId> _current; ///< The block emit() appends to; none before switchTo().
};

template <typename Visit>
void Function::forEachTarget(Instruction const& instruction, Visit const& visit) const
{
    auto const& row = operationRow(instruction.operation);
    auto const& schema = immediateSchemaRow(row.immediates);
    auto const values = immediates(instruction.immediates);
    auto argumentBegin = uint64_t { instruction.operands.begin } + operandSignatureRow(row.operands).count;
    auto index = std::size_t { 0 };
    while (index < values.size())
    {
        if (immediateFieldAt(schema, index) != ImmediateField::Target)
        {
            ++index;
            continue;
        }
        Require(index + 1 < values.size());
        auto const count = values[index + 1];
        visit(BranchTarget { .block = static_cast<BlockId>(values[index]),
                             .arguments = OperandRange { .begin = static_cast<uint32_t>(argumentBegin),
                                                         .count = static_cast<uint32_t>(count) } });
        argumentBegin += count;
        index += 2;
    }
}

} // namespace isoline::compiler::intermediate
