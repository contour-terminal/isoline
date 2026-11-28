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

/// Modules shaped to make a lowering that is not linear in its input blow up: the inputs of the
/// interpreter's bounds on lowered code, lowering work and the compile budget, shared by the unit tests
/// and the memory suite.
namespace isoline::interpreter::testing
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

/// A function of one i32 param whose body nests @p depth blocks, loops or ifs, each of whose bodies is
/// the next one, then a br 0; an if's else branch is a br 0 too.
///
/// A lowering that recurses on nesting overflows its stack; one that walks its open blocks at each branch
/// or end does @p depth² work.
/// @param depth The nesting depth.
/// @param opcode block (0x02), loop (0x03) or if (0x04).
/// @return The module.
[[nodiscard]] inline binary::testing::ModuleBuilder nestedBlocks(uint32_t depth, uint8_t opcode)
{
    auto const conditional = opcode == 0x04;
    auto body = binary::testing::Bytes {};
    body.reserve(std::size_t { depth } * 9);
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, depth))
    {
        if (conditional)
            body.insert(body.end(), { 0x20, 0x00 }); // local.get 0
        body.insert(body.end(), { opcode, 0x40 });
    }
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, depth))
    {
        body.insert(body.end(), { 0x0C, 0x00 }); // br 0
        if (conditional)
            body.insert(body.end(), { 0x05, 0x0C, 0x00 }); // else, br 0
        body.push_back(0x0B);
    }

    auto builder = binary::testing::ModuleBuilder {};
    builder.type(binary::FuncType { .params = { binary::I32 }, .results = {} });
    builder.function(0, {}, std::move(body));
    return builder;
}

/// The function type of type index 1 in the multi-value modules below: @p params i32 params and
/// @p results i32 results.
[[nodiscard]] inline binary::FuncType i32Signature(uint32_t params, uint32_t results)
{
    return binary::FuncType { .params = std::vector<binary::ValueType>(params, binary::I32),
                              .results = std::vector<binary::ValueType>(results, binary::I32) };
}

/// A function whose body is a block of @p results i32 results holding an i32 below them, then
/// @p branches times (i32.const 0, br_if 0), then br 0.
///
/// Each br_if carries the @p results values one slot up from the block's: a lowering that moves them
/// one by one emits @p results × @p branches moves.
/// @param results The block's results, at most DecodeLimits::maxResults.
/// @param branches The br_ifs.
/// @return The module.
[[nodiscard]] inline binary::testing::ModuleBuilder branchesWithValues(uint32_t results, uint32_t branches)
{
    auto body = binary::testing::Bytes {};
    body.reserve((std::size_t { results } * 3) + (std::size_t { branches } * 4) + 8);
    body.insert(body.end(), { 0x02, 0x01, 0x41, 0x00 }); // block (type 1), i32.const 0
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, results))
        body.insert(body.end(), { 0x41, 0x01 }); // i32.const 1
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, branches))
        body.insert(body.end(), { 0x41, 0x00, 0x0D, 0x00 }); // i32.const 0, br_if 0
    body.insert(body.end(), { 0x0C, 0x00, 0x0B });           // br 0, end
    body.insert(body.end(), results, 0x1A);                  // drop

    auto builder = binary::testing::ModuleBuilder {};
    builder.type(binary::FuncType { .params = {}, .results = {} });
    builder.type(i32Signature(0, results));
    builder.function(0, {}, std::move(body));
    return builder;
}

/// A function whose body is a block of @p results i32 results inside another such block, holding an i32
/// below them, ending with a br_table of @p labels labels and a default, to the inner and the outer
/// block in turn.
///
/// A lowering that moves the values per label emits @p results × @p labels moves.
/// @param results The blocks' results, at most DecodeLimits::maxResults.
/// @param labels The br_table's labels before its default, at most DecodeLimits::maxBrTableSize.
/// @return The module.
[[nodiscard]] inline binary::testing::ModuleBuilder branchTableWithValues(uint32_t results, uint32_t labels)
{
    auto body = binary::testing::Bytes {};
    body.reserve((std::size_t { results } * 3) + (std::size_t { labels } * 2) + 32);
    body.insert(body.end(), { 0x02, 0x01, 0x02, 0x01, 0x41, 0x00 }); // block (type 1) twice, i32.const 0
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, results))
        body.insert(body.end(), { 0x41, 0x01 });   // i32.const 1
    body.insert(body.end(), { 0x41, 0x00, 0x0E }); // i32.const 0, br_table
    auto const count = binary::testing::encodeU32(labels);
    body.insert(body.end(), count.begin(), count.end());
    for (auto const index: std::views::iota(uint32_t { 0 }, labels + 1))
        body.push_back(static_cast<uint8_t>(index % 2)); // the inner block, the outer block
    body.insert(body.end(), { 0x0B, 0x0B });             // end, end
    body.insert(body.end(), results, 0x1A);              // drop

    auto builder = binary::testing::ModuleBuilder {};
    builder.type(binary::FuncType { .params = {}, .results = {} });
    builder.type(i32Signature(0, results));
    builder.function(0, {}, std::move(body));
    return builder;
}

/// A function of one i32 param whose body pushes @p arity i32 values, then @p count times a block, or an if
/// and its else, of @p arity params and as many results, each of whose branches is a br 0; then drops them.
///
/// After each br the values are those of the block's end again: a lowering that pushes them one by one
/// does @p arity × @p count work.
/// @param arity The blocks' params and results, at most DecodeLimits::maxParams.
/// @param count The blocks.
/// @param opcode block (0x02) or if (0x04).
/// @return The module.
[[nodiscard]] inline binary::testing::ModuleBuilder reenteredBlocks(uint32_t arity,
                                                                    uint32_t count,
                                                                    uint8_t opcode)
{
    auto const conditional = opcode == 0x04;
    auto body = binary::testing::Bytes {};
    body.reserve((std::size_t { arity } * 3) + (std::size_t { count } * 10));
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, arity))
        body.insert(body.end(), { 0x41, 0x01 }); // i32.const 1
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
    {
        if (conditional)
            body.insert(body.end(), { 0x20, 0x00 });           // local.get 0
        body.insert(body.end(), { opcode, 0x01, 0x0C, 0x00 }); // block or if (type 1), br 0
        if (conditional)
            body.insert(body.end(), { 0x05, 0x0C, 0x00 }); // else, br 0
        body.push_back(0x0B);
    }
    body.insert(body.end(), arity, 0x1A); // drop

    auto builder = binary::testing::ModuleBuilder {};
    builder.type(binary::FuncType { .params = { binary::I32 }, .results = {} });
    builder.type(i32Signature(arity, arity));
    builder.function(0, {}, std::move(body));
    return builder;
}

/// A module whose function 1, () -> (), declares @p locals exnref locals and calls function 0, () -> (), @p
/// calls times. A lowering that copies the exnref locals, or the handlers, per call site makes @p locals × @p
/// calls records.
/// @param locals The exnref locals.
/// @param calls The call sites.
/// @return The module.
[[nodiscard]] inline binary::testing::ModuleBuilder exnrefLocalsAroundCalls(uint32_t locals, uint32_t calls)
{
    auto body = binary::testing::Bytes {};
    body.reserve(2 * std::size_t { calls });
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, calls))
        body.insert(body.end(), { 0x10, 0x00 }); // call 0
    auto builder = binary::testing::ModuleBuilder {};
    builder.type(binary::FuncType {});
    builder.function(0, {}, {});
    builder.function(0, std::vector<binary::ValueType>(locals, binary::ExnRef), std::move(body));
    return builder;
}

/// A module whose function 1, () -> (), calls function 0, () -> (), @p calls times inside @p depth nested
/// try_tables, each of one catch_all to the label outside it. A lowering that copies the handlers around a
/// site into it makes @p depth × @p calls records.
/// @param depth The nested try_tables.
/// @param calls The call sites.
/// @return The module.
[[nodiscard]] inline binary::testing::ModuleBuilder nestedTryTablesAroundCalls(uint32_t depth, uint32_t calls)
{
    auto body = binary::testing::Bytes {};
    body.reserve((5 * std::size_t { depth }) + (2 * std::size_t { calls }) + depth);
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, depth))
        body.insert(body.end(), { 0x1F, 0x40, 0x01, 0x02, 0x00 }); // try_table (catch_all 0)
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, calls))
        body.insert(body.end(), { 0x10, 0x00 }); // call 0
    body.insert(body.end(), depth, 0x0B);        // end
    auto builder = binary::testing::ModuleBuilder {};
    builder.type(binary::FuncType {});
    builder.function(0, {}, {});
    builder.function(0, {}, std::move(body));
    return builder;
}

} // namespace isoline::interpreter::testing
