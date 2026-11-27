// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/intermediate/Verifier.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/intermediate/Guards.hpp>

namespace isoline::compiler::intermediate
{

namespace
{
    /// A value's definition: where it is, by block and by position in the block's list, a parameter at
    /// position 0 and the instruction listed at i at position i + 1, so that a definition dominates a use
    /// in its own block exactly when its position is the smaller.
    struct Definition
    {
        uint32_t block = std::numeric_limits<uint32_t>::max(); ///< Its block; the maximum while undefined.
        uint32_t position = 0;                                 ///< Its position in the block.
        /// The operation whose result it is; Removed for a block parameter.
        Operation operation = Operation::Removed;
        uint32_t instruction = 0; ///< The instruction whose result it is; 0 for a block parameter.

        /// Whether the value has a definition.
        [[nodiscard]] bool isDefined() const noexcept
        {
            return block != std::numeric_limits<uint32_t>::max();
        }
    };

    /// How far guardSpanOf() has worked a value out.
    enum class SpanState : uint8_t
    {
        Unknown,   ///< Not yet.
        Resolving, ///< On the current chain: meeting it again is a loop.
        Resolved,  ///< It leads to a guard.
        Refused,   ///< It leads to none, loops, or its offsets overflow.
    };

    /// Where a pointer's guardAddress chain leads.
    struct GuardSpan
    {
        SpanState state = SpanState::Unknown; ///< How far it is worked out.
        uint32_t root = 0;                    ///< The guard's instruction, once Resolved.
        uint64_t offset = 0;                  ///< The chain's offsets, summed, once Resolved.
    };

    /// The types an OperandType admits.
    struct OperandTypeRow
    {
        OperandType type; ///< The operand type; equal to the row's index.
        TypeMask admits;  ///< The types it admits; none for SameAsOperand0, which is no set.
    };

    /// Every OperandType, in order.
    constexpr auto OperandTypeTable = std::to_array<OperandTypeRow>({
        { .type = OperandType::I32, .admits = maskOf(Type::I32) },
        { .type = OperandType::I64, .admits = maskOf(Type::I64) },
        { .type = OperandType::F32, .admits = maskOf(Type::F32) },
        { .type = OperandType::F64, .admits = maskOf(Type::F64) },
        { .type = OperandType::Ref, .admits = maskOf(Type::Ref) },
        { .type = OperandType::Pointer, .admits = maskOf(Type::Pointer) },
        { .type = OperandType::Integer, .admits = IndexTypes },
        { .type = OperandType::Float,
          .admits = static_cast<TypeMask>(maskOf(Type::F32) | maskOf(Type::F64)) },
        { .type = OperandType::Any, .admits = AnyType },
        { .type = OperandType::Wasm, .admits = WasmTypes },
        { .type = OperandType::SameAsOperand0, .admits = 0 },
    });
    static_assert(detail::isInEnumeratorOrder(OperandTypeTable,
                                              [](OperandTypeRow const& row) { return row.type; })
                      && OperandTypeTable.back().type == OperandType::SameAsOperand0,
                  "OperandTypeTable must hold one row per OperandType, at the index of its enumerator");

    /// Whether @p begin + @p count stays within a pool of @p size, computed without wrapping.
    [[nodiscard]] constexpr bool fits(uint32_t begin, uint32_t count, std::size_t size) noexcept
    {
        return uint64_t { begin } + count <= size;
    }

    /// Whether @p value is one of @p field's values.
    [[nodiscard]] constexpr bool admits(ImmediateFieldRow const& field, uint64_t value) noexcept
    {
        switch (field.values)
        {
            case ImmediateValues::UpTo: return value <= field.maximum;
            case ImmediateValues::PowerOfTwo: return std::has_single_bit(value) && value <= field.maximum;
        }
        return false; // uncovered(immediate-values-without-case)
    }

    /// Every block's ID of @p function, in order.
    [[nodiscard]] auto blocksOf(Function const& function) noexcept
    {
        return std::views::iota(uint32_t { 0 }, function.blockCount())
               | std::views::transform([](uint32_t index) { return BlockId { index }; });
    }

    /// Whether @p schema has a target field.
    [[nodiscard]] constexpr bool hasTarget(ImmediateSchemaRow const& schema) noexcept
    {
        return std::ranges::find(schema.fixed, ImmediateField::Target) != schema.fixed.end()
               || std::ranges::find(schema.repeated, ImmediateField::Target) != schema.repeated.end();
    }

    /// The checks of one function, sharing what they learn: the definitions of its values.
    class Verification
    {
      public:
        explicit Verification(Function const& function):
            _function { function },
            _definitions(function.valueCount(), Definition {}, &function.memory()),
            _isListed(function.instructionCount(), false, &function.memory()),
            _spans(function.valueCount(), GuardSpan {}, &function.memory()),
            _chain(&function.memory())
        {
        }

        /// Every check, in VerifierErrorCode's order of phases: the structure, then each instruction
        /// against its row, then dominance.
        [[nodiscard]] std::expected<void, VerifierError> run()
        {
            if (_function.blockCount() == 0)
                return fault(VerifierErrorCode::NoBlocks, std::nullopt, std::nullopt);
            for (auto const value: std::views::iota(uint32_t { 0 }, _function.valueCount()))
                if (!isType(_function.typeOf(ValueId { value })))
                    return fault(VerifierErrorCode::UnknownType, std::nullopt, std::nullopt);
            for (auto const block: blocksOf(_function))
                if (auto checked = checkStructure(block); !checked)
                    return checked;
            for (auto const block: blocksOf(_function))
                for (auto const index: _function.block(block).instructions)
                    if (_function.instruction(index).operation != Operation::Removed)
                        if (auto checked = checkRow(index); !checked)
                            return fault(checked.error(), block, index);
            if (auto agreed = checkReturns(); !agreed)
                return agreed;
            return checkDominance().and_then([this] { return checkCountdown(); });
        }

      private:
        /// The fault @p code at @p block and @p instruction.
        [[nodiscard]] static std::unexpected<VerifierError> fault(
            VerifierErrorCode code,
            std::optional<BlockId> block,
            std::optional<uint32_t> instruction) noexcept
        {
            return std::unexpected { VerifierError {
                .code = code, .block = block, .instruction = instruction } };
        }

        /// Records @p value's definition at @p block and @p position.
        [[nodiscard]] std::expected<void, VerifierErrorCode> define(ValueId value,
                                                                    uint32_t block,
                                                                    uint32_t position,
                                                                    Operation operation = Operation::Removed,
                                                                    uint32_t instruction = 0)
        {
            if (std::to_underlying(value) >= _definitions.size())
                return std::unexpected { VerifierErrorCode::UndefinedValue };
            auto& definition = _definitions[std::to_underlying(value)];
            if (definition.isDefined())
                return std::unexpected { VerifierErrorCode::RedefinedValue };
            definition = Definition {
                .block = block, .position = position, .operation = operation, .instruction = instruction
            };
            return {};
        }

        /// What one block's structure must be: its ranges within their pools, its instructions listed
        /// once each and known, every value it defines defined once, one terminator and that last.
        [[nodiscard]] std::expected<void, VerifierError> checkStructure(BlockId blockId)
        {
            auto const blockIndex = std::to_underlying(blockId);
            auto const& block = _function.block(blockId);
            if (!fits(block.parameters.begin, block.parameters.count, _function.operandPoolSize()))
                return fault(VerifierErrorCode::RangeOutOfBounds, blockId, std::nullopt);
            if (std::to_underlying(block.hotness) > std::to_underlying(Hotness::Cold))
                return fault(VerifierErrorCode::UnknownHotness, blockId, std::nullopt);
            for (auto const parameter: _function.operands(block.parameters))
                if (auto defined = define(parameter, blockIndex, 0); !defined)
                    return fault(defined.error(), blockId, std::nullopt);

            // Every index within the function and listed once, before anything is read through one.
            for (auto const index: block.instructions)
            {
                if (index >= _function.instructionCount())
                    return fault(VerifierErrorCode::InstructionOutOfBounds, blockId, index);
                if (_isListed[index])
                    return fault(VerifierErrorCode::InstructionListedTwice, blockId, index);
                _isListed[index] = true;
            }

            // The last instruction that is not removed, which must be the one terminator.
            auto lastLive = std::optional<std::size_t> {};
            for (auto const [position, index]: core::views::enumerate(block.instructions))
                if (_function.instruction(index).operation != Operation::Removed)
                    lastLive = position;

            for (auto const [position, index]: core::views::enumerate(block.instructions))
            {
                auto const& instruction = _function.instruction(index);
                if (instruction.operation == Operation::Removed)
                    continue;
                if (!isOperation(instruction.operation))
                    return fault(VerifierErrorCode::UnknownOperation, blockId, index);
                if (!fits(instruction.operands.begin, instruction.operands.count, _function.operandPoolSize())
                    || !fits(
                        instruction.results.begin, instruction.results.count, _function.operandPoolSize())
                    || !fits(instruction.immediates.begin,
                             instruction.immediates.count,
                             _function.immediatePoolSize()))
                    return fault(VerifierErrorCode::RangeOutOfBounds, blockId, index);
                for (auto const result: _function.operands(instruction.results))
                    if (auto defined = define(result,
                                              blockIndex,
                                              static_cast<uint32_t>(position + 1),
                                              instruction.operation,
                                              index);
                        !defined)
                        return fault(defined.error(), blockId, index);
                if (isTerminator(instruction.operation) && position != lastLive)
                    return fault(VerifierErrorCode::TerminatorNotLast, blockId, index);
            }
            if (!lastLive || !isTerminator(_function.instruction(block.instructions[*lastLive]).operation))
                return fault(VerifierErrorCode::MissingTerminator, blockId, std::nullopt);
            return {};
        }

        /// What one instruction must be against its row: its operands defined, its immediates as its
        /// schema lays them out and each within its field, its targets within the function, its operands
        /// and results as many and of the types its row says, and each target's arguments its parameters'.
        [[nodiscard]] std::expected<void, VerifierErrorCode> checkRow(uint32_t index) const
        {
            auto const& instruction = _function.instruction(index);
            for (auto const operand: _function.operands(instruction.operands))
                if (std::to_underlying(operand) >= _definitions.size()
                    || !_definitions[std::to_underlying(operand)].isDefined())
                    return std::unexpected { VerifierErrorCode::UndefinedValue };
            return checkImmediates(instruction).and_then([&](uint64_t arguments) {
                return checkOperands(instruction, arguments)
                    .and_then([&] { return checkTypedImmediates(instruction); })
                    .and_then([&] { return checkResults(instruction); })
                    .and_then([&] { return checkArguments(instruction); })
                    .and_then([&] { return checkGuardedUse(instruction); });
            });
        }

        /// The immediate of @p field of the instruction at @p index, if its immediates are long enough to
        /// hold its schema's fixed fields: how a guard's row is read before that row's own check.
        [[nodiscard]] std::optional<uint64_t> fixedImmediateOf(uint32_t index, ImmediateField field) const
        {
            auto const& instruction = _function.instruction(index);
            auto const& schema = immediateSchemaRow(operationRow(instruction.operation).immediates);
            if (instruction.immediates.count < schema.fixed.size())
                return std::nullopt;
            return immediateOf(instruction, field);
        }

        /// Where @p pointer's guardAddress chain leads: the guard at its root, and the chain's offsets
        /// summed; none when it leads to no guard or loops. Each value is worked out once, iteratively.
        [[nodiscard]] std::optional<GuardSpan> guardSpanOf(ValueId pointer) const
        {
            _chain.clear();
            auto value = pointer;
            while (_spans[std::to_underlying(value)].state == SpanState::Unknown)
            {
                auto const& definition = _definitions[std::to_underlying(value)];
                if (definition.operation != Operation::GuardAddress)
                    break;
                auto const operands =
                    _function.operands(_function.instruction(definition.instruction).operands);
                if (operands.empty() || std::to_underlying(operands[0]) >= _spans.size())
                    break;
                _spans[std::to_underlying(value)].state = SpanState::Resolving;
                _chain.push_back(value);
                value = operands[0];
            }
            // The chain's start: a span already worked out, a guard, or what leads to none (a loop back
            // into the chain, which is Resolving, among them).
            auto& start = _spans[std::to_underlying(value)];
            auto const& definition = _definitions[std::to_underlying(value)];
            if (start.state == SpanState::Unknown && definition.operation != Operation::GuardAddress
                && guardRowOf(definition.operation).has_value())
                start =
                    GuardSpan { .state = SpanState::Resolved, .root = definition.instruction, .offset = 0 };
            auto base = start.state == SpanState::Resolved || start.state == SpanState::Refused
                            ? start
                            : GuardSpan { .state = SpanState::Refused };
            for (auto const link: _chain | std::views::reverse)
            {
                auto const delta = fixedImmediateOf(_definitions[std::to_underlying(link)].instruction,
                                                    ImmediateField::Offset);
                // Each link's own use checks its offset against the root's size, overflow refused, so
                // a sum past 2^64 - 1 is a function the verifier refuses at that link anyway.
                if (base.state != SpanState::Resolved || !delta)
                    base = GuardSpan { .state = SpanState::Refused };
                else
                    base.offset += *delta;
                _spans[std::to_underlying(link)] = base;
            }
            if (base.state != SpanState::Resolved)
                return std::nullopt;
            return base;
        }

        /// The immediate a use of @p scope must share with its guard; none for GuardScope::None.
        [[nodiscard]] static std::optional<ImmediateField> scopeFieldOf(GuardScope scope) noexcept
        {
            switch (scope)
            {
                case GuardScope::None: return std::nullopt;
                case GuardScope::Memory: return ImmediateField::Memory;
                case GuardScope::Table: return ImmediateField::Table;
            }
            return std::nullopt; // uncovered(guard-scope-without-case)
        }

        /// A use of a guarded pointer takes it from its GuardedUseTable row's guard, directly or through
        /// guardAddress: of the same memory or table, and reaching no further than the guard checked.
        [[nodiscard]] std::expected<void, VerifierErrorCode> checkGuardedUse(
            Instruction const& instruction) const
        {
            auto const use = guardedUseOf(instruction.operation);
            if (!use)
                return {};
            auto const unguarded = std::unexpected { VerifierErrorCode::UnguardedAddress };
            auto const span = guardSpanOf(_function.operands(instruction.operands)[use->operand]);
            if (!span || _function.instruction(span->root).operation != use->guard)
                return unguarded;
            if (auto const field = scopeFieldOf(use->scope))
            {
                auto const guarded = fixedImmediateOf(span->root, *field);
                if (!guarded || immediateOf(instruction, *field) != guarded)
                    return unguarded;
            }
            auto reach = uint64_t { 0 };
            switch (use->extent)
            {
                // guardAddress's own row takes only a checkMemory's chain, so an element or a row is
                // never reached through a move.
                case UseExtent::Element: return {};
                case UseExtent::Size:
                    reach = immediateOf(instruction, ImmediateField::Size).value_or(0);
                    break;
                case UseExtent::Offset:
                    reach = immediateOf(instruction, ImmediateField::Offset).value_or(0);
                    break;
            }
            // What the root checkMemory covered: its `size` bytes past the address it returned.
            auto const covered = fixedImmediateOf(span->root, ImmediateField::Size);
            if (!covered || span->offset > std::numeric_limits<uint64_t>::max() - reach
                || span->offset + reach > *covered)
                return unguarded;
            return {};
        }

        /// The fixed fields, then whole repeated groups only, each value within its field, each target
        /// within the function.
        /// @return The arguments the targets take, summed; or the fault.
        [[nodiscard]] std::expected<uint64_t, VerifierErrorCode> checkImmediates(
            Instruction const& instruction) const
        {
            auto const& schema = immediateSchemaRow(operationRow(instruction.operation).immediates);
            auto const immediates = _function.immediates(instruction.immediates);
            if (immediates.size() < schema.fixed.size())
                return std::unexpected { VerifierErrorCode::ImmediateCount };
            auto const rest = immediates.size() - schema.fixed.size();
            auto const wholeGroups = schema.repeated.empty() ? rest == 0 : rest % schema.repeated.size() == 0;
            if (!wholeGroups)
                return std::unexpected { VerifierErrorCode::ImmediateCount };
            auto arguments = uint64_t { 0 };
            for (auto const [position, value]: core::views::enumerate(immediates))
            {
                auto const field = immediateFieldAt(schema, position);
                if (!admits(immediateFieldRow(field), value))
                    return std::unexpected { VerifierErrorCode::ImmediateValue };
                if (field == ImmediateField::Target && value >= _function.blockCount())
                    return std::unexpected { VerifierErrorCode::TargetOutOfBounds };
                if (field == ImmediateField::Target && value == 0)
                    return std::unexpected { VerifierErrorCode::TargetIsEntry };
                if (field == ImmediateField::ArgumentCount)
                    arguments += value;
            }
            return arguments;
        }

        /// The fixed operands, of their types, then as many as the targets take, or any number for a
        /// variadic row without targets.
        [[nodiscard]] std::expected<void, VerifierErrorCode> checkOperands(Instruction const& instruction,
                                                                           uint64_t arguments) const
        {
            auto const& row = operationRow(instruction.operation);
            auto const& signature = operandSignatureRow(row.operands);
            auto const& schema = immediateSchemaRow(row.immediates);
            auto const operands = _function.operands(instruction.operands);
            auto const fixed = signature.fixed();
            auto expectedCount = std::optional<uint64_t> { fixed.size() };
            if (signature.tail == OperandTail::Variadic)
                expectedCount = hasTarget(schema) ? std::optional { fixed.size() + arguments } : std::nullopt;
            if (expectedCount ? operands.size() != *expectedCount : operands.size() < fixed.size())
                return std::unexpected { VerifierErrorCode::OperandCount };
            for (auto const [position, expected]: core::views::enumerate(fixed))
            {
                auto const type = _function.typeOf(operands[position]);
                if (expected == OperandType::SameAsOperand0)
                {
                    if (type != _function.typeOf(operands[0]))
                        return std::unexpected { VerifierErrorCode::OperandType };
                }
                else if (!holds(OperandTypeTable[std::to_underlying(expected)].admits, type))
                    return std::unexpected { VerifierErrorCode::OperandType };
            }
            return {};
        }

        /// The immediate of @p field in @p instruction, which its schema holds among its fixed fields.
        /// @return Its value; none when the schema has no such fixed field.
        [[nodiscard]] std::optional<uint64_t> immediateOf(Instruction const& instruction,
                                                          ImmediateField field) const
        {
            auto const& fields = immediateSchemaRow(operationRow(instruction.operation).immediates).fixed;
            auto const found = std::ranges::find(fields, field);
            if (found == fields.end())
                return std::nullopt;
            return _function.immediates(
                instruction.immediates)[static_cast<std::size_t>(found - fields.begin())];
        }

        /// What the immediates must agree on with the instruction's types: a `type` immediate within the
        /// schema's types; a `field` immediate's ContextValue as the `type` immediate, or, without one, as
        /// the stored operand, which must be a ContextValue generated code writes; an access's `size` at
        /// most its type's width, exactly it for a float, and a sign extension only when narrower.
        [[nodiscard]] std::expected<void, VerifierErrorCode> checkTypedImmediates(
            Instruction const& instruction) const
        {
            auto const& schema = immediateSchemaRow(operationRow(instruction.operation).immediates);
            auto const operands = _function.operands(instruction.operands);
            auto const type = immediateOf(instruction, ImmediateField::Type).transform([](uint64_t value) {
                return static_cast<Type>(value);
            });
            if (type && !holds(schema.types, *type))
                return std::unexpected { VerifierErrorCode::ImmediateValue };
            if (auto const field = immediateOf(instruction, ImmediateField::Field))
            {
                auto const& context = contextValueRow(static_cast<ContextValue>(*field));
                if (type && *type != context.type)
                    return std::unexpected { VerifierErrorCode::ImmediateValue };
                if (!type && context.access != ContextAccess::ReadWrite)
                    return std::unexpected { VerifierErrorCode::ImmediateValue };
                if (!type && _function.typeOf(operands[0]) != context.type)
                    return std::unexpected { VerifierErrorCode::OperandType };
            }
            if (schema.width == AccessWidth::None)
                return {};
            auto const accessType =
                schema.width == AccessWidth::OfType ? *type : _function.typeOf(operands[1]);
            if (!holds(NumberTypes, accessType))
                return std::unexpected { VerifierErrorCode::OperandType };
            auto const size = immediateOf(instruction, ImmediateField::Size).value_or(0);
            auto const bytes = typeRow(accessType).bytes;
            auto const isSignExtended = immediateOf(instruction, ImmediateField::Extension)
                                        == std::optional { uint64_t { std::to_underlying(Extension::Sign) } };
            if (size > bytes || (isFloat(accessType) && size != bytes) || (isSignExtended && size == bytes))
                return std::unexpected { VerifierErrorCode::ImmediateValue };
            return {};
        }

        /// None, one or any number of results, of the type the rule gives; the instruction's own type
        /// its first result's.
        [[nodiscard]] std::expected<void, VerifierErrorCode> checkResults(
            Instruction const& instruction) const
        {
            auto const& row = operationRow(instruction.operation);
            auto const results = _function.operands(instruction.results);
            auto const isSingle = row.result != ResultRule::None && row.result != ResultRule::Multiple;
            if ((row.result == ResultRule::None && !results.empty()) || (isSingle && results.size() != 1))
                return std::unexpected { VerifierErrorCode::ResultCount };
            if (isSingle && _function.typeOf(results[0]) != singleResultType(instruction))
                return std::unexpected { VerifierErrorCode::ResultType };
            auto const firstResultType = results.empty() ? Type::I32 : _function.typeOf(results[0]);
            if (instruction.type != firstResultType)
                return std::unexpected { VerifierErrorCode::ResultType };
            return {};
        }

        /// The type the row of @p instruction, which has a single result, gives it.
        [[nodiscard]] Type singleResultType(Instruction const& instruction) const
        {
            auto const& row = operationRow(instruction.operation);
            if (row.result == ResultRule::SameAsOperand0)
                return _function.typeOf(_function.operands(instruction.operands)[0]);
            if (row.result == ResultRule::FromImmediate)
            {
                auto const& fields = immediateSchemaRow(row.immediates).fixed;
                auto const position = std::ranges::find(fields, ImmediateField::Type) - fields.begin();
                return static_cast<Type>(
                    _function.immediates(instruction.immediates)[static_cast<std::size_t>(position)]);
            }
            auto const fixed = fixedTypeOf(row.result);
            Require(fixed.has_value());
            return *fixed;
        }

        /// Each target passed as many arguments as it has parameters, each of its parameter's type.
        [[nodiscard]] std::expected<void, VerifierErrorCode> checkArguments(
            Instruction const& instruction) const
        {
            auto argumentFault = std::optional<VerifierErrorCode> {};
            _function.forEachTarget(instruction, [&](BranchTarget const& target) {
                if (argumentFault)
                    return;
                auto const parameters = _function.operands(_function.block(target.block).parameters);
                auto const passed = _function.operands(target.arguments);
                if (passed.size() != parameters.size())
                {
                    argumentFault = VerifierErrorCode::ArgumentCount;
                    return;
                }
                for (auto const [position, argument]: core::views::enumerate(passed))
                    if (_function.typeOf(argument) != _function.typeOf(parameters[position]))
                    {
                        argumentFault = VerifierErrorCode::ArgumentType;
                        return;
                    }
            });
            if (argumentFault)
                return std::unexpected { *argumentFault };
            return {};
        }

        /// Whether every return returns as many values as the first, in block order, of the same types.
        [[nodiscard]] std::expected<void, VerifierError> checkReturns() const
        {
            auto first = std::optional<std::span<ValueId const>> {};
            for (auto const block: blocksOf(_function))
                for (auto const index: _function.block(block).instructions)
                {
                    auto const& instruction = _function.instruction(index);
                    if (instruction.operation != Operation::Return)
                        continue;
                    auto const values = _function.operands(instruction.operands);
                    if (!first)
                    {
                        first = values;
                        continue;
                    }
                    auto const sameType = [this](ValueId lhs, ValueId rhs) {
                        return _function.typeOf(lhs) == _function.typeOf(rhs);
                    };
                    if (!std::ranges::equal(*first, values, sameType))
                        return fault(VerifierErrorCode::ReturnsDisagree, block, index);
                }
            return {};
        }

        /// Whether every use is dominated by its value's definition.
        [[nodiscard]] std::expected<void, VerifierError> checkDominance() const
        {
            auto& memory = _function.memory();
            auto const graph = controlFlowGraphOf(_function, memory);
            auto const tree = DominatorTree { graph, memory };
            for (auto const block: blocksOf(_function))
                for (auto const [position, index]:
                     core::views::enumerate(_function.block(block).instructions))
                {
                    auto const& instruction = _function.instruction(index);
                    if (instruction.operation == Operation::Removed)
                        continue;
                    for (auto const operand: _function.operands(instruction.operands))
                    {
                        auto const& definition = _definitions[std::to_underlying(operand)];
                        auto const isDominated = definition.block == std::to_underlying(block)
                                                     ? definition.position < position + 1
                                                     : tree.dominates(BlockId { definition.block }, block);
                        if (!isDominated)
                            return fault(VerifierErrorCode::UseNotDominated, block, index);
                    }
                }
            return {};
        }

        /// Whether the live step countdown is the one the execution state holds.
        enum class Countdown : uint8_t
        {
            Stored,   ///< It is: a trap here exits with the right countdown.
            Unstored, ///< A poll changed it since it was stored.
        };

        /// The countdown's state after @p block entered as @p entry, and, when @p fail is Yes, the first trap
        /// site of it that finds the countdown unstored, or a store of a countdown the block superseded.
        enum class Report : uint8_t
        {
            No,  ///< Only the state at the end.
            Yes, ///< Faults too.
        };

        [[nodiscard]] std::expected<Countdown, VerifierError> countdownThrough(BlockId block,
                                                                               Countdown entry,
                                                                               Report report) const
        {
            auto state = entry;
            auto live = std::optional<ValueId> {};  // the countdown the block last polled or reloaded
            auto stale = std::optional<ValueId> {}; // the one that was live before the block's last call
            auto const isCountdownField = [this](Instruction const& instruction) {
                return _function.immediates(instruction.immediates)[0]
                       == std::to_underlying(ContextValue::StepCountdown);
            };
            for (auto const index: _function.block(block).instructions)
            {
                auto const& instruction = _function.instruction(index);
                if (instruction.operation == Operation::Removed)
                    continue;
                switch (countdownRuleOf(instruction.operation))
                {
                    case CountdownRule::Neutral: break;
                    case CountdownRule::TrapSite:
                        if (state == Countdown::Unstored && report == Report::Yes)
                            return fault(VerifierErrorCode::CountdownNotStored, block, index);
                        break;
                    case CountdownRule::Call:
                        if (state == Countdown::Unstored && report == Report::Yes)
                            return fault(VerifierErrorCode::CountdownNotStored, block, index);
                        stale = live;
                        live.reset();
                        break;
                    case CountdownRule::Changes:
                        state = Countdown::Unstored;
                        live = _function.operands(instruction.results)[0];
                        break;
                    case CountdownRule::Stores:
                        if (!isCountdownField(instruction))
                            break;
                        if (auto const stored = _function.operands(instruction.operands)[0];
                            ((live && stored != *live) || (stale && stored == *stale))
                            && report == Report::Yes)
                            return fault(VerifierErrorCode::CountdownNotStored, block, index);
                        state = Countdown::Stored;
                        break;
                    case CountdownRule::Reloads:
                        if (!isCountdownField(instruction))
                            break;
                        state = Countdown::Stored;
                        live = _function.operands(instruction.results)[0];
                        break;
                }
            }
            return state;
        }

        /// Whether every trap site finds the live countdown stored, on every path: a forward must-analysis
        /// over one bit per block entry, which only ever falls from Stored to Unstored and is pushed to the
        /// successors when it does, so each block is walked at most twice, and each edge followed at most
        /// once, before the final walk that reports.
        [[nodiscard]] std::expected<void, VerifierError> checkCountdown() const
        {
            auto& memory = _function.memory();
            auto const graph = controlFlowGraphOf(_function, memory);
            auto entries = std::pmr::vector<Countdown>(_function.blockCount(), Countdown::Stored, &memory);
            auto worklist = std::pmr::vector<BlockId> { &memory };
            for (auto const block: blocksOf(_function))
                worklist.push_back(block);
            while (!worklist.empty())
            {
                auto const block = worklist.back();
                worklist.pop_back();
                if (*countdownThrough(block, entries[std::to_underlying(block)], Report::No)
                    == Countdown::Stored)
                    continue;
                for (auto const successor: graph.successors(block))
                {
                    auto& entry = entries[std::to_underlying(successor)];
                    if (entry == Countdown::Unstored || std::to_underlying(successor) == 0)
                        continue;
                    entry = Countdown::Unstored;
                    worklist.push_back(successor);
                }
            }
            for (auto const block: blocksOf(_function))
                if (auto walked = countdownThrough(block, entries[std::to_underlying(block)], Report::Yes);
                    !walked)
                    return std::unexpected { walked.error() };
            return {};
        }

        Function const& _function;
        std::pmr::vector<Definition> _definitions;
        std::pmr::vector<bool> _isListed;
        /// Per value, where its guardAddress chain leads, worked out once by guardSpanOf(): what keeps the
        /// guarded-use checks linear however long a chain is or however often its end is used.
        mutable std::pmr::vector<GuardSpan> _spans;
        mutable std::pmr::vector<ValueId> _chain; ///< guardSpanOf()'s worklist.
    };
} // namespace

std::string describe(VerifierError const& error)
{
    auto text = std::string { VerifierErrorTable[std::to_underlying(error.code)].text };
    if (error.block)
        std::format_to(std::back_inserter(text), " in block {}", std::to_underlying(*error.block));
    if (error.instruction)
        std::format_to(std::back_inserter(text), " at instruction {}", *error.instruction);
    return text;
}

std::expected<void, VerifierError> verify(Function const& function)
{
    return Verification { function }.run();
}

} // namespace isoline::compiler::intermediate
