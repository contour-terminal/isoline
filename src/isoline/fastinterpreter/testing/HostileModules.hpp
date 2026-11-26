// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

/// Modules shaped to make a lowering that is not linear in its input blow up: the inputs of the fast
/// interpreter's bounds on lowered code, lowering work and the compile budget, shared by the unit tests
/// and the memory suite.
namespace isoline::fastinterpreter::testing
{

/// A function of two i32 params and @p locals declared locals, each a run of its own (i32, funcref and
/// v128 in turn), whose body forwards local 0 @p height times, sets local 0 from local 1 @p pairs times
/// over that stack, then drops it.
///
/// Every local.set of local 0 meets a @p height-high stack; only the first finds forwarders of it. A
/// lowering that rescans the stack at each local.set does @p height × @p pairs work.
/// @param locals The declared locals, at most DecodeLimits::maxLocals.
/// @param height The operand stack's height.
/// @param pairs The local.get 1, local.set 0 pairs.
/// @return The module.
[[nodiscard]] inline binary::testing::ModuleBuilder forwardedStack(uint32_t locals,
                                                                   uint32_t height,
                                                                   uint32_t pairs)
{
    auto const types = std::to_array<binary::ValueType>({ binary::I32, binary::FuncRef, binary::V128 });
    auto declared = std::vector<binary::ValueType> {};
    declared.reserve(locals);
    for (auto const index: std::views::iota(uint32_t { 0 }, locals))
        declared.push_back(types[index % types.size()]);

    auto body = binary::testing::Bytes {};
    body.reserve((std::size_t { height } * 3) + (std::size_t { pairs } * 4));
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, height))
        body.insert(body.end(), { 0x20, 0x00 }); // local.get 0
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, pairs))
        body.insert(body.end(), { 0x20, 0x01, 0x21, 0x00 }); // local.get 1, local.set 0
    body.insert(body.end(), height, 0x1A);                   // drop

    auto builder = binary::testing::ModuleBuilder {};
    builder.type(binary::FuncType { .params = { binary::I32, binary::I32 }, .results = {} });
    builder.function(0, std::move(declared), std::move(body));
    return builder;
}

/// A function whose body is i32.const 0, drop, repeated over about @p bodyBytes bytes.
/// @param bodyBytes The body's size, at most DecodeLimits::maxFunctionBodySize less a few bytes.
/// @return The module.
[[nodiscard]] inline binary::testing::ModuleBuilder constantsAndDrops(uint32_t bodyBytes)
{
    auto body = binary::testing::Bytes {};
    body.reserve(bodyBytes);
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, bodyBytes / 3))
        body.insert(body.end(), { 0x41, 0x00, 0x1A }); // i32.const 0, drop

    auto builder = binary::testing::ModuleBuilder {};
    builder.type(binary::FuncType { .params = {}, .results = {} });
    builder.function(0, {}, std::move(body));
    return builder;
}

} // namespace isoline::fastinterpreter::testing
