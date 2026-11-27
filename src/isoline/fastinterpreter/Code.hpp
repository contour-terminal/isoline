// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/LoweringAllocator.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/ModuleCode.hpp>

namespace isoline::fastinterpreter
{

/// One cell of a frame: every value takes one slot, or two (a v128, a reference; see SlotClass).
using Slot = uint64_t;

/// One cell of lowered code: an operation's header word, two 32-bit slot operands, or an immediate.
using Word = uint64_t;

/// The layout of what follows an operation's header word in the code: which operands it reads and
/// writes, and its immediates. Listings and the lowering tables group operations by it.
enum class OperationForm : uint8_t
{
    Nullary,         ///< Nothing.
    Unary,           ///< A destination slot and one source slot.
    Binary,          ///< A destination slot and two source slots.
    BinaryImmediate, ///< A destination slot, one source slot and an immediate, the folded constant operand.
    Move,            ///< A destination and its source: a slot, a run of slots, or an immediate.
    Load,            ///< A destination slot, an address slot and the memory argument.
    Store,           ///< An address slot, a value slot and the memory argument.
    Branch,      ///< Its condition's slots, if any, and immediates, the last of them the target code offset.
    BranchMove,  ///< A condition slot, the destination and source slots of the values it moves when it
                 ///< branches, and the target code offset with the values' slot count (BranchTargetMask).
    BranchTable, ///< An index slot, the values' run, and one fixed-size record per label.
    Call,        ///< The callee and the base slot of its arguments, which becomes its frame.
    Bulk,        ///< The operand slots of a bulk memory or table operation and its indices.
    Special,     ///< A layout of its own, which the operation documents.
};

/// The operations a dispatch executes alike: which tail-call handler file holds their handlers, and whether
/// the switch dispatch runs them inline or out of line (OperationFamilyTable).
enum class OperationFamily : uint8_t
{
    Control,    ///< Moves, constants, branches, calls and leaving the function.
    Numeric,    ///< The numeric operations and their immediate twins.
    Variable,   ///< The instance's state reached through the store: globals, memories other than memory 0,
                ///< tables, segments, references to its functions and types, and the bulk operations.
    Memory,     ///< Linear memory access.
    Simd,       ///< The vector operations.
    Exceptions, ///< Throwing and catching.
};

/// Where the switch dispatch runs the operations of a family.
enum class SwitchPlacement : uint8_t
{
    Inline,    ///< In its loop: the hot operations.
    OutOfLine, ///< In another translation unit, which the loop calls: the cold ones, kept out of the loop's
               ///< frame and code.
};

/// What the dispatches know about one OperationFamily.
struct OperationFamilyRow
{
    OperationFamily family;    ///< The family; equal to the row's index in OperationFamilyTable.
    std::string_view name;     ///< Its name, as the tail-call handler files' names spell it.
    SwitchPlacement placement; ///< Where the switch dispatch runs its operations.
};

/// Every OperationFamily, in enumerator order.
inline constexpr auto OperationFamilyTable = std::to_array<OperationFamilyRow>({
    { .family = OperationFamily::Control, .name = "Control", .placement = SwitchPlacement::Inline },
    { .family = OperationFamily::Numeric, .name = "Numeric", .placement = SwitchPlacement::Inline },
    { .family = OperationFamily::Variable, .name = "Variable", .placement = SwitchPlacement::OutOfLine },
    { .family = OperationFamily::Memory, .name = "Memory", .placement = SwitchPlacement::Inline },
    { .family = OperationFamily::Simd, .name = "Simd", .placement = SwitchPlacement::OutOfLine },
    { .family = OperationFamily::Exceptions, .name = "Exceptions", .placement = SwitchPlacement::OutOfLine },
});

/// Every operation lowered code holds, one enumerator per row of Operations.def, in its order.
///
/// The base type is the smallest that holds every row (more than 256 since the fused branches and their
/// loop twins); an enumerator that outgrows it is a build error, and the row that adds it widens it.
enum class OperationId : uint16_t
{
#define ISOLINE_FAST_OPERATION(Name, Text, Form, Slots, Immediates, Family) Name,
#include <isoline/fastinterpreter/Operations.def>
#undef ISOLINE_FAST_OPERATION
};

/// The code words that @p slots slot operands and @p immediates immediates take after a header word:
/// the slot operands two to a word, then one word per immediate.
/// @param slots The number of 32-bit slot operands.
/// @param immediates The number of 64-bit immediates.
/// @return The header word plus the operand words.
[[nodiscard]] constexpr uint32_t operationWords(uint32_t slots, uint32_t immediates) noexcept
{
    return 1 + ((slots + 1) / 2) + immediates;
}

/// What names the function a call operation calls (CallTable).
enum class CalleeKind : uint8_t
{
    Internal,  ///< A function of the caller's own module, by its code index: no look-up through the store.
    Import,    ///< An imported function, by its function index, through the running instance and the store.
    Table,     ///< A table element, by the index on the operand stack (call_indirect).
    Reference, ///< A function reference on the operand stack (call_ref).
};

/// What a call operation does with its caller's frame.
enum class CallMode : uint8_t
{
    Call, ///< Keeps it: the caller goes on after the callee returns.
    Tail, ///< Replaces it with the callee's (return_call and its twins).
};

/// What the operation table knows about one operation.
struct OperationRow
{
    OperationId id;        ///< The operation; equal to the row's index in OperationTable.
    std::string_view name; ///< The enumerator's own name.
    std::string_view text; ///< Its name in listings.
    OperationForm form;    ///< The layout of its operands.
    uint32_t slots;        ///< Its 32-bit slot operands, destination first, two to a code word.
    uint32_t immediates;   ///< Its 64-bit immediates, one code word each, after the slot operands.
    uint32_t words; ///< Its fixed size in code words, header included; a br_table adds its labels' records.
    OperationFamily family; ///< The operations it is executed with.
};

/// Every operation, one row per OperationId enumerator, in enumerator order.
inline constexpr auto OperationTable = std::to_array<OperationRow>({
#define ISOLINE_FAST_OPERATION(Name, Text, Form, Slots, Immediates, Family) \
    { .id = OperationId::Name,                                              \
      .name = #Name,                                                        \
      .text = (Text),                                                       \
      .form = OperationForm::Form,                                          \
      .slots = (Slots),                                                     \
      .immediates = (Immediates),                                           \
      .words = operationWords((Slots), (Immediates)),                       \
      .family = OperationFamily::Family },
#include <isoline/fastinterpreter/Operations.def>
#undef ISOLINE_FAST_OPERATION
});

/// The number of operations in OperationTable.
inline constexpr auto OperationCount = OperationTable.size();

namespace detail
{
    /// Whether every row of OperationTable sits at the index of its id.
    [[nodiscard]] consteval bool operationRowsInIdOrder() noexcept
    {
        return std::ranges::all_of(
            std::views::iota(std::size_t { 0 }, OperationCount),
            [](std::size_t index) { return std::to_underlying(OperationTable[index].id) == index; });
    }
} // namespace detail

static_assert(detail::operationRowsInIdOrder(), "OperationTable holds one row per OperationId, in order");

namespace detail
{
    /// Whether every row of OperationFamilyTable sits at the index of its family.
    [[nodiscard]] consteval bool familyRowsInOrder() noexcept
    {
        return std::ranges::all_of(std::views::iota(std::size_t { 0 }, OperationFamilyTable.size()),
                                   [](std::size_t index) {
                                       return std::to_underlying(OperationFamilyTable[index].family) == index;
                                   });
    }
} // namespace detail

static_assert(detail::familyRowsInOrder(),
              "OperationFamilyTable holds one row per OperationFamily, in order");

/// What the table knows about @p family.
/// @param family The family.
/// @return Its row of OperationFamilyTable.
[[nodiscard]] constexpr OperationFamilyRow const& familyRow(OperationFamily family) noexcept
{
    return OperationFamilyTable[std::to_underlying(family)];
}

/// What the table knows about @p id.
/// @param id The operation.
/// @return Its row of OperationTable.
[[nodiscard]] constexpr OperationRow const& operationRow(OperationId id) noexcept
{
    return OperationTable[std::to_underlying(id)];
}

/// Whether a transfer of control polls the interrupt: one that goes to a loop label does, as a loop
/// back-edge (runtime::PollSite::LoopBackEdge), through the executor's runtime::PollGate. Decided once,
/// when the code is lowered.
enum class LoopPoll : uint8_t
{
    No,  ///< A forward transfer, to a block, if or try_table label, or the function's end.
    Yes, ///< A transfer to a loop label.
};

/// The bits of a branch's target word that hold the target, a word offset in the function's code. The high
/// 32 bits hold the values' slot count of a BranchMove operation and the destination slot of a br_table
/// record, and are zero otherwise.
inline constexpr auto BranchTargetMask = Word { 0xFFFF'FFFF };

/// The code words of one label's record of a br_table, which follow the operation's own words, one record
/// per label and the default last: the target (BranchTargetMask) with the destination slot of the label's
/// values in the high 32 bits, then the LoopPoll.
inline constexpr auto BranchTableEntryWords = uint32_t { 2 };

/// What names the access of a load or store of a memory other than memory 0 (LoadIndexed, StoreIndexed): the
/// memory's index in the module, and the memory 0 operation of the same access and address type, whose body
/// the access runs over the other memory's bytes.
struct IndexedAccess
{
    uint32_t memory = 0;                   ///< The memory's index in the module's memory index space.
    OperationId access = OperationId::Nop; ///< The memory 0 operation of the same access.

    /// Whether both name the same access of the same memory.
    [[nodiscard]] constexpr bool operator==(IndexedAccess const&) const = default;
};

/// The immediate word of @p access: the memory in the low 32 bits, the operation in the high ones.
/// @param access The access.
/// @return Its word.
[[nodiscard]] constexpr Word indexedAccessWord(IndexedAccess access) noexcept
{
    return Word { access.memory } | (Word { std::to_underlying(access.access) } << 32U);
}

/// The access an immediate word of indexedAccessWord() names.
/// @param word The word.
/// @return The access.
[[nodiscard]] constexpr IndexedAccess indexedAccessOf(Word word) noexcept
{
    return IndexedAccess { .memory = static_cast<uint32_t>(word),
                           .access = static_cast<OperationId>(word >> 32U) };
}

/// The immediate word of two indices a bulk operation names (Operations.def): @p low in the low 32 bits,
/// @p high in the high ones.
/// @param low The first index: the destination's, or the segment's.
/// @param high The second index: the source's, or the memory's or table's a segment goes to; 0 where the
///        operation names one index only.
/// @return Its word.
[[nodiscard]] constexpr Word indexPairWord(uint32_t low, uint32_t high) noexcept
{
    return Word { low } | (Word { high } << 32U);
}

/// The first index of an indexPairWord().
/// @param word The word.
/// @return Its low 32 bits.
[[nodiscard]] constexpr uint32_t firstIndexOf(Word word) noexcept
{
    return static_cast<uint32_t>(word);
}

/// The second index of an indexPairWord().
/// @param word The word.
/// @return Its high 32 bits.
[[nodiscard]] constexpr uint32_t secondIndexOf(Word word) noexcept
{
    return static_cast<uint32_t>(word >> 32U);
}

/// The immediate word of @p heap, a heap as a module names it (ref.null's): its kind in the low 32 bits, its
/// type index in the high ones.
/// @param heap The heap.
/// @return Its word.
[[nodiscard]] constexpr Word heapWord(binary::HeapType heap) noexcept
{
    return indexPairWord(std::to_underlying(heap.kind), heap.typeIndex);
}

/// The heap a heapWord() names.
/// @param word The word.
/// @return The heap.
[[nodiscard]] constexpr binary::HeapType heapOf(Word word) noexcept
{
    return binary::HeapType { .kind = static_cast<binary::AbstractHeapType>(firstIndexOf(word)),
                              .typeIndex = secondIndexOf(word) };
}

/// The header word of @p operation in code lowered for @p dispatch: its OperationId under Switch, the address
/// of its handler under TailCall.
///
/// Precondition (checked): @p dispatch is available.
/// @param dispatch The dispatch the code is for.
/// @param operation The operation.
/// @return Its header word.
[[nodiscard]] Word headerWord(Dispatch dispatch, OperationId operation);

/// The operation a header word of code lowered for any available dispatch is the header of: what a listing
/// reads, whichever dispatch the code is for.
///
/// Precondition (checked): @p header is the header word of some operation (headerWord()).
/// @param header The header word.
/// @return Its operation.
[[nodiscard]] OperationId operationOfHeader(Word header);

/// The code words of the operation that @p code starts with: its row's words, and a br_table's records.
///
/// Precondition (checked): @p code holds the whole operation.
/// @param operation The operation, as its header word names it (operationOfHeader()).
/// @param code The code from its header word on.
/// @return Its words, header included.
[[nodiscard]] std::size_t operationExtent(OperationId operation, std::span<Word const> code);

/// The parent of a root OperandNode, and a SiteRecord's operands when the stack below it is empty.
inline constexpr auto NoNode = std::numeric_limits<uint32_t>::max();

/// The parent of an outermost HandlerRecord, and a SiteRecord's handler outside every try_table.
inline constexpr auto NoHandler = std::numeric_limits<uint32_t>::max();

/// What an operand stack entry described by an OperandNode is.
enum class OperandNodeKind : uint8_t
{
    Slot,     ///< A value in a frame slot.
    Local,    ///< A forwarded local.get: the value is the local's, which no slot copies.
    Constant, ///< A folded constant: the value is an immediate of the code, which no slot copies.
    Run,      ///< The results of a call, block or try_table: a run of consecutive slots.
};

/// One entry, or run of entries, of a function's operand stack at some point of its code.
///
/// The nodes of a function form a persistent tree, one node per pushing instruction: a node's parent
/// is the stack below it, so the whole stack at a site is one node index (SiteRecord::operands),
/// whatever its height. This is how the exception roots name every operand of a frame at O(1) per
/// instruction.
struct OperandNode
{
    OperandNodeKind kind = OperandNodeKind::Slot; ///< What the entry is.
    uint32_t parent = NoNode;                     ///< The node of the stack below it, or NoNode.
    uint32_t source = 0;          ///< The slot base for Slot and Run, the local index for Local, the
                                  ///< constant's index for Constant.
    uint32_t typeOrSignature = 0; ///< The entry's value type or, for a Run, the signature whose results it
                                  ///< holds, as an index the lowering assigns.
};

/// One call or throw site of a function: what the exception roots and unwinding need there.
struct SiteRecord
{
    uint32_t codeOffset = 0;    ///< The word offset of the site's operation in LoweredFunction::code.
    uint32_t operands = NoNode; ///< The operand stack below the call's arguments or the throw's payload.
    uint32_t innermostHandler = NoHandler; ///< The innermost try_table around the site, or NoHandler.
    uint32_t labelDepth = 0;               ///< The block, loop, if and try_table labels open at the site.
};

/// One try_table of a function: its catch clauses, and the try_table around it.
struct HandlerRecord
{
    uint32_t labelDepth = 0;     ///< The labels open inside it, its own included.
    uint32_t clausesBegin = 0;   ///< Its first clause in LoweredFunction::catches.
    uint32_t clausesCount = 0;   ///< The number of its clauses.
    uint32_t parent = NoHandler; ///< The try_table around it, or NoHandler.
};

/// One catch clause of a try_table, lowered: what it catches and where it goes.
struct CatchRecord
{
    binary::CatchKind kind = binary::CatchKind::Catch; ///< What it catches, and what it hands its label.
    uint32_t tag = 0;        ///< The tag it catches, as the module indexes it; Catch and CatchRef only.
    uint32_t target = 0;     ///< The word offset in LoweredFunction::code its label continues at.
    uint32_t resultSlot = 0; ///< The first slot of what it hands the label: the payload, then the exnref.
    LoopPoll poll = LoopPoll::No; ///< Whether its label is a loop's, so taking it polls.
};

/// How a LocalInitRun initializes its slots.
enum class LocalInit : uint8_t
{
    Zero, ///< Every slot zero: the default of the number and vector types.
    Null, ///< Every pair of slots the null reference of the run's heap.
};

/// One run of a function's declared locals, all initialized alike when a frame is entered.
///
/// Derived from the module's run-length local declarations, so a function has O(declarations) runs.
/// The heap is the module's: a concrete heap names a type index of the module, which frame entry maps
/// through the running instance (nullSlots()), never a TypeId baked into code shared by every store.
struct LocalInitRun
{
    uint32_t slotBase = 0;            ///< The run's first slot.
    uint32_t slotCount = 0;           ///< The slots it covers; two per local for a Null run.
    LocalInit kind = LocalInit::Zero; ///< What its slots start as.
    binary::HeapType heap {};         ///< The null's heap, as the module names it; Null runs only.
};

/// The most code words lowering emits per decoded body unit (bodyUnits()), amortized over the function.
///
/// Every operation is a fixed number of words (OperationRow::words), at most this many, value moves
/// (MoveRange) included; a br_table adds one record of BranchTableEntryWords per label, each label a unit of
/// its own. What a unit emits beyond its own operation is charged to another unit, each at most once: a
/// value materialized late (a Const64 or a move, at most three words) to the local.get or constant that
/// pushed it, which emits nothing itself; the MoveRange before a br's jump to the else or end its dead code
/// runs to, which emits nothing after dead code. So a function's code is at most
/// bodyUnits() × MaxWordsPerBodyUnit + FunctionOverheadWords words in total, though not at every single unit.
inline constexpr auto MaxWordsPerBodyUnit = uint64_t { 4 };

/// The code words lowering may emit per function beyond MaxWordsPerBodyUnit's: its entry and its return.
inline constexpr auto FunctionOverheadWords = uint64_t { 16 };

namespace detail
{
    /// Whether every operation of OperationTable fits the share of code one body unit may emit.
    [[nodiscard]] consteval bool everyOperationFitsAUnit() noexcept
    {
        return std::ranges::all_of(OperationTable,
                                   [](OperationRow const& row) { return row.words <= MaxWordsPerBodyUnit; });
    }
} // namespace detail

static_assert(detail::everyOperationFitsAUnit(),
              "an operation larger than MaxWordsPerBodyUnit breaks the bound on lowered code");
static_assert(BranchTableEntryWords <= MaxWordsPerBodyUnit,
              "a br_table record larger than its label's share breaks the bound on lowered code");

/// What lowering a module took: the measures its published bounds are stated in.
struct LoweringStats
{
    uint64_t bodyUnits = 0; ///< The decoded body units of every function (bodyUnits()), summed.
    /// The abstract operand stack entries lowering touched: each push, pop and materialization one, a run
    /// of a block's values pushed at once one too, and each look-up and binding of a branch's label one.
    /// At most WorkPerBodyUnit per body unit, so lowering's time is linear in the module.
    uint64_t work = 0;

    /// Whether both record the same measures.
    [[nodiscard]] bool operator==(LoweringStats const&) const = default;
};

/// The decoded body units of @p function: its instructions, br_table labels, catch clauses, typed
/// select types and local runs, what the bound on lowered code is counted in.
///
/// The decoder keeps no body length, but each unit took at least one byte of the body, so a bound per
/// unit is at most the same bound per body byte.
/// @param function A decoded function.
/// @return The number of its units.
[[nodiscard]] constexpr uint64_t bodyUnits(binary::Function const& function) noexcept
{
    return uint64_t { function.body.size() } + function.brTableLabels.size() + function.catchClauses.size()
           + function.selectTypes.size() + function.locals.size();
}

/// One function's lowered code and the records its execution needs.
///
/// A frame is laid out as the parameters, then the declared locals (together localSlots), then the
/// operand stack, frameSlots in all. Every container allocates its storage from the lowering arena
/// (LoweringAllocator), which the LoweredModule holding the function keeps alive.
struct LoweredFunction
{
    /// A function with no code yet, whose containers allocate their storage from @p resource. Not noexcept: a
    /// standard library may allocate bookkeeping in a container's constructor (MSVC's debug containers
    /// allocate a proxy, from the process heap: LoweringAllocator), which may fail.
    /// @param resource The lowering arena.
    explicit LoweredFunction(std::pmr::memory_resource* resource):
        code(resource),
        localInit(resource),
        operands(resource),
        sites(resource),
        handlers(resource),
        catches(resource)
    {
    }

    uint32_t codeIndex = 0;  ///< Its index in the code section (its function index less the imports).
    uint64_t frameCost = 0;  ///< The stack values entering it charges (validate::ValidatedModule::frameCost).
    uint32_t paramSlots = 0; ///< The slots of its parameters, which open the frame.
    uint32_t localSlots = 0; ///< The slots of its parameters and declared locals: where operands begin.
    /// The slots of its whole frame: the highest operand stack, and at least localSlots + resultSlots, so
    /// that the frame holds its results however its body ends.
    uint32_t frameSlots = 0;
    uint32_t resultSlots = 0; ///< The slots of its results.

    LoweringVector<Word> code;              ///< Its operations.
    LoweringVector<LocalInitRun> localInit; ///< How its declared locals start, in slot order.
    LoweringVector<OperandNode> operands;   ///< Its operand stack tree, which sites name nodes of.
    LoweringVector<SiteRecord> sites;       ///< Its call and throw sites, in code order.
    LoweringVector<HandlerRecord> handlers; ///< Its try_tables.
    LoweringVector<CatchRecord> catches;    ///< Their catch clauses, one run per handler.
};

/// A module's code as the fast interpreter runs it: every defined function, lowered once, at load.
///
/// Immutable and shared by every instance of the module in every store, so it holds no TypeId and no
/// address: what differs between instances is resolved through the running instance.
class LoweredModule final: public runtime::ModuleCode
{
  public:
    /// The lowered code of a module.
    ///
    /// Precondition (checked): @p functions and every container of each function allocate from
    /// @p arena, which the module keeps alive as long as they are.
    /// @param arena The lowering arena every container allocates from.
    /// @param functions The defined functions, by code index.
    /// @param dispatch The dispatch the code's header words are for.
    /// @param mode The execution mode the code was lowered in.
    /// @param loweredBytes What the arena charged for lowering the module.
    /// @param stats What lowering it took.
    LoweredModule(std::shared_ptr<std::pmr::memory_resource> arena,
                  LoweringVector<LoweredFunction> functions,
                  Dispatch dispatch,
                  runtime::ExecutionMode mode,
                  uint64_t loweredBytes,
                  LoweringStats stats);

    /// The engine whose code this is.
    /// @return ExecutionEngine::Fast.
    [[nodiscard]] runtime::ExecutionEngine engine() const noexcept override;

    /// The defined functions' code.
    /// @return The functions, by code index.
    [[nodiscard]] std::span<LoweredFunction const> functions() const noexcept;

    /// The dispatch the code was lowered for.
    /// @return The dispatch.
    [[nodiscard]] Dispatch dispatch() const noexcept;

    /// The execution mode the code was lowered in.
    /// @return The mode.
    [[nodiscard]] runtime::ExecutionMode mode() const noexcept;

    /// What the lowering arena charged for the module: the compile budget it used.
    /// @return The bytes.
    [[nodiscard]] uint64_t loweredBytes() const noexcept;

    /// What lowering the module took, which its bounds on work are checked against.
    /// @return The measures.
    [[nodiscard]] LoweringStats const& stats() const noexcept;

  private:
    // Declared first, so it is destroyed last: every container below allocates from it.
    std::shared_ptr<std::pmr::memory_resource> _arena;
    LoweringVector<LoweredFunction> _functions;
    Dispatch _dispatch;
    runtime::ExecutionMode _mode;
    uint64_t _loweredBytes;
    LoweringStats _stats;
};

} // namespace isoline::fastinterpreter
