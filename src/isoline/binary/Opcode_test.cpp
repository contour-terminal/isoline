// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>

#include <isoline/Features.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>

using isoline::binary::lookupOpcode;
using isoline::binary::Opcode;
using isoline::binary::opcodeInfo;
using isoline::binary::OpcodeSignature;
using isoline::binary::OpcodeTable;
using isoline::binary::signatureInfo;
using isoline::binary::ValueKind;

TEST_CASE("Opcode.lookupRoundTrips")
{
    for (auto const& row: OpcodeTable)
    {
        INFO(row.text);
        CHECK(lookupOpcode(row.prefix, row.code) == row.opcode);
        CHECK(opcodeInfo(row.opcode).text == row.text);
    }
}

TEST_CASE("Opcode.knownEncodings")
{
    CHECK(lookupOpcode(0x00, 0x6A) == Opcode::I32Add);
    CHECK(lookupOpcode(0xFC, 0) == Opcode::I32TruncSatF32S);
    CHECK(lookupOpcode(0xFC, 11) == Opcode::MemoryFill);
    CHECK(lookupOpcode(0x00, 0x1C) == Opcode::SelectTyped);
    CHECK(lookupOpcode(0x00, 0xD2) == Opcode::RefFunc);
    CHECK(opcodeInfo(Opcode::I32Add).text == "i32.add");
    CHECK(opcodeInfo(Opcode::Call).immediate == isoline::binary::ImmediateKind::FunctionIndex);
    CHECK(lookupOpcode(0xFD, 0x0C) == Opcode::V128Const);
    CHECK(lookupOpcode(0xFD, 0xFF) == Opcode::F64x2ConvertLowI32x4U);
    CHECK(opcodeInfo(Opcode::I8x16Shuffle).immediate == isoline::binary::ImmediateKind::Shuffle);
    CHECK(opcodeInfo(Opcode::V128Store64Lane).immediate == isoline::binary::ImmediateKind::MemArgLane);
    CHECK(opcodeInfo(Opcode::I16x8ExtractLaneU).immediate == isoline::binary::ImmediateKind::LaneIndex);
    // 465 opcodes do not fit a byte.
    STATIC_CHECK(std::is_same_v<std::underlying_type_t<Opcode>, uint16_t>);
    CHECK(isoline::binary::OpcodeCount == 465);
}

TEST_CASE("Opcode.typedFunctionReferenceEncodings")
{
    using isoline::binary::ImmediateKind;
    struct Row
    {
        uint8_t code;
        Opcode opcode;
        std::string_view text;
        ImmediateKind immediate;
    };
    auto const rows = std::to_array<Row>({
        { .code = 0x14,
          .opcode = Opcode::CallRef,
          .text = "call_ref",
          .immediate = ImmediateKind::TypeIndex },
        { .code = 0xD4,
          .opcode = Opcode::RefAsNonNull,
          .text = "ref.as_non_null",
          .immediate = ImmediateKind::None },
        { .code = 0xD5,
          .opcode = Opcode::BrOnNull,
          .text = "br_on_null",
          .immediate = ImmediateKind::LabelIndex },
        { .code = 0xD6,
          .opcode = Opcode::BrOnNonNull,
          .text = "br_on_non_null",
          .immediate = ImmediateKind::LabelIndex },
    });
    for (auto const& row: rows)
    {
        INFO(row.text);
        CHECK(lookupOpcode(0x00, row.code) == row.opcode);
        CHECK(opcodeInfo(row.opcode).text == row.text);
        CHECK(opcodeInfo(row.opcode).immediate == row.immediate);
        CHECK(opcodeInfo(row.opcode).signature == isoline::binary::OpcodeSignature::Special);
    }
    // 0xD3 is GC's ref.eq, and 0x15 tail calls' return_call_ref: neither is a row.
    CHECK(lookupOpcode(0x00, 0xD3) == std::nullopt);
    CHECK(lookupOpcode(0x00, 0x15) == std::nullopt);
}

TEST_CASE("Opcode.relaxedEncodings")
{
    CHECK(lookupOpcode(0xFD, 0x100) == Opcode::I8x16RelaxedSwizzle);
    CHECK(lookupOpcode(0xFD, 0x105) == Opcode::F32x4RelaxedMadd);
    CHECK(lookupOpcode(0xFD, 0x10C) == Opcode::I64x2RelaxedLaneselect);
    CHECK(lookupOpcode(0xFD, 0x111) == Opcode::I16x8RelaxedQ15mulrS);
    CHECK(lookupOpcode(0xFD, 0x113) == Opcode::I32x4RelaxedDotI8x16I7x16AddS);
    CHECK(opcodeInfo(Opcode::I32x4RelaxedTruncF64x2UZero).text == "i32x4.relaxed_trunc_f64x2_u_zero");

    // All 20 are relaxed SIMD's, of fixed type, with no immediate.
    auto const relaxed = std::ranges::count_if(
        OpcodeTable, [](auto const& row) { return row.proposal == isoline::Feature::RelaxedSimd; });
    CHECK(relaxed == 20);

    struct SignatureCase
    {
        Opcode opcode;
        OpcodeSignature signature;
    };
    auto const cases = std::to_array<SignatureCase>({
        { .opcode = Opcode::I8x16RelaxedSwizzle, .signature = OpcodeSignature::V128V128ToV128 },
        { .opcode = Opcode::I32x4RelaxedTruncF32x4S, .signature = OpcodeSignature::V128ToV128 },
        { .opcode = Opcode::F64x2RelaxedNmadd, .signature = OpcodeSignature::V128V128V128ToV128 },
        { .opcode = Opcode::I8x16RelaxedLaneselect, .signature = OpcodeSignature::V128V128V128ToV128 },
        { .opcode = Opcode::F64x2RelaxedMax, .signature = OpcodeSignature::V128V128ToV128 },
        { .opcode = Opcode::I16x8RelaxedDotI8x16I7x16S, .signature = OpcodeSignature::V128V128ToV128 },
        { .opcode = Opcode::I32x4RelaxedDotI8x16I7x16AddS, .signature = OpcodeSignature::V128V128V128ToV128 },
    });
    for (auto const& testCase: cases)
    {
        INFO(opcodeInfo(testCase.opcode).text);
        CHECK(opcodeInfo(testCase.opcode).signature == testCase.signature);
        CHECK(opcodeInfo(testCase.opcode).immediate == isoline::binary::ImmediateKind::None);
    }
}

TEST_CASE("Opcode.wideEncodings")
{
    CHECK(lookupOpcode(0xFC, 19) == Opcode::I64Add128);
    CHECK(lookupOpcode(0xFC, 20) == Opcode::I64Sub128);
    CHECK(lookupOpcode(0xFC, 21) == Opcode::I64MulWideS);
    CHECK(lookupOpcode(0xFC, 22) == Opcode::I64MulWideU);
    CHECK(opcodeInfo(Opcode::I64MulWideU).text == "i64.mul_wide_u");
    CHECK(opcodeInfo(Opcode::I64Add128).signature == OpcodeSignature::I64I64I64I64ToI64I64);
    CHECK(opcodeInfo(Opcode::I64MulWideS).signature == OpcodeSignature::I64I64ToI64I64);

    auto const& add128 = signatureInfo(OpcodeSignature::I64I64I64I64ToI64I64);
    CHECK(add128.paramKinds().size() == 4);
    CHECK(std::ranges::all_of(add128.paramKinds(), [](ValueKind kind) { return kind == ValueKind::I64; }));
    REQUIRE(add128.resultKinds().size() == 2);
    CHECK(add128.resultKinds()[0] == ValueKind::I64);
    CHECK(add128.resultKinds()[1] == ValueKind::I64);

    auto const& mulWide = signatureInfo(OpcodeSignature::I64I64ToI64I64);
    CHECK(mulWide.paramKinds().size() == 2);
    CHECK(mulWide.resultKinds().size() == 2);
}

TEST_CASE("Opcode.unknownEncodings")
{
    CHECK_FALSE(lookupOpcode(0x00, 0x06).has_value()); // try, exception handling, is not in P1
    CHECK_FALSE(lookupOpcode(0xFC, 18).has_value());
    CHECK_FALSE(lookupOpcode(0x00, 0xFC).has_value()); // a prefix is not an opcode of its own
    CHECK_FALSE(lookupOpcode(0x00, 0x100).has_value());
    CHECK_FALSE(lookupOpcode(0xFC, 0xFFFF'FFFF).has_value());
    CHECK_FALSE(lookupOpcode(0xFD, 0x9A).has_value());  // a gap in the SIMD encodings
    CHECK_FALSE(lookupOpcode(0xFD, 0x114).has_value()); // past the last relaxed SIMD encoding
    CHECK_FALSE(lookupOpcode(0xFB, 0).has_value());     // not a prefix
}

TEST_CASE("Opcode.tableHasNoDuplicates")
{
    auto encodings = std::set<std::pair<uint8_t, uint32_t>> {};
    auto texts = std::set<std::string_view> {};
    for (auto const& row: OpcodeTable)
    {
        INFO(row.text);
        CHECK(encodings.emplace(row.prefix, row.code).second);
        CHECK(texts.emplace(row.text).second);
    }
}

TEST_CASE("Opcode.signatureMatchesText")
{
    struct SignatureCase
    {
        Opcode opcode;
        OpcodeSignature signature;
    };
    auto const cases = std::to_array<SignatureCase>({
        { .opcode = Opcode::I32Add, .signature = OpcodeSignature::I32I32ToI32 },
        { .opcode = Opcode::I32Eqz, .signature = OpcodeSignature::I32ToI32 },
        { .opcode = Opcode::I64Eqz, .signature = OpcodeSignature::I64ToI32 },
        { .opcode = Opcode::I64LtU, .signature = OpcodeSignature::I64I64ToI32 },
        { .opcode = Opcode::I64Rotr, .signature = OpcodeSignature::I64I64ToI64 },
        { .opcode = Opcode::F32Neg, .signature = OpcodeSignature::F32ToF32 },
        { .opcode = Opcode::F64Ge, .signature = OpcodeSignature::F64F64ToI32 },
        { .opcode = Opcode::F64Copysign, .signature = OpcodeSignature::F64F64ToF64 },
        { .opcode = Opcode::I32WrapI64, .signature = OpcodeSignature::I64ToI32 },
        { .opcode = Opcode::I64ExtendI32U, .signature = OpcodeSignature::I32ToI64 },
        { .opcode = Opcode::F64ConvertI32S, .signature = OpcodeSignature::I32ToF64 },
        { .opcode = Opcode::F32DemoteF64, .signature = OpcodeSignature::F64ToF32 },
        { .opcode = Opcode::I64ReinterpretF64, .signature = OpcodeSignature::F64ToI64 },
        { .opcode = Opcode::I64Extend32S, .signature = OpcodeSignature::I64ToI64 },
        { .opcode = Opcode::I32TruncSatF64U, .signature = OpcodeSignature::F64ToI32 },
        { .opcode = Opcode::F32Const, .signature = OpcodeSignature::ToF32 },
        { .opcode = Opcode::Nop, .signature = OpcodeSignature::None },
        { .opcode = Opcode::Block, .signature = OpcodeSignature::Special },
        { .opcode = Opcode::LocalGet, .signature = OpcodeSignature::Special },
        { .opcode = Opcode::I32Load, .signature = OpcodeSignature::Special },
        { .opcode = Opcode::MemoryFill, .signature = OpcodeSignature::Special },
        { .opcode = Opcode::RefIsNull, .signature = OpcodeSignature::Special },
        { .opcode = Opcode::Select, .signature = OpcodeSignature::Special },
    });
    for (auto const& testCase: cases)
    {
        INFO(opcodeInfo(testCase.opcode).text);
        CHECK(opcodeInfo(testCase.opcode).signature == testCase.signature);
    }
}

TEST_CASE("Opcode.signatureRows")
{
    auto const& add = signatureInfo(OpcodeSignature::I32I32ToI32);
    CHECK(add.signature == OpcodeSignature::I32I32ToI32);
    REQUIRE(add.paramKinds().size() == 2);
    CHECK(add.paramKinds()[0] == ValueKind::I32);
    CHECK(add.paramKinds()[1] == ValueKind::I32);
    REQUIRE(add.resultKinds().size() == 1);
    CHECK(add.resultKinds()[0] == ValueKind::I32);

    auto const& convert = signatureInfo(OpcodeSignature::I32ToF64);
    REQUIRE(convert.paramKinds().size() == 1);
    CHECK(convert.paramKinds()[0] == ValueKind::I32);
    REQUIRE(convert.resultKinds().size() == 1);
    CHECK(convert.resultKinds()[0] == ValueKind::F64);

    auto const& constant = signatureInfo(OpcodeSignature::ToI64);
    CHECK(constant.paramKinds().empty());
    REQUIRE(constant.resultKinds().size() == 1);
    CHECK(constant.resultKinds()[0] == ValueKind::I64);

    CHECK(signatureInfo(OpcodeSignature::None).paramKinds().empty());
    CHECK(signatureInfo(OpcodeSignature::None).resultKinds().empty());
    CHECK(signatureInfo(OpcodeSignature::Special).paramKinds().empty());
    CHECK(signatureInfo(OpcodeSignature::Special).resultKinds().empty());
}

TEST_CASE("Opcode.numericSignaturesFollowTheTextPrefix")
{
    // Every fixed-typed opcode other than nop names its result's type first: "f64.convert_i32_s"
    // yields f64, except tests and comparisons, which yield i32 whatever their operands.
    struct PrefixRow
    {
        std::string_view prefix;
        ValueKind kind;
    };
    auto const prefixes = std::to_array<PrefixRow>({
        { .prefix = "i32.", .kind = ValueKind::I32 },
        { .prefix = "i64.", .kind = ValueKind::I64 },
        { .prefix = "f32.", .kind = ValueKind::F32 },
        { .prefix = "f64.", .kind = ValueKind::F64 },
    });
    for (auto const& row: OpcodeTable)
    {
        // SIMD's shape prefixes (i8x16. …) name lanes, not a value type; Opcode.simdSignatures checks those.
        if (row.signature == OpcodeSignature::Special || row.signature == OpcodeSignature::None
            || row.prefix == 0xFD)
            continue;
        INFO(row.text);
        auto const results = signatureInfo(row.signature).resultKinds();
        REQUIRE_FALSE(results.empty());
        // A wide instruction's results are all of its prefix's type.
        CHECK(std::ranges::all_of(results, [&](ValueKind kind) { return kind == results[0]; }));
        auto const prefixKind = std::ranges::find_if(
            prefixes, [&](PrefixRow const& prefix) { return row.text.starts_with(prefix.prefix); });
        REQUIRE(prefixKind != prefixes.end());
        CHECK((results[0] == prefixKind->kind || results[0] == ValueKind::I32));
        auto const params = signatureInfo(row.signature).paramKinds();
        if (results[0] != prefixKind->kind)
        {
            // A test or comparison: its operands are of the prefix's type.
            REQUIRE_FALSE(params.empty());
            CHECK(params[0] == prefixKind->kind);
        }
    }
}

TEST_CASE("Opcode.simdSignatures")
{
    // Every SIMD instruction of fixed type yields a v128, except tests and extract_lane, which yield
    // the i32 or lane scalar; every one but v128.const and the splats pops a v128 first. Relaxed
    // SIMD's instructions are SIMD instructions too.
    auto count = std::size_t { 0 };
    for (auto const& row: OpcodeTable)
    {
        if (row.proposal != isoline::Feature::Simd && row.proposal != isoline::Feature::RelaxedSimd)
            continue;
        INFO(row.text);
        ++count;
        CHECK(row.prefix == 0xFD);
        if (row.signature == OpcodeSignature::Special)
        {
            // Only the memory accesses are typed by code.
            CHECK((row.immediate == isoline::binary::ImmediateKind::MemArg
                   || row.immediate == isoline::binary::ImmediateKind::MemArgLane));
            continue;
        }
        auto const& signature = signatureInfo(row.signature);
        REQUIRE(signature.resultKinds().size() == 1);
        auto const yieldsScalar = row.text.contains("extract_lane") || row.text.ends_with("_true")
                                  || row.text.ends_with(".bitmask");
        CHECK((signature.resultKinds()[0] == ValueKind::V128) != yieldsScalar);
        auto const params = signature.paramKinds();
        auto const takesScalar = row.text.ends_with(".splat") || row.text == "v128.const";
        CHECK((params.empty() || params[0] != ValueKind::V128) == takesScalar);
    }
    CHECK(count == 236 + 20);

    struct SignatureCase
    {
        Opcode opcode;
        OpcodeSignature signature;
    };
    auto const cases = std::to_array<SignatureCase>({
        { .opcode = Opcode::V128Const, .signature = OpcodeSignature::ToV128 },
        { .opcode = Opcode::I8x16Shuffle, .signature = OpcodeSignature::V128V128ToV128 },
        { .opcode = Opcode::V128Bitselect, .signature = OpcodeSignature::V128V128V128ToV128 },
        { .opcode = Opcode::I64x2ExtractLane, .signature = OpcodeSignature::V128ToI64 },
        { .opcode = Opcode::F32x4ExtractLane, .signature = OpcodeSignature::V128ToF32 },
        { .opcode = Opcode::F64x2ReplaceLane, .signature = OpcodeSignature::V128F64ToV128 },
        { .opcode = Opcode::I64x2ReplaceLane, .signature = OpcodeSignature::V128I64ToV128 },
        { .opcode = Opcode::I64x2Shl, .signature = OpcodeSignature::V128I32ToV128 },
        { .opcode = Opcode::F64x2Splat, .signature = OpcodeSignature::F64ToV128 },
        { .opcode = Opcode::I64x2Splat, .signature = OpcodeSignature::I64ToV128 },
        { .opcode = Opcode::I8x16Bitmask, .signature = OpcodeSignature::V128ToI32 },
        { .opcode = Opcode::V128Not, .signature = OpcodeSignature::V128ToV128 },
        { .opcode = Opcode::V128Load8Lane, .signature = OpcodeSignature::Special },
    });
    for (auto const& testCase: cases)
    {
        INFO(opcodeInfo(testCase.opcode).text);
        CHECK(opcodeInfo(testCase.opcode).signature == testCase.signature);
    }
    auto const& replace = signatureInfo(OpcodeSignature::V128F32ToV128);
    REQUIRE(replace.paramKinds().size() == 2);
    CHECK(replace.paramKinds()[0] == ValueKind::V128);
    CHECK(replace.paramKinds()[1] == ValueKind::F32);
    CHECK(signatureInfo(OpcodeSignature::V128V128V128ToV128).paramKinds().size() == 3);
}

TEST_CASE("Opcode.encodingRanges")
{
    // The encodings, range by range: every code inside a range names an opcode, none outside does.
    struct EncodingRange
    {
        uint8_t prefix;
        uint32_t first;
        uint32_t last;
    };
    auto const ranges = std::to_array<EncodingRange>({
        { .prefix = 0x00, .first = 0x00, .last = 0x05 }, // unreachable .. else
        { .prefix = 0x00, .first = 0x0B, .last = 0x11 }, // end .. call_indirect
        { .prefix = 0x00, .first = 0x14, .last = 0x14 }, // call_ref
        { .prefix = 0x00, .first = 0x1A, .last = 0x1C }, // drop, select, typed select
        { .prefix = 0x00, .first = 0x20, .last = 0x26 }, // local.get .. table.set
        { .prefix = 0x00, .first = 0x28, .last = 0x40 }, // loads, stores, memory.size, memory.grow
        { .prefix = 0x00, .first = 0x41, .last = 0xC4 }, // constants and numeric instructions
        { .prefix = 0x00, .first = 0xD0, .last = 0xD2 }, // ref.null, ref.is_null, ref.func
        { .prefix = 0x00, .first = 0xD4, .last = 0xD6 }, // ref.as_non_null, br_on_null, br_on_non_null
        { .prefix = 0xFC, .first = 0, .last = 17 },      // saturating truncation, bulk memory, tables
        { .prefix = 0xFC, .first = 19, .last = 22 },     // wide arithmetic
        // SIMD: 0xFD 0x00 .. 0xFF, but for the gaps the proposal left
        { .prefix = 0xFD, .first = 0x00, .last = 0x99 },
        { .prefix = 0xFD, .first = 0x9B, .last = 0xA1 },
        { .prefix = 0xFD, .first = 0xA3, .last = 0xA4 },
        { .prefix = 0xFD, .first = 0xA7, .last = 0xAE },
        { .prefix = 0xFD, .first = 0xB1, .last = 0xB1 },
        { .prefix = 0xFD, .first = 0xB5, .last = 0xBA },
        { .prefix = 0xFD, .first = 0xBC, .last = 0xC1 },
        { .prefix = 0xFD, .first = 0xC3, .last = 0xC4 },
        { .prefix = 0xFD, .first = 0xC7, .last = 0xCE },
        { .prefix = 0xFD, .first = 0xD1, .last = 0xD1 },
        { .prefix = 0xFD, .first = 0xD5, .last = 0xE1 },
        { .prefix = 0xFD, .first = 0xE3, .last = 0xED },
        { .prefix = 0xFD, .first = 0xEF, .last = 0xFF },
        { .prefix = 0xFD, .first = 0x100, .last = 0x113 }, // relaxed SIMD
    });
    auto const inRange = [&](uint8_t prefix, uint32_t code) {
        return std::ranges::any_of(ranges, [&](EncodingRange const& range) {
            return range.prefix == prefix && range.first <= code && code <= range.last;
        });
    };

    auto expectedCount = std::size_t { 0 };
    for (auto const& range: ranges)
        expectedCount += range.last - range.first + 1;
    CHECK(expectedCount == isoline::binary::OpcodeCount);

    // Every code of each prefix up to one past the largest in use, plus the u32 extreme.
    for (auto const prefix: { uint8_t { 0x00 }, uint8_t { 0xFC }, uint8_t { 0xFD } })
    {
        for (auto const code: std::views::iota(uint32_t { 0 }, uint32_t { 0x115 }))
        {
            INFO("prefix " << int { prefix } << " code " << code);
            CHECK(lookupOpcode(prefix, code).has_value() == inRange(prefix, code));
        }
        CHECK_FALSE(lookupOpcode(prefix, 0xFFFF'FFFF).has_value());
    }
}

TEST_CASE("Opcode.proposals")
{
    using isoline::Feature;
    // A sample of each proposal's rows, and the MVP's.
    struct Row
    {
        Opcode opcode;
        std::optional<Feature> proposal;
    };
    auto const rows = std::to_array<Row>({
        { .opcode = Opcode::I32Add, .proposal = std::nullopt },
        { .opcode = Opcode::CallIndirect, .proposal = std::nullopt },
        { .opcode = Opcode::Select, .proposal = std::nullopt },
        { .opcode = Opcode::SelectTyped, .proposal = Feature::ReferenceTypes },
        { .opcode = Opcode::TableGet, .proposal = Feature::ReferenceTypes },
        { .opcode = Opcode::RefFunc, .proposal = Feature::ReferenceTypes },
        { .opcode = Opcode::TableFill, .proposal = Feature::ReferenceTypes },
        { .opcode = Opcode::I64Extend32S, .proposal = Feature::SignExtension },
        { .opcode = Opcode::I32TruncSatF32S, .proposal = Feature::NonTrappingFloatToInt },
        { .opcode = Opcode::MemoryInit, .proposal = Feature::BulkMemory },
        { .opcode = Opcode::TableCopy, .proposal = Feature::BulkMemory },
        { .opcode = Opcode::I64Add128, .proposal = Feature::WideArithmetic },
        { .opcode = Opcode::I64MulWideU, .proposal = Feature::WideArithmetic },
        { .opcode = Opcode::V128Load, .proposal = Feature::Simd },
        { .opcode = Opcode::I8x16Shuffle, .proposal = Feature::Simd },
        { .opcode = Opcode::F64x2ConvertLowI32x4U, .proposal = Feature::Simd },
        { .opcode = Opcode::I8x16RelaxedSwizzle, .proposal = Feature::RelaxedSimd },
        { .opcode = Opcode::I32x4RelaxedDotI8x16I7x16AddS, .proposal = Feature::RelaxedSimd },
        { .opcode = Opcode::CallRef, .proposal = Feature::TypedFunctionReferences },
        { .opcode = Opcode::RefAsNonNull, .proposal = Feature::TypedFunctionReferences },
        { .opcode = Opcode::BrOnNull, .proposal = Feature::TypedFunctionReferences },
        { .opcode = Opcode::BrOnNonNull, .proposal = Feature::TypedFunctionReferences },
    });
    for (auto const& row: rows)
    {
        INFO(opcodeInfo(row.opcode).text);
        CHECK(opcodeInfo(row.opcode).proposal == row.proposal);
    }
    // Every prefixed instruction of P1 came after the MVP, whose encodings are all single bytes.
    for (auto const& row: OpcodeTable)
    {
        INFO(row.text);
        if (row.prefix != isoline::binary::NoPrefix)
            CHECK(row.proposal.has_value());
    }
}
