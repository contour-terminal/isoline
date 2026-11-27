// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <string_view>

#include <isoline/binary/Types.hpp>
#include <isoline/meta/EnumRows.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>

namespace isoline::text
{

/// Every keyword of the text format that names no instruction, sorted: the reference lexer's keyword
/// cases (`interpreter/text/lexer.mll`) that are no opcode's text, without the GC proposal's (Isoline
/// implements no GC: neither its heap types, nofunc and noextern among them, nor their reference
/// types), with custom-page-sizes' `pagesize`. With opcodeNamed() it is the vocabulary
/// that tells a misplaced keyword from an unknown one. P11 adds the component model's keywords as rows.
inline constexpr auto Keywords = std::to_array<std::string_view>({
    "assert_exception",
    "assert_exhaustion",
    "assert_invalid",
    "assert_invalid_custom",
    "assert_malformed",
    "assert_malformed_custom",
    "assert_return",
    "assert_trap",
    "assert_unlinkable",
    "binary",
    "catch",
    "catch_all",
    "catch_all_ref",
    "catch_ref",
    "data",
    "declare",
    "definition",
    "either",
    "elem",
    "else",
    "end",
    "exn",
    "exnref",
    "export",
    "extern",
    "externref",
    "f32",
    "f32x4",
    "f64",
    "f64x2",
    "func",
    "funcref",
    "get",
    "global",
    "i16x8",
    "i32",
    "i32x4",
    "i64",
    "i64x2",
    "i8x16",
    "import",
    "input",
    "instance",
    "invoke",
    "item",
    "local",
    "memory",
    "module",
    "mut",
    "nan:arithmetic",
    "nan:canonical",
    "noexn",
    "null",
    "nullexnref",
    "offset",
    "output",
    "pagesize",
    "param",
    "quote",
    "ref",
    "register",
    "result",
    "script",
    "start",
    "table",
    "tag",
    "then",
    "type",
    "v128",
});

static_assert(std::ranges::is_sorted(Keywords) && std::ranges::adjacent_find(Keywords) == Keywords.end(),
              "Keywords must be sorted and hold each keyword once, as isKnownKeyword searches it");

/// One keyword that spells a value type by itself.
struct ValueTypeKeyword
{
    std::string_view keyword; ///< The keyword.
    binary::ValueType type;   ///< The type it spells.
};

/// Every value type a keyword spells: the number and vector types, and the abbreviated reference
/// types (spec §6.4.2, without the GC proposal's). The writer spells a type by its row, if it has one.
inline constexpr auto ValueTypeKeywords = std::to_array<ValueTypeKeyword>({
    { .keyword = "i32", .type = binary::I32 },
    { .keyword = "i64", .type = binary::I64 },
    { .keyword = "f32", .type = binary::F32 },
    { .keyword = "f64", .type = binary::F64 },
    { .keyword = "v128", .type = binary::V128 },
    { .keyword = "funcref", .type = binary::FuncRef },
    { .keyword = "externref", .type = binary::ExternRef },
    { .keyword = "exnref", .type = binary::ExnRef },
    { .keyword = "nullexnref", .type = binary::NullExnRef },
});

static_assert(meta::firstRepeatedKey(ValueTypeKeywords, &ValueTypeKeyword::keyword) == meta::NoRepeatedKey,
              "ValueTypeKeywords must hold each keyword once, as the reader takes the first row it names");
static_assert(
    meta::firstRepeatedKey(ValueTypeKeywords, &ValueTypeKeyword::type) == meta::NoRepeatedKey,
    "ValueTypeKeywords must hold each type once, as the writer spells a type by the first row that holds it");

/// One keyword that spells an abstract heap type.
struct HeapTypeKeyword
{
    std::string_view keyword;      ///< The keyword.
    binary::AbstractHeapType heap; ///< The heap type it spells.
};

/// Every abstract heap type a keyword spells, without the GC proposal's.
inline constexpr auto HeapTypeKeywords = std::to_array<HeapTypeKeyword>({
    { .keyword = "func", .heap = binary::AbstractHeapType::Func },
    { .keyword = "extern", .heap = binary::AbstractHeapType::Extern },
    { .keyword = "exn", .heap = binary::AbstractHeapType::Exn },
    { .keyword = "noexn", .heap = binary::AbstractHeapType::NoExn },
});

static_assert(meta::firstRepeatedKey(HeapTypeKeywords, &HeapTypeKeyword::keyword) == meta::NoRepeatedKey,
              "HeapTypeKeywords must hold each keyword once, as the reader takes the first row it names");
static_assert(meta::firstRepeatedKey(HeapTypeKeywords, &HeapTypeKeyword::heap) == meta::NoRepeatedKey,
              "HeapTypeKeywords must hold each heap type once, as the writer spells a heap type by the first "
              "row that holds it");

/// One keyword that spells an address type.
struct AddressTypeKeyword
{
    std::string_view keyword; ///< The keyword.
    binary::AddressType type; ///< The address type it spells.
};

/// Every address type a memory or table type may start with (memory64, table64).
inline constexpr auto AddressTypeKeywords = std::to_array<AddressTypeKeyword>({
    { .keyword = "i32", .type = binary::AddressType::I32 },
    { .keyword = "i64", .type = binary::AddressType::I64 },
});

static_assert(meta::firstRepeatedKey(AddressTypeKeywords, &AddressTypeKeyword::keyword)
                  == meta::NoRepeatedKey,
              "AddressTypeKeywords must hold each keyword once, as the reader takes the first row it names");

/// Whether a keyword is part of the text format's vocabulary.
/// @param text The keyword.
/// @return Whether it is a row of Keywords or an instruction's name (opcodeNamed()).
[[nodiscard]] bool isKnownKeyword(std::string_view text) noexcept;

/// The error a node is refused with where the grammar does not admit it, as the reference reports it.
///
/// The reference lexer refuses a keyword outside its vocabulary, and every reserved token, as an
/// unknown operator wherever it appears; its parser refuses every other token where its grammar does
/// not admit it as an unexpected token. Every place the reader refuses an atom reports through this.
/// @param tree The tree @p node belongs to.
/// @param node The refused node.
/// @return UnknownOperator with the token's text when @p node is a Reserved atom, or a Keyword atom
///         that is no number, no `offset=`/`align=` and no known keyword (isKnownKeyword()); else
///         UnexpectedToken. Either at the node's position.
[[nodiscard]] TextError unexpectedKeyword(SExpressionTree const& tree, NodeId node);

} // namespace isoline::text
