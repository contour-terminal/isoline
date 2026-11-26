// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <isoline/Features.hpp>
#include <isoline/binary/Types.hpp>

namespace isoline::binary
{

/// What follows an opcode in the binary format, and so how the decoder reads its immediates.
enum class ImmediateKind : uint8_t
{
    None,          ///< Nothing.
    BlockType,     ///< A block type: empty, a value type, or an s33 type index.
    LabelIndex,    ///< A u32 label index.
    LabelTable,    ///< A vector of u32 label indices and a default label (br_table).
    FunctionIndex, ///< A u32 function index.
    CallIndirect,  ///< A u32 type index and a u32 table index.
    LocalIndex,    ///< A u32 local index.
    GlobalIndex,   ///< A u32 global index.
    TableIndex,    ///< A u32 table index.
    TableCopy,     ///< A u32 destination and a u32 source table index.
    TableInit,     ///< A u32 element segment index and a u32 table index.
    ElemIndex,     ///< A u32 element segment index.
    MemArg,        ///< A memory argument: alignment (with memory index flag), optional memory, offset.
    MemoryIndex,   ///< A memory index (a zero byte before multi-memory).
    MemoryCopy,    ///< A destination and a source memory index.
    MemoryInit,    ///< A u32 data segment index and a memory index.
    DataIndex,     ///< A u32 data segment index.
    I32Const,      ///< An s32.
    I64Const,      ///< An s64.
    F32Const,      ///< A little-endian 32-bit float.
    F64Const,      ///< A little-endian 64-bit float.
    HeapType,      ///< A heap type (ref.null).
    SelectTypes,   ///< A vector of value types (typed select).
    V128Const,     ///< 16 raw bytes: the vector, lowest lane byte first (v128.const).
    Shuffle,       ///< 16 lane index bytes, each below 32 (i8x16.shuffle).
    LaneIndex,     ///< One lane index byte (extract_lane, replace_lane).
    MemArgLane,    ///< A memory argument as MemArg, then one lane index byte (load and store lane).
};

/// The operand and result types of an opcode whose typing is fixed, for the validator.
///
/// An enumerator reads `<params>To<results>`: I32I32ToI32 pops two i32 and pushes one, ToF32 pushes
/// an f32 and pops nothing, I64I64ToI64I64 pops two i64 and pushes two. Special opcodes take code to type:
/// control, parametric, variable, table, memory and reference instructions. Every enumerator is a row of
/// SignatureTable, in order.
enum class OpcodeSignature : uint8_t
{
    Special, ///< Typed by code, not by SignatureTable.
    None,    ///< [] -> [].
    ToI32,
    ToI64,
    ToF32,
    ToF64,
    I32ToI32,
    I32ToI64,
    I32ToF32,
    I32ToF64,
    I64ToI32,
    I64ToI64,
    I64ToF32,
    I64ToF64,
    F32ToI32,
    F32ToI64,
    F32ToF32,
    F32ToF64,
    F64ToI32,
    F64ToI64,
    F64ToF32,
    F64ToF64,
    I32I32ToI32,
    I64I64ToI32,
    I64I64ToI64,
    F32F32ToI32,
    F32F32ToF32,
    F64F64ToI32,
    F64F64ToF64,
    I64I64ToI64I64,       ///< i64.mul_wide_s and _u: two i64 to a 128-bit {low, high} pair.
    I64I64I64I64ToI64I64, ///< i64.add128 and sub128: two {low, high} pairs to one.
    ToV128,               ///< v128.const.
    V128ToV128,           ///< Lane-wise unary operators and conversions.
    V128V128ToV128,       ///< Lane-wise binary operators, comparisons, shuffle and swizzle.
    V128V128V128ToV128,   ///< v128.bitselect.
    V128ToI32,            ///< Tests (any_true, all_true, bitmask) and the i8, i16 and i32 extract_lane.
    V128ToI64,            ///< i64x2.extract_lane.
    V128ToF32,            ///< f32x4.extract_lane.
    V128ToF64,            ///< f64x2.extract_lane.
    I32ToV128,            ///< The i8, i16 and i32 splats.
    I64ToV128,            ///< i64x2.splat.
    F32ToV128,            ///< f32x4.splat.
    F64ToV128,            ///< f64x2.splat.
    V128I32ToV128,        ///< Shifts, and the i8, i16 and i32 replace_lane.
    V128I64ToV128,        ///< i64x2.replace_lane.
    V128F32ToV128,        ///< f32x4.replace_lane.
    V128F64ToV128,        ///< f64x2.replace_lane.
};

/// The last OpcodeSignature enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastOpcodeSignature = OpcodeSignature::V128F64ToV128;

/// One row of SignatureTable: the value kinds an OpcodeSignature pops and pushes.
///
/// Fixed-size storage with a count rather than a vector or optional keeps the table a constant;
/// paramKinds() and resultKinds() view the used part.
struct SignatureRow
{
    OpcodeSignature signature;           ///< The signature this row describes; equal to its index.
    std::array<ValueKind, 4> params {};  ///< The operand kinds, bottom of the stack first.
    uint8_t paramCount = 0;              ///< How many of params are used.
    std::array<ValueKind, 2> results {}; ///< The result kinds, in the order they are pushed.
    uint8_t resultCount = 0;             ///< How many of results are used.

    /// The operand kinds, bottom of the stack first.
    /// @return The first paramCount entries of params.
    [[nodiscard]] constexpr std::span<ValueKind const> paramKinds() const noexcept
    {
        return std::span { params }.first(paramCount);
    }

    /// The result kinds, in the order they are pushed.
    /// @return The first resultCount entries of results.
    [[nodiscard]] constexpr std::span<ValueKind const> resultKinds() const noexcept
    {
        return std::span { results }.first(resultCount);
    }
};

namespace detail
{
    /// The row of a signature popping one @p param and pushing one @p result.
    [[nodiscard]] consteval SignatureRow unaryRow(OpcodeSignature signature,
                                                  ValueKind param,
                                                  ValueKind result) noexcept
    {
        return SignatureRow { .signature = signature,
                              .params = { param },
                              .paramCount = 1,
                              .results = { result },
                              .resultCount = 1 };
    }

    /// The row of a signature popping two @p param and pushing one @p result.
    [[nodiscard]] consteval SignatureRow binaryRow(OpcodeSignature signature,
                                                   ValueKind param,
                                                   ValueKind result) noexcept
    {
        return SignatureRow { .signature = signature,
                              .params = { param, param },
                              .paramCount = 2,
                              .results = { result },
                              .resultCount = 1 };
    }

    /// The row of a signature popping @p first, then @p second, and pushing one @p result.
    [[nodiscard]] consteval SignatureRow mixedRow(OpcodeSignature signature,
                                                  ValueKind first,
                                                  ValueKind second,
                                                  ValueKind result) noexcept
    {
        return SignatureRow { .signature = signature,
                              .params = { first, second },
                              .paramCount = 2,
                              .results = { result },
                              .resultCount = 1 };
    }

    /// The row of a signature popping three @p param and pushing one @p result.
    [[nodiscard]] consteval SignatureRow ternaryRow(OpcodeSignature signature,
                                                    ValueKind param,
                                                    ValueKind result) noexcept
    {
        return SignatureRow { .signature = signature,
                              .params = { param, param, param },
                              .paramCount = 3,
                              .results = { result },
                              .resultCount = 1 };
    }

    /// The row of a signature popping nothing and pushing one @p result.
    [[nodiscard]] consteval SignatureRow nullaryRow(OpcodeSignature signature, ValueKind result) noexcept
    {
        return SignatureRow {
            .signature = signature, .params = {}, .paramCount = 0, .results = { result }, .resultCount = 1
        };
    }

    /// The row of a wide-arithmetic signature popping @p paramCount i64 and pushing two: a 128-bit
    /// value's low and high halves.
    [[nodiscard]] consteval SignatureRow wideRow(OpcodeSignature signature, uint8_t paramCount) noexcept
    {
        return SignatureRow { .signature = signature,
                              .params = { ValueKind::I64, ValueKind::I64, ValueKind::I64, ValueKind::I64 },
                              .paramCount = paramCount,
                              .results = { ValueKind::I64, ValueKind::I64 },
                              .resultCount = 2 };
    }
} // namespace detail

/// Every OpcodeSignature's operand and result kinds, one row per enumerator, in enumerator order.
inline constexpr auto SignatureTable = std::to_array<SignatureRow>({
    { .signature = OpcodeSignature::Special },
    { .signature = OpcodeSignature::None },
    detail::nullaryRow(OpcodeSignature::ToI32, ValueKind::I32),
    detail::nullaryRow(OpcodeSignature::ToI64, ValueKind::I64),
    detail::nullaryRow(OpcodeSignature::ToF32, ValueKind::F32),
    detail::nullaryRow(OpcodeSignature::ToF64, ValueKind::F64),
    detail::unaryRow(OpcodeSignature::I32ToI32, ValueKind::I32, ValueKind::I32),
    detail::unaryRow(OpcodeSignature::I32ToI64, ValueKind::I32, ValueKind::I64),
    detail::unaryRow(OpcodeSignature::I32ToF32, ValueKind::I32, ValueKind::F32),
    detail::unaryRow(OpcodeSignature::I32ToF64, ValueKind::I32, ValueKind::F64),
    detail::unaryRow(OpcodeSignature::I64ToI32, ValueKind::I64, ValueKind::I32),
    detail::unaryRow(OpcodeSignature::I64ToI64, ValueKind::I64, ValueKind::I64),
    detail::unaryRow(OpcodeSignature::I64ToF32, ValueKind::I64, ValueKind::F32),
    detail::unaryRow(OpcodeSignature::I64ToF64, ValueKind::I64, ValueKind::F64),
    detail::unaryRow(OpcodeSignature::F32ToI32, ValueKind::F32, ValueKind::I32),
    detail::unaryRow(OpcodeSignature::F32ToI64, ValueKind::F32, ValueKind::I64),
    detail::unaryRow(OpcodeSignature::F32ToF32, ValueKind::F32, ValueKind::F32),
    detail::unaryRow(OpcodeSignature::F32ToF64, ValueKind::F32, ValueKind::F64),
    detail::unaryRow(OpcodeSignature::F64ToI32, ValueKind::F64, ValueKind::I32),
    detail::unaryRow(OpcodeSignature::F64ToI64, ValueKind::F64, ValueKind::I64),
    detail::unaryRow(OpcodeSignature::F64ToF32, ValueKind::F64, ValueKind::F32),
    detail::unaryRow(OpcodeSignature::F64ToF64, ValueKind::F64, ValueKind::F64),
    detail::binaryRow(OpcodeSignature::I32I32ToI32, ValueKind::I32, ValueKind::I32),
    detail::binaryRow(OpcodeSignature::I64I64ToI32, ValueKind::I64, ValueKind::I32),
    detail::binaryRow(OpcodeSignature::I64I64ToI64, ValueKind::I64, ValueKind::I64),
    detail::binaryRow(OpcodeSignature::F32F32ToI32, ValueKind::F32, ValueKind::I32),
    detail::binaryRow(OpcodeSignature::F32F32ToF32, ValueKind::F32, ValueKind::F32),
    detail::binaryRow(OpcodeSignature::F64F64ToI32, ValueKind::F64, ValueKind::I32),
    detail::binaryRow(OpcodeSignature::F64F64ToF64, ValueKind::F64, ValueKind::F64),
    detail::wideRow(OpcodeSignature::I64I64ToI64I64, 2),
    detail::wideRow(OpcodeSignature::I64I64I64I64ToI64I64, 4),
    detail::nullaryRow(OpcodeSignature::ToV128, ValueKind::V128),
    detail::unaryRow(OpcodeSignature::V128ToV128, ValueKind::V128, ValueKind::V128),
    detail::binaryRow(OpcodeSignature::V128V128ToV128, ValueKind::V128, ValueKind::V128),
    detail::ternaryRow(OpcodeSignature::V128V128V128ToV128, ValueKind::V128, ValueKind::V128),
    detail::unaryRow(OpcodeSignature::V128ToI32, ValueKind::V128, ValueKind::I32),
    detail::unaryRow(OpcodeSignature::V128ToI64, ValueKind::V128, ValueKind::I64),
    detail::unaryRow(OpcodeSignature::V128ToF32, ValueKind::V128, ValueKind::F32),
    detail::unaryRow(OpcodeSignature::V128ToF64, ValueKind::V128, ValueKind::F64),
    detail::unaryRow(OpcodeSignature::I32ToV128, ValueKind::I32, ValueKind::V128),
    detail::unaryRow(OpcodeSignature::I64ToV128, ValueKind::I64, ValueKind::V128),
    detail::unaryRow(OpcodeSignature::F32ToV128, ValueKind::F32, ValueKind::V128),
    detail::unaryRow(OpcodeSignature::F64ToV128, ValueKind::F64, ValueKind::V128),
    detail::mixedRow(OpcodeSignature::V128I32ToV128, ValueKind::V128, ValueKind::I32, ValueKind::V128),
    detail::mixedRow(OpcodeSignature::V128I64ToV128, ValueKind::V128, ValueKind::I64, ValueKind::V128),
    detail::mixedRow(OpcodeSignature::V128F32ToV128, ValueKind::V128, ValueKind::F32, ValueKind::V128),
    detail::mixedRow(OpcodeSignature::V128F64ToV128, ValueKind::V128, ValueKind::F64, ValueKind::V128),
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: SignatureTable)
        {
            if (static_cast<std::size_t>(row.signature) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastOpcodeSignature) + 1;
    }(),
    "SignatureTable must hold one row per OpcodeSignature, at the index of its enumerator");

/// The operand and result kinds of @p signature.
/// @param signature The signature to describe.
/// @return Its row of SignatureTable; Special's row is empty.
[[nodiscard]] constexpr SignatureRow const& signatureInfo(OpcodeSignature signature) noexcept
{
    return SignatureTable[static_cast<std::size_t>(signature)];
}

/// Every instruction Isoline decodes, one enumerator per row of Opcodes.def, in its order.
///
/// The 461 rows do not fit a byte, so the base type is uint16_t.
enum class Opcode : uint16_t
{
#define ISOLINE_OPCODE(Name, Prefix, Code, Text, Immediate, Signature, Proposal) Name,
#include <isoline/binary/Opcodes.def>
#undef ISOLINE_OPCODE
};

/// The byte that precedes a single-byte opcode: none.
inline constexpr auto NoPrefix = uint8_t { 0x00 };

/// What the opcode table knows about one instruction.
struct OpcodeInfo
{
    Opcode opcode;             ///< The instruction; equal to the row's index in OpcodeTable.
    uint8_t prefix;            ///< NoPrefix, or the prefix byte (0xFC, 0xFD) it follows.
    uint32_t code;             ///< The opcode byte, or the u32 sub-opcode after the prefix.
    std::string_view text;     ///< Its name in the text format; typed select's is "select_t", an
                               ///< internal name, as the text format spells both selects "select".
    ImmediateKind immediate;   ///< What follows it in the binary format.
    OpcodeSignature signature; ///< Its fixed operand and result types, or Special.

    /// The proposal that introduced it, which must be enabled for it to decode; nullopt for the MVP.
    std::optional<Feature> proposal;
};

namespace detail::proposal
{
    /// What Opcodes.def's Proposal column names: Mvp, or a Feature enumerator, spelled bare.
    inline constexpr auto Mvp = std::optional<Feature> {};
    using enum Feature;
} // namespace detail::proposal

/// Every instruction's information, one row per Opcode enumerator, in enumerator order.
inline constexpr auto OpcodeTable = std::to_array<OpcodeInfo>({
#define ISOLINE_OPCODE(Name, Prefix, Code, Text, Immediate, Signature, Proposal) \
    { .opcode = Opcode::Name,                                                    \
      .prefix = (Prefix),                                                        \
      .code = (Code),                                                            \
      .text = (Text),                                                            \
      .immediate = ImmediateKind::Immediate,                                     \
      .signature = OpcodeSignature::Signature,                                   \
      .proposal = std::optional<Feature> { detail::proposal::Proposal } },
#include <isoline/binary/Opcodes.def>
#undef ISOLINE_OPCODE
});

/// The number of instructions in OpcodeTable.
inline constexpr auto OpcodeCount = OpcodeTable.size();

static_assert(OpcodeCount == 461,
              "every non-SIMD WebAssembly 2.0 instruction (201), SIMD's 236, relaxed SIMD's 20 and wide "
              "arithmetic's four");

/// What the table knows about @p opcode.
/// @param opcode The instruction to describe.
/// @return Its row of OpcodeTable.
[[nodiscard]] OpcodeInfo const& opcodeInfo(Opcode opcode) noexcept;

/// The instruction a binary encoding names, in constant time.
/// @param prefix NoPrefix for a single-byte opcode, or the prefix byte read before @p code.
/// @param code The opcode byte, or the sub-opcode read after the prefix.
/// @return The instruction, or nullopt when the encoding names none Isoline decodes.
[[nodiscard]] std::optional<Opcode> lookupOpcode(uint8_t prefix, uint32_t code) noexcept;

} // namespace isoline::binary
