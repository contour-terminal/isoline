// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/intermediate/Function.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

namespace isoline::compiler::intermediate
{

namespace
{
    /// The room a block's parameters get when they first need any.
    constexpr auto InitialParameterCapacity = uint32_t { 4 };

    /// @p size as a pool index, which every pool keeps below 2^32 (Require).
    [[nodiscard]] uint32_t poolIndex(std::size_t size) noexcept
    {
        Require(size <= std::numeric_limits<uint32_t>::max());
        return static_cast<uint32_t>(size);
    }

    /// Appends @p values to @p pool.
    /// @return The run they occupy.
    template <typename Value>
    [[nodiscard]] OperandRange appendRun(std::pmr::vector<Value>& pool, std::span<Value const> values)
    {
        auto const begin = poolIndex(pool.size());
        pool.insert(pool.end(), values.begin(), values.end());
        return OperandRange { .begin = begin, .count = poolIndex(values.size()) };
    }
} // namespace

Function::Function(std::pmr::memory_resource& memory):
    _memory { &memory },
    _blocks { &memory },
    _parameterCapacity { &memory },
    _instructions { &memory },
    _operands { &memory },
    _immediates { &memory },
    _valueTypes { &memory }
{
}

Function& Function::operator=(Function&& other) noexcept
{
    Require(_memory == other._memory);
    _blocks = std::move(other._blocks);
    _parameterCapacity = std::move(other._parameterCapacity);
    _instructions = std::move(other._instructions);
    _operands = std::move(other._operands);
    _immediates = std::move(other._immediates);
    _valueTypes = std::move(other._valueTypes);
    return *this;
}

uint64_t Function::parameterGrowth(BlockId block) const noexcept
{
    auto const count = this->block(block).parameters.count;
    auto const capacity = uint64_t { _parameterCapacity[std::to_underlying(block)] };
    if (count < capacity)
        return 0;
    return std::max(uint64_t { InitialParameterCapacity }, capacity * 2);
}

Block const& Function::block(BlockId block) const noexcept
{
    Require(std::to_underlying(block) < _blocks.size());
    return _blocks[std::to_underlying(block)];
}

Block& Function::block(BlockId block) noexcept
{
    Require(std::to_underlying(block) < _blocks.size());
    return _blocks[std::to_underlying(block)];
}

Instruction const& Function::instruction(uint32_t index) const noexcept
{
    Require(index < _instructions.size());
    return _instructions[index];
}

Instruction& Function::instruction(uint32_t index) noexcept
{
    Require(index < _instructions.size());
    return _instructions[index];
}

Type Function::typeOf(ValueId value) const noexcept
{
    Require(std::to_underlying(value) < _valueTypes.size());
    return _valueTypes[std::to_underlying(value)];
}

std::span<ValueId const> Function::operands(OperandRange range) const noexcept
{
    Require(uint64_t { range.begin } + range.count <= _operands.size());
    return std::span { _operands }.subspan(range.begin, range.count);
}

std::span<ValueId> Function::operands(OperandRange range) noexcept
{
    Require(uint64_t { range.begin } + range.count <= _operands.size());
    return std::span { _operands }.subspan(range.begin, range.count);
}

std::span<uint64_t const> Function::immediates(ImmediateRange range) const noexcept
{
    Require(uint64_t { range.begin } + range.count <= _immediates.size());
    return std::span { _immediates }.subspan(range.begin, range.count);
}

std::span<uint64_t> Function::immediates(ImmediateRange range) noexcept
{
    Require(uint64_t { range.begin } + range.count <= _immediates.size());
    return std::span { _immediates }.subspan(range.begin, range.count);
}

std::pmr::vector<BranchTarget> Function::targets(Instruction const& instruction) const
{
    auto result = std::pmr::vector<BranchTarget> { _memory };
    forEachTarget(instruction, [&result](BranchTarget const& target) { result.push_back(target); });
    return result;
}

std::optional<uint32_t> Function::terminator(BlockId block) const noexcept
{
    for (auto const index: this->block(block).instructions | std::views::reverse)
    {
        auto const operation = instruction(index).operation;
        if (operation == Operation::Removed)
            continue;
        if (!isTerminator(operation))
            return std::nullopt;
        return index;
    }
    return std::nullopt;
}

std::pmr::vector<BlockId> Function::successors(BlockId block) const
{
    auto result = std::pmr::vector<BlockId> { _memory };
    if (auto const index = terminator(block))
        forEachTarget(instruction(*index),
                      [&result](BranchTarget const& target) { result.push_back(target.block); });
    return result;
}

void Function::remove(uint32_t index) noexcept
{
    instruction(index).operation = Operation::Removed;
}

void Function::compact()
{
    // Number the values that stay in block order: each block's parameters, then each kept instruction's
    // results, so that equal functions compact to equal numberings.
    auto renumbered = std::pmr::vector<uint32_t>(_valueTypes.size(), Unnumbered, _memory);
    auto valueTypes = std::pmr::vector<Type> { _memory };
    auto const number = [&](ValueId value) {
        Require(std::to_underlying(value) < renumbered.size());
        renumbered[std::to_underlying(value)] = poolIndex(valueTypes.size());
        valueTypes.push_back(_valueTypes[std::to_underlying(value)]);
    };
    for (auto const& block: _blocks)
    {
        for (auto const value: operands(block.parameters))
            number(value);
        for (auto const index: block.instructions)
            if (auto const& kept = instruction(index); kept.operation != Operation::Removed)
                for (auto const value: operands(kept.results))
                    number(value);
    }
    auto const renumber = [&renumbered](ValueId value) {
        Require(std::to_underlying(value) < renumbered.size());
        auto const mapped = renumbered[std::to_underlying(value)];
        Require(mapped != Unnumbered); // a use of a value whose definition was removed
        return ValueId { mapped };
    };

    // Repack the pools and the instructions in the same order.
    auto operandPool = std::pmr::vector<ValueId> { _memory };
    auto immediatePool = std::pmr::vector<uint64_t> { _memory };
    auto instructionList = std::pmr::vector<Instruction> { _memory };
    auto const appendRenumbered = [&](OperandRange range) {
        auto const begin = poolIndex(operandPool.size());
        for (auto const value: operands(range))
            operandPool.push_back(renumber(value));
        return OperandRange { .begin = begin, .count = range.count };
    };
    for (auto const blockIndex: std::views::iota(std::size_t { 0 }, _blocks.size()))
    {
        auto& block = _blocks[blockIndex];
        block.parameters = appendRenumbered(block.parameters);
        _parameterCapacity[blockIndex] = block.parameters.count;
        auto kept = std::pmr::vector<uint32_t> { _memory };
        for (auto const index: block.instructions)
        {
            auto const& old = instruction(index);
            if (old.operation == Operation::Removed)
                continue;
            auto const immediateBegin = poolIndex(immediatePool.size());
            auto const values = immediates(old.immediates);
            immediatePool.insert(immediatePool.end(), values.begin(), values.end());
            kept.push_back(poolIndex(instructionList.size()));
            instructionList.push_back(Instruction {
                .operation = old.operation,
                .type = old.type,
                .operands = appendRenumbered(old.operands),
                .immediates = ImmediateRange { .begin = immediateBegin, .count = old.immediates.count },
                .results = appendRenumbered(old.results),
            });
        }
        block.instructions = std::move(kept);
    }
    _operands = std::move(operandPool);
    _immediates = std::move(immediatePool);
    _instructions = std::move(instructionList);
    _valueTypes = std::move(valueTypes);
}

Builder::Builder(Function& function) noexcept: _function { &function }
{
}

BlockId Builder::createBlock()
{
    auto const id = BlockId { poolIndex(_function->_blocks.size()) };
    _function->_blocks.push_back(Block { .parameters = {},
                                         .instructions = std::pmr::vector<uint32_t> { _function->_memory },
                                         .hotness = Hotness::Normal });
    _function->_parameterCapacity.push_back(0);
    return id;
}

ValueId Builder::addParameter(BlockId block, Type type)
{
    auto& function = *_function;
    auto& parameters = function.block(block).parameters;
    auto& capacity = function._parameterCapacity[std::to_underlying(block)];
    if (auto const growth = function.parameterGrowth(block); growth > 0)
    {
        // Move the run to the pool's end with twice the room, so that parameters added to a block
        // while others grow cost amortized O(1) each. The room is counted in 64 bits: doubling a
        // capacity of 2^31 must not wrap to 0.
        auto& pool = function._operands;
        auto const begin = poolIndex(pool.size());
        auto const newCapacity = poolIndex(uint64_t { begin } + growth) - begin;
        pool.resize(pool.size() + newCapacity);
        std::ranges::copy(pool.begin() + parameters.begin,
                          pool.begin() + parameters.begin + parameters.count,
                          pool.begin() + begin);
        parameters.begin = begin;
        capacity = newCapacity;
    }
    auto const value = createValue(type);
    function._operands[parameters.begin + parameters.count] = value;
    ++parameters.count;
    return value;
}

void Builder::switchTo(BlockId block) noexcept
{
    Require(std::to_underlying(block) < _function->blockCount());
    _current = block;
}

std::span<ValueId const> Builder::emit(Operation operation,
                                       std::span<Type const> resultTypes,
                                       std::span<ValueId const> operands,
                                       std::span<uint64_t const> immediates)
{
    auto const parts = std::array { operands };
    return append(operation, resultTypes, parts, immediates);
}

void Builder::jump(BlockId target, std::span<ValueId const> arguments)
{
    auto const immediates =
        std::array { uint64_t { std::to_underlying(target) }, uint64_t { arguments.size() } };
    auto const parts = std::array { arguments };
    std::ignore = append(Operation::Jump, {}, parts, immediates);
}

void Builder::branch(ValueId condition,
                     BlockId ifNonZero,
                     std::span<ValueId const> ifNonZeroArguments,
                     BlockId ifZero,
                     std::span<ValueId const> ifZeroArguments)
{
    auto const immediates = std::array { uint64_t { std::to_underlying(ifNonZero) },
                                         uint64_t { ifNonZeroArguments.size() },
                                         uint64_t { std::to_underlying(ifZero) },
                                         uint64_t { ifZeroArguments.size() } };
    auto const conditions = std::array { condition };
    auto const parts =
        std::array<std::span<ValueId const>, 3> { conditions, ifNonZeroArguments, ifZeroArguments };
    std::ignore = append(Operation::Branch, {}, parts, immediates);
}

void Builder::switchOn(ValueId index, std::span<BranchTo const> targets, BranchTo fallback)
{
    auto immediates = std::pmr::vector<uint64_t> { _function->_memory };
    auto parts = std::pmr::vector<std::span<ValueId const>> { _function->_memory };
    auto const indices = std::array { index };
    parts.emplace_back(indices);
    auto const add = [&](BranchTo const& target) {
        immediates.push_back(std::to_underlying(target.block));
        immediates.push_back(target.arguments.size());
        parts.push_back(target.arguments);
    };
    add(fallback);
    for (auto const& target: targets)
        add(target);
    std::ignore = append(Operation::Switch, {}, parts, immediates);
}

void Builder::replaceOperands(uint32_t index, std::span<ValueId const> operands)
{
    auto& function = *_function;
    Require(index < function._instructions.size());
    auto const run = appendRun(function._operands, operands);
    function._instructions[index].operands = run;
}

void Builder::ret(std::span<ValueId const> values)
{
    std::ignore = emit(Operation::Return, {}, values);
}

void Builder::trap(runtime::TrapCode code)
{
    auto const immediates = std::array { uint64_t { std::to_underlying(code) } };
    std::ignore = emit(Operation::Trap, {}, {}, immediates);
}

std::span<ValueId const> Builder::append(Operation operation,
                                         std::span<Type const> resultTypes,
                                         std::span<std::span<ValueId const> const> operandParts,
                                         std::span<uint64_t const> immediates)
{
    Require(_current.has_value());
    auto& function = *_function;
    auto const operandBegin = poolIndex(function._operands.size());
    for (auto const part: operandParts)
        function._operands.insert(function._operands.end(), part.begin(), part.end());
    auto const operands =
        OperandRange { .begin = operandBegin, .count = poolIndex(function._operands.size() - operandBegin) };
    auto const immediateRun = appendRun(function._immediates, immediates);
    auto const resultBegin = poolIndex(function._operands.size());
    for (auto const type: resultTypes)
    {
        auto const value = createValue(type);
        function._operands.push_back(value);
    }
    auto const results = OperandRange { .begin = resultBegin, .count = poolIndex(resultTypes.size()) };
    function.block(*_current).instructions.push_back(poolIndex(function._instructions.size()));
    function._instructions.push_back(Instruction {
        .operation = operation,
        .type = resultTypes.empty() ? Type::I32 : resultTypes.front(),
        .operands = operands,
        .immediates = ImmediateRange { .begin = immediateRun.begin, .count = immediateRun.count },
        .results = results,
    });
    return function.operands(results);
}

ValueId Builder::createValue(Type type)
{
    auto const value = ValueId { poolIndex(_function->_valueTypes.size()) };
    _function->_valueTypes.push_back(type);
    return value;
}

} // namespace isoline::compiler::intermediate
