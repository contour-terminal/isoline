// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/x86_64/Selector.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/machine/Linearize.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::compiler::x86_64
{

using intermediate::BlockId;
using intermediate::Effect;
using intermediate::Operation;
using intermediate::Type;
using intermediate::ValueId;
using machine::MachineOperand;
using machine::OperandRole;
using machine::PhysicalRegister;
using machine::RegisterClass;
using machine::VirtualRegister;

namespace
{
    /// How many instructions selection handles between polls of its cancellation.
    constexpr auto CancellationInterval = uint32_t { 65'536 };

    /// The width of an integer type.
    [[nodiscard]] constexpr IntegerWidth integerWidthOf(Type type) noexcept
    {
        return type == Type::I32 ? IntegerWidth::Bits32 : IntegerWidth::Bits64;
    }

    /// The width of a float type.
    [[nodiscard]] constexpr FloatWidth floatWidthOf(Type type) noexcept
    {
        return type == Type::F32 ? FloatWidth::Single : FloatWidth::Double;
    }

    /// The bytes a full-width access of @p type moves.
    [[nodiscard]] constexpr uint64_t bytesOf(Type type) noexcept
    {
        return type == Type::I32 || type == Type::F32 ? 4 : 8;
    }

    /// One load form: a load of @p size bytes extended by @p extension into a @p type.
    struct LoadForm
    {
        Type type;                         ///< The result's type.
        uint8_t size;                      ///< The bytes read.
        intermediate::Extension extension; ///< How a narrower read is extended.
        Mnemonic form;                     ///< The instruction.
    };

    /// Every load the IR has, by result type, width and extension.
    constexpr auto LoadFormTable = std::to_array<LoadForm>({
        { Type::I32, 1, intermediate::Extension::Zero, Mnemonic::Movzx32M8 },
        { Type::I32, 1, intermediate::Extension::Sign, Mnemonic::Movsx32M8 },
        { Type::I32, 2, intermediate::Extension::Zero, Mnemonic::Movzx32M16 },
        { Type::I32, 2, intermediate::Extension::Sign, Mnemonic::Movsx32M16 },
        { Type::I32, 4, intermediate::Extension::Zero, Mnemonic::Mov32RM },
        { Type::I32, 4, intermediate::Extension::Sign, Mnemonic::Mov32RM },
        { Type::I64, 1, intermediate::Extension::Zero, Mnemonic::Movzx32M8 },
        { Type::I64, 1, intermediate::Extension::Sign, Mnemonic::Movsx64M8 },
        { Type::I64, 2, intermediate::Extension::Zero, Mnemonic::Movzx32M16 },
        { Type::I64, 2, intermediate::Extension::Sign, Mnemonic::Movsx64M16 },
        { Type::I64, 4, intermediate::Extension::Zero, Mnemonic::Mov32RM },
        { Type::I64, 4, intermediate::Extension::Sign, Mnemonic::Movsxd64M32 },
        { Type::I64, 8, intermediate::Extension::Zero, Mnemonic::Mov64RM },
        { Type::I64, 8, intermediate::Extension::Sign, Mnemonic::Mov64RM },
        { Type::F32, 4, intermediate::Extension::Zero, Mnemonic::MovssRM },
        { Type::F64, 8, intermediate::Extension::Zero, Mnemonic::MovsdRM },
    });

    /// One store form: a store of @p size bytes of a @p type.
    struct StoreForm
    {
        Type type;     ///< The value's type.
        uint8_t size;  ///< The bytes written.
        Mnemonic form; ///< The instruction.
    };

    /// Every store the IR has, by value type and width.
    constexpr auto StoreFormTable = std::to_array<StoreForm>({
        { Type::I32, 1, Mnemonic::Mov8MR },
        { Type::I32, 2, Mnemonic::Mov16MR },
        { Type::I32, 4, Mnemonic::Mov32MR },
        { Type::I64, 1, Mnemonic::Mov8MR },
        { Type::I64, 2, Mnemonic::Mov16MR },
        { Type::I64, 4, Mnemonic::Mov32MR },
        { Type::I64, 8, Mnemonic::Mov64MR },
        { Type::F32, 4, Mnemonic::MovssMR },
        { Type::F64, 8, Mnemonic::MovsdMR },
    });

    /// The IR type of a WebAssembly value type the compiler compiles.
    [[nodiscard]] Type typeOf(binary::ValueType const& type) noexcept
    {
        switch (type.kind)
        {
            case binary::ValueKind::I32: return Type::I32;
            case binary::ValueKind::I64: return Type::I64;
            case binary::ValueKind::F32: return Type::F32;
            case binary::ValueKind::F64: return Type::F64;
            case binary::ValueKind::Ref: return Type::Ref;
            case binary::ValueKind::V128: break; // uncovered(simd-value-type)
        }
        Require(false);   // checkModule refuses SIMD before translation
        return Type::I64; // uncovered(simd-value-type)
    }

    /// Whether @p row's emitter is an integer comparison's, which CompareIntoBranch and CompareIntoSelect
    /// fold.
    [[nodiscard]] constexpr bool isIntegerComparison(Operation operation) noexcept
    {
        auto const& range = RowsByOperation[static_cast<std::size_t>(operation)];
        auto const kind = SelectionTable[range.end - 1].emitter.kind;
        return kind == EmitterKind::Compare || kind == EmitterKind::TestZero;
    }

    /// Whether an instruction of @p effect is one a load may be moved across: one that writes nothing and
    /// moves no memory.
    [[nodiscard]] constexpr bool letsALoadPass(Effect effect) noexcept
    {
        return effect == Effect::Pure || effect == Effect::MayTrap;
    }

    /// What selection decided an instruction is.
    enum class Coverage : uint8_t
    {
        Emitted, ///< Selected by its own row.
        Folded,  ///< Folded into a later instruction's row: emitted by it.
    };

    /// One function's selection.
    class FunctionSelection
    {
      public:
        FunctionSelection(intermediate::Function const& function,
                          machine::SelectionContext const& context,
                          machine::CpuFeatures features,
                          CallConventions conventions,
                          SelectionStatistics& statistics):
            _function { function },
            _context { context },
            _features { features },
            _conventions { conventions },
            _statistics { statistics },
            _memory { context.memory },
            _machine { context.memory },
            _writer { _machine, machine::BlockId { 0 } },
            _definition(function.valueCount(), intermediate::Unnumbered, &_memory),
            _uses(function.valueCount(), 0, &_memory),
            _foldedUses(function.valueCount(), 0, &_memory),
            _registers(function.valueCount(), intermediate::Unnumbered, &_memory),
            _rows(function.instructionCount(), intermediate::Unnumbered, &_memory),
            _coverage(function.instructionCount(), Coverage::Emitted, &_memory),
            _position(function.instructionCount(), 0, &_memory),
            _blockOf(function.instructionCount(), 0, &_memory),
            _barrierBefore(function.instructionCount(), -1, &_memory),
            _blocks(function.blockCount(), intermediate::Unnumbered, &_memory),
            _predecessors(function.blockCount(), 0, &_memory),
            _resultTypes { &_memory },
            _parameterTypes { &_memory }
        {
        }

        std::expected<machine::MachineFunction, CompileError> run()
        {
            readSignature();
            return countUses()
                .and_then([this] { return decide(); })
                .and_then([this] { return emit(); })
                .transform([this] { return std::move(_machine); });
        }

      private:
        // --- Analysis
        // ---------------------------------------------------------------------------------------

        void readSignature()
        {
            auto const& module = _context.module.module();
            Require(_context.codeIndex < module.functions.size());
            auto const& type = module.types[module.functions[_context.codeIndex]];
            for (auto const& parameter: type.params)
                _parameterTypes.push_back(typeOf(parameter));
            for (auto const& result: type.results)
                _resultTypes.push_back(typeOf(result));
        }

        /// Every phase's poll, one count across them: every CancellationInterval instructions handled.
        [[nodiscard]] std::expected<void, CompileError> poll()
        {
            if (++_handled % CancellationInterval == 0
                && _context.cancellation.state() == CancellationState::Cancelled)
                return std::unexpected { CompileError { .code = CompileErrorCode::Cancelled } };
            return {};
        }

        /// Every value's definition and use count, every instruction's block and position, the latest
        /// instruction before each that a load may not pass, and every block's incoming edges.
        [[nodiscard]] std::expected<void, CompileError> countUses()
        {
            // The entry block is also entered from the machine function's own entry, which passes the
            // arguments.
            if (_function.blockCount() != 0)
                _predecessors[0] = 1;
            for (auto const block: std::views::iota(uint32_t { 0 }, _function.blockCount()))
            {
                auto barrier = int64_t { -1 };
                for (auto const [position, index]:
                     core::views::enumerate(_function.block(BlockId { block }).instructions))
                {
                    if (auto polled = poll(); !polled)
                        return polled;
                    auto const& instruction = _function.instruction(index);
                    _position[index] = static_cast<uint32_t>(position);
                    _blockOf[index] = block;
                    _barrierBefore[index] = barrier;
                    if (!letsALoadPass(intermediate::effectOf(instruction.operation)))
                        barrier = static_cast<int64_t>(position);
                    for (auto const value: _function.operands(instruction.operands))
                        ++_uses[std::to_underlying(value)];
                    for (auto const result: _function.operands(instruction.results))
                        _definition[std::to_underlying(result)] = index;
                    if (intermediate::isTerminator(instruction.operation))
                        _function.forEachTarget(instruction, [&](intermediate::BranchTarget const& target) {
                            ++_predecessors[std::to_underlying(target.block)];
                        });
                }
            }
            return {};
        }

        [[nodiscard]] std::optional<uint32_t> definitionOf(ValueId value) const noexcept
        {
            auto const index = _definition[std::to_underlying(value)];
            if (index == intermediate::Unnumbered)
                return std::nullopt;
            return index;
        }

        /// The constant @p value is, if an integer constant defines it.
        [[nodiscard]] std::optional<uint64_t> constantOf(ValueId value) const noexcept
        {
            auto const definition = definitionOf(value);
            if (!definition)
                return std::nullopt;
            auto const& instruction = _function.instruction(*definition);
            if (instruction.operation != Operation::I32Const && instruction.operation != Operation::I64Const)
                return std::nullopt;
            return _function.immediates(instruction.immediates)[0];
        }

        /// Whether @p value is a constant that fits a sign-extended 32-bit immediate of an operation on @p
        /// type.
        [[nodiscard]] bool fitsImmediate(ValueId value, Type type) const noexcept
        {
            auto const constant = constantOf(value);
            if (!constant)
                return false;
            if (type == Type::I32)
                return true;
            auto const signedValue = static_cast<int64_t>(*constant);
            return signedValue >= std::numeric_limits<int32_t>::min()
                   && signedValue <= std::numeric_limits<int32_t>::max();
        }

        /// The load defining @p value, when it may fold into @p user: of one use, earlier in the block, a
        /// full-width read of @p type, and with nothing between them that a load may not pass.
        [[nodiscard]] std::optional<uint32_t> foldableLoad(uint32_t user,
                                                           ValueId value,
                                                           Type type) const noexcept
        {
            auto const definition = definitionOf(value);
            if (!definition || _uses[std::to_underlying(value)] != 1)
                return std::nullopt;
            auto const& load = _function.instruction(*definition);
            if (load.operation != Operation::Load || _blockOf[*definition] != _blockOf[user]
                || _position[*definition] >= _position[user])
                return std::nullopt;
            auto const immediates = _function.immediates(load.immediates);
            if (immediates[1] != bytesOf(type) || static_cast<Type>(immediates[3]) != type)
                return std::nullopt;
            // The load itself is the latest instruction it may not pass, or one between them is.
            if (std::cmp_greater(_barrierBefore[user], _position[*definition]))
                return std::nullopt;
            return definition;
        }

        /// The comparison defining @p value, when it may fold into @p user: an integer comparison of one use,
        /// earlier in the block. A comparison is Pure: it moves to its user.
        [[nodiscard]] std::optional<uint32_t> foldableCompare(uint32_t user, ValueId value) const noexcept
        {
            auto const definition = definitionOf(value);
            if (!definition || _uses[std::to_underlying(value)] != 1)
                return std::nullopt;
            auto const& compare = _function.instruction(*definition);
            if (!isIntegerComparison(compare.operation) || _blockOf[*definition] != _blockOf[user])
                return std::nullopt;
            return definition;
        }

        /// The instruction a pattern folds into @p index, and the constant it reads as an immediate.
        struct Match
        {
            std::optional<uint32_t> folded {};   ///< A load or comparison emitted by this row.
            std::optional<ValueId> immediate {}; ///< A constant read as an immediate.
        };

        /// Whether @p pattern matches the instruction at @p index, and what it folds.
        [[nodiscard]] std::optional<Match> matches(Pattern pattern, uint32_t index) const
        {
            auto const& instruction = _function.instruction(index);
            auto const operands = _function.operands(instruction.operands);
            switch (pattern)
            {
                case Pattern::Any: return Match {};
                case Pattern::RhsConst32:
                case Pattern::AddressAddConst:
                    if (operands.size() >= 2 && fitsImmediate(operands[1], _function.typeOf(operands[0])))
                        return Match { .immediate = operands[1] };
                    return std::nullopt;
                case Pattern::RhsSingleUseLoad:
                    if (auto const load = foldableLoad(index, operands[1], instruction.type))
                        return Match { .folded = load };
                    return std::nullopt;
                case Pattern::CompareIntoBranch:
                case Pattern::CompareIntoSelect: {
                    auto const condition = pattern == Pattern::CompareIntoBranch ? operands[0] : operands[2];
                    auto const compare = foldableCompare(index, condition);
                    if (!compare)
                        return std::nullopt;
                    auto match = Match { .folded = compare };
                    auto const compareOperands = _function.operands(_function.instruction(*compare).operands);
                    if (compareOperands.size() == 2
                        && fitsImmediate(compareOperands[1], _function.typeOf(compareOperands[0])))
                        match.immediate = compareOperands[1];
                    return match;
                }
                case Pattern::FunctionEntry:
                    if (isAtFunctionEntry(index))
                        return Match {};
                    return std::nullopt;
            }
            return std::nullopt; // uncovered(pattern-without-case)
        }

        /// Whether the instruction at @p index is the function's entry poll or frame charge where the
        /// prologue runs them: the FunctionEntry poll first in the entry block, the charge first or right
        /// after that poll.
        [[nodiscard]] bool isAtFunctionEntry(uint32_t index) const noexcept
        {
            auto const isEntryPoll = [this](uint32_t candidate) {
                auto const& instruction = _function.instruction(candidate);
                return instruction.operation == Operation::PollStored
                       && static_cast<runtime::PollSite>(_function.immediates(instruction.immediates)[0])
                              == runtime::PollSite::FunctionEntry;
            };
            if (_blockOf[index] != 0)
                return false;
            auto const& instructions = _function.block(BlockId { 0 }).instructions;
            auto const position = _position[index];
            if (_function.instruction(index).operation == Operation::PollStored)
                return position == 0 && isEntryPoll(index);
            return position == 0 || (position == 1 && isEntryPoll(instructions[0]));
        }

        /// Whether the target has what @p row requires.
        [[nodiscard]] bool hasRequirement(SelectionRow const& row) const noexcept
        {
            return !row.requirement || _features.contains(*row.requirement);
        }

        /// Bottom-up, per block: each instruction's row, and what it folds.
        [[nodiscard]] std::expected<void, CompileError> decide()
        {
            for (auto const block: std::views::iota(uint32_t { 0 }, _function.blockCount()))
            {
                auto const instructions = std::span { _function.block(BlockId { block }).instructions };
                for (auto const index: std::views::reverse(instructions))
                {
                    if (auto polled = poll(); !polled)
                        return polled;
                    if (_coverage[index] == Coverage::Folded)
                        continue;
                    auto const operation = _function.instruction(index).operation;
                    auto const& range = RowsByOperation[static_cast<std::size_t>(operation)];
                    for (auto const rowIndex: std::views::iota(range.begin, range.end))
                    {
                        auto const& row = SelectionTable[rowIndex];
                        if (!hasRequirement(row))
                            continue;
                        auto const match = matches(row.pattern, index);
                        if (!match)
                            continue;
                        _rows[index] = rowIndex;
                        if (match->folded)
                            _coverage[*match->folded] = Coverage::Folded;
                        if (match->immediate)
                            ++_foldedUses[std::to_underlying(*match->immediate)];
                        break;
                    }
                    Require(_rows[index] != intermediate::Unnumbered); // every operation has a baseline row
                }
            }
            return {};
        }

        // --- Values
        // -------------------------------------------------------------------------------------------

        [[nodiscard]] VirtualRegister registerOf(ValueId value)
        {
            auto& reg = _registers[std::to_underlying(value)];
            if (reg == intermediate::Unnumbered)
                reg = std::to_underlying(
                    _machine.createVirtualRegister(machine::registerClassOf(_function.typeOf(value))));
            return VirtualRegister { reg };
        }

        /// Gives @p value the register @p reg, which holds it.
        void bind(ValueId value, VirtualRegister reg)
        {
            auto& existing = _registers[std::to_underlying(value)];
            // Blocks are emitted in reverse postorder, reachable ones only, so a definition, which dominates
            // its uses, is emitted before every one of them reads it.
            Require(existing == intermediate::Unnumbered);
            existing = std::to_underlying(reg);
        }

        [[nodiscard]] machine::BlockId blockOf(BlockId block) noexcept
        {
            auto& mapped = _blocks[std::to_underlying(block)];
            if (mapped == intermediate::Unnumbered)
                mapped = std::to_underlying(_machine.createBlock(_function.block(block).hotness));
            return machine::BlockId { mapped };
        }

        // --- Emission
        // -----------------------------------------------------------------------------------------

        [[nodiscard]] std::expected<void, CompileError> emit()
        {
            // The entry block receives the arguments where the internal convention puts them.
            auto const entry = _machine.createBlock(intermediate::Hotness::Normal);
            _writer.moveTo(entry);
            emitEntry();
            auto const graph = intermediate::controlFlowGraphOf(_function, _memory);
            auto const order = machine::reversePostorderOf(graph, _memory);
            for (auto const block: order)
            {
                auto const id = BlockId { block };
                _writer.moveTo(blockOf(id));
                auto parameters = std::pmr::vector<VirtualRegister> { &_memory };
                for (auto const parameter: _function.operands(_function.block(id).parameters))
                    parameters.push_back(registerOf(parameter));
                if (block != 0)
                    _machine.setParameters(blockOf(id), parameters);
                for (auto const index: _function.block(id).instructions)
                {
                    if (auto polled = poll(); !polled)
                        return polled;
                    if (_coverage[index] == Coverage::Folded || isFoldedConstant(index))
                        continue;
                    auto const& row = SelectionTable[_rows[index]];
                    ++_statistics.hits[static_cast<std::size_t>(row.row)];
                    emitRow(row, index);
                }
            }
            return {};
        }

        /// The entry block: each argument copied out of its register, or loaded from its stack slot, then a
        /// jump into the function's first block with them.
        void emitEntry()
        {
            // The prologue's Entry pseudo-instruction, filled in by the entry poll's and charge's rows.
            auto const entry = std::array { immediate(0), immediate(0), immediate(0) };
            _entryInstruction = _writer.append(Pseudo::Entry, entry);
            auto const assignment =
                machine::assignCall(_conventions.internal, _parameterTypes, _resultTypes, _memory);
            auto const hidden = assignment.resultPointer.has_value() ? std::size_t { 1 } : 0;
            auto const copyIn = [&](machine::ValueLocation const& location, RegisterClass registerClass) {
                auto const reg = _machine.createVirtualRegister(registerClass);
                if (auto const* physicalRegister = std::get_if<PhysicalRegister>(&location))
                    _writer.emit(
                        registerClass == RegisterClass::Float ? Mnemonic::MovapsRR : Mnemonic::Mov64RR,
                        { _writer.def(reg),
                          machine::RegisterOperand { .name = machine::RegisterName::of(*physicalRegister),
                                                     .role = OperandRole::Use } });
                else
                    _writer.emit(
                        registerClass == RegisterClass::Float ? Pseudo::LoadIncomingFloat
                                                              : Pseudo::LoadIncomingGeneral,
                        { _writer.def(reg), immediate(std::get<machine::StackOffset>(location).bytes) });
                return reg;
            };
            if (hidden != 0)
                _resultArea = copyIn(*assignment.resultPointer, RegisterClass::General);
            auto arguments = std::pmr::vector<VirtualRegister> { &_memory };
            for (auto const [index, type]: core::views::enumerate(_parameterTypes))
                arguments.push_back(
                    copyIn(assignment.arguments.locations[hidden + static_cast<std::size_t>(index)],
                           machine::registerClassOf(type)));
            auto const first = blockOf(BlockId { 0 });
            // The function's first block's parameters are its arguments.
            auto const parameters = _function.operands(_function.block(BlockId { 0 }).parameters);
            Require(parameters.size() == arguments.size());
            for (auto const [index, parameter]: core::views::enumerate(parameters))
                bind(parameter, arguments[static_cast<std::size_t>(index)]);
            _machine.setEdges(_writer.block(), std::array { machine::EdgeTo { .target = first } });
            _writer.emit(Mnemonic::Jmp, { machine::LabelOperand { .block = first } });
        }

        /// Whether the instruction at @p index is a constant every use of which reads it as an immediate.
        [[nodiscard]] bool isFoldedConstant(uint32_t index) const noexcept
        {
            auto const& instruction = _function.instruction(index);
            if (instruction.operation != Operation::I32Const && instruction.operation != Operation::I64Const)
                return false;
            auto const value = std::to_underlying(_function.operands(instruction.results)[0]);
            return _foldedUses[value] == _uses[value];
        }

        [[nodiscard]] std::span<ValueId const> operandsOf(uint32_t index) const noexcept
        {
            return _function.operands(_function.instruction(index).operands);
        }

        [[nodiscard]] std::span<uint64_t const> immediatesOf(uint32_t index) const noexcept
        {
            return _function.immediates(_function.instruction(index).immediates);
        }

        void define(uint32_t index, VirtualRegister reg)
        {
            bind(_function.operands(_function.instruction(index).results)[0], reg);
        }

        /// The folded load's address register, read by a memory operand.
        [[nodiscard]] MachineOperand foldedLoadAt(uint32_t index, std::size_t position)
        {
            auto const load = definitionOf(operandsOf(index)[position]);
            Require(load.has_value());
            return _writer.at(registerOf(operandsOf(*load)[0]));
        }

        /// The immediate of a constant operand, as the 32-bit field of an operation on @p type takes it.
        [[nodiscard]] int64_t immediateOperand(ValueId value, Type type) const noexcept
        {
            auto const constant = constantOf(value);
            Require(constant.has_value());
            if (type == Type::I32)
                return static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(*constant)));
            return static_cast<int64_t>(*constant);
        }

        [[nodiscard]] static constexpr bool fitsByte(int64_t value) noexcept
        {
            return value >= std::numeric_limits<int8_t>::min() && value <= std::numeric_limits<int8_t>::max();
        }

        /// A General copy of @p value at @p width, for a two-address form.
        [[nodiscard]] VirtualRegister copyOf(ValueId value, Type type)
        {
            auto const copy = _writer.general();
            _writer.emit(type == Type::I32 ? Mnemonic::Mov32RR : Mnemonic::Mov64RR,
                         { _writer.def(copy), _writer.use(registerOf(value)) });
            return copy;
        }

        [[nodiscard]] VirtualRegister floatCopyOf(ValueId value)
        {
            auto const copy = _writer.vector();
            _writer.emit(Mnemonic::MovapsRR, { _writer.def(copy), _writer.use(registerOf(value)) });
            return copy;
        }

        /// setcc of @p condition into a fresh register, zero-extended.
        [[nodiscard]] VirtualRegister flagOf(ConditionCode condition)
        {
            auto const flag = _writer.general();
            _writer.emit(Mnemonic::Setcc8R, { conditionOperand(condition), _writer.def(flag) });
            auto const value = _writer.general();
            _writer.emit(Mnemonic::Movzx32R8, { _writer.def(value), _writer.use(flag) });
            return value;
        }

        /// The comparison at @p compare emitted as its cmp or test: its condition.
        [[nodiscard]] ConditionCode compareFolded(uint32_t compare)
        {
            auto const& instruction = _function.instruction(compare);
            auto const operands = operandsOf(compare);
            auto const& range = RowsByOperation[static_cast<std::size_t>(instruction.operation)];
            auto const& baseline = SelectionTable[range.end - 1].emitter;
            auto const type = _function.typeOf(operands[0]);
            if (baseline.kind == EmitterKind::TestZero)
            {
                auto const reg = registerOf(operands[0]);
                _writer.emit(baseline.form, { _writer.use(reg), _writer.use(reg) });
                return baseline.condition;
            }
            if (fitsImmediate(operands[1], type))
            {
                auto const& immediateRow = SelectionTable[range.begin].emitter;
                Require(immediateRow.kind == EmitterKind::CompareImmediate);
                auto const value = immediateOperand(operands[1], type);
                _writer.emit(fitsByte(value) ? immediateRow.alternate : immediateRow.form,
                             { _writer.use(registerOf(operands[0])), immediate(value) });
            }
            else
                _writer.emit(baseline.form,
                             { _writer.use(registerOf(operands[0])), _writer.use(registerOf(operands[1])) });
            return baseline.condition;
        }

        /// The register views of table @p table of the instance.
        [[nodiscard]] VirtualRegister tableViewOf(uint64_t table)
        {
            auto const tables = _writer.general();
            _writer.emit(
                Mnemonic::Mov64RM,
                { _writer.def(tables), at(Gpr::R14, layoutOf(InstanceContextField::Tables).offset) });
            auto const view = _writer.general();
            _writer.emit(
                Mnemonic::Mov64RM,
                { _writer.def(view), _writer.at(tables, static_cast<int64_t>(table * sizeof(void*))) });
            return view;
        }

        /// The MemoryView of memory @p memory of the instance.
        [[nodiscard]] VirtualRegister memoryViewOf(uint64_t memory)
        {
            auto const memories = _writer.general();
            _writer.emit(
                Mnemonic::Mov64RM,
                { _writer.def(memories), at(Gpr::R14, layoutOf(InstanceContextField::Memories).offset) });
            auto const view = _writer.general();
            _writer.emit(
                Mnemonic::Mov64RM,
                { _writer.def(view), _writer.at(memories, static_cast<int64_t>(memory * sizeof(void*))) });
            return view;
        }

        /// The field @p field of a record @p record points at, at 64 or 32 bits.
        [[nodiscard]] VirtualRegister fieldOf(VirtualRegister record, std::size_t offset, IntegerWidth width)
        {
            auto const value = _writer.general();
            _writer.emit(width == IntegerWidth::Bits32 ? Mnemonic::Mov32RM : Mnemonic::Mov64RM,
                         { _writer.def(value), _writer.at(record, static_cast<int64_t>(offset)) });
            return value;
        }

        /// The cell of global @p global.
        [[nodiscard]] VirtualRegister globalCellOf(uint64_t global)
        {
            auto const cells = _writer.general();
            _writer.emit(
                Mnemonic::Mov64RM,
                { _writer.def(cells), at(Gpr::R14, layoutOf(InstanceContextField::Globals).offset) });
            return fieldOf(cells, global * sizeof(void*), IntegerWidth::Bits64);
        }

        /// The arguments of a call, from @p first on.
        [[nodiscard]] std::pmr::vector<CallValue> callArguments(uint32_t index, std::size_t first)
        {
            auto values = std::pmr::vector<CallValue> { &_memory };
            for (auto const value: operandsOf(index).subspan(first))
                values.push_back(CallValue { .reg = registerOf(value), .type = _function.typeOf(value) });
            return values;
        }

        [[nodiscard]] std::pmr::vector<Type> resultTypesOf(uint32_t index) const
        {
            auto types = std::pmr::vector<Type> { &_memory };
            for (auto const value: _function.operands(_function.instruction(index).results))
                types.push_back(_function.typeOf(value));
            return types;
        }

        void defineAll(uint32_t index, std::span<VirtualRegister const> registers)
        {
            for (auto const [position, value]:
                 core::views::enumerate(_function.operands(_function.instruction(index).results)))
                bind(value, registers[static_cast<std::size_t>(position)]);
        }

        /// The physical clobbers of a call under @p row, appended to @p operands.
        static void appendClobbers(std::pmr::vector<MachineOperand>& operands,
                                   machine::ConventionRow const& row)
        {
            machine::forEachCallerSaved(row, AllocatableGeneral, AllocatableFloat, [&](PhysicalRegister reg) {
                operands.emplace_back(machine::RegisterOperand { .name = machine::RegisterName::of(reg),
                                                                 .role = OperandRole::Def });
            });
        }

        /// The edges of a terminator and its targets' labels: each target with its arguments, through a split
        /// block when the edge is critical (two successors here, two predecessors there).
        [[nodiscard]] std::pmr::vector<machine::BlockId> edgesOf(uint32_t index)
        {
            auto const targets = _function.targets(_function.instruction(index));
            auto const critical = targets.size() > 1;
            auto edges = std::pmr::vector<machine::EdgeTo> { &_memory };
            auto labels = std::pmr::vector<machine::BlockId> { &_memory };
            auto arguments = std::pmr::vector<std::pmr::vector<VirtualRegister>> { &_memory };
            arguments.reserve(targets.size());
            for (auto const& target: targets)
            {
                auto& passed = arguments.emplace_back();
                for (auto const value: _function.operands(target.arguments))
                    passed.push_back(registerOf(value));
            }
            auto const here = _writer.block();
            for (auto const [position, target]: core::views::enumerate(targets))
            {
                auto const destination = blockOf(target.block);
                auto const& passed = arguments[static_cast<std::size_t>(position)];
                if (critical && _predecessors[std::to_underlying(target.block)] > 1)
                {
                    auto const split = _machine.createBlock(intermediate::Hotness::Normal);
                    _machine.setEdges(
                        split, std::array { machine::EdgeTo { .target = destination, .arguments = passed } });
                    std::ignore = _machine.append(
                        split,
                        opcodeOf(Mnemonic::Jmp),
                        std::array { MachineOperand { machine::LabelOperand { .block = destination } } });
                    edges.push_back(machine::EdgeTo { .target = split });
                    labels.push_back(split);
                }
                else
                {
                    edges.push_back(machine::EdgeTo { .target = destination, .arguments = passed });
                    labels.push_back(destination);
                }
            }
            _machine.setEdges(here, edges);
            return labels;
        }

        void emitRow(SelectionRow const& row, uint32_t index);
        void emitArithmetic(SelectionRow const& row, uint32_t index);
        void emitMemory(SelectionRow const& row, uint32_t index);
        void emitState(SelectionRow const& row, uint32_t index);
        void emitControl(SelectionRow const& row, uint32_t index);
        void emitSequence(Sequence sequence, uint32_t index);
        void emitReturn(uint32_t index);

        intermediate::Function const& _function;
        machine::SelectionContext const& _context;
        machine::CpuFeatures _features;
        CallConventions _conventions;
        SelectionStatistics& _statistics;
        std::pmr::memory_resource& _memory;
        machine::MachineFunction _machine;
        Writer _writer;
        std::pmr::vector<uint32_t> _definition;   ///< Per value, its defining instruction, or Unnumbered.
        std::pmr::vector<uint32_t> _uses;         ///< Per value, its operand uses.
        std::pmr::vector<uint32_t> _foldedUses;   ///< Per value, its uses read as immediates.
        std::pmr::vector<uint32_t> _registers;    ///< Per value, its virtual register, or Unnumbered.
        std::pmr::vector<uint32_t> _rows;         ///< Per instruction, its row.
        std::pmr::vector<Coverage> _coverage;     ///< Per instruction, whether a later row emits it.
        std::pmr::vector<uint32_t> _position;     ///< Per instruction, its position in its block.
        std::pmr::vector<uint32_t> _blockOf;      ///< Per instruction, its block.
        std::pmr::vector<int64_t> _barrierBefore; ///< Per instruction, the last position before it a load may
                                                  ///< not pass; -1 for none.
        std::pmr::vector<uint32_t> _blocks;       ///< Per IR block, its machine block, or Unnumbered.
        std::pmr::vector<uint32_t> _predecessors; ///< Per IR block, its incoming edges.
        std::pmr::vector<Type> _resultTypes;      ///< The function's results.
        std::pmr::vector<Type> _parameterTypes;   ///< The function's parameters.
        std::optional<VirtualRegister> _resultArea {}; ///< The caller's result area, when it passes one.
        uint32_t _entryInstruction = 0;                ///< The Entry pseudo-instruction.
        uint32_t _handled = 0;                         ///< The instructions handled, every phase's (poll()).
    };

    void FunctionSelection::emitRow(SelectionRow const& row, uint32_t index)
    {
        // The emitters by group, each a switch of its own: values, then memory and tables, then the state the
        // runtime keeps, then control and calls.
        auto const kind = row.emitter.kind;
        if (kind <= EmitterKind::Sequence)
            emitArithmetic(row, index);
        else if (kind <= EmitterKind::TableSize)
            emitMemory(row, index);
        else if (kind <= EmitterKind::StoreContext)
            emitState(row, index);
        else
            emitControl(row, index);
    }

    void FunctionSelection::emitArithmetic(SelectionRow const& row, uint32_t index)
    {
        auto const& instruction = _function.instruction(index);
        auto const& emitter = row.emitter;
        auto const operands = operandsOf(index);
        auto const immediates = immediatesOf(index);
        auto const type = instruction.type;
        switch (row.emitter.kind)
        {
            case EmitterKind::Nothing: return;
            case EmitterKind::Constant:
                define(index, materializeInteger(_writer, integerWidthOf(type), immediates[0]));
                return;
            case EmitterKind::FloatConstant:
                define(index, materializeFloat(_writer, floatWidthOf(type), immediates[0]));
                return;
            case EmitterKind::Binary: {
                auto const result = copyOf(operands[0], type);
                _writer.emit(emitter.form, { _writer.useDef(result), _writer.use(registerOf(operands[1])) });
                define(index, result);
            }
                return;
            case EmitterKind::BinaryImmediate: {
                auto const result = copyOf(operands[0], type);
                auto const value = immediateOperand(operands[1], type);
                _writer.emit(fitsByte(value) ? emitter.alternate : emitter.form,
                             { _writer.useDef(result), immediate(value) });
                define(index, result);
            }
                return;
            case EmitterKind::BinaryMemory: {
                auto const result = copyOf(operands[0], type);
                _writer.emit(emitter.form, { _writer.useDef(result), foldedLoadAt(index, 1) });
                define(index, result);
            }
                return;
            case EmitterKind::AddImmediate: {
                auto const result = _writer.general();
                _writer.emit(Mnemonic::Lea64RM,
                             { _writer.def(result),
                               _writer.at(registerOf(operands[0]), immediateOperand(operands[1], type)) });
                define(index, result);
            }
                return;
            case EmitterKind::MultiplyImmediate: {
                auto const result = _writer.general();
                _writer.emit(emitter.form,
                             { _writer.def(result),
                               _writer.use(registerOf(operands[0])),
                               immediate(immediateOperand(operands[1], type)) });
                define(index, result);
            }
                return;
            case EmitterKind::Shift: {
                auto const result = copyOf(operands[0], type);
                _writer.emit(emitter.form, { _writer.useDef(result), _writer.use(registerOf(operands[1])) });
                define(index, result);
            }
                return;
            case EmitterKind::ShiftImmediate: {
                auto const result = copyOf(operands[0], type);
                auto const mask = type == Type::I32 ? 31 : 63;
                _writer.emit(
                    emitter.form,
                    { _writer.useDef(result), immediate(immediateOperand(operands[1], type) & mask) });
                define(index, result);
            }
                return;
            case EmitterKind::Compare:
                _writer.emit(emitter.form,
                             { _writer.use(registerOf(operands[0])), _writer.use(registerOf(operands[1])) });
                define(index, flagOf(emitter.condition));
                return;
            case EmitterKind::CompareImmediate: {
                auto const value = immediateOperand(operands[1], _function.typeOf(operands[0]));
                _writer.emit(fitsByte(value) ? emitter.alternate : emitter.form,
                             { _writer.use(registerOf(operands[0])), immediate(value) });
                define(index, flagOf(emitter.condition));
            }
                return;
            case EmitterKind::TestZero: {
                auto const reg = registerOf(operands[0]);
                _writer.emit(emitter.form, { _writer.use(reg), _writer.use(reg) });
                define(index, flagOf(emitter.condition));
            }
                return;
            case EmitterKind::FloatCompare: {
                auto const swapped = static_cast<OperandOrder>(emitter.parameter) == OperandOrder::Swapped;
                auto const first = registerOf(operands[swapped ? 1 : 0]);
                auto const second = registerOf(operands[swapped ? 0 : 1]);
                _writer.emit(emitter.form, { _writer.use(first), _writer.use(second) });
                define(index, flagOf(emitter.condition));
            }
                return;
            case EmitterKind::FloatEqual:
            case EmitterKind::FloatNotEqual: {
                // Equal: ZF set and PF clear; not equal: ZF clear or PF set (unordered).
                auto const isEqual = emitter.kind == EmitterKind::FloatEqual;
                _writer.emit(emitter.form,
                             { _writer.use(registerOf(operands[0])), _writer.use(registerOf(operands[1])) });
                auto const zero = flagOf(isEqual ? ConditionCode::Equal : ConditionCode::NotEqual);
                auto const parity = flagOf(isEqual ? ConditionCode::NoParity : ConditionCode::Parity);
                _writer.emit(isEqual ? Mnemonic::And32RR : Mnemonic::Or32RR,
                             { _writer.useDef(zero), _writer.use(parity) });
                define(index, zero);
            }
                return;
            case EmitterKind::Unary: {
                auto const result = _writer.create(machine::registerClassOf(type));
                _writer.emit(emitter.form, { _writer.def(result), _writer.use(registerOf(operands[0])) });
                define(index, result);
            }
                return;
            case EmitterKind::FloatBinary: {
                auto const result = floatCopyOf(operands[0]);
                _writer.emit(emitter.form, { _writer.useDef(result), _writer.use(registerOf(operands[1])) });
                define(index, result);
            }
                return;
            case EmitterKind::FloatBinaryMemory: {
                auto const result = floatCopyOf(operands[0]);
                _writer.emit(emitter.form, { _writer.useDef(result), foldedLoadAt(index, 1) });
                define(index, result);
            }
                return;
            case EmitterKind::Rounding:
                define(index,
                       nearestSse41(_writer,
                                    floatWidthOf(type),
                                    static_cast<RoundingMode>(emitter.parameter),
                                    registerOf(operands[0])));
                return;
            case EmitterKind::RoundingStub:
                define(index,
                       nearestBaseline(_writer,
                                       _conventions,
                                       floatWidthOf(type),
                                       static_cast<FloatRoundOperation>(emitter.parameter),
                                       registerOf(operands[0])));
                return;
            case EmitterKind::Sequence: emitSequence(static_cast<Sequence>(emitter.parameter), index); return;
            default: Require(false); // uncovered(kind-of-another-group)
        }
    }

    void FunctionSelection::emitMemory(SelectionRow const& row, uint32_t index)
    {
        auto const& instruction = _function.instruction(index);
        auto const& emitter = row.emitter;
        auto const operands = operandsOf(index);
        auto const immediates = immediatesOf(index);
        auto const type = instruction.type;
        auto const spectre = _context.config.spectre;
        switch (row.emitter.kind)
        {
            case EmitterKind::Select:
            case EmitterKind::CompareSelect: {
                // select(a, b, c) is c ? a : b: b's bits, replaced by a's on the condition.
                auto const isFloat = machine::registerClassOf(type) == RegisterClass::Float;
                auto const bitsOf = [&](ValueId value) {
                    if (!isFloat)
                        return copyOf(value, Type::I64);
                    auto const bits = _writer.general();
                    _writer.emit(Mnemonic::MovqRX, { _writer.def(bits), _writer.use(registerOf(value)) });
                    return bits;
                };
                auto const result = bitsOf(operands[1]);
                auto const chosen = bitsOf(operands[0]);
                auto condition = ConditionCode::NotEqual;
                if (emitter.kind == EmitterKind::CompareSelect)
                {
                    auto const compare = definitionOf(operands[2]);
                    Require(compare.has_value());
                    condition = compareFolded(*compare);
                }
                else
                {
                    auto const reg = registerOf(operands[2]);
                    _writer.emit(Mnemonic::Test32RR, { _writer.use(reg), _writer.use(reg) });
                }
                _writer.emit(Mnemonic::Cmovcc64RR,
                             { conditionOperand(condition), _writer.useDef(result), _writer.use(chosen) });
                if (isFloat)
                {
                    auto const value = _writer.vector();
                    _writer.emit(Mnemonic::MovqXR, { _writer.def(value), _writer.use(result) });
                    define(index, value);
                }
                else
                    define(index, result);
            }
                return;
            case EmitterKind::CheckMemory: {
                auto const check = MemoryCheck { .index = registerOf(operands[0]),
                                                 .base = registerOf(operands[1]),
                                                 .bound = registerOf(operands[2]),
                                                 .offset = immediates[1],
                                                 .size = immediates[2],
                                                 .spectre = spectre };
                define(index,
                       _function.typeOf(operands[0]) == Type::I32 ? checkMemory32(_writer, check)
                                                                  : checkMemory64(_writer, check));
            }
                return;
            case EmitterKind::GuardAddress:
                define(index, guardAddress(_writer, registerOf(operands[0]), immediates[0], spectre));
                return;
            case EmitterKind::InBoundsAddress:
                define(index, inBoundsAddress(_writer, registerOf(operands[0]), immediates[1]));
                return;
            case EmitterKind::Load: {
                auto const size = static_cast<uint8_t>(immediates[1]);
                auto const extension = static_cast<intermediate::Extension>(immediates[2]);
                auto const loaded = static_cast<Type>(immediates[3]);
                auto const form = std::ranges::find_if(LoadFormTable, [&](LoadForm const& candidate) {
                    return candidate.type == loaded && candidate.size == size
                           && (candidate.extension == extension || candidate.type == Type::F32
                               || candidate.type == Type::F64);
                });
                Require(form != LoadFormTable.end());
                auto const result = _writer.create(machine::registerClassOf(loaded));
                _writer.emit(form->form, { _writer.def(result), _writer.at(registerOf(operands[0])) });
                define(index, result);
            }
                return;
            case EmitterKind::Store: {
                auto const valueType = _function.typeOf(operands[1]);
                auto const size = static_cast<uint8_t>(immediates[1]);
                auto const form = std::ranges::find_if(StoreFormTable, [&](StoreForm const& candidate) {
                    return candidate.type == valueType && candidate.size == size;
                });
                Require(form != StoreFormTable.end());
                _writer.emit(form->form,
                             { _writer.at(registerOf(operands[0])), _writer.use(registerOf(operands[1])) });
            }
                return;
            case EmitterKind::MemoryBase:
            case EmitterKind::MemoryBound: {
                auto const view = memoryViewOf(immediates[0]);
                auto const field =
                    emitter.kind == EmitterKind::MemoryBase ? MemoryViewField::Base : MemoryViewField::Bytes;
                define(index, fieldOf(view, layoutOf(field).offset, IntegerWidth::Bits64));
            }
                return;
            case EmitterKind::CheckTable: {
                auto const view = tableViewOf(immediates[0]);
                auto const check = TableCheck {
                    .index = registerOf(operands[0]),
                    .indexType = _function.typeOf(operands[0]),
                    .data = fieldOf(view, layoutOf(TableViewField::Data).offset, IntegerWidth::Bits64),
                    .size = fieldOf(view, layoutOf(TableViewField::Size).offset, IntegerWidth::Bits64),
                    .code = static_cast<runtime::TrapCode>(immediates[1]),
                    .spectre = spectre
                };
                define(index, checkTable(_writer, check));
            }
                return;
            case EmitterKind::LoadTableElement:
            case EmitterKind::StoreTableElement: {
                auto const offsets = RefOffsets { .kind = static_cast<int64_t>(immediates[1]),
                                                  .heapKind = static_cast<int64_t>(immediates[2]),
                                                  .heapTypeIndex = static_cast<int64_t>(immediates[3]),
                                                  .address = static_cast<int64_t>(immediates[4]) };
                if (emitter.kind == EmitterKind::LoadTableElement)
                    define(index, loadRef(_writer, registerOf(operands[0]), offsets));
                else
                    storeRef(_writer, registerOf(operands[0]), registerOf(operands[1]), offsets);
            }
                return;
            case EmitterKind::TableSize:
                define(index,
                       fieldOf(tableViewOf(immediates[0]),
                               layoutOf(TableViewField::Size).offset,
                               IntegerWidth::Bits64));
                return;
            default: Require(false); // uncovered(kind-of-another-group)
        }
    }

    void FunctionSelection::emitState(SelectionRow const& row, uint32_t index)
    {
        auto const operands = operandsOf(index);
        auto const immediates = immediatesOf(index);
        auto const spectre = _context.config.spectre;
        switch (row.emitter.kind)
        {
            case EmitterKind::CheckDispatch: {
                auto const view = _writer.general();
                _writer.emit(
                    Mnemonic::Mov64RM,
                    { _writer.def(view), at(Gpr::R15, layoutOf(ExecutionStateField::Dispatch).offset) });
                auto const check = DispatchCheck { .reference = registerOf(operands[0]),
                                                   .index = registerOf(operands[1]),
                                                   .indexType = _function.typeOf(operands[1]),
                                                   .view = view,
                                                   .code = static_cast<runtime::TrapCode>(immediates[0]),
                                                   .spectre = spectre };
                define(index, checkDispatch(_writer, check));
            }
                return;
            case EmitterKind::LoadDispatchType:
                define(index,
                       fieldOf(registerOf(operands[0]),
                               layoutOf(FunctionDispatchField::TypeId).offset,
                               IntegerWidth::Bits32));
                return;
            case EmitterKind::TypeId: {
                auto const identifiers = _writer.general();
                _writer.emit(Mnemonic::Mov64RM,
                             { _writer.def(identifiers),
                               at(Gpr::R14, layoutOf(InstanceContextField::TypeIds).offset) });
                define(index,
                       fieldOf(identifiers, immediates[0] * sizeof(runtime::TypeId), IntegerWidth::Bits32));
            }
                return;
            case EmitterKind::RefNull:
                define(index,
                       materializeInteger(_writer,
                                          IntegerWidth::Bits64,
                                          uint64_t { std::to_underlying(runtime::RefKind::Null) }
                                              | (immediates[0] << intermediate::RefWordHeapKindShift)));
                return;
            case EmitterKind::RefFunc: {
                auto const functions = _writer.general();
                _writer.emit(Mnemonic::Mov64RM,
                             { _writer.def(functions),
                               at(Gpr::R14, layoutOf(InstanceContextField::Functions).offset) });
                auto const word =
                    fieldOf(functions, immediates[0] * sizeof(runtime::FuncAddr), IntegerWidth::Bits32);
                _writer.emit(Mnemonic::Shl64RI8,
                             { _writer.useDef(word), immediate(intermediate::RefWordAddressShift) });
                auto const kind = uint64_t { std::to_underlying(runtime::RefKind::Func) }
                                  | (uint64_t { std::to_underlying(binary::AbstractHeapType::Func) }
                                     << intermediate::RefWordHeapKindShift);
                _writer.emit(Mnemonic::Or64RI,
                             { _writer.useDef(word), immediate(static_cast<int64_t>(kind)) });
                define(index, word);
            }
                return;
            case EmitterKind::RefIsNull: {
                auto const kind = _writer.general();
                _writer.emit(Mnemonic::Movzx32R8,
                             { _writer.def(kind), _writer.use(registerOf(operands[0])) });
                static_assert(std::to_underlying(runtime::RefKind::Null) == 0, "a null Ref's kind byte is 0");
                _writer.emit(Mnemonic::Test32RR, { _writer.use(kind), _writer.use(kind) });
                define(index, flagOf(ConditionCode::Equal));
            }
                return;
            case EmitterKind::LoadGlobal:
            case EmitterKind::StoreGlobal: {
                // A global's RawValue cell: a value at its own width in word 0, a Ref in both words.
                auto const cell = globalCellOf(immediates[0]);
                auto const isLoad = row.emitter.kind == EmitterKind::LoadGlobal;
                auto const valueType =
                    isLoad ? static_cast<Type>(immediates[1]) : _function.typeOf(operands[0]);
                auto const value =
                    isLoad ? _writer.create(machine::registerClassOf(valueType)) : registerOf(operands[0]);
                if (valueType == Type::Ref)
                {
                    auto const first = _writer.general();
                    auto const second = _writer.general();
                    auto const reg = [&](RefRegister which, OperandRole role) {
                        auto name = which == RefRegister::First ? first : second;
                        if (which == RefRegister::Word)
                            name = value;
                        return MachineOperand { machine::RegisterOperand { .name = _machine.name(name),
                                                                           .role = role } };
                    };
                    auto const emit = [&](Mnemonic mnemonic, std::initializer_list<MachineOperand> list) {
                        std::ignore = _machine.append(
                            _writer.block(), opcodeOf(mnemonic), std::span { list.begin(), list.size() });
                    };
                    if (isLoad)
                        refWordFromCell(emit, reg, _writer.at(cell), _writer.at(cell, 8));
                    else
                        refWordToCell(emit, reg, _writer.at(cell), _writer.at(cell, 8));
                }
                else if (isLoad)
                    _writer.emit(valueFormOf(valueType).load, { _writer.def(value), _writer.at(cell) });
                else
                    _writer.emit(valueFormOf(valueType).store, { _writer.at(cell), _writer.use(value) });
                if (isLoad)
                    define(index, value);
            }
                return;
            case EmitterKind::LoadContext: {
                switch (static_cast<intermediate::ContextValue>(immediates[0]))
                {
                    case intermediate::ContextValue::StepCountdown: {
                        auto const steps = _writer.general();
                        _writer.emit(Mnemonic::Mov64RM,
                                     { _writer.def(steps),
                                       at(Gpr::R15, layoutOf(ExecutionStateField::StepsLeft).offset) });
                        define(index, steps);
                    }
                    break;
                    case intermediate::ContextValue::Memory0Base:
                    case intermediate::ContextValue::Memory0Bound: {
                        auto const view = _writer.general();
                        _writer.emit(Mnemonic::Mov64RM,
                                     { _writer.def(view),
                                       at(Gpr::R14, layoutOf(InstanceContextField::Memory0).offset) });
                        auto const field = static_cast<intermediate::ContextValue>(immediates[0])
                                                   == intermediate::ContextValue::Memory0Base
                                               ? MemoryViewField::Base
                                               : MemoryViewField::Bytes;
                        define(index, fieldOf(view, layoutOf(field).offset, IntegerWidth::Bits64));
                    }
                    break;
                }
            }
                return;
            case EmitterKind::StoreContext:
                Require(static_cast<intermediate::ContextValue>(immediates[0])
                        == intermediate::ContextValue::StepCountdown);
                _writer.emit(Mnemonic::Mov64MR,
                             { at(Gpr::R15, layoutOf(ExecutionStateField::StepsLeft).offset),
                               _writer.use(registerOf(operands[0])) });
                return;
            default: Require(false); // uncovered(kind-of-another-group)
        }
    }

    void FunctionSelection::emitControl(SelectionRow const& row, uint32_t index)
    {
        auto const& instruction = _function.instruction(index);
        auto const& emitter = row.emitter;
        auto const operands = operandsOf(index);
        auto const immediates = immediatesOf(index);
        auto const spectre = _context.config.spectre;
        switch (row.emitter.kind)
        {
            case EmitterKind::Poll:
            case EmitterKind::PollStored: {
                auto pollOperands = std::pmr::vector<MachineOperand> { &_memory };
                auto steps = std::optional<VirtualRegister> {};
                if (emitter.kind == EmitterKind::Poll)
                {
                    steps = copyOf(operands[0], Type::I64);
                    pollOperands.push_back(_writer.useDef(*steps));
                }
                pollOperands.push_back(immediate(static_cast<int64_t>(immediates[0])));
                appendClobbers(pollOperands, _conventions.host);
                auto const poll = _writer.append(
                    emitter.kind == EmitterKind::Poll ? Pseudo::Poll : Pseudo::PollStored, pollOperands);
                _machine.addCallSite(poll, stubOutgoingBytes(_conventions.host, 1, _memory));
                if (steps)
                    define(index, *steps);
            }
                return;
            case EmitterKind::EnterFrame: chargeFrame(_writer, immediates[0]); return;
            case EmitterKind::LeaveFrame: releaseFrame(_writer, immediates[0]); return;
            case EmitterKind::Call: {
                Require(immediates[0] >= _context.importedFunctions);
                auto const codeIndex = static_cast<uint32_t>(immediates[0] - _context.importedFunctions);
                defineAll(
                    index,
                    callDirect(
                        _writer, _conventions, codeIndex, callArguments(index, 0), resultTypesOf(index)));
            }
                return;
            case EmitterKind::CallIndirect: {
                auto const check = definitionOf(operands[0]);
                Require(check.has_value()); // its first operand is checkDispatch's row (GuardedUseTable)
                auto const reference = registerOf(operandsOf(*check)[0]);
                defineAll(index,
                          callIndirect(_writer,
                                       _conventions,
                                       registerOf(operands[0]),
                                       reference,
                                       callArguments(index, 1),
                                       resultTypesOf(index)));
            }
                return;
            case EmitterKind::CallImport:
                defineAll(index,
                          callImport(_writer,
                                     _conventions,
                                     static_cast<uint32_t>(immediates[0]),
                                     callArguments(index, 0),
                                     resultTypesOf(index)));
                return;
            case EmitterKind::CallStub: {
                auto stubOperands = std::pmr::vector<VirtualRegister> { &_memory };
                for (auto const value: operands)
                {
                    // Every operand is one 64-bit word: an I32 constant index is zero-extended.
                    auto const reg = registerOf(value);
                    stubOperands.push_back(_function.typeOf(value) == Type::I32 ? copyOf(value, Type::I32)
                                                                                : reg);
                }
                auto const value =
                    callStub(_writer, _conventions, static_cast<RuntimeStub>(immediates[0]), stubOperands);
                if (instruction.results.count != 0)
                    define(index, value);
            }
                return;
            case EmitterKind::TrapIf:
            case EmitterKind::TrapIfSubject: {
                auto const condition = registerOf(operands[0]);
                _writer.emit(Mnemonic::Test32RR, { _writer.use(condition), _writer.use(condition) });
                auto const code = immediate(static_cast<int64_t>(immediates[0]));
                if (emitter.kind == EmitterKind::TrapIf)
                    _writer.emit(Pseudo::TrapIf, { conditionOperand(ConditionCode::NotEqual), code });
                else
                {
                    auto const subjectValue = operands[1];
                    auto const subject = _function.typeOf(subjectValue) == Type::I32
                                             ? copyOf(subjectValue, Type::I32)
                                             : registerOf(subjectValue);
                    // The zero-extension's mov keeps the flags: test's condition reaches the branch.
                    _writer.emit(Pseudo::TrapIfSubject,
                                 { conditionOperand(ConditionCode::NotEqual), code, _writer.use(subject) });
                }
            }
                return;
            case EmitterKind::Jump: {
                auto const labels = edgesOf(index);
                _writer.emit(Mnemonic::Jmp, { machine::LabelOperand { .block = labels[0] } });
            }
                return;
            case EmitterKind::Branch: {
                auto const condition = registerOf(operands[0]);
                auto const labels = edgesOf(index);
                _writer.emit(Pseudo::Branch,
                             { _writer.use(condition),
                               machine::LabelOperand { .block = labels[0] },
                               machine::LabelOperand { .block = labels[1] } });
            }
                return;
            case EmitterKind::CompareBranch: {
                auto const compare = definitionOf(operands[0]);
                Require(compare.has_value());
                auto const& compareInstruction = _function.instruction(*compare);
                auto const compareOperands = operandsOf(*compare);
                auto const& range = RowsByOperation[static_cast<std::size_t>(compareInstruction.operation)];
                auto const& baseline = SelectionTable[range.end - 1].emitter;
                auto const compareType = _function.typeOf(compareOperands[0]);
                auto form = baseline.form;
                auto left = _writer.use(registerOf(compareOperands[0]));
                auto right = left;
                if (baseline.kind == EmitterKind::CompareImmediate || baseline.kind == EmitterKind::Compare)
                {
                    if (fitsImmediate(compareOperands[1], compareType))
                    {
                        auto const& immediateRow = SelectionTable[range.begin].emitter;
                        auto const value = immediateOperand(compareOperands[1], compareType);
                        form = fitsByte(value) ? immediateRow.alternate : immediateRow.form;
                        right = immediate(value);
                    }
                    else
                        right = _writer.use(registerOf(compareOperands[1]));
                }
                auto const labels = edgesOf(index);
                _writer.emit(Pseudo::CompareBranch,
                             { immediate(std::to_underlying(form)),
                               conditionOperand(baseline.condition),
                               left,
                               right,
                               machine::LabelOperand { .block = labels[0] },
                               machine::LabelOperand { .block = labels[1] } });
            }
                return;
            case EmitterKind::Switch: {
                auto const indexReg = registerOf(operands[0]);
                auto const labels = edgesOf(index);
                auto switchOperands = std::pmr::vector<MachineOperand> { &_memory };
                switchOperands.push_back(_writer.use(indexReg));
                switchOperands.push_back(_writer.early(_writer.general()));
                switchOperands.push_back(_writer.early(_writer.general()));
                switchOperands.push_back(immediate(static_cast<int64_t>(labels.size() - 1)));
                switchOperands.push_back(immediate(std::to_underlying(spectre)));
                for (auto const label: labels)
                    switchOperands.emplace_back(machine::LabelOperand { .block = label });
                _writer.emit(Pseudo::Switch, switchOperands);
            }
                return;
            case EmitterKind::Return: emitReturn(index); return;
            case EmitterKind::EntryPoll: _machine.operands(_entryInstruction)[0] = immediate(1); return;
            case EmitterKind::EntryCharge: {
                auto entry = _machine.operands(_entryInstruction);
                entry[1] = immediate(1);
                entry[2] = immediate(static_cast<int64_t>(immediates[0]));
            }
                return;
            case EmitterKind::Trap:
                _machine.setEdges(_writer.block(), {});
                _writer.emit(Pseudo::Trap, { immediate(static_cast<int64_t>(immediates[0])) });
                return;
            default: Require(false); // uncovered(kind-of-another-group)
        }
    }
    void FunctionSelection::emitSequence(Sequence sequence, uint32_t index)
    {
        auto const& instruction = _function.instruction(index);
        auto const operands = operandsOf(index);
        auto const type = instruction.type;
        auto const a = registerOf(operands[0]);
        auto const second = [&] {
            return registerOf(operands[1]);
        };
        auto const result = [&]() -> VirtualRegister {
            switch (sequence)
            {
                case Sequence::FloatMin: return floatMin(_writer, floatWidthOf(type), a, second());
                case Sequence::FloatMax: return floatMax(_writer, floatWidthOf(type), a, second());
                case Sequence::FloatAbs:
                    return floatSign(_writer, floatWidthOf(type), SignOperation::Abs, a, std::nullopt);
                case Sequence::FloatNeg:
                    return floatSign(_writer, floatWidthOf(type), SignOperation::Neg, a, std::nullopt);
                case Sequence::FloatCopysign:
                    return floatSign(_writer, floatWidthOf(type), SignOperation::Copysign, a, second());
                case Sequence::TruncateChecked:
                case Sequence::TruncateSaturating: {
                    auto const row = truncationOf(instruction.operation);
                    Require(row.has_value());
                    return sequence == Sequence::TruncateChecked ? truncateChecked(_writer, *row, a)
                                                                 : truncateSaturating(_writer, *row, a);
                }
                case Sequence::ConvertU32: return convertU32(_writer, floatWidthOf(type), a);
                case Sequence::ConvertU64:
                    return type == Type::F32 ? convertU64ToF32(_writer, a) : convertU64ToF64(_writer, a);
                case Sequence::PopcntBaseline: return popcntBaseline(_writer, integerWidthOf(type), a);
                case Sequence::ClzBaseline: return clzBaseline(_writer, integerWidthOf(type), a);
                case Sequence::CtzBaseline: return ctzBaseline(_writer, integerWidthOf(type), a);
                case Sequence::DivSigned: return divSigned(_writer, integerWidthOf(type), a, second());
                case Sequence::RemSigned: return remSigned(_writer, integerWidthOf(type), a, second());
                case Sequence::DivUnsigned:
                    return divideUnsigned(
                        _writer, integerWidthOf(type), DivisionResult::Quotient, a, second());
                case Sequence::RemUnsigned:
                    return divideUnsigned(
                        _writer, integerWidthOf(type), DivisionResult::Remainder, a, second());
                case Sequence::CanonicalizeNan: return canonicalizeNan(_writer, floatWidthOf(type), a);
            }
            Require(false);
            return a; // uncovered(sequence-without-case)
        }();
        define(index, result);
    }

    void FunctionSelection::emitReturn(uint32_t index)
    {
        auto const values = operandsOf(index);
        auto types = std::pmr::vector<Type> { &_memory };
        for (auto const value: values)
            types.push_back(_function.typeOf(value));
        auto const assignment = machine::assignResults(_conventions.internal, types, _memory);
        // The Return pseudo-instruction reads the internal result registers by position: rax, rdx, xmm0,
        // xmm1.
        auto operands = std::array { immediate(0), immediate(0), immediate(0), immediate(0) };
        auto const positionOf = [&](PhysicalRegister reg) -> std::size_t {
            auto const& row = _conventions.internal;
            for (auto const [position, candidate]: core::views::enumerate(row.integerResults))
                if (candidate == reg)
                    return static_cast<std::size_t>(position);
            for (auto const [position, candidate]: core::views::enumerate(row.floatResults))
                if (candidate == reg)
                    return 2 + static_cast<std::size_t>(position);
            Require(false); // a result register is one of the row's
            return 0;       // uncovered(result-register-of-another-row)
        };
        for (auto const [position, location]: core::views::enumerate(assignment.locations))
        {
            auto const value = registerOf(values[static_cast<std::size_t>(position)]);
            if (auto const* reg = std::get_if<PhysicalRegister>(&location))
            {
                operands[positionOf(*reg)] = _writer.use(value);
                continue;
            }
            Require(_resultArea.has_value()); // the caller passed its area
            auto const offset = static_cast<int64_t>(std::get<machine::StackOffset>(location).bytes);
            auto const isFloat = _machine.registerClass(value) == RegisterClass::Float;
            _writer.emit(isFloat ? Mnemonic::MovsdMR : Mnemonic::Mov64MR,
                         { _writer.at(*_resultArea, offset), _writer.use(value) });
        }
        // The internal convention splits its results (SplitAcrossRegisters): no pointer to them comes back.
        Require(assignment.area != machine::ResultArea::HiddenPointer);
        _machine.setEdges(_writer.block(), {});
        _writer.emit(Pseudo::Return, operands);
    }
} // namespace

std::expected<machine::MachineFunction, CompileError> Selector::select(
    intermediate::Function const& function, machine::SelectionContext const& context)
{
    return FunctionSelection { function, context, _features, _conventions, _statistics }.run();
}

} // namespace isoline::compiler::x86_64
