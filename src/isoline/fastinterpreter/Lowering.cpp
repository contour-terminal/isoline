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

#include <isoline/fastinterpreter/IndexMap.hpp>
#include <isoline/fastinterpreter/LoweringTable.hpp>
#include <isoline/fastinterpreter/SlotCodec.hpp>

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
        std::span<TypeSlots const> typeSlots;       ///< The slots of each type's params and results.

        /// The value type of a global.
        /// @param index The global's index, the imported ones first.
        [[nodiscard]] ValueType globalType(uint32_t index) const
        {
            if (index < importedGlobals.size())
                return importedGlobals[index];
            return module.globals.at(index - importedGlobals.size()).type.type;
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

    /// The lowering of one function's body: one forward pass over its instructions with an abstract
    /// operand stack whose entries are slots, forwarded locals, constants or runs of a block's values.
    ///
    /// Each entry's home slot is fixed by its height. A local.get pushes an entry that forwards the local,
    /// linked into the local's chain, which a local.set or local.tee of it materializes, and only its own.
    /// Every entry that is not in its slot is in an index list too, which a block boundary or a conditional
    /// branch materializes whole and a terminator as far as its values reach: each entry is materialized at
    /// most once, so the work is linear in the body. A branch to a block waits in a chain threaded through
    /// the target words of the branches waiting for the same end, which resolves them when it is lowered.
    /// Dead code after a terminator is skipped up to its block's else or end. Every container allocates from
    /// the lowering arena; the code's capacity is the function's bound, reserved beforehand.
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
            _function.frameSlots = _maxTop;
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
                case Opcode::GlobalGet: lowerGlobalGet(instruction.a); return;
                case Opcode::GlobalSet: lowerGlobalSet(instruction.a); return;
                case Opcode::I32Const:
                case Opcode::I64Const:
                case Opcode::F32Const:
                case Opcode::F64Const:
                    push(Entry { .kind = EntryKind::Constant,
                                 .type = resultTypeOf(instruction.opcode),
                                 .constant = instruction.c });
                    return;
                // uncovered-begin(lowering-later-task)
                default:
                    core::todo(std::format(
                        "lowering {}: calls, memory, references, SIMD and exceptions are lowered by later "
                        "parts of the fast interpreter",
                        binary::opcodeInfo(instruction.opcode).text));
                    // uncovered-end(lowering-later-task)
            }
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
                auto& newest = _chains.findOrInsert(entry.local, NoEntry);
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
            auto* const newest = _chains.find(entry.local);
            Require(newest != nullptr && *newest == position);
            if (entry.olderForwarder == NoEntry)
                _chains.erase(entry.local);
            else
                *newest = entry.olderForwarder;
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
            auto const* const newest = _chains.find(local);
            if (newest == nullptr)
                return;
            auto position = *newest;
            while (position != NoEntry)
            {
                auto& entry = _stack[position];
                materialize(entry);
                position = entry.olderForwarder;
                entry.kind = EntryKind::Slot;
                entry.olderForwarder = NoEntry;
            }
            _chains.erase(local);
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
        IndexMap _chains;                         ///< Each forwarded local's newest forwarder.
        LoweringVector<Control> _controls;        ///< The open blocks, the function's body first.
        uint32_t _top = 0;                        ///< The home slot of the next entry pushed.
        uint32_t _maxTop = 0;                     ///< The highest _top reached: the frame's slots.
        uint32_t _height = 0;                     ///< The values on the stack.
        std::size_t _position = 0;                ///< The body index of the instruction being lowered.
        Reachability _reachability = Reachability::Reachable;
        uint64_t _work = 0;
    };

    /// The most slots a frame of @p function may take, in 64 bits: its params' and locals' slots, and two
    /// per operand of its deepest operand stack (validate::FrameShape::maxOperands).
    [[nodiscard]] uint64_t frameSlotsOf(validate::ValidatedModule const& validated, uint32_t codeIndex)
    {
        auto const& module = validated.module();
        auto const& function = module.code[codeIndex];
        auto slots = uint64_t { 2 } * validated.frameShape(codeIndex).maxOperands;
        for (auto const& param: module.types.at(function.typeIndex).params)
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

    /// The LoweringFailure of each RefusalReason of the arena, indexed by the reason.
    inline constexpr auto RefusalFailures =
        std::to_array<LoweringFailure>({ LoweringFailure::Budget, LoweringFailure::HostMemory });

    /// Lowers every function of @p validated into @p functions, reserving each one's bound first; a
    /// function of a shape no 32-bit operand names (unnameableShape()) refuses the module before anything
    /// is allocated.
    /// @param codeIndex Set to the function being lowered, which a refusal names; NoFunction before the
    /// first.
    [[nodiscard]] std::expected<LoweringStats, LoweringError> lowerFunctions(
        validate::ValidatedModule const& validated,
        ILoweringArena& arena,
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
        auto const typeSlots = typeSlotsOf(module, &arena);
        auto const context =
            ModuleContext { .module = module, .importedGlobals = importedGlobals, .typeSlots = typeSlots };
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
    std::shared_ptr<ILoweringArena> arena)
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
