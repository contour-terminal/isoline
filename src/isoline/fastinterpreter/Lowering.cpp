// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/Lowering.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/fastinterpreter/LoweringTable.hpp>
#include <isoline/fastinterpreter/SlotCodec.hpp>
#include <isoline/numerics/SimdKernels.hpp>
#include <isoline/numerics/SimdShape.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::fastinterpreter
{

namespace
{
    using binary::Opcode;
    using binary::ValueType;

    /// What an entry of the abstract operand stack holds.
    enum class EntryKind : uint8_t
    {
        Slot,     ///< A value in its home slot.
        Local,    ///< A forwarded local.get: the local's current value, which no slot copies yet.
        Constant, ///< A constant, which no slot holds yet.
        Run,      ///< The values of a block's label, in their home slots: one entry for all of them.
    };

    /// The end of a forwarding chain: no older entry forwards the same local.
    inline constexpr auto NoEntry = std::numeric_limits<uint32_t>::max();

    /// The end of a chain of branches waiting for their target: no older branch waits for the same label.
    inline constexpr auto NoPending = std::numeric_limits<uint32_t>::max();

    /// The most code words a function may have: branch targets are 32-bit offsets, and NoPending is none.
    inline constexpr auto MaxCodeWords = uint64_t { NoPending } - 1;

    /// One entry of the abstract operand stack: one value, or a Run of them.
    struct Entry
    {
        EntryKind kind = EntryKind::Slot; ///< What it holds.
        ValueType type {};                ///< Its value's type; a Run's are runTypes.
        uint32_t slot = 0;   ///< Its home slot: the locals' slots plus the widths of the values below it.
        uint32_t height = 0; ///< The values below it.
        uint32_t local = 0;  ///< Local: the local it forwards.
        uint32_t olderForwarder = NoEntry;   ///< Local: the next entry below forwarding the same local.
        uint64_t constant = 0;               ///< Constant: its bits, an i32 or f32 zero-extended.
        ValueType const* runTypes = nullptr; ///< Run: the types of its values, which a block type names.
        uint32_t runCount = 0;               ///< Run: the number of its values.
    };

    /// A run of a function's locals of one type: a parameter, or a declared run.
    struct LocalRun
    {
        uint32_t firstLocal = 0; ///< The index of its first local, the parameters counted.
        uint32_t firstSlot = 0;  ///< The slot of its first local.
        ValueType type {};       ///< The type of its locals.
    };

    /// Whether the instruction being lowered follows code that may run.
    enum class Reachability : uint8_t
    {
        Reachable,   ///< The previous instruction may fall through to it.
        Unreachable, ///< It is the else or end that dead code after a terminator ran to.
    };

    /// What a control instruction opened.
    enum class ControlKind : uint8_t
    {
        Function, ///< The function's body, whose label a branch returns through.
        Block,    ///< A block.
        Loop,     ///< A loop, whose label is its start.
        If,       ///< An if, before its else.
        Else,     ///< An if's else branch.
    };

    /// The slots of a function type's params and results, which a block type names.
    struct TypeSlots
    {
        uint64_t params = 0;  ///< The slots of its params.
        uint64_t results = 0; ///< The slots of its results.
    };

    /// The params and results of a block, and their slots.
    struct BlockSignature
    {
        std::span<ValueType const> params;  ///< Its params, on the stack when it is entered.
        std::span<ValueType const> results; ///< Its results.
        uint32_t paramSlots = 0;            ///< The slots of its params.
        uint32_t resultSlots = 0;           ///< The slots of its results.
    };

    /// Where a branch to a label goes, and what it carries.
    struct Label
    {
        uint32_t slots = 0;       ///< The slots of the values it carries: a loop's params, a block's results.
        uint32_t destination = 0; ///< The first slot they go to: the block's base.
        LoopPoll poll = LoopPoll::No; ///< Whether the label is a loop's, so a branch to it polls.
    };

    /// One open block, loop, if or the function's body.
    struct Control
    {
        ControlKind kind = ControlKind::Block;            ///< What opened it.
        binary::Instruction const* instruction = nullptr; ///< Its instruction; null for the function.
        BlockSignature signature {};                      ///< Its params and results.
        uint32_t baseHeight = 0;                          ///< The values below its params.
        uint32_t baseSlot = 0;                            ///< The slot its params and its results start at.
        uint32_t loopStart = 0;                           ///< Loop: the code offset its label continues at.
        uint32_t pending = NoPending;  ///< The newest branch waiting for its end, linked to the older.
        uint32_t ifTarget = NoPending; ///< If: the word of the if's target, its else or end.
        uint32_t deadEnd = 0;          ///< The body index of its end, where dead code inside it runs to.
    };

    /// The label of @p control.
    [[nodiscard]] Label labelOf(Control const& control) noexcept
    {
        if (control.kind == ControlKind::Loop)
            return { .slots = control.signature.paramSlots,
                     .destination = control.baseSlot,
                     .poll = LoopPoll::Yes };
        return { .slots = control.signature.resultSlots,
                 .destination = control.baseSlot,
                 .poll = LoopPoll::No };
    }

    /// The move of a value of each SlotClass from slot to slot, indexed by the class.
    inline constexpr auto MoveOperations =
        std::to_array<OperationId>({ OperationId::Move64, OperationId::Move128, OperationId::Move128 });

    /// The select of two values of each SlotClass, indexed by the class.
    inline constexpr auto SelectOperations =
        std::to_array<OperationId>({ OperationId::Select64, OperationId::Select128, OperationId::Select128 });

    /// The operation of @p table for values of @p type.
    [[nodiscard]] OperationId operationFor(std::span<OperationId const, 3> table,
                                           ValueType const& type) noexcept
    {
        return table[std::to_underlying(slotClassOf(type))];
    }

    /// The slots a value of @p type takes.
    [[nodiscard]] uint32_t widthOf(ValueType const& type) noexcept
    {
        return slotWidth(slotClassOf(type));
    }

    /// The type of the one result of an instruction whose signature is fixed.
    [[nodiscard]] ValueType resultTypeOf(Opcode opcode) noexcept
    {
        auto const& signature = binary::signatureInfo(binary::opcodeInfo(opcode).signature);
        return ValueType { .kind = signature.resultKinds().front() };
    }

    /// What every function's lowering reads of its module.
    struct ModuleContext
    {
        binary::Module const& module;               ///< The module.
        std::span<ValueType const> importedGlobals; ///< The value types of its imported globals, in order.
        std::span<uint32_t const>
            importedFunctions;                ///< The type indices of its imported functions, in order.
        std::span<TypeSlots const> typeSlots; ///< The slots of each type's params and results.
        std::span<binary::AddressType const>
            memoryAddressTypes; ///< The address type of each memory, the imported ones first.
        std::span<binary::TableType const> tables; ///< The type of each table, the imported ones first.

        /// The value type of a global.
        /// @param index The global's index, the imported ones first.
        [[nodiscard]] ValueType globalType(uint32_t index) const
        {
            if (index < importedGlobals.size())
                return importedGlobals[index];
            return module.globals.at(index - importedGlobals.size()).type.type;
        }

        /// Whether a function is imported.
        /// @param index The function's index, the imported ones first.
        [[nodiscard]] bool importsFunction(uint32_t index) const noexcept
        {
            return index < importedFunctions.size();
        }

        /// The type index of a function.
        /// @param index The function's index, the imported ones first.
        [[nodiscard]] uint32_t functionType(uint32_t index) const
        {
            if (importsFunction(index))
                return importedFunctions[index];
            return module.code.at(index - importedFunctions.size()).typeIndex;
        }
    };

    /// The slots of the params and results of each of @p module's types, allocated from @p resource: what
    /// a block of the type keeps on the stack, computed once for every function.
    [[nodiscard]] LoweringVector<TypeSlots> typeSlotsOf(binary::Module const& module,
                                                        std::pmr::memory_resource* resource)
    {
        auto const slotsOf = [](std::span<ValueType const> types) {
            auto slots = uint64_t { 0 };
            for (auto const& type: types)
                slots += widthOf(type);
            return slots;
        };
        auto slots = LoweringVector<TypeSlots>(resource);
        slots.reserve(module.types.size());
        for (auto const& type: module.types)
            slots.push_back(TypeSlots { .params = slotsOf(type.params), .results = slotsOf(type.results) });
        return slots;
    }

    /// The address type of each of @p module's memories, the imported ones first, allocated from @p resource:
    /// which operation a load or store of it lowers to.
    [[nodiscard]] LoweringVector<binary::AddressType> memoryAddressTypesOf(
        binary::Module const& module, std::pmr::memory_resource* resource)
    {
        auto types = LoweringVector<binary::AddressType>(resource);
        types.reserve(module.importedCount(binary::ExternKind::Memory) + module.memories.size());
        for (auto const& import: module.imports)
            if (import.kind == binary::ExternKind::Memory)
                types.push_back(import.memory.limits.addressType);
        for (auto const& memory: module.memories)
            types.push_back(memory.limits.addressType);
        return types;
    }

    /// The type of each of @p module's tables, the imported ones first, allocated from @p resource: what
    /// table.get pushes, and the address type of its indices and sizes.
    [[nodiscard]] LoweringVector<binary::TableType> tableTypesOf(binary::Module const& module,
                                                                 std::pmr::memory_resource* resource)
    {
        auto types = LoweringVector<binary::TableType>(resource);
        types.reserve(module.importedCount(binary::ExternKind::Table) + module.tables.size());
        for (auto const& import: module.imports)
            if (import.kind == binary::ExternKind::Table)
                types.push_back(import.table);
        for (auto const& table: module.tables)
            types.push_back(table.type);
        return types;
    }

    /// The value types of @p module's imported globals, in order, allocated from @p resource.
    [[nodiscard]] LoweringVector<ValueType> importedGlobalTypes(binary::Module const& module,
                                                                std::pmr::memory_resource* resource)
    {
        auto types = LoweringVector<ValueType>(resource);
        types.reserve(module.importedCount(binary::ExternKind::Global));
        for (auto const& import: module.imports)
            if (import.kind == binary::ExternKind::Global)
                types.push_back(import.global.type);
        return types;
    }

    /// The type indices of @p module's imported functions, in order, allocated from @p resource.
    [[nodiscard]] LoweringVector<uint32_t> importedFunctionTypes(binary::Module const& module,
                                                                 std::pmr::memory_resource* resource)
    {
        auto types = LoweringVector<uint32_t>(resource);
        types.reserve(module.importedCount(binary::ExternKind::Func));
        for (auto const& import: module.imports)
            if (import.kind == binary::ExternKind::Func)
                types.push_back(import.typeIndex);
        return types;
    }

    /// The lowering of one function's body: one forward pass over its instructions with an abstract
    /// operand stack whose entries are slots, forwarded locals, constants or runs of a block's values.
    ///
    /// Each entry's home slot is fixed by its height. A local.get pushes an entry that forwards the local,
    /// linked into the local's chain, which a local.set or local.tee of it materializes, and only its own.
    /// The chains' heads are a table indexed by local, sized to the function's locals before the body is
    /// lowered, so finding a local's chain is O(1) in the worst case, whatever locals the body forwards.
    /// Every entry that is not in its slot is in an index list too, which a block boundary or a conditional
    /// branch materializes whole and a terminator as far as its values reach: each entry is materialized at
    /// most once, so the work is linear in the body, beside O(locals) once per function for the chains'
    /// table. A branch to a block waits in a chain threaded through the target words of the branches waiting
    /// for the same end, which resolves them when it is lowered. Dead code after a terminator is skipped up
    /// to its block's else or end. Every container allocates from the lowering arena; the code's capacity is
    /// the function's bound, reserved beforehand.
    class FunctionLowering
    {
      public:
        /// Prepares the lowering of one function into @p function, whose code and local-initialization
        /// runs have their reserved capacity already.
        /// @param context The module.
        /// @param validated The validated module, for the function's frame shape and cost.
        /// @param codeIndex The function's index in the code section.
        /// @param function Where its code goes.
        /// @param resource The lowering arena.
        FunctionLowering(ModuleContext const& context,
                         validate::ValidatedModule const& validated,
                         uint32_t codeIndex,
                         LoweredFunction& function,
                         std::pmr::memory_resource* resource):
            _source { context.module.code.at(codeIndex) },
            _type { context.module.types.at(_source.typeIndex) },
            _context { context },
            _function { function },
            _code { function.code },
            _localRuns(resource),
            _stack(resource),
            _unmaterialized(resource),
            _chains(resource),
            _controls(resource),
            _top { layOutLocals() },
            _maxTop { _top }
        {
            _function.codeIndex = codeIndex;
            _function.frameCost = validated.frameCost(codeIndex);
            _function.resultSlots = static_cast<uint32_t>(_context.typeSlots[_source.typeIndex].results);
            auto const& shape = validated.frameShape(codeIndex);
            _stack.reserve(shape.maxOperands);
            _unmaterialized.reserve(shape.maxOperands);
            _controls.reserve(std::size_t { shape.maxLabels } + 1);
            // Every local's chain head, NoEntry for one nobody forwards: an O(1) look-up whatever locals the
            // body forwards, allocated before the body is lowered, where a refusal is caught.
            auto locals = uint64_t { _type.params.size() };
            for (auto const& run: _source.locals)
                locals += run.count;
            _chains.assign(static_cast<std::size_t>(locals), NoEntry);
        }

        /// Lowers the body, then fits the code's capacity to its size.
        /// @return The abstract operand stack entries it touched (LoweringStats::work).
        [[nodiscard]] uint64_t run()
        {
            auto const body = std::span { _source.body };
            Require(!body.empty() && body.back().opcode == Opcode::End);
            enterControl(Control { .kind = ControlKind::Function,
                                   .signature = { .params = {},
                                                  .results = _type.results,
                                                  .paramSlots = 0,
                                                  .resultSlots = _function.resultSlots },
                                   .baseSlot = _function.localSlots,
                                   .deadEnd = static_cast<uint32_t>(body.size() - 1) });
            while (!_controls.empty())
            {
                lowerInstruction(body[_position]);
                if (_controls.empty())
                    break;
                if (continuationAfter(body[_position].opcode) == Continuation::Dead)
                    skipDeadCode();
                else
                    ++_position;
            }
            // Room for its results too: a frame that never pushes them (its body ends in a return_call, or
            // its end is dead code) still receives them in its slots, from a host function it tail-calls or
            // as the invocation's own, from its first slot on.
            _function.frameSlots = std::max(_maxTop, _function.localSlots + _function.resultSlots);
            // Reserved at the bound; the copy holds what the function needs, and frees the rest.
            _code = LoweringVector<Word>(_code.begin(), _code.end(), _code.get_allocator());
            return _work;
        }

      private:
        /// Assigns the parameters and declared locals their slots, and derives the local-initialization runs.
        /// @return The slots of the parameters and declared locals: where the operand stack begins.
        [[nodiscard]] uint32_t layOutLocals()
        {
            _localRuns.reserve(_type.params.size() + _source.locals.size());
            // Counted in 64 bits: frameSlotsOf() refused every frame past MaxFrameSlots before this.
            auto slot = uint64_t { 0 };
            auto local = uint32_t { 0 };
            for (auto const& param: _type.params)
            {
                _localRuns.push_back(LocalRun {
                    .firstLocal = local, .firstSlot = static_cast<uint32_t>(slot), .type = param });
                ++local;
                slot += widthOf(param);
            }
            _function.paramSlots = static_cast<uint32_t>(slot);
            for (auto const& run: _source.locals)
            {
                if (run.count == 0)
                    continue;
                _localRuns.push_back(LocalRun {
                    .firstLocal = local, .firstSlot = static_cast<uint32_t>(slot), .type = run.type });
                auto const slots = uint64_t { run.count } * widthOf(run.type);
                appendLocalInit(static_cast<uint32_t>(slot), static_cast<uint32_t>(slots), run.type);
                local += run.count;
                slot += slots;
            }
            Require(slot <= MaxFrameSlots);
            _function.localSlots = static_cast<uint32_t>(slot);
            return _function.localSlots;
        }

        /// Appends the initialization of @p slotCount slots from @p slotBase, of declared locals of @p type,
        /// merging it into the previous run where both start alike.
        void appendLocalInit(uint32_t slotBase, uint32_t slotCount, ValueType const& type)
        {
            auto const kind = slotClassOf(type) == SlotClass::Reference ? LocalInit::Null : LocalInit::Zero;
            auto const heap = kind == LocalInit::Null ? type.heap : binary::HeapType {};
            auto& runs = _function.localInit;
            if (!runs.empty() && runs.back().kind == kind && runs.back().heap == heap)
            {
                runs.back().slotCount += slotCount;
                return;
            }
            runs.push_back(
                LocalInitRun { .slotBase = slotBase, .slotCount = slotCount, .kind = kind, .heap = heap });
        }

        /// The run holding @p local.
        [[nodiscard]] LocalRun const& runOf(uint32_t local) const
        {
            auto const after = std::ranges::upper_bound(_localRuns, local, {}, &LocalRun::firstLocal);
            Require(after != _localRuns.begin());
            return *std::prev(after);
        }

        /// The slot of @p local.
        [[nodiscard]] uint32_t localSlot(uint32_t local) const
        {
            auto const& run = runOf(local);
            return run.firstSlot + ((local - run.firstLocal) * widthOf(run.type));
        }

        /// Lowers one reachable instruction, or the else or end dead code ran to.
        void lowerInstruction(binary::Instruction const& instruction)
        {
            auto const& row = loweringRow(instruction.opcode);
            switch (row.form)
            {
                case OperationForm::Unary:
                    if (!lowerFused(row, instruction))
                        lowerUnary(row);
                    return;
                case OperationForm::Binary:
                case OperationForm::BinaryImmediate:
                    if (!lowerFused(row, instruction))
                        lowerBinary(row);
                    return;
                case OperationForm::Load: lowerLoad(row, instruction); return;
                case OperationForm::Store: lowerStore(row, instruction); return;
                default: lowerSpecial(instruction); return;
            }
        }

        /// Lowers an instruction of a Special row.
        void lowerSpecial(binary::Instruction const& instruction)
        {
            switch (instruction.opcode)
            {
                case Opcode::Nop: return;
                case Opcode::Unreachable: emit(OperationId::Unreachable, {}, {}); return;
                case Opcode::Block:
                    materializeAll();
                    enterBlock(instruction, ControlKind::Block, NoPending);
                    return;
                case Opcode::Loop:
                    materializeAll();
                    enterBlock(instruction, ControlKind::Loop, NoPending);
                    return;
                case Opcode::If: lowerIf(instruction); return;
                case Opcode::Else: lowerElse(); return;
                case Opcode::End: lowerEnd(); return;
                case Opcode::Br: lowerBr(instruction.a); return;
                case Opcode::BrIf:
                case Opcode::BrOnNull:
                case Opcode::BrOnNonNull: lowerConditionalBranch(instruction); return;
                case Opcode::BrTable: lowerBrTable(instruction); return;
                case Opcode::Return: lowerReturn(); return;
                case Opcode::Drop: std::ignore = pop(); return;
                case Opcode::Select:
                case Opcode::SelectTyped: lowerSelect(); return;
                case Opcode::LocalGet: pushLocal(instruction.a); return;
                case Opcode::LocalSet: std::ignore = setLocal(instruction.a); return;
                case Opcode::LocalTee: push(setLocal(instruction.a)); return;
                case Opcode::Call:
                case Opcode::CallIndirect:
                case Opcode::CallRef:
                case Opcode::ReturnCall:
                case Opcode::ReturnCallIndirect:
                case Opcode::ReturnCallRef: lowerCall(instruction); return;
                case Opcode::GlobalGet: lowerGlobalGet(instruction.a); return;
                case Opcode::GlobalSet: lowerGlobalSet(instruction.a); return;
                case Opcode::MemorySize: lowerMemorySize(instruction.a); return;
                case Opcode::MemoryGrow: lowerMemoryGrow(instruction.a); return;
                case Opcode::MemoryFill:
                    lowerBulk(OperationId::MemoryFill, indexPairWord(instruction.a, 0));
                    return;
                case Opcode::MemoryCopy:
                    lowerBulk(OperationId::MemoryCopy, indexPairWord(instruction.a, instruction.b));
                    return;
                case Opcode::MemoryInit:
                    lowerBulk(OperationId::MemoryInit, indexPairWord(instruction.a, instruction.b));
                    return;
                case Opcode::DataDrop: emit(OperationId::DataDrop, {}, { instruction.a }); return;
                case Opcode::TableFill:
                    lowerBulk(OperationId::TableFill, indexPairWord(instruction.a, 0));
                    return;
                case Opcode::TableCopy:
                    lowerBulk(OperationId::TableCopy, indexPairWord(instruction.a, instruction.b));
                    return;
                case Opcode::TableInit:
                    lowerBulk(OperationId::TableInit, indexPairWord(instruction.a, instruction.b));
                    return;
                case Opcode::ElemDrop: emit(OperationId::ElemDrop, {}, { instruction.a }); return;
                case Opcode::TableGet: lowerTableGet(instruction.a); return;
                case Opcode::TableSet: lowerTableSet(instruction.a); return;
                case Opcode::TableSize: lowerTableSize(instruction.a); return;
                case Opcode::TableGrow: lowerTableGrow(instruction.a); return;
                case Opcode::RefNull: lowerRefNull(instruction.type); return;
                case Opcode::RefFunc: lowerRefFunc(instruction.a); return;
                case Opcode::RefIsNull: lowerRefIsNull(); return;
                case Opcode::RefAsNonNull: lowerRefAsNonNull(); return;
                case Opcode::I32Const:
                case Opcode::I64Const:
                case Opcode::F32Const:
                case Opcode::F64Const:
                    push(Entry { .kind = EntryKind::Constant,
                                 .type = resultTypeOf(instruction.opcode),
                                 .constant = instruction.c });
                    return;
                case Opcode::V128Const: lowerV128Const(instruction); return;
                case Opcode::I8x16Shuffle: lowerShuffle(instruction); return;
                default: break;
            }
            if (auto const* lane = simdLaneRowOf(instruction.opcode))
            {
                lowerLaneOperation(*lane, instruction.lane);
                return;
            }
            if (auto const* wide = wideArithmeticRowOf(instruction.opcode))
            {
                lowerWideArithmetic(*wide);
                return;
            }
            if (auto const operation = simdKernelOperationOf(numerics::simdShape(instruction.opcode));
                operation != OperationId::Nop)
            {
                lowerSimdKernel(instruction.opcode, operation);
                return;
            }
            // uncovered-begin(lowering-later-task)
            core::todo(
                std::format("lowering {}: exceptions are lowered by a later part of the fast interpreter",
                            binary::opcodeInfo(instruction.opcode).text));
        }
        // uncovered-end(lowering-later-task)

        /// The home slot of the v128 @p entry, once what it holds is there: a forwarded local is moved to it,
        /// and no v128 is a constant entry (v128.const writes its slots at once). What an operation reads
        /// from the slots its result replaces, or from the slots it finds right above another operand.
        [[nodiscard]] uint32_t homed(Entry const& entry)
        {
            if (entry.kind == EntryKind::Local)
                materialize(entry);
            return entry.slot;
        }

        /// Lowers v128.const into a fresh slot pair.
        void lowerV128Const(binary::Instruction const& instruction)
        {
            auto const slot = _top;
            auto const vector = runtime::v128FromBytes(binary::v128LiteralOf(_context.module, instruction));
            push(Entry { .type = ValueType { .kind = binary::ValueKind::V128 } });
            emit(OperationId::Const128, { slot }, { vector.lanes[0], vector.lanes[1] });
        }

        /// Lowers i8x16.shuffle: the result replaces the first operand, which is moved to its home slots
        /// first, and the 16 lane indices are two immediates, eight to the word.
        void lowerShuffle(binary::Instruction const& instruction)
        {
            auto second = pop();
            auto first = pop();
            auto const destination = homed(first);
            auto const secondSource = readable(second);
            auto const lanes = runtime::v128FromBytes(binary::v128LiteralOf(_context.module, instruction));
            emit(
                OperationId::I8x16Shuffle, { destination, secondSource }, { lanes.lanes[0], lanes.lanes[1] });
            push(Entry { .type = first.type });
        }

        /// Lowers a lane operation of @p row at lane @p lane: its result in its first operand's home slot.
        void lowerLaneOperation(SimdLaneRow const& row, uint8_t lane)
        {
            auto const result = resultTypeOf(row.opcode);
            if (row.shape == numerics::SimdShape::Splat)
            {
                auto scalar = pop();
                emit(row.operation, { scalar.slot, readable(scalar) }, {});
            }
            else if (row.shape == numerics::SimdShape::ExtractLane)
            {
                auto vector = pop();
                emit(row.operation, { vector.slot, readable(vector) }, { lane });
            }
            else
            {
                Require(row.shape == numerics::SimdShape::ReplaceLane);
                auto scalar = pop();
                auto vector = pop();
                auto const vectorSource = readable(vector);
                emit(row.operation, { vector.slot, vectorSource, readable(scalar) }, { lane });
            }
            push(Entry { .type = result });
        }

        /// Lowers wide arithmetic: its two results, low then high, in its first operand's home slot and the
        /// slot after it.
        void lowerWideArithmetic(WideArithmeticRow const& row)
        {
            auto const& signature = binary::signatureInfo(binary::opcodeInfo(row.opcode).signature);
            auto const i64 = ValueType { .kind = binary::ValueKind::I64 };
            if (signature.paramCount == 4)
            {
                auto rightHigh = pop();
                auto rightLow = pop();
                auto leftHigh = pop();
                auto leftLow = pop();
                auto const leftLowSource = readable(leftLow);
                auto const leftHighSource = readable(leftHigh);
                auto const rightLowSource = readable(rightLow);
                emit(row.operation,
                     { leftLow.slot, leftLowSource, leftHighSource, rightLowSource, readable(rightHigh) },
                     {});
                push(Entry { .type = i64 });
                push(Entry { .type = i64 });
                return;
            }
            auto right = pop();
            auto left = pop();
            auto const leftSource = readable(left);
            emit(row.operation, { left.slot, leftSource, readable(right) }, {});
            push(Entry { .type = i64 });
            push(Entry { .type = i64 });
        }

        /// Lowers a vector operator of a kernel shape to @p operation, its shape's generic one, whose last
        /// word is the operator's numerics kernel, resolved here once: its result in its first operand's home
        /// slot.
        void lowerSimdKernel(Opcode opcode, OperationId operation)
        {
            auto const& signature = binary::signatureInfo(binary::opcodeInfo(opcode).signature);
            auto const count = signature.paramCount;
            auto operands = std::array<Entry, 3> {};
            for (auto& operand: std::span { operands }.first(count) | std::views::reverse)
                operand = pop();
            auto sources = std::array<uint32_t, 3> {};
            for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { count }))
                sources[index] = readable(operands[index]);
            auto const destination = operands[0].slot;
            auto const kernel = simdKernelWordOf(opcode);
            Require(kernel != 0);
            switch (count)
            {
                case 1: emit(operation, { destination, sources[0] }, { kernel }); break;
                case 2: emit(operation, { destination, sources[0], sources[1] }, { kernel }); break;
                default:
                    emit(operation, { destination, sources[0], sources[1], sources[2] }, { kernel });
                    break;
            }
            push(Entry { .type = resultTypeOf(opcode) });
        }

        /// Lowers a Unary row: its operand's slot to a result in the operand's home slot.
        void lowerUnary(LoweringRow const& row)
        {
            auto operand = pop();
            auto const source = readable(operand);
            emit(row.operation, { operand.slot, source }, {});
            push(Entry { .type = resultTypeOf(row.opcode) });
        }

        /// Lowers a Binary or BinaryImmediate row: a constant right operand folds into the immediate twin
        /// where the row has one, any other constant is materialized.
        void lowerBinary(LoweringRow const& row)
        {
            auto right = pop();
            auto left = pop();
            auto const leftSource = readable(left);
            if (row.form == OperationForm::BinaryImmediate && right.kind == EntryKind::Constant)
                emit(row.immediate, { left.slot, leftSource }, { right.constant });
            else
                emit(row.operation, { left.slot, leftSource, readable(right) }, {});
            push(Entry { .type = resultTypeOf(row.opcode) });
        }

        /// Lowers the comparison of @p row and the br_if or if right after it as one operation, where
        /// FusionTable fuses the two and the br_if moves no values.
        /// @return Whether it did; the br_if or if is then lowered too.
        [[nodiscard]] bool lowerFused(LoweringRow const& row, binary::Instruction const& instruction)
        {
            // A comparison is never the body's last instruction, its end (run() requires it), so the next
            // one exists.
            auto const& branch = _source.body[_position + 1];
            auto const* fusion = fusionOf(instruction.opcode, branch.opcode);
            if (fusion == nullptr)
                return false;
            auto const unary = row.form == OperationForm::Unary;
            // The comparison's operands are i32 or i64, one slot each, and its result would take the first.
            auto const resultSlot = _top - (unary ? 1U : 2U);
            auto* target = static_cast<Control*>(nullptr);
            auto label = Label {};
            if (branch.opcode == Opcode::BrIf)
            {
                target = &controlAt(branch.a);
                label = labelOf(*target);
                if (label.slots > 0 && resultSlot - label.slots != label.destination)
                    return false;
            }
            auto right = unary ? Entry {} : pop();
            auto left = pop();
            auto const leftSource = readable(left);
            auto const folds = !unary && right.kind == EntryKind::Constant;
            auto const rightSource = unary || folds ? 0 : readable(right);
            materializeAll();
            auto const loop = label.poll == LoopPoll::Yes;
            auto const plain = loop ? fusion->fusedLoop : fusion->fused;
            auto const folded = loop ? fusion->immediateLoop : fusion->immediate;
            auto const operation = folds ? folded : plain;
            if (folds)
                emit(operation, { leftSource }, { right.constant, NoPending });
            else if (unary)
                emit(operation, { leftSource }, { NoPending });
            else
                emit(operation, { leftSource, rightSource }, { NoPending });
            ++_position;
            if (target != nullptr)
                bindTarget(*target, _code.size() - 1);
            else
                enterBlock(branch, ControlKind::If, static_cast<uint32_t>(_code.size() - 1));
            return true;
        }

        /// Lowers select and typed select: the result goes to the first value's home slot.
        void lowerSelect()
        {
            auto condition = pop();
            auto second = pop();
            auto first = pop();
            auto const firstSource = readable(first);
            auto const secondSource = readable(second);
            emit(operationFor(SelectOperations, first.type),
                 { first.slot, firstSource, secondSource, readable(condition) },
                 {});
            push(Entry { .type = first.type });
        }

        /// The address type of memory @p memory, as a value type: memory.size's and memory.grow's.
        [[nodiscard]] ValueType addressValueType(uint32_t memory) const
        {
            auto const type = _context.memoryAddressTypes[memory];
            return ValueType { .kind = type == binary::AddressType::I64 ? binary::ValueKind::I64
                                                                        : binary::ValueKind::I32 };
        }

        /// Emits the load or store of @p row over @p slots, for @p instruction's memory, offset and lane: to
        /// memory 0, the row's own operation of the memory's address type, a lane access's lane after the
        /// offset; to any other memory, @p indexed naming the memory, that operation and the lane.
        void emitAccess(LoweringRow const& row,
                        binary::Instruction const& instruction,
                        OperationId indexed,
                        std::initializer_list<uint32_t> slots)
        {
            auto const memory = instruction.b;
            auto const access = _context.memoryAddressTypes[memory] == binary::AddressType::I64
                                    ? row.memory64
                                    : row.operation;
            auto const lane = binary::memoryAccessOf(row.opcode)->shape == binary::VectorShape::Lane;
            if (memory != 0)
                emit(indexed,
                     slots,
                     { instruction.c,
                       indexedAccessWord(
                           IndexedAccess { .memory = memory, .access = access, .lane = instruction.lane }) });
            else if (lane)
                emit(access, slots, { instruction.c, instruction.lane });
            else
                emit(access, slots, { instruction.c });
        }

        /// Lowers a Load row: the address's slot to the value, in the address's home slot. A lane load's
        /// vector, above the address, is moved to its home slots first, where the access finds it.
        void lowerLoad(LoweringRow const& row, binary::Instruction const& instruction)
        {
            auto const& access = *binary::memoryAccessOf(row.opcode);
            auto vector = Entry {};
            if (access.shape == binary::VectorShape::Lane)
                vector = pop();
            auto address = pop();
            auto const source = readable(address);
            if (access.shape == binary::VectorShape::Lane)
                Require(homed(vector) == address.slot + 1);
            emitAccess(row, instruction, OperationId::LoadIndexed, { address.slot, source });
            // Its signature is its address type's (Special); the value's type is the access's.
            push(Entry { .type = ValueType { .kind = access.value } });
        }

        /// Lowers a Store row: the value's slot to memory at the address's.
        void lowerStore(LoweringRow const& row, binary::Instruction const& instruction)
        {
            auto value = pop();
            auto address = pop();
            auto const addressSource = readable(address);
            auto const valueSource = readable(value);
            emitAccess(row, instruction, OperationId::StoreIndexed, { addressSource, valueSource });
        }

        /// Lowers memory.size of @p memory into a fresh slot.
        void lowerMemorySize(uint32_t memory)
        {
            auto const slot = _top;
            push(Entry { .type = addressValueType(memory) });
            emit(OperationId::MemorySize, { slot }, { memory });
        }

        /// Lowers memory.grow of @p memory: the delta's slot to the result, in the delta's home slot.
        void lowerMemoryGrow(uint32_t memory)
        {
            auto delta = pop();
            auto const source = readable(delta);
            emit(OperationId::MemoryGrow, { delta.slot, source }, { memory });
            push(Entry { .type = addressValueType(memory) });
        }

        /// The value type of an index, a size or a delta of @p table: its address type's.
        [[nodiscard]] ValueType tableAddressValueType(uint32_t table) const
        {
            return ValueType { .kind = _context.tables[table].limits.addressType == binary::AddressType::I64
                                           ? binary::ValueKind::I64
                                           : binary::ValueKind::I32 };
        }

        /// Lowers a bulk operation of three operands, its destination, its source or value and its count, to
        /// @p operation over their slots, naming the indices @p indices (indexPairWord()).
        void lowerBulk(OperationId operation, Word indices)
        {
            auto count = pop();
            auto source = pop();
            auto destination = pop();
            emit(operation, { readable(destination), readable(source), readable(count) }, { indices });
        }

        /// Lowers table.get of @p table: the element at the index, in the index's home slot.
        void lowerTableGet(uint32_t table)
        {
            auto index = pop();
            emit(OperationId::TableGet, { index.slot, readable(index) }, { table });
            push(Entry { .type = _context.tables[table].element });
        }

        /// Lowers table.set of @p table.
        void lowerTableSet(uint32_t table)
        {
            auto value = pop();
            auto index = pop();
            emit(OperationId::TableSet, { readable(index), readable(value) }, { table });
        }

        /// Lowers table.size of @p table into a fresh slot.
        void lowerTableSize(uint32_t table)
        {
            auto const slot = _top;
            push(Entry { .type = tableAddressValueType(table) });
            emit(OperationId::TableSize, { slot }, { table });
        }

        /// Lowers table.grow of @p table: the initial reference's and the delta's slots to the result, in the
        /// reference's home slot.
        void lowerTableGrow(uint32_t table)
        {
            auto delta = pop();
            auto init = pop();
            emit(OperationId::TableGrow, { init.slot, readable(init), readable(delta) }, { table });
            push(Entry { .type = tableAddressValueType(table) });
        }

        /// Lowers ref.null of @p type into fresh slots: an operation that makes the null through the running
        /// instance, never a constant, as a concrete heap names a TypeId of the instance's store
        /// (nullSlots()).
        void lowerRefNull(ValueType const& type)
        {
            auto const slot = _top;
            push(Entry { .type = type });
            emit(OperationId::RefNull, { slot }, { heapWord(type.heap) });
        }

        /// Lowers ref.func of @p function into fresh slots.
        void lowerRefFunc(uint32_t function)
        {
            auto const slot = _top;
            push(Entry { .type = ValueType { .kind = binary::ValueKind::Ref,
                                             .nullable = binary::Nullable::No,
                                             .heap = { .kind = binary::AbstractHeapType::Func } } });
            emit(OperationId::RefFunc, { slot }, { function });
        }

        /// Lowers ref.is_null: the reference's slots to an i32 in its home slot.
        void lowerRefIsNull()
        {
            auto reference = pop();
            emit(OperationId::RefIsNull, { reference.slot, readable(reference) }, {});
            push(Entry { .type = binary::I32 });
        }

        /// Lowers ref.as_non_null: the reference, checked, to its home slots, of its type made non-null.
        void lowerRefAsNonNull()
        {
            auto reference = pop();
            emit(OperationId::RefAsNonNull, { reference.slot, readable(reference) }, {});
            auto type = reference.type;
            type.nullable = binary::Nullable::No;
            push(Entry { .type = type });
        }

        /// Lowers global.get of @p global into a fresh slot.
        void lowerGlobalGet(uint32_t global)
        {
            auto const type = _context.globalType(global);
            auto const slot = _top;
            push(Entry { .type = type });
            emit(OperationId::GlobalGet, { slot }, { global });
        }

        /// Lowers global.set of @p global from the operand on top.
        void lowerGlobalSet(uint32_t global)
        {
            auto value = pop();
            emit(OperationId::GlobalSet, { readable(value) }, { global });
        }

        /// Lowers call, call_indirect, call_ref and their return_call twins (CallTable): the arguments are
        /// materialized into consecutive slots on top, where the callee's frame begins and where its results
        /// come back to. A function of the module's own code is called by code index, any other through the
        /// store. Only the arguments are materialized: the callee's frame begins above every other entry,
        /// and no callee writes the caller's locals, so a forwarded local or a constant below stays valid.
        void lowerCall(binary::Instruction const& instruction)
        {
            auto callee = CalleeKind::Table;
            auto typeIndex = instruction.a;
            switch (instruction.opcode)
            {
                case Opcode::Call:
                case Opcode::ReturnCall:
                    callee =
                        _context.importsFunction(instruction.a) ? CalleeKind::Import : CalleeKind::Internal;
                    typeIndex = _context.functionType(instruction.a);
                    break;
                case Opcode::CallRef:
                case Opcode::ReturnCallRef: callee = CalleeKind::Reference; break;
                default: break;
            }
            auto const* row = callOf(instruction.opcode, callee);
            Require(row != nullptr);
            // The callee's table index or reference is above the arguments, read before the frame overlaps
            // it.
            auto operandSlot = uint32_t { 0 };
            if (callSlots(callee) == 2)
            {
                auto operand = pop();
                operandSlot = readable(operand);
            }
            auto const& type = _context.module.types.at(typeIndex);
            auto const& slots = _context.typeSlots[typeIndex];
            auto const argumentSlot = _top - static_cast<uint32_t>(slots.params);
            materializeValues(argumentSlot);
            switch (callee)
            {
                case CalleeKind::Internal:
                    emit(row->operation,
                         { argumentSlot },
                         { instruction.a - static_cast<uint32_t>(_context.importedFunctions.size()) });
                    break;
                case CalleeKind::Import: emit(row->operation, { argumentSlot }, { instruction.a }); break;
                case CalleeKind::Table:
                    emit(row->operation, { argumentSlot, operandSlot }, { instruction.a, instruction.b });
                    break;
                case CalleeKind::Reference:
                    emit(row->operation, { argumentSlot, operandSlot }, { instruction.a });
                    break;
            }
            truncate(_height - static_cast<uint32_t>(type.params.size()), argumentSlot);
            if (row->mode == CallMode::Call)
                pushRun(type.results, static_cast<uint32_t>(slots.results));
        }

        // --- Control flow ---

        /// The params and results of the block, loop or if @p instruction opens.
        [[nodiscard]] BlockSignature blockSignature(binary::Instruction const& instruction) const
        {
            switch (instruction.blockKind)
            {
                case binary::BlockType::Kind::Empty: return {};
                case binary::BlockType::Kind::Value:
                    return { .params = {},
                             .results = std::span { &instruction.type, 1 },
                             .paramSlots = 0,
                             .resultSlots = widthOf(instruction.type) };
                case binary::BlockType::Kind::TypeIndex: break;
            }
            auto const& type = _context.module.types.at(instruction.a);
            auto const& slots = _context.typeSlots[instruction.a];
            // Both are on the operand stack, within the frame's slots (frameSlotsOf()).
            return { .params = type.params,
                     .results = type.results,
                     .paramSlots = static_cast<uint32_t>(slots.params),
                     .resultSlots = static_cast<uint32_t>(slots.results) };
        }

        /// Opens @p control: the validator's label high-water mark sized the stack, so it never reallocates.
        void enterControl(Control control)
        {
            Require(_controls.size() < _controls.capacity());
            _controls.push_back(control);
        }

        /// Opens the block, loop or if of @p instruction over the params on top, every entry of the stack
        /// materialized already. @p ifTarget is an if's target word, which its else or end resolves.
        void enterBlock(binary::Instruction const& instruction, ControlKind kind, uint32_t ifTarget)
        {
            auto const signature = blockSignature(instruction);
            enterControl(Control { .kind = kind,
                                   .instruction = &instruction,
                                   .signature = signature,
                                   .baseHeight = _height - static_cast<uint32_t>(signature.params.size()),
                                   .baseSlot = _top - signature.paramSlots,
                                   .loopStart = static_cast<uint32_t>(_code.size()),
                                   .ifTarget = ifTarget,
                                   .deadEnd = instruction.endIndex });
        }

        /// The control @p depth labels out from the innermost, the function's body last.
        [[nodiscard]] Control& controlAt(uint32_t depth)
        {
            Require(depth < _controls.size());
            ++_work;
            return _controls[_controls.size() - 1 - depth];
        }

        /// Lowers if: the operation continues at the else branch or the end when the condition is zero.
        void lowerIf(binary::Instruction const& instruction)
        {
            auto condition = pop();
            auto const source = readable(condition);
            materializeAll();
            emit(OperationId::If, { source }, { NoPending });
            enterBlock(instruction, ControlKind::If, static_cast<uint32_t>(_code.size() - 1));
        }

        /// Lowers else: the then branch jumps over it to the end, and the else branch starts from the if's
        /// params again, in their slots.
        void lowerElse()
        {
            auto& control = _controls.back();
            Require(control.kind == ControlKind::If);
            if (_reachability == Reachability::Reachable)
            {
                materializeAll();
                emit(OperationId::Jump, {}, { NoPending });
                bindTarget(control, _code.size() - 1);
            }
            resolve(control.ifTarget);
            truncate(control.baseHeight, control.baseSlot);
            pushRun(control.signature.params, control.signature.paramSlots);
            control.kind = ControlKind::Else;
            _reachability = Reachability::Reachable;
        }

        /// Lowers end: the branches to a block resolve here, with its results in its slots. The function's
        /// end returns them.
        void lowerEnd()
        {
            auto& control = _controls.back();
            if (control.kind == ControlKind::Function)
            {
                if (_reachability == Reachability::Reachable)
                {
                    materializeAll();
                    // The validator left exactly the results on the stack.
                    Require(_top - control.signature.resultSlots == control.baseSlot);
                }
                if (_reachability == Reachability::Reachable || control.pending != NoPending)
                {
                    resolve(control.pending);
                    emit(OperationId::Return, { control.baseSlot }, {});
                }
                _controls.pop_back();
                return;
            }
            if (_reachability == Reachability::Reachable)
                materializeAll();
            else
            {
                truncate(control.baseHeight, control.baseSlot);
                pushRun(control.signature.results, control.signature.resultSlots);
            }
            if (control.kind == ControlKind::If)
                resolve(control.ifTarget);
            resolve(control.pending);
            _controls.pop_back();
            _reachability = Reachability::Reachable;
        }

        /// Lowers br to the label @p depth out: its values materialized and moved into the label's slots,
        /// once, then a jump; to the function's body, a return.
        void lowerBr(uint32_t depth)
        {
            auto& control = controlAt(depth);
            if (control.kind == ControlKind::Function)
            {
                lowerReturn();
                return;
            }
            auto const label = labelOf(control);
            auto const source = _top - label.slots;
            materializeValues(source);
            if (label.slots > 0 && source != label.destination)
                emit(OperationId::MoveRange, { label.destination, source }, { label.slots });
            emit(label.poll == LoopPoll::Yes ? OperationId::JumpLoop : OperationId::Jump, {}, { NoPending });
            bindTarget(control, _code.size() - 1);
        }

        /// Lowers br_if, br_on_null and br_on_non_null: one operation, which moves the label's values
        /// when it branches where they are not in the label's slots, and polls to a loop label
        /// (ConditionalBranchTable).
        void lowerConditionalBranch(binary::Instruction const& instruction)
        {
            auto const* row = conditionalBranchOf(instruction.opcode);
            Require(row != nullptr);
            auto& control = controlAt(instruction.a);
            auto const label = labelOf(control);
            auto condition = Entry {};
            auto conditionSlot = uint32_t { 0 };
            auto source = uint32_t { 0 };
            if (instruction.opcode == Opcode::BrOnNonNull)
            {
                // The reference is the label's last value, so it must be in its home slot to go along.
                materializeAll();
                condition = pop();
                conditionSlot = condition.slot;
                source = _top + widthOf(condition.type) - label.slots;
            }
            else
            {
                condition = pop();
                conditionSlot = readable(condition);
                source = _top - label.slots;
                materializeAll();
            }
            auto const moves = label.slots > 0 && source != label.destination;
            auto const loop = label.poll == LoopPoll::Yes;
            if (moves)
                emit(loop ? row->moveLoop : row->move,
                     { conditionSlot, label.destination, source },
                     { (Word { label.slots } << 32U) | NoPending });
            else
                emit(loop ? row->branchLoop : row->branch, { conditionSlot }, { NoPending });
            bindTarget(control, _code.size() - 1);
            if (instruction.opcode == Opcode::BrOnNull)
            {
                // Past a br_on_null the reference is not null.
                condition.type.nullable = binary::Nullable::No;
                push(condition);
            }
        }

        /// Lowers br_table: the values' run is shared, and each label's record names its target, the slots
        /// its values go to, and whether it polls.
        void lowerBrTable(binary::Instruction const& instruction)
        {
            auto index = pop();
            auto const indexSource = readable(index);
            auto const labels = binary::labelsOf(_source, instruction);
            // Every label carries values of the same widths: the default's.
            auto const slots = labelOf(controlAt(labels.back())).slots;
            auto const source = _top - slots;
            materializeValues(source);
            Require(_code.size() + operationRow(OperationId::BrTable).words
                        + (labels.size() * BranchTableEntryWords)
                    <= _code.capacity());
            emit(OperationId::BrTable, { indexSource, source }, { slots, labels.size() - 1 });
            for (auto const depth: labels)
            {
                auto& control = controlAt(depth);
                auto const label = labelOf(control);
                _code.push_back((Word { label.destination } << 32U) | NoPending);
                bindTarget(control, _code.size() - 1);
                _code.push_back(Word { std::to_underlying(label.poll) });
            }
        }

        /// Lowers return, and a br to the function's body: the results are materialized into their home
        /// slots, which are consecutive, and the first of them returned.
        void lowerReturn()
        {
            auto const first = _top - _function.resultSlots;
            materializeValues(first);
            emit(OperationId::Return, { first }, {});
        }

        /// Skips the dead code after a terminator: up to the innermost block's end, or an if's else.
        void skipDeadCode()
        {
            auto const& control = _controls.back();
            auto const hasElse = control.kind == ControlKind::If
                                 && control.instruction->elseIndex != control.instruction->endIndex;
            _position = hasElse ? control.instruction->elseIndex : control.deadEnd;
            _reachability = Reachability::Unreachable;
        }

        /// Points the target word at @p word to @p control's label: a loop's start, or the chain of
        /// branches its end resolves.
        void bindTarget(Control& control, std::size_t word)
        {
            ++_work;
            auto& target = _code[word];
            if (control.kind == ControlKind::Loop)
            {
                target = (target & ~BranchTargetMask) | control.loopStart;
                return;
            }
            target = (target & ~BranchTargetMask) | control.pending;
            control.pending = static_cast<uint32_t>(word);
        }

        /// Points every target word of the chain from @p head to the code's end, and empties the chain.
        void resolve(uint32_t& head)
        {
            auto const here = static_cast<uint32_t>(_code.size());
            while (head != NoPending)
            {
                auto& target = _code[head];
                head = static_cast<uint32_t>(target & BranchTargetMask);
                target = (target & ~BranchTargetMask) | here;
            }
        }

        // --- The abstract operand stack ---

        /// Pops the operand on top into @p local, after materializing every entry forwarding the local's
        /// old value, and only those.
        /// @return The popped entry, which local.tee pushes again: its value is the local's now.
        [[nodiscard]] Entry setLocal(uint32_t local)
        {
            auto value = pop();
            // The local's own value written back changes nothing: its forwarders stay forwarders.
            if (value.kind == EntryKind::Local && value.local == local)
                return value;
            materializeChain(local);
            auto const target = localSlot(local);
            switch (value.kind)
            {
                case EntryKind::Slot:
                case EntryKind::Run:
                    emit(operationFor(MoveOperations, value.type), { target, value.slot }, {});
                    break;
                case EntryKind::Local:
                    emit(operationFor(MoveOperations, value.type), { target, localSlot(value.local) }, {});
                    break;
                case EntryKind::Constant: emit(OperationId::Const64, { target }, { value.constant }); break;
            }
            return value;
        }

        /// Pushes an entry forwarding @p local.
        void pushLocal(uint32_t local)
        {
            push(Entry { .kind = EntryKind::Local, .type = runOf(local).type, .local = local });
        }

        /// Pushes @p entry at the top: its home slot is the top's, a forwarded local joins its chain, and
        /// an entry not in its slot joins the index list.
        void push(Entry entry)
        {
            // The validator's operand high-water mark sized the stack, so it never reallocates.
            Require(_stack.size() < _stack.capacity());
            auto const position = static_cast<uint32_t>(_stack.size());
            entry.slot = _top;
            entry.height = _height;
            _top += widthOf(entry.type);
            ++_height;
            _maxTop = std::max(_maxTop, _top);
            entry.olderForwarder = NoEntry;
            if (entry.kind == EntryKind::Local)
            {
                auto& newest = _chains[entry.local];
                entry.olderForwarder = newest;
                newest = position;
            }
            if (entry.kind != EntryKind::Slot)
                _unmaterialized.push_back(position);
            _stack.push_back(entry);
            ++_work;
        }

        /// Pushes the values of @p types, in their slots, as one entry: what a block leaves on the stack
        /// after dead code, in O(1) whatever its arity.
        /// @param slots The slots of @p types.
        void pushRun(std::span<ValueType const> types, uint32_t slots)
        {
            if (types.empty())
                return;
            if (types.size() == 1)
            {
                push(Entry { .type = types.front() });
                return;
            }
            Require(_stack.size() < _stack.capacity());
            _stack.push_back(Entry { .kind = EntryKind::Run,
                                     .slot = _top,
                                     .height = _height,
                                     .runTypes = types.data(),
                                     .runCount = static_cast<uint32_t>(types.size()) });
            _top += slots;
            _height += static_cast<uint32_t>(types.size());
            _maxTop = std::max(_maxTop, _top);
            ++_work;
        }

        /// Pops the value on top: an entry, or the last value of a run, which the run then loses.
        [[nodiscard]] Entry pop()
        {
            Require(!_stack.empty());
            ++_work;
            auto& top = _stack.back();
            if (top.kind == EntryKind::Run)
            {
                auto const type = top.runTypes[top.runCount - 1];
                --top.runCount;
                _top -= widthOf(type);
                --_height;
                if (top.runCount == 0)
                    _stack.pop_back();
                return Entry { .type = type, .slot = _top, .height = _height };
            }
            auto const entry = top;
            removeTop();
            return entry;
        }

        /// Removes the entry on top: a forwarded local leaves its chain, of which it is the newest, and an
        /// entry not in its slot the index list, of which it is the last.
        void removeTop()
        {
            auto const entry = _stack.back();
            _stack.pop_back();
            auto const position = static_cast<uint32_t>(_stack.size());
            if (entry.kind == EntryKind::Local)
                leaveChain(entry, position);
            if (!_unmaterialized.empty() && _unmaterialized.back() == position)
                _unmaterialized.pop_back();
            _top = entry.slot;
            _height = entry.height;
        }

        /// Unlinks @p entry, at @p position, from its local's chain, whose newest forwarder it is.
        void leaveChain(Entry const& entry, uint32_t position)
        {
            auto& newest = _chains[entry.local];
            Require(newest == position);
            newest = entry.olderForwarder;
        }

        /// Cuts the stack back to @p height values, the top at @p slot: what dead code leaves of it at a
        /// block's else or end. Each entry is removed once, and a run straddling the cut shrinks in O(1).
        void truncate(uint32_t height, uint32_t slot)
        {
            while (!_stack.empty() && _stack.back().height >= height)
            {
                removeTop();
                ++_work;
            }
            if (!_stack.empty() && _stack.back().kind == EntryKind::Run
                && _stack.back().height + _stack.back().runCount > height)
            {
                _stack.back().runCount = height - _stack.back().height;
                ++_work;
            }
            _height = height;
            _top = slot;
        }

        /// The slot an operation reads @p entry from: its own, its local's, or, for a constant, its home
        /// slot once the constant is materialized there.
        [[nodiscard]] uint32_t readable(Entry const& entry)
        {
            switch (entry.kind)
            {
                case EntryKind::Slot:
                case EntryKind::Run: break;
                case EntryKind::Local: return localSlot(entry.local);
                case EntryKind::Constant: materialize(entry); break;
            }
            return entry.slot;
        }

        /// Copies what @p entry holds into its home slot, unless it is there already.
        void materialize(Entry const& entry)
        {
            switch (entry.kind)
            {
                // Every caller passes a forwarded local or a constant: what is in its slot already, an
                // entry of no index list and no chain, never comes here.
                // uncovered-begin(materialize-in-slot)
                case EntryKind::Slot:
                case EntryKind::Run: return;
                // uncovered-end(materialize-in-slot)
                case EntryKind::Local:
                    emit(
                        operationFor(MoveOperations, entry.type), { entry.slot, localSlot(entry.local) }, {});
                    break;
                case EntryKind::Constant:
                    emit(OperationId::Const64, { entry.slot }, { entry.constant });
                    break;
            }
            ++_work;
        }

        /// Materializes the entry of the index list's last position, and drops it from the list.
        void materializeLast()
        {
            auto const position = _unmaterialized.back();
            _unmaterialized.pop_back();
            auto& entry = _stack[position];
            if (entry.kind == EntryKind::Slot)
                return; // Its local's chain materialized it already.
            materialize(entry);
            // Every entry above it is in its slot, so a forwarder is the newest of its chain.
            if (entry.kind == EntryKind::Local)
                leaveChain(entry, position);
            entry.kind = EntryKind::Slot;
            entry.olderForwarder = NoEntry;
        }

        /// Materializes every entry not in its slot: at a block boundary and a conditional branch, after
        /// which code on either path reads every entry from its slot.
        void materializeAll()
        {
            while (!_unmaterialized.empty())
                materializeLast();
        }

        /// Materializes the entries not in their slot from @p slot up: the values a terminator carries,
        /// whatever is below them being dead.
        void materializeValues(uint32_t slot)
        {
            while (!_unmaterialized.empty() && _stack[_unmaterialized.back()].slot >= slot)
                materializeLast();
        }

        /// Materializes every entry forwarding @p local, which then holds its value in its own slot.
        void materializeChain(uint32_t local)
        {
            auto position = std::exchange(_chains[local], NoEntry);
            while (position != NoEntry)
            {
                auto& entry = _stack[position];
                materialize(entry);
                position = entry.olderForwarder;
                entry.kind = EntryKind::Slot;
                entry.olderForwarder = NoEntry;
            }
        }

        /// Appends @p operation with its slot operands and immediates, as OperationTable lays it out.
        void emit(OperationId operation,
                  std::initializer_list<uint32_t> slots,
                  std::initializer_list<uint64_t> immediates)
        {
            auto const& row = operationRow(operation);
            Require(slots.size() == row.slots && immediates.size() == row.immediates);
            // The capacity is the function's bound (codeBoundWords()); outgrowing it breaks the published
            // bound.
            Require(_code.size() + row.words <= _code.capacity());
            _code.push_back(Word { std::to_underlying(operation) });
            auto const operands = std::span { slots.begin(), slots.size() };
            for (auto const pair: std::views::iota(std::size_t { 0 }, (operands.size() + 1) / 2))
            {
                auto word = Word { operands[2 * pair] };
                if ((2 * pair) + 1 < operands.size())
                    word |= Word { operands[(2 * pair) + 1] } << 32U;
                _code.push_back(word);
            }
            _code.insert(_code.end(), immediates.begin(), immediates.end());
        }

        binary::Function const& _source;
        binary::FuncType const& _type;
        ModuleContext const& _context;
        LoweredFunction& _function;
        LoweringVector<Word>& _code;
        LoweringVector<LocalRun> _localRuns;
        LoweringVector<Entry> _stack;
        LoweringVector<uint32_t> _unmaterialized; ///< The positions of entries not in their slot, rising.
        LoweringVector<uint32_t>
            _chains; ///< Each local's newest forwarder, by local index; NoEntry for none.
        LoweringVector<Control> _controls; ///< The open blocks, the function's body first.
        uint32_t _top = 0;                 ///< The home slot of the next entry pushed.
        uint32_t _maxTop = 0;              ///< The highest _top reached: the frame's slots.
        uint32_t _height = 0;              ///< The values on the stack.
        std::size_t _position = 0;         ///< The body index of the instruction being lowered.
        Reachability _reachability = Reachability::Reachable;
        uint64_t _work = 0;
    };

    /// The most slots a frame of @p function may take, in 64 bits: its params' and locals' slots, then two
    /// per operand of its deepest operand stack (validate::FrameShape::maxOperands) or its results' slots,
    /// whichever is more (the frame holds its results, whether or not its body pushes them).
    [[nodiscard]] uint64_t frameSlotsOf(validate::ValidatedModule const& validated, uint32_t codeIndex)
    {
        auto const& module = validated.module();
        auto const& function = module.code[codeIndex];
        auto const& type = module.types.at(function.typeIndex);
        auto resultSlots = uint64_t { 0 };
        for (auto const& result: type.results)
            resultSlots += widthOf(result);
        auto slots = std::max(uint64_t { 2 } * validated.frameShape(codeIndex).maxOperands, resultSlots);
        for (auto const& param: type.params)
            slots += widthOf(param);
        for (auto const& run: function.locals)
            slots += uint64_t { run.count } * widthOf(run.type);
        return slots;
    }

    /// The refusal of function @p codeIndex of @p validated for a shape no 32-bit operand names: a frame
    /// of more than MaxFrameSlots slots, or code that may pass a 32-bit branch target.
    /// @return The refusal, or nothing where the function's shape is lowerable.
    [[nodiscard]] std::optional<LoweringError> unnameableShape(validate::ValidatedModule const& validated,
                                                               uint32_t codeIndex)
    {
        if (auto const slots = frameSlotsOf(validated, codeIndex); slots > MaxFrameSlots)
            return LoweringError { .codeIndex = codeIndex,
                                   .requested = slots,
                                   .limit = MaxFrameSlots,
                                   .reason = LoweringFailure::FrameSlots };
        if (auto const words = codeBoundWords(validated.module().code[codeIndex]); words > MaxCodeWords)
            // uncovered-begin(code-words-past-32-bits)
            return LoweringError { .codeIndex = codeIndex,
                                   .requested = words,
                                   .limit = MaxCodeWords,
                                   .reason = LoweringFailure::CodeWords };
        // uncovered-end(code-words-past-32-bits)
        return std::nullopt;
    }

    /// The LoweringFailure of each runtime::ArenaRefusalReason of the arena, indexed by the reason.
    inline constexpr auto RefusalFailures =
        std::to_array<LoweringFailure>({ LoweringFailure::Budget, LoweringFailure::HostMemory });

    /// Lowers every function of @p validated into @p functions, reserving each one's bound first; a
    /// function of a shape no 32-bit operand names (unnameableShape()) refuses the module before anything
    /// is allocated.
    /// @param codeIndex Set to the function being lowered, which a refusal names; NoFunction before the
    /// first.
    [[nodiscard]] std::expected<LoweringStats, LoweringError> lowerFunctions(
        validate::ValidatedModule const& validated,
        runtime::ICountingArena& arena,
        LoweringVector<LoweredFunction>& functions,
        uint32_t& codeIndex)
    {
        auto const& module = validated.module();
        // A frame or code that no 32-bit operand names is refused before anything is allocated.
        for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(module.code.size())))
        {
            if (auto const refusal = unnameableShape(validated, index))
                return std::unexpected(*refusal);
        }
        auto const importedGlobals = importedGlobalTypes(module, &arena);
        auto const importedFunctions = importedFunctionTypes(module, &arena);
        auto const typeSlots = typeSlotsOf(module, &arena);
        auto const memoryAddressTypes = memoryAddressTypesOf(module, &arena);
        auto const tables = tableTypesOf(module, &arena);
        auto const context = ModuleContext { .module = module,
                                             .importedGlobals = importedGlobals,
                                             .importedFunctions = importedFunctions,
                                             .typeSlots = typeSlots,
                                             .memoryAddressTypes = memoryAddressTypes,
                                             .tables = tables };
        auto stats = LoweringStats {};
        for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(module.code.size())))
        {
            codeIndex = index;
            auto const reservation = loweringReservation(module, index);
            if (auto const reserved = arena.reserve(reservation); !reserved)
                return std::unexpected(
                    LoweringError { .codeIndex = index, .requested = reservation, .limit = arena.budget() });
            // What the reservation covers, allocated first so that it draws on it: the records of every
            // function with the first, then this function's local-initialization runs and code bound.
            if (functions.empty())
                functions.reserve(module.code.size());
            auto& function = functions.emplace_back(&arena);
            auto const& source = module.code[index];
            function.localInit.reserve(source.locals.size());
            function.code.reserve(codeBoundWords(source));
            stats.bodyUnits += bodyUnits(source);
            stats.work += FunctionLowering(context, validated, index, function, &arena).run();
        }
        return stats;
    }

    /// Writes every operation's header word for @p dispatch (headerWord()): lowering emits OperationIds,
    /// which TailCall code replaces by the addresses of their handlers. One pass over the code, linear.
    void threadCode(LoweringVector<LoweredFunction>& functions, Dispatch dispatch)
    {
        for (auto& function: functions)
        {
            auto const code = std::span { function.code };
            auto offset = std::size_t { 0 };
            while (offset < code.size())
            {
                auto const operation = static_cast<OperationId>(code[offset]);
                auto const extent = operationExtent(operation, code.subspan(offset));
                code[offset] = headerWord(dispatch, operation);
                offset += extent;
            }
        }
    }
} // namespace

uint64_t loweringReservation(binary::Module const& module, uint32_t codeIndex)
{
    Require(codeIndex < module.code.size());
    auto const& function = module.code[codeIndex];
    auto bytes = (codeBoundWords(function) * sizeof(Word)) + (function.locals.size() * sizeof(LocalInitRun));
    if (codeIndex == 0)
        bytes += module.code.size() * sizeof(LoweredFunction);
    return bytes;
}

std::expected<std::shared_ptr<LoweredModule const>, LoweringError> lower(
    validate::ValidatedModule const& module,
    LoweringConfig const& config,
    std::shared_ptr<runtime::ICountingArena> arena)
{
    Require(arena != nullptr);
    Require(std::ranges::find(availableDispatches(), config.dispatch) != availableDispatches().end());
    auto const chargedBefore = arena->charged();
    auto const budget = arena->budget();
    auto codeIndex = NoFunction;
    auto stats = std::expected<LoweringStats, LoweringError> {};
    // Constructed within the try: a standard library may allocate bookkeeping in a container's constructor
    // (MSVC's debug containers allocate a proxy, from the process heap: LoweringAllocator), which may fail.
    auto functions = std::optional<LoweringVector<LoweredFunction>> {};
    try
    {
        functions.emplace(arena.get());
        stats = lowerFunctions(module, *arena, *functions, codeIndex);
    }
    catch (std::bad_alloc const&)
    {
        // The arena refused an allocation, for its budget or for the host's memory. What was allocated
        // unwinds with the throw; what is left of the reservation is returned here.
        arena->release(arena->reserved());
        auto const refusal = arena->lastRefusal();
        Require(refusal.has_value());
        return std::unexpected(
            LoweringError { .codeIndex = codeIndex,
                            .requested = refusal->requested,
                            .limit = budget,
                            .reason = RefusalFailures[std::to_underlying(refusal->reason)] });
    }
    if (!stats)
        return std::unexpected(stats.error());
    threadCode(*functions, config.dispatch);
    auto const loweredBytes = arena->charged() - chargedBefore;
    // The module's own record comes from the host's memory, not the arena: it owns the arena.
    auto const host = std::pmr::polymorphic_allocator<> { &arena->hostResource() };
    auto resource = std::shared_ptr<std::pmr::memory_resource> { std::move(arena) };
    try
    {
        return std::allocate_shared<LoweredModule const>(host,
                                                         std::move(resource),
                                                         std::move(*functions),
                                                         config.dispatch,
                                                         config.mode,
                                                         loweredBytes,
                                                         *stats);
    }
    catch (std::bad_alloc const&)
    {
        return std::unexpected(LoweringError { .codeIndex = NoFunction,
                                               .requested = sizeof(LoweredModule),
                                               .limit = budget,
                                               .reason = LoweringFailure::HostMemory });
    }
}

} // namespace isoline::fastinterpreter
