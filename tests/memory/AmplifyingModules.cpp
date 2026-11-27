// SPDX-License-Identifier: Apache-2.0
#include "AmplifyingModules.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::Nullable;
using isoline::binary::ValueType;
using isoline::binary::testing::encodeName;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::ModuleBuilder;

namespace
{

/// Appends @p bytes to @p out.
void append(std::vector<uint8_t>& out, std::vector<uint8_t> const& bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// A module whose name section holds @p subsections, each an id and its contents, in order.
std::vector<std::byte> nameSectionModule(
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> const& subsections)
{
    auto payload = encodeName("name");
    for (auto const& [id, contents]: subsections)
    {
        payload.push_back(id);
        append(payload, encodeU32(static_cast<uint32_t>(contents.size())));
        append(payload, contents);
    }
    auto builder = ModuleBuilder {};
    builder.rawSection(0, std::move(payload));
    return builder.build();
}

} // namespace

namespace isoline::testing
{

/// A module whose import section is one compact group (0x7E) of @p items global imports, all from
/// one module named by @p nameBytes bytes, each item named by the empty name: one input byte each.
std::vector<std::byte> compactImportGroup(std::size_t nameBytes, uint32_t items)
{
    auto payload = encodeU32(1);
    append(payload, encodeName(std::string(nameBytes, 'm')));
    append(payload, encodeName(""));
    append(payload, { 0x7E, 0x03, 0x7F, 0x00 }); // shared type: global i32 (immutable)
    append(payload, encodeU32(items));
    payload.resize(payload.size() + items, 0x00); // every item's name: empty
    auto builder = ModuleBuilder {};
    builder.rawSectionBeforeCode(2, std::move(payload));
    return builder.build();
}

/// A module of @p tags tags, all of one type of @p params i32 params: two input bytes per tag.
std::vector<std::byte> wideTags(std::size_t params, uint32_t tags)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = std::vector<ValueType>(params, I32), .results = {} });
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, tags))
        builder.tag(0);
    return builder.build();
}

/// A module of @p count functions of type [] -> [] with empty bodies.
std::vector<std::byte> manyFunctions(uint32_t count)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {});
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        builder.function(0, {}, {});
    return builder.build();
}

/// A module of one function that pushes and drops @p count v128 constants.
std::vector<std::byte> manyV128Literals(uint32_t count)
{
    auto body = std::vector<uint8_t> {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
    {
        append(body, { 0xFD, 0x0C }); // v128.const
        body.resize(body.size() + 16, 0x00);
        body.push_back(0x1A); // drop
    }
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, std::move(body));
    return builder.build();
}

/// A module of one function whose try_table has @p count catch clauses, each of tag 0 to label 0.
std::vector<std::byte> manyCatchClauses(uint32_t count)
{
    auto body = std::vector<uint8_t> { 0x1F, 0x40 }; // try_table, empty block type
    append(body, encodeU32(count));
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        append(body, { 0x00, 0x00, 0x00 }); // catch tag 0, label 0
    body.push_back(0x0B);                   // end
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).tag(0).function(0, {}, std::move(body));
    return builder.build();
}

/// A module of @p depth types, each taking a nullable reference to the one before it.
std::vector<std::byte> deepTypeChain(uint32_t depth)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {});
    for (auto const index: std::views::iota(uint32_t { 1 }, depth))
        builder.type(
            FuncType { .params = { isoline::binary::concreteRef(index - 1, Nullable::Yes) }, .results = {} });
    return builder.build();
}

/// A module of @p count function imports, all of one type of @p params i32 params, each from the
/// module "" named "": four input bytes per import.
std::vector<std::byte> manyFunctionImports(std::size_t params, uint32_t count)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = std::vector<ValueType>(params, I32), .results = {} });
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        builder.importFunction("", "", 0);
    return builder.build();
}

/// A module of a function $f of type [] -> [i32 x 1000], and a function [] -> [] whose body is
/// @p calls calls of $f: two input bytes per call, each pushing a thousand operands.
std::vector<std::byte> multiValueCalls(uint32_t calls)
{
    auto body = std::vector<uint8_t> {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, calls))
        append(body, { 0x10, 0x00 }); // call $f
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = std::vector<ValueType>(1000, I32) })
        .type(FuncType {})
        .function(0, {}, { 0x00 })
        .function(1, {}, std::move(body));
    return builder.build();
}

/// A module whose name section names @p count functions and gives @p count functions a local map, every
/// name and map empty: an index and one byte per entry.
std::vector<std::byte> manyNames(uint32_t count)
{
    auto entries = encodeU32(count);
    for (auto const index: std::views::iota(uint32_t { 0 }, count))
    {
        append(entries, encodeU32(index));
        entries.push_back(0x00); // the empty name, or the empty local map
    }
    return nameSectionModule({ { 1, entries }, { 2, entries } }); // function names, local names
}

/// A module whose name section names @p count functions, and one local of each of @p count functions,
/// at indices spread up to 2^32 - 1, each by a name of @p nameBytes bytes.
std::vector<std::byte> sparseLongNames(uint32_t count, std::size_t nameBytes)
{
    auto const stride = std::numeric_limits<uint32_t>::max() / std::max(count - 1, uint32_t { 1 });
    auto const name = encodeName(std::string(nameBytes, 'n'));
    auto functions = encodeU32(count);
    auto locals = encodeU32(count);
    for (auto const ordinal: std::views::iota(uint32_t { 0 }, count))
    {
        auto const index = encodeU32(ordinal * stride);
        append(functions, index);
        append(functions, name);
        append(locals, index);
        append(locals, encodeU32(1));
        append(locals, index);
        append(locals, name);
    }
    return nameSectionModule({ { 1, functions }, { 2, locals } });
}

/// A module whose name section has one function name subsection of @p bytes bytes that claims as many
/// entries as it has bytes, every byte after the count 0xFF: the first index is a LEB128 too large for a u32.
std::vector<std::byte> lyingNameCount(uint32_t bytes)
{
    auto contents = encodeU32(bytes);
    contents.resize(bytes, 0xFF);
    return nameSectionModule({ { 1, contents } });
}

/// A module of @p functions functions, each of one run of @p locals i32 locals and an empty body.
std::vector<std::byte> wideLocalRuns(uint32_t functions, uint32_t locals)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {});
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, functions))
        builder.function(0, std::vector<ValueType>(locals, I32), {});
    return builder.build();
}

/// A module of @p functions functions of one type of @p params i32 params, whose name section names
/// the first parameter of each.
std::vector<std::byte> namedParameterFunctions(std::size_t params, uint32_t functions)
{
    auto locals = encodeU32(functions);
    for (auto const index: std::views::iota(uint32_t { 0 }, functions))
    {
        append(locals, encodeU32(index));
        append(locals, encodeU32(1));
        append(locals, encodeU32(0));
        append(locals, encodeName("p"));
    }
    auto payload = encodeName("name");
    payload.push_back(0x02); // local names
    append(payload, encodeU32(static_cast<uint32_t>(locals.size())));
    append(payload, locals);
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = std::vector<ValueType>(params, I32), .results = {} });
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, functions))
        builder.function(0, {}, {});
    builder.rawSection(0, std::move(payload));
    return builder.build();
}

/// A module of one function whose body nests @p depth empty blocks.
std::vector<std::byte> nestedBlocks(uint32_t depth)
{
    auto body = std::vector<uint8_t> {};
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, depth))
        append(body, { 0x02, 0x40 });     // block
    body.insert(body.end(), depth, 0x0B); // end
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, std::move(body));
    return builder.build();
}

/// compactImportGroup(@p nameBytes, @p items), and a function [i32] -> [] whose if a branch hint
/// section, before the code section, marks likely.
std::vector<std::byte> hintedImportGroup(std::size_t nameBytes, uint32_t items)
{
    auto const section = [](std::vector<uint8_t>& out, uint8_t id, std::vector<uint8_t> const& payload) {
        out.push_back(id);
        append(out, encodeU32(static_cast<uint32_t>(payload.size())));
        append(out, payload);
    };
    auto imports = encodeU32(1);
    append(imports, encodeName(std::string(nameBytes, 'm')));
    append(imports, encodeName(""));
    append(imports, { 0x7E, 0x03, 0x7F, 0x00 }); // shared type: global i32 (immutable)
    append(imports, encodeU32(items));
    imports.resize(imports.size() + items, 0x00); // every item's name: empty
    auto hints = encodeName("metadata.code.branch_hint");
    // Function 0, one hint at offset 3 (the locals declaration, then local.get 0), size 1, likely.
    append(hints, { 0x01, 0x00, 0x01, 0x03, 0x01, 0x01 });
    // One body: no locals, local.get 0, if (empty block type), end, end.
    auto const body = std::vector<uint8_t> { 0x00, 0x20, 0x00, 0x04, 0x40, 0x0B, 0x0B };
    auto code = encodeU32(1);
    append(code, encodeU32(static_cast<uint32_t>(body.size())));
    append(code, body);
    auto bytes = std::vector<uint8_t> { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };
    section(bytes, 1, { 0x01, 0x60, 0x01, 0x7F, 0x00 }); // type [i32] -> []
    section(bytes, 2, imports);
    section(bytes, 3, { 0x01, 0x00 }); // one function of type 0
    section(bytes, 0, hints);
    section(bytes, 10, code);
    auto result = std::vector<std::byte> {};
    result.reserve(bytes.size());
    std::ranges::transform(
        bytes, std::back_inserter(result), [](uint8_t byte) { return std::byte { byte }; });
    return result;
}

} // namespace isoline::testing
