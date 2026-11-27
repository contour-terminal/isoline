// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Listing.hpp>
#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/validate/ValidatedModule.hpp>

/// What the lowering tests share: modules of one function, lowered under a budget, and their listings.
namespace isoline::fastinterpreter::testing
{

using binary::testing::Bytes;
using binary::testing::ModuleBuilder;

/// The compile budget the lowering tests run under unless they test the budget: P4's default.
inline constexpr auto DefaultBudget = uint64_t { 256 } * 1024 * 1024;

/// An instruction of no immediates.
[[nodiscard]] inline Bytes op(uint8_t opcode)
{
    return { opcode };
}

/// An instruction of one u32 immediate.
[[nodiscard]] inline Bytes op(uint8_t opcode, uint32_t immediate)
{
    return binary::testing::code({ { opcode }, binary::testing::encodeU32(immediate) });
}

/// The bytes of a function of @p params to @p results, declaring @p locals, with body @p body.
[[nodiscard]] inline ModuleBuilder functionOf(std::vector<binary::ValueType> params,
                                              std::vector<binary::ValueType> results,
                                              std::vector<binary::ValueType> locals,
                                              Bytes body)
{
    auto builder = ModuleBuilder {};
    builder.type(binary::FuncType { .params = std::move(params), .results = std::move(results) });
    builder.function(0, std::move(locals), std::move(body));
    return builder;
}

/// @p builder's module, decoded and validated with every feature.
[[nodiscard]] inline std::shared_ptr<validate::ValidatedModule const> validated(ModuleBuilder const& builder)
{
    auto result =
        validate::ValidatedModule::load(builder.build(), isoline::FeatureSet::all(), binary::DecodeLimits {});
    REQUIRE(result.has_value());
    return *result;
}

/// A counting arena of @p budget bytes over the global heap.
[[nodiscard]] inline std::shared_ptr<runtime::CountingArena> arenaOf(uint64_t budget = DefaultBudget)
{
    return std::make_shared<runtime::CountingArena>(budget, *std::pmr::new_delete_resource());
}

/// @p module lowered with @p config under the default budget.
[[nodiscard]] inline std::shared_ptr<LoweredModule const> lowered(
    std::shared_ptr<validate::ValidatedModule const> const& module, LoweringConfig const& config = {})
{
    auto result = lower(*module, config, arenaOf());
    REQUIRE(result.has_value());
    return *result;
}

/// The listing of the first function of @p builder's module.
[[nodiscard]] inline std::string listingOf(ModuleBuilder const& builder, LoweringConfig const& config = {})
{
    auto const module = lowered(validated(builder), config);
    REQUIRE_FALSE(module->functions().empty());
    return describe(module->functions().front());
}

/// The value types of a signature's kinds.
[[nodiscard]] inline std::vector<binary::ValueType> typesOf(std::span<binary::ValueKind const> kinds)
{
    auto types = std::vector<binary::ValueType> {};
    for (auto const kind: kinds)
        types.push_back(binary::ValueType { .kind = kind });
    return types;
}

/// The binary encoding of @p opcode, with no immediates.
[[nodiscard]] inline Bytes encodingOf(binary::Opcode opcode)
{
    auto const& info = binary::opcodeInfo(opcode);
    if (info.prefix == binary::NoPrefix)
        return { static_cast<uint8_t>(info.code) };
    return binary::testing::code({ { info.prefix }, binary::testing::encodeU32(info.code) });
}

/// @p builder's module lowered under the default budget, checked against every published bound: its
/// code per function and in all, its work, what the arena kept and its peak.
///
/// The budget is the module's reservations, room for the exact copy of the largest function's code (at most
/// its code bound), and @p temporaries, the allowance for the abstract stack, the forwarding chains and the
/// locals' runs: a lowering that allocates past that is refused, not merely measured.
[[nodiscard]] inline std::shared_ptr<LoweredModule const> loweredWithinTheBounds(ModuleBuilder const& builder,
                                                                                 uint64_t temporaries)
{
    auto const module = validated(builder);
    auto const& source = module->module();
    auto budget = temporaries;
    auto largestCode = uint64_t { 0 };
    for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(source.code.size())))
    {
        budget += loweringReservation(source, index);
        largestCode = std::max(largestCode, codeBoundWords(source.code[index]) * sizeof(Word));
    }
    budget += largestCode;
    auto const arena = arenaOf(budget);
    auto const result = lower(*module, LoweringConfig {}, arena);
    if (!result)
        UNSCOPED_INFO("refused at " << result.error().codeIndex << ": " << result.error().requested
                                    << " bytes of " << result.error().limit << ", peak " << arena->peak());
    REQUIRE(result.has_value());
    auto const& lowered = **result;
    auto reservations = uint64_t { 0 };
    auto codeWords = uint64_t { 0 };
    for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(source.code.size())))
    {
        reservations += loweringReservation(source, index);
        codeWords += lowered.functions()[index].code.size();
        CHECK(lowered.functions()[index].code.size() <= codeBoundWords(source.code[index]));
    }
    auto const& stats = lowered.stats();
    CHECK(codeWords
          <= (stats.bodyUnits * MaxWordsPerBodyUnit) + (source.code.size() * FunctionOverheadWords));
    CHECK(lowered.loweredBytes() <= reservations);
    CHECK(stats.work <= WorkPerBodyUnit * stats.bodyUnits);
    CHECK(arena->peak() <= arena->budget());
    return *result;
}

} // namespace isoline::fastinterpreter::testing
