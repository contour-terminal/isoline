// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/optimization/Rewrite.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>

namespace isoline::compiler::optimization
{

using intermediate::BlockId;
using intermediate::Function;
using intermediate::ImmediateField;
using intermediate::Instruction;
using intermediate::Operation;
using intermediate::Unnumbered;
using intermediate::ValueId;

namespace
{
    /// The blocks of @p function, as indices.
    [[nodiscard]] auto blocksOf(Function const& function) noexcept
    {
        return std::views::iota(uint32_t { 0 }, function.blockCount())
               | std::views::transform([](uint32_t index) { return BlockId { index }; });
    }

    /// The listed instruction of @p function that @p block ends with, when it ends with a terminator.
    [[nodiscard]] Instruction const* terminatorOf(Function const& function, BlockId block) noexcept
    {
        auto const index = function.terminator(block);
        return index ? &function.instruction(*index) : nullptr;
    }
} // namespace

CancellationPoll::CancellationPoll(PassContext const& context) noexcept: _context { &context }
{
}

PassOutcome CancellationPoll::step(uint64_t units) noexcept
{
    _sinceLastLook += units;
    if (_sinceLastLook < CancellationInterval)
        return {};
    _sinceLastLook = 0;
    if (_context->cancellation.state() == CancellationState::Cancelled)
        return std::unexpected { CompileError { .code = CompileErrorCode::Cancelled,
                                                .function = _context->codeIndex } };
    return {};
}

Substitution::Substitution(uint32_t valueCount, std::pmr::memory_resource& memory): _next { &memory }
{
    _next.reserve(valueCount);
    for (auto const value: std::views::iota(uint32_t { 0 }, valueCount))
        _next.push_back(ValueId { value });
}

void Substitution::replace(ValueId from, ValueId to)
{
    auto const index = std::to_underlying(from);
    Require(index < _next.size() && _next[index] == from);
    Require(resolve(to) != from);
    _next[index] = to;
    ++_replaced;
}

ValueId Substitution::resolve(ValueId value) noexcept
{
    Require(std::to_underlying(value) < _next.size());
    auto root = value;
    while (_next[std::to_underlying(root)] != root)
        root = _next[std::to_underlying(root)];
    // Point every value of the chain at its root, so that the next walk is one step.
    while (value != root)
    {
        auto const next = _next[std::to_underlying(value)];
        _next[std::to_underlying(value)] = root;
        value = next;
    }
    return root;
}

void Substitution::applyTo(Function& function)
{
    if (isEmpty())
        return;
    for (auto const block: blocksOf(function))
        for (auto const index: function.block(block).instructions)
        {
            auto const& instruction = function.instruction(index);
            if (instruction.operation == Operation::Removed)
                continue;
            for (auto& operand: function.operands(instruction.operands))
                operand = resolve(operand);
        }
}

void dropParameters(Function& function, std::span<ParameterFate const> fates)
{
    Require(fates.size() == function.valueCount());
    auto const isDropped = [fates](ValueId value) {
        return fates[std::to_underlying(value)] == ParameterFate::Dropped;
    };
    for (auto const value: function.operands(function.block(BlockId { 0 }).parameters))
        Require(!isDropped(value));

    // Every terminator's arguments first, while each target's parameters are still the ones they pass to.
    for (auto const block: blocksOf(function))
    {
        auto const index = function.terminator(block);
        if (!index)
            continue;
        auto& terminator = function.instruction(*index);
        auto const& row = intermediate::operationRow(terminator.operation);
        auto const& schema = intermediate::immediateSchemaRow(row.immediates);
        auto const immediates = function.immediates(terminator.immediates);
        auto const operands = function.operands(terminator.operands);
        auto read = std::size_t { intermediate::operandSignatureRow(row.operands).count };
        auto write = read;
        for (auto const position: std::views::iota(std::size_t { 0 }, immediates.size()))
        {
            if (intermediate::immediateFieldAt(schema, position) != ImmediateField::Target)
                continue;
            auto const target = BlockId { static_cast<uint32_t>(immediates[position]) };
            auto const parameters = function.operands(function.block(target).parameters);
            auto const count = static_cast<std::size_t>(immediates[position + 1]);
            Require(count == parameters.size() && read + count <= operands.size());
            for (auto const argument: std::views::iota(std::size_t { 0 }, count))
                if (!isDropped(parameters[argument]))
                    operands[write++] = operands[read + argument];
            read += count;
            immediates[position + 1] =
                count - static_cast<std::size_t>(std::ranges::count_if(parameters, isDropped));
        }
        // Operands past the targets' (a return's values) are no arguments: they stay as they are.
        for (auto const operand: std::views::iota(read, operands.size()))
            operands[write++] = operands[operand];
        terminator.operands.count = static_cast<uint32_t>(write);
    }
    for (auto const block: blocksOf(function))
    {
        auto& parameters = function.block(block).parameters;
        auto const values = function.operands(parameters);
        auto write = std::size_t { 0 };
        for (auto const value: values)
            if (!isDropped(value))
                values[write++] = value;
        parameters.count = static_cast<uint32_t>(write);
    }
}

std::pmr::vector<BlockId> reversePostorderOf(Function const& function, std::pmr::memory_resource& memory)
{
    // An explicit-stack depth-first walk: each frame is a block and how many of its targets it has visited.
    struct Frame
    {
        BlockId block;
        std::size_t next;
    };
    auto visited = std::pmr::vector<uint8_t>(function.blockCount(), 0, &memory);
    auto postorder = std::pmr::vector<BlockId> { &memory };
    auto stack = std::pmr::vector<Frame> { &memory };
    auto targets = std::pmr::vector<BlockId> { &memory };
    auto const targetsOf = [&](BlockId block) {
        targets.clear();
        if (auto const* terminator = terminatorOf(function, block))
            function.forEachTarget(*terminator, [&targets](intermediate::BranchTarget const& target) {
                targets.push_back(target.block);
            });
    };
    auto successorBegins = std::pmr::vector<std::size_t> { &memory };
    auto successors = std::pmr::vector<BlockId> { &memory };
    successorBegins.reserve(std::size_t { function.blockCount() } + 1);
    for (auto const block: blocksOf(function))
    {
        successorBegins.push_back(successors.size());
        targetsOf(block);
        successors.insert(successors.end(), targets.begin(), targets.end());
    }
    successorBegins.push_back(successors.size());
    if (function.blockCount() == 0)
        return postorder;
    visited[0] = 1;
    stack.push_back(Frame { .block = BlockId { 0 }, .next = successorBegins[0] });
    while (!stack.empty())
    {
        auto& frame = stack.back();
        auto const end = successorBegins[std::to_underlying(frame.block) + 1];
        if (frame.next == end)
        {
            postorder.push_back(frame.block);
            stack.pop_back();
            continue;
        }
        auto const successor = successors[frame.next++];
        if (visited[std::to_underlying(successor)] != 0)
            continue;
        visited[std::to_underlying(successor)] = 1;
        stack.push_back(Frame { .block = successor, .next = successorBegins[std::to_underlying(successor)] });
    }
    std::ranges::reverse(postorder);
    return postorder;
}

uint32_t removeUnreachableBlocks(Function& function, std::pmr::memory_resource& memory)
{
    auto const reached = reversePostorderOf(function, memory);
    if (reached.size() == function.blockCount())
        return 0;
    auto isReached = std::pmr::vector<uint8_t>(function.blockCount(), 0, &memory);
    for (auto const block: reached)
        isReached[std::to_underlying(block)] = 1;
    auto removed = std::pmr::vector<BlockId> { &memory };
    for (auto const block: blocksOf(function))
        if (isReached[std::to_underlying(block)] == 0)
            removed.push_back(block);
    function.removeBlocks(removed);
    return static_cast<uint32_t>(removed.size());
}

std::pmr::vector<uint32_t> definitionsOf(Function const& function, std::pmr::memory_resource& memory)
{
    auto definitions = std::pmr::vector<uint32_t>(function.valueCount(), Unnumbered, &memory);
    for (auto const block: blocksOf(function))
        for (auto const index: function.block(block).instructions)
        {
            auto const& instruction = function.instruction(index);
            if (instruction.operation == Operation::Removed)
                continue;
            for (auto const value: function.operands(instruction.results))
                definitions[std::to_underlying(value)] = index;
        }
    return definitions;
}

ParameterPlaces parameterPlacesOf(Function const& function, std::pmr::memory_resource& memory)
{
    auto places =
        ParameterPlaces { .block = std::pmr::vector<uint32_t>(function.valueCount(), Unnumbered, &memory),
                          .position = std::pmr::vector<uint32_t>(function.valueCount(), 0, &memory) };
    for (auto const block: blocksOf(function))
    {
        auto position = uint32_t { 0 };
        for (auto const value: function.operands(function.block(block).parameters))
        {
            places.block[std::to_underlying(value)] = std::to_underlying(block);
            places.position[std::to_underlying(value)] = position++;
        }
    }
    return places;
}

IncomingEdges incomingEdgesOf(Function const& function, std::pmr::memory_resource& memory)
{
    auto incoming =
        IncomingEdges { .begins =
                            std::pmr::vector<uint32_t>(std::size_t { function.blockCount() } + 1, 0, &memory),
                        .edges = std::pmr::vector<IncomingEdge> { &memory } };
    auto const forEachEdge = [&function](auto const& visit) {
        for (auto const block: blocksOf(function))
            if (auto const* terminator = terminatorOf(function, block))
                function.forEachTarget(*terminator, visit);
    };
    forEachEdge([&incoming](intermediate::BranchTarget const& target) {
        ++incoming.begins[std::to_underlying(target.block) + 1];
    });
    for (auto const index: std::views::iota(std::size_t { 1 }, incoming.begins.size()))
        incoming.begins[index] += incoming.begins[index - 1];
    incoming.edges.resize(incoming.begins.back());
    auto filled = std::pmr::vector<uint32_t>(incoming.begins.begin(), incoming.begins.end() - 1, &memory);
    forEachEdge([&](intermediate::BranchTarget const& target) {
        incoming.edges[filled[std::to_underlying(target.block)]++] =
            IncomingEdge { .arguments = target.arguments.begin };
    });
    return incoming;
}

std::optional<uint64_t> constantOf(Function const& function,
                                   std::span<uint32_t const> definitions,
                                   ValueId value) noexcept
{
    auto const definition = definitions[std::to_underlying(value)];
    if (definition == Unnumbered)
        return std::nullopt;
    auto const& instruction = function.instruction(definition);
    switch (instruction.operation)
    {
        case Operation::I32Const:
        case Operation::I64Const:
        case Operation::F32Const:
        case Operation::F64Const: return function.immediates(instruction.immediates)[0];
        default: return std::nullopt;
    }
}

} // namespace isoline::compiler::optimization
