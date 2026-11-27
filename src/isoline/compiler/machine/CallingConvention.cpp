// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/machine/CallingConvention.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>

namespace isoline::compiler::machine
{

namespace
{
    /// The row of HostConventionTable for @p target's (architecture, ABI), if any.
    [[nodiscard]] HostConventionRow const* findHostRow(TargetDescription const& target) noexcept
    {
        auto const found = std::ranges::find_if(HostConventionTable, [&target](HostConventionRow const& row) {
            return row.architecture == target.architecture && row.abi == target.abi;
        });
        return found == HostConventionTable.end() ? nullptr : &*found;
    }

    /// @p bytes rounded up to a multiple of @p alignment (a power of two, Require).
    [[nodiscard]] uint32_t alignUp(uint64_t bytes, uint32_t alignment) noexcept
    {
        Require(alignment != 0 && (alignment & (alignment - 1)) == 0);
        auto const aligned = (bytes + alignment - 1) & ~uint64_t { alignment - 1 };
        Require(aligned <= UINT32_MAX);
        return static_cast<uint32_t>(aligned);
    }

    /// The next memory slot's offset after @p used slots of @p slotBytes from @p base.
    [[nodiscard]] uint32_t slotOffset(uint32_t base, std::size_t used, uint32_t slotBytes) noexcept
    {
        auto const offset = uint64_t { base } + (uint64_t { used } * slotBytes);
        Require(offset <= UINT32_MAX);
        return static_cast<uint32_t>(offset);
    }
} // namespace

RegisterClass registerClassOf(intermediate::Type type) noexcept
{
    switch (type)
    {
        case intermediate::Type::F32:
        case intermediate::Type::F64: return RegisterClass::Float;
        case intermediate::Type::I32:
        case intermediate::Type::I64:
        case intermediate::Type::Ref:
        case intermediate::Type::Pointer: return RegisterClass::General;
    }
    return RegisterClass::General; // uncovered(type-without-register-class)
}

Assignment assignArguments(ConventionRow const& row,
                           std::span<intermediate::Type const> types,
                           std::pmr::memory_resource& memory)
{
    auto assignment = Assignment { .locations = std::pmr::vector<ValueLocation> { &memory },
                                   .memoryBytes = 0,
                                   .area = ResultArea::None };
    assignment.locations.reserve(types.size());
    auto integers = std::size_t { 0 };
    auto floats = std::size_t { 0 };
    auto stacked = std::size_t { 0 };
    for (auto const [position, type]: core::views::enumerate(types))
    {
        auto const isFloat = registerClassOf(type) == RegisterClass::Float;
        auto const& registers = isFloat ? row.floatArguments : row.integerArguments;
        // Independent slots count each class on its own; shared slots count positions.
        auto& counted = isFloat ? floats : integers;
        auto const slot = row.slots == ArgumentSlots::Shared ? static_cast<std::size_t>(position) : counted;
        if (slot < registers.size())
        {
            assignment.locations.emplace_back(registers[slot]);
            ++counted;
        }
        else
        {
            assignment.locations.emplace_back(
                StackOffset { .bytes = slotOffset(row.shadowSpaceBytes, stacked, row.stackSlotBytes) });
            ++stacked;
        }
    }
    auto const area = uint64_t { row.shadowSpaceBytes } + (uint64_t { stacked } * row.stackSlotBytes);
    assignment.memoryBytes = alignUp(area, row.stackAlignment);
    return assignment;
}

Assignment assignResults(ConventionRow const& row,
                         std::span<intermediate::Type const> types,
                         std::pmr::memory_resource& memory)
{
    auto assignment = Assignment { .locations = std::pmr::vector<ValueLocation> { &memory },
                                   .memoryBytes = 0,
                                   .area = ResultArea::None };
    assignment.locations.reserve(types.size());
    // A host convention returns an aggregate too large for its registers wholly in memory.
    auto const aggregateBytes = uint64_t { types.size() } * row.stackSlotBytes;
    auto const inMemory = row.aggregateResults == AggregateResults::HiddenPointerBeyond
                          && aggregateBytes > row.aggregateRegisterBytes;
    auto integers = std::size_t { 0 };
    auto floats = std::size_t { 0 };
    auto stored = std::size_t { 0 };
    for (auto const type: types)
    {
        auto const isFloat = registerClassOf(type) == RegisterClass::Float;
        auto const& registers = isFloat ? row.floatResults : row.integerResults;
        auto& counted = isFloat ? floats : integers;
        if (!inMemory && counted < registers.size())
        {
            assignment.locations.emplace_back(registers[counted]);
            ++counted;
        }
        else
        {
            assignment.locations.emplace_back(
                StackOffset { .bytes = slotOffset(0, stored, row.stackSlotBytes) });
            ++stored;
        }
    }
    assignment.memoryBytes = slotOffset(0, stored, row.stackSlotBytes);
    if (inMemory)
        assignment.area = ResultArea::HiddenPointer;
    else if (stored != 0)
        assignment.area = ResultArea::CallerArea;
    return assignment;
}

CallAssignment assignCall(ConventionRow const& row,
                          std::span<intermediate::Type const> parameters,
                          std::span<intermediate::Type const> results,
                          std::pmr::memory_resource& memory)
{
    auto call =
        CallAssignment { .arguments = Assignment { .locations = std::pmr::vector<ValueLocation> { &memory },
                                                   .memoryBytes = 0,
                                                   .area = ResultArea::None },
                         .results = assignResults(row, results, memory),
                         .resultPointer = std::nullopt,
                         .returnedPointer = std::nullopt };
    if (call.results.area == ResultArea::None || row.resultAreaPointer)
    {
        call.arguments = assignArguments(row, parameters, memory);
        if (call.results.area != ResultArea::None)
            call.resultPointer = *row.resultAreaPointer;
    }
    else
    {
        // The hidden pointer is the first argument: the others move up by one.
        auto shifted = std::pmr::vector<intermediate::Type> { &memory };
        shifted.reserve(parameters.size() + 1);
        shifted.push_back(intermediate::Type::Pointer);
        shifted.insert(shifted.end(), parameters.begin(), parameters.end());
        call.arguments = assignArguments(row, shifted, memory);
        call.resultPointer = call.arguments.locations.front();
    }
    if (call.results.area == ResultArea::HiddenPointer && row.pointerReturned == PointerReturned::Yes
        && !row.integerResults.empty())
        call.returnedPointer = row.integerResults.front();
    return call;
}

bool hasHostConvention(TargetDescription const& target) noexcept
{
    return findHostRow(target) != nullptr;
}

Convention hostConvention(TargetDescription const& target) noexcept
{
    auto const* row = findHostRow(target);
    Require(row != nullptr);
    return row->convention;
}

} // namespace isoline::compiler::machine
