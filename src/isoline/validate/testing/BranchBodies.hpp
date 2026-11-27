// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

/// Modules whose bodies are runs of branches to labels of many operands: the shapes that bound what
/// validating a branch costs, shared by the validator's unit tests and the memory suite.
namespace isoline::validate::testing
{

/// A module of one function of type [] -> [i32 x @p arity], which calls itself for its results and then
/// branches @p count times with br_if to its own label: four input bytes per br_if, each a branch whose
/// label carries @p arity operands.
/// @param arity The operands the function's label carries.
/// @param count How many br_ifs the body runs.
/// @return The module's bytes.
[[nodiscard]] inline std::vector<std::byte> brIfsToWideLabel(std::size_t arity, uint32_t count)
{
    auto body = std::vector<uint8_t> { 0x10, 0x00 }; // call 0
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        body.insert(body.end(), { 0x41, 0x00, 0x0D, 0x00 }); // i32.const 0; br_if 0
    auto builder = binary::testing::ModuleBuilder {};
    builder
        .type(
            binary::FuncType { .params = {}, .results = std::vector<binary::ValueType>(arity, binary::I32) })
        .function(0, {}, std::move(body));
    return builder.build();
}

/// A module of one function of type [] -> [i32 x @p arity], which calls itself for its results, opens
/// @p depth blocks of type [i32 x @p arity] -> [i32 x @p arity], and in them runs @p tables br_tables
/// of @p labels labels each, every one in a block of that type of its own. The labels cycle through
/// the open blocks and the function's own label: one or two input bytes per label, each a branch
/// whose label carries @p arity operands.
/// @param arity The operands every label carries.
/// @param depth How many blocks the br_tables are nested in.
/// @param labels The labels of each br_table, its default label excluded.
/// @param tables How many br_tables the body runs.
/// @return The module's bytes.
[[nodiscard]] inline std::vector<std::byte> wideBrTables(std::size_t arity,
                                                         uint32_t depth,
                                                         uint32_t labels,
                                                         uint32_t tables)
{
    auto body = std::vector<uint8_t> { 0x10, 0x00 }; // call 0
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, depth))
        body.insert(body.end(), { 0x02, 0x01 }); // block (type 1)
    for ([[maybe_unused]] auto const table: std::views::iota(uint32_t { 0 }, tables))
    {
        body.insert(body.end(), { 0x02, 0x01, 0x41, 0x00, 0x0E }); // block (type 1); i32.const 0; br_table
        auto const count = binary::testing::encodeU32(labels);
        body.insert(body.end(), count.begin(), count.end());
        for (auto const label: std::views::iota(uint32_t { 0 }, labels))
        {
            auto const depthOf = binary::testing::encodeU32(label % (depth + 2));
            body.insert(body.end(), depthOf.begin(), depthOf.end());
        }
        body.insert(body.end(), { 0x00, 0x0B }); // the default label, 0; end
    }
    body.resize(body.size() + depth, 0x0B); // end of every block
    auto const results = std::vector<binary::ValueType>(arity, binary::I32);
    auto builder = binary::testing::ModuleBuilder {};
    builder.type(binary::FuncType { .params = {}, .results = results })
        .type(binary::FuncType { .params = results, .results = results })
        .function(0, {}, std::move(body));
    return builder.build();
}

} // namespace isoline::validate::testing
