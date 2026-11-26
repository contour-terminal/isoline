// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

#include <isoline/Features.hpp>
#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/interp/Simd.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::binary::Opcode;
using isoline::binary::OpcodeSignature;
using isoline::binary::OpcodeTable;
using isoline::interp::applySimdBinary;
using isoline::interp::applySimdShift;
using isoline::interp::applySimdTernary;
using isoline::interp::applySimdTest;
using isoline::interp::applySimdUnary;
using isoline::interp::extractLane;
using isoline::interp::LaneCount;
using isoline::interp::laneOf;
using isoline::interp::replaceLane;
using isoline::interp::setLane;
using isoline::interp::SimdShape;
using isoline::interp::simdShape;
using isoline::interp::splat;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::V128;
using isoline::runtime::v128FromBytes;
using isoline::runtime::v128ToBytes;
using isoline::runtime::Value;

namespace
{

/// The 16 bytes first, first + 1, … (wrapping).
std::array<std::byte, 16> bytesFrom(uint8_t first)
{
    auto bytes = std::array<std::byte, 16> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, bytes.size()))
        bytes[index] = static_cast<std::byte>(first + index);
    return bytes;
}

/// The vector whose every byte is @p byte.
V128 filled(uint8_t byte)
{
    auto bytes = std::array<std::byte, 16> {};
    bytes.fill(std::byte { byte });
    return v128FromBytes(bytes);
}

/// The vector of i32x4 lanes @p lanes, lane 0 first.
V128 i32x4(std::array<uint32_t, 4> const& lanes)
{
    auto v = V128 {};
    for (auto const index: std::views::iota(0U, 4U))
        setLane<uint32_t>(v, index, lanes[index]);
    return v;
}

/// The vector of lanes @p lanes of type @p Lane, lane 0 first.
template <std::unsigned_integral Lane>
V128 vectorOf(std::array<Lane, LaneCount<Lane>> const& lanes)
{
    auto v = V128 {};
    for (auto const index: std::views::iota(0U, LaneCount<Lane>))
        setLane<Lane>(v, index, lanes[index]);
    return v;
}

/// The bits of the f32 @p value; for test data only, never a NaN.
uint32_t f32Bits(float value)
{
    return std::bit_cast<uint32_t>(value);
}

/// The bits of the f64 @p value; for test data only, never a NaN.
uint64_t f64Bits(double value)
{
    return std::bit_cast<uint64_t>(value);
}

/// The vector whose every lane of type @p Lane is @p lane.
template <std::unsigned_integral Lane>
V128 every(Lane lane)
{
    auto lanes = std::array<Lane, LaneCount<Lane>> {};
    lanes.fill(lane);
    return vectorOf<Lane>(lanes);
}

} // namespace

TEST_CASE("Simd.laneOrderIsLittleEndian")
{
    auto const bytes = bytesFrom(0x00);
    auto const v = v128FromBytes(bytes);
    CHECK(laneOf<uint32_t>(v, 0) == 0x0302'0100);
    CHECK(laneOf<uint64_t>(v, 1) == 0x0f0e'0d0c'0b0a'0908);
    CHECK(laneOf<uint8_t>(v, 15) == 0x0f);
    CHECK(laneOf<uint16_t>(v, 4) == 0x0908);
    CHECK(v.lanes[0] == 0x0706'0504'0302'0100);
    CHECK(v128ToBytes(v) == bytes);
}

TEST_CASE("Simd.setLaneChangesOneLane")
{
    auto v = filled(0xAA);
    setLane<uint16_t>(v, 5, 0x1234);
    CHECK(laneOf<uint16_t>(v, 5) == 0x1234);
    CHECK(laneOf<uint16_t>(v, 4) == 0xAAAA);
    CHECK(laneOf<uint16_t>(v, 6) == 0xAAAA);
    setLane<uint64_t>(v, 0, 0);
    CHECK(v.lanes[0] == 0);
    CHECK(v.lanes[1] == 0xAAAA'AAAA'1234'AAAA);
}

TEST_CASE("Simd.shuffleTakesSecondOperand")
{
    auto const a = v128FromBytes(bytesFrom(0x00));
    auto const b = v128FromBytes(bytesFrom(0x10));
    auto const second = bytesFrom(16);
    CHECK(isoline::interp::shuffle(a, b, second) == b);
    // Lanes {0, 16, 1, 17, …} interleave the two operands' bytes.
    auto interleave = std::array<std::byte, 16> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, interleave.size()))
        interleave[index] = static_cast<std::byte>((index / 2) + (index % 2 == 0 ? 0 : 16));
    auto const expected = std::to_array<uint8_t>(
        { 0x00, 0x10, 0x01, 0x11, 0x02, 0x12, 0x03, 0x13, 0x04, 0x14, 0x05, 0x15, 0x06, 0x16, 0x07, 0x17 });
    auto const result = v128ToBytes(isoline::interp::shuffle(a, b, interleave));
    for (auto const index: std::views::iota(std::size_t { 0 }, expected.size()))
        CHECK(std::to_integer<uint8_t>(result[index]) == expected[index]);
}

TEST_CASE("Simd.swizzleOutOfRangeIsZero")
{
    auto const bytes = v128FromBytes(bytesFrom(0x40));
    auto indices = V128 {};
    setLane<uint8_t>(indices, 0, 0x10);
    setLane<uint8_t>(indices, 1, 0x80);
    setLane<uint8_t>(indices, 2, 0x0F);
    setLane<uint8_t>(indices, 3, 0xFF);
    // Lanes 4 .. 15 stay index 0.
    auto const result = applySimdBinary(Opcode::I8x16Swizzle, bytes, indices);
    CHECK(laneOf<uint8_t>(result, 0) == 0);
    CHECK(laneOf<uint8_t>(result, 1) == 0);
    CHECK(laneOf<uint8_t>(result, 2) == 0x4F);
    CHECK(laneOf<uint8_t>(result, 3) == 0);
    CHECK(laneOf<uint8_t>(result, 4) == 0x40);
}

TEST_CASE("Simd.bitselect")
{
    CHECK(applySimdTernary(Opcode::V128Bitselect, filled(0xFF), filled(0x00), filled(0x0F)) == filled(0x0F));
    CHECK(applySimdTernary(Opcode::V128Bitselect, filled(0xAA), filled(0x55), filled(0xF0)) == filled(0xA5));
}

TEST_CASE("Simd.bitwise")
{
    auto const a = filled(0xCC);
    auto const b = filled(0xAA);
    CHECK(applySimdUnary(Opcode::V128Not, a) == filled(0x33));
    CHECK(applySimdBinary(Opcode::V128And, a, b) == filled(0x88));
    CHECK(applySimdBinary(Opcode::V128Andnot, a, b) == filled(0x44));
    CHECK(applySimdBinary(Opcode::V128Or, a, b) == filled(0xEE));
    CHECK(applySimdBinary(Opcode::V128Xor, a, b) == filled(0x66));
    CHECK(applySimdTest(Opcode::V128AnyTrue, V128 {}) == I32 { 0 });
    CHECK(applySimdTest(Opcode::V128AnyTrue, V128 { .lanes = { 0, 1ULL << 63U } }) == I32 { 1 });
    CHECK(applySimdTest(Opcode::V128AnyTrue, V128 { .lanes = { 1, 0 } }) == I32 { 1 });
}

TEST_CASE("Simd.shifts")
{
    auto const bytes = filled(0x81);
    CHECK(applySimdShift(Opcode::I8x16Shl, bytes, 1) == filled(0x02));
    CHECK(applySimdShift(Opcode::I8x16ShrS, bytes, 1) == filled(0xC0));
    CHECK(applySimdShift(Opcode::I8x16ShrU, bytes, 1) == filled(0x40));
    // The count is taken modulo the lane width: 9 shifts an i8 lane by 1.
    CHECK(applySimdShift(Opcode::I8x16Shl, bytes, 9) == filled(0x02));

    auto const halves = filled(0x80);
    CHECK(applySimdShift(Opcode::I16x8Shl, halves, 4)
          == V128 { .lanes = { 0x0800'0800'0800'0800, 0x0800'0800'0800'0800 } });
    CHECK(applySimdShift(Opcode::I16x8ShrS, halves, 17)
          == V128 { .lanes = { 0xC040'C040'C040'C040, 0xC040'C040'C040'C040 } });
    CHECK(applySimdShift(Opcode::I16x8ShrU, halves, 8)
          == V128 { .lanes = { 0x0080'0080'0080'0080, 0x0080'0080'0080'0080 } });

    auto const words = i32x4({ 0x8000'0001, 1, 0xFFFF'FFFF, 0x10 });
    CHECK(applySimdShift(Opcode::I32x4Shl, words, 33) == i32x4({ 2, 2, 0xFFFF'FFFE, 0x20 }));
    CHECK(applySimdShift(Opcode::I32x4ShrS, words, 4) == i32x4({ 0xF800'0000, 0, 0xFFFF'FFFF, 1 }));
    CHECK(applySimdShift(Opcode::I32x4ShrU, words, 4) == i32x4({ 0x0800'0000, 0, 0x0FFF'FFFF, 1 }));

    auto const longs = V128 { .lanes = { 0x8000'0000'0000'0000, 3 } };
    CHECK(applySimdShift(Opcode::I64x2Shl, longs, 65) == V128 { .lanes = { 0, 6 } });
    CHECK(applySimdShift(Opcode::I64x2ShrS, longs, 63) == V128 { .lanes = { ~0ULL, 0 } });
    CHECK(applySimdShift(Opcode::I64x2ShrU, longs, 63) == V128 { .lanes = { 1, 0 } });
}

TEST_CASE("Simd.splat")
{
    CHECK(splat(Opcode::I8x16Splat, I32 { 0x1234'56AB }) == filled(0xAB));
    CHECK(splat(Opcode::I16x8Splat, I32 { 0x1234'56AB })
          == V128 { .lanes = { 0x56AB'56AB'56AB'56AB, 0x56AB'56AB'56AB'56AB } });
    CHECK(splat(Opcode::I32x4Splat, I32 { 0x1234'56AB })
          == i32x4({ 0x1234'56AB, 0x1234'56AB, 0x1234'56AB, 0x1234'56AB }));
    CHECK(splat(Opcode::I64x2Splat, I64 { 0x0102'0304'0506'0708 })
          == V128 { .lanes = { 0x0102'0304'0506'0708, 0x0102'0304'0506'0708 } });
    // A NaN's payload survives: floats are carried as bits.
    CHECK(splat(Opcode::F32x4Splat, F32 { 0x7FA0'0001 })
          == i32x4({ 0x7FA0'0001, 0x7FA0'0001, 0x7FA0'0001, 0x7FA0'0001 }));
    CHECK(splat(Opcode::F64x2Splat, F64 { 0xFFF0'0000'0000'0001 })
          == V128 { .lanes = { 0xFFF0'0000'0000'0001, 0xFFF0'0000'0000'0001 } });
}

TEST_CASE("Simd.extractLaneSigned")
{
    auto v = V128 {};
    setLane<uint8_t>(v, 0, 0x80);
    CHECK(extractLane(Opcode::I8x16ExtractLaneS, v, 0) == Value { I32 { 0xFFFF'FF80 } });
    CHECK(extractLane(Opcode::I8x16ExtractLaneU, v, 0) == Value { I32 { 0x80 } });
    setLane<uint16_t>(v, 7, 0x8001);
    CHECK(extractLane(Opcode::I16x8ExtractLaneS, v, 7) == Value { I32 { 0xFFFF'8001 } });
    CHECK(extractLane(Opcode::I16x8ExtractLaneU, v, 7) == Value { I32 { 0x8001 } });
    // A positive lane is the same either way.
    CHECK(extractLane(Opcode::I8x16ExtractLaneS, v, 1) == Value { I32 { 0 } });
}

TEST_CASE("Simd.extractLane")
{
    auto const v = v128FromBytes(bytesFrom(0x00));
    CHECK(extractLane(Opcode::I32x4ExtractLane, v, 3) == Value { I32 { 0x0F0E'0D0C } });
    CHECK(extractLane(Opcode::I64x2ExtractLane, v, 1) == Value { I64 { 0x0F0E'0D0C'0B0A'0908 } });
    CHECK(extractLane(Opcode::F32x4ExtractLane, v, 1) == Value { F32 { 0x0706'0504 } });
    CHECK(extractLane(Opcode::F64x2ExtractLane, v, 0) == Value { F64 { 0x0706'0504'0302'0100 } });
}

TEST_CASE("Simd.replaceLane")
{
    auto const v = filled(0xEE);
    auto const byte = replaceLane(Opcode::I8x16ReplaceLane, v, 15, I32 { 0x1234 });
    CHECK(laneOf<uint8_t>(byte, 15) == 0x34);
    CHECK(laneOf<uint8_t>(byte, 14) == 0xEE);
    auto const half = replaceLane(Opcode::I16x8ReplaceLane, v, 2, I32 { 0x0001'2345 });
    CHECK(laneOf<uint16_t>(half, 2) == 0x2345);
    CHECK(laneOf<uint16_t>(half, 3) == 0xEEEE);
    CHECK(laneOf<uint32_t>(replaceLane(Opcode::I32x4ReplaceLane, v, 1, I32 { 7 }), 1) == 7);
    CHECK(replaceLane(Opcode::I64x2ReplaceLane, v, 1, I64 { 9 })
          == V128 { .lanes = { 0xEEEE'EEEE'EEEE'EEEE, 9 } });
    CHECK(laneOf<uint32_t>(replaceLane(Opcode::F32x4ReplaceLane, v, 3, F32 { 0x7FC0'0001 }), 3)
          == 0x7FC0'0001);
    CHECK(replaceLane(Opcode::F64x2ReplaceLane, v, 0, F64 { 1 })
          == V128 { .lanes = { 1, 0xEEEE'EEEE'EEEE'EEEE } });
}

TEST_CASE("Simd.shapesFollowSignatures")
{
    // A row's shape and its opcode's signature are two statements of one fact; they must agree.
    struct ShapeRow
    {
        SimdShape shape;
        std::array<OpcodeSignature, 4> signatures;
    };
    auto const shapes = std::to_array<ShapeRow>({
        { .shape = SimdShape::Unary, .signatures = { OpcodeSignature::V128ToV128 } },
        { .shape = SimdShape::Binary, .signatures = { OpcodeSignature::V128V128ToV128 } },
        { .shape = SimdShape::Ternary, .signatures = { OpcodeSignature::V128V128V128ToV128 } },
        { .shape = SimdShape::Test, .signatures = { OpcodeSignature::V128ToI32 } },
        { .shape = SimdShape::Shift, .signatures = { OpcodeSignature::V128I32ToV128 } },
        { .shape = SimdShape::Splat,
          .signatures = { OpcodeSignature::I32ToV128,
                          OpcodeSignature::I64ToV128,
                          OpcodeSignature::F32ToV128,
                          OpcodeSignature::F64ToV128 } },
        { .shape = SimdShape::ExtractLane,
          .signatures = { OpcodeSignature::V128ToI32,
                          OpcodeSignature::V128ToI64,
                          OpcodeSignature::V128ToF32,
                          OpcodeSignature::V128ToF64 } },
        { .shape = SimdShape::ReplaceLane,
          .signatures = { OpcodeSignature::V128I32ToV128,
                          OpcodeSignature::V128I64ToV128,
                          OpcodeSignature::V128F32ToV128,
                          OpcodeSignature::V128F64ToV128 } },
    });
    for (auto const& row: OpcodeTable)
    {
        INFO(row.text);
        auto const shape = simdShape(row.opcode);
        if (shape == SimdShape::None)
            continue;
        CHECK(row.proposal == isoline::Feature::Simd);
        auto const entry = std::ranges::find(shapes, shape, &ShapeRow::shape);
        REQUIRE(entry != shapes.end());
        CHECK(std::ranges::find(entry->signatures, row.signature) != entry->signatures.end());
        // Lane access is what a lane index immediate is for.
        auto const takesLane = shape == SimdShape::ExtractLane || shape == SimdShape::ReplaceLane;
        CHECK((row.immediate == isoline::binary::ImmediateKind::LaneIndex) == takesLane);
    }
    CHECK(simdShape(Opcode::I32Add) == SimdShape::None);
    CHECK(simdShape(Opcode::V128Load) == SimdShape::None);
    CHECK(simdShape(Opcode::V128Const) == SimdShape::None);
    CHECK(simdShape(Opcode::I8x16Shuffle) == SimdShape::None);
    CHECK(simdShape(Opcode::V128Xor) == SimdShape::Binary);
    CHECK(simdShape(Opcode::I64x2ShrU) == SimdShape::Shift);
}

TEST_CASE("Simd.i8x16AddWraps")
{
    CHECK(applySimdBinary(Opcode::I8x16Add, filled(0xFF), filled(0x01)) == filled(0x00));
}

TEST_CASE("Simd.shiftCountIsModular")
{
    auto const words = i32x4({ 0x8000'0001, 1, 0x4000'0000, 0x10 });
    CHECK(applySimdShift(Opcode::I32x4Shl, words, 33) == applySimdShift(Opcode::I32x4Shl, words, 1));
}

TEST_CASE("Simd.q15mulrSatSaturates")
{
    // -1.0 * -1.0 is 1.0, which Q15 cannot hold: it saturates to 0x7FFF.
    CHECK(applySimdBinary(Opcode::I16x8Q15mulrSatS, every<uint16_t>(0x8000), every<uint16_t>(0x8000))
          == every<uint16_t>(0x7FFF));
    // 0.5 * 0.5 is 0.25; -0.5 * 0.5 is -0.25; the product rounds half up.
    CHECK(applySimdBinary(Opcode::I16x8Q15mulrSatS, every<uint16_t>(0x4000), every<uint16_t>(0x4000))
          == every<uint16_t>(0x2000));
    CHECK(applySimdBinary(Opcode::I16x8Q15mulrSatS, every<uint16_t>(0xC000), every<uint16_t>(0x4000))
          == every<uint16_t>(0xE000));
    CHECK(applySimdBinary(Opcode::I16x8Q15mulrSatS, every<uint16_t>(1), every<uint16_t>(0x4000))
          == every<uint16_t>(1));
    // -2^-15 * 0.5 is -2^-16, half a unit below 0: it rounds up to 0, not away from zero to -1.
    CHECK(applySimdBinary(Opcode::I16x8Q15mulrSatS, every<uint16_t>(0xFFFF), every<uint16_t>(0x4000))
          == every<uint16_t>(0));
}

TEST_CASE("Simd.narrowSaturates")
{
    auto const wide = vectorOf<uint16_t>({ 300, static_cast<uint16_t>(-300), 5, 0xFFFF, 127, 128, 255, 256 });
    auto const other = every<uint16_t>(0x8000);
    auto const narrowS = applySimdBinary(Opcode::I8x16NarrowI16x8S, wide, other);
    CHECK(narrowS
          == vectorOf<uint8_t>(
              { 127, 0x80, 5, 0xFF, 127, 127, 127, 127, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 }));
    // Unsigned narrowing reads the lanes as signed too: -1 is 0, not 255.
    auto const narrowU = applySimdBinary(Opcode::I8x16NarrowI16x8U, wide, other);
    CHECK(narrowU == vectorOf<uint8_t>({ 255, 0, 5, 0, 127, 128, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0 }));

    auto const words = i32x4({ 70000, static_cast<uint32_t>(-70000), 0xFFFF'FFFF, 0x8000 });
    auto const otherWords = i32x4({ 1, 2, 3, 0x7FFF });
    CHECK(applySimdBinary(Opcode::I16x8NarrowI32x4S, words, otherWords)
          == vectorOf<uint16_t>({ 0x7FFF, 0x8000, 0xFFFF, 0x7FFF, 1, 2, 3, 0x7FFF }));
    CHECK(applySimdBinary(Opcode::I16x8NarrowI32x4U, words, otherWords)
          == vectorOf<uint16_t>({ 0xFFFF, 0, 0, 0x8000, 1, 2, 3, 0x7FFF }));
}

TEST_CASE("Simd.bitmask")
{
    auto bytes = V128 {};
    setLane<uint8_t>(bytes, 0, 0x80);
    setLane<uint8_t>(bytes, 15, 0xFF);
    setLane<uint8_t>(bytes, 7, 0x7F);
    CHECK(applySimdTest(Opcode::I8x16Bitmask, bytes) == I32 { 0x8001 });
    CHECK(applySimdTest(Opcode::I16x8Bitmask,
                        vectorOf<uint16_t>({ 0x8000, 0x7FFF, 0, 0xFFFF, 0, 0, 0, 0x8001 }))
          == I32 { 0x89 });
    CHECK(applySimdTest(Opcode::I32x4Bitmask, i32x4({ 0, 0x8000'0000, 0x7FFF'FFFF, 0xFFFF'FFFF }))
          == I32 { 0xA });
    CHECK(applySimdTest(Opcode::I64x2Bitmask, V128 { .lanes = { 1ULL << 63U, 1 } }) == I32 { 1 });
}

TEST_CASE("Simd.i64x2Abs")
{
    auto const minimum = uint64_t { 1 } << 63U;
    CHECK(applySimdUnary(Opcode::I64x2Abs, V128 { .lanes = { minimum, ~0ULL } })
          == V128 { .lanes = { minimum, 1 } });
    CHECK(applySimdUnary(Opcode::I32x4Abs, i32x4({ 0x8000'0000, 0xFFFF'FFFB, 5, 0 }))
          == i32x4({ 0x8000'0000, 5, 5, 0 }));
    CHECK(applySimdUnary(Opcode::I16x8Abs, every<uint16_t>(0xFFFF)) == every<uint16_t>(1));
    CHECK(applySimdUnary(Opcode::I8x16Abs, filled(0x80)) == filled(0x80));
}

TEST_CASE("Simd.integerArithmetic")
{
    // Every lane wraps: the products are taken modulo the lane width (0xFFFF * 0xFFFF is 1, which
    // a 16-bit product computed in int would overflow).
    CHECK(applySimdBinary(Opcode::I16x8Add, every<uint16_t>(0xFFFF), every<uint16_t>(2))
          == every<uint16_t>(1));
    CHECK(applySimdBinary(Opcode::I32x4Add, i32x4({ 0xFFFF'FFFF, 1, 2, 3 }), i32x4({ 1, 1, 1, 1 }))
          == i32x4({ 0, 2, 3, 4 }));
    CHECK(applySimdBinary(Opcode::I64x2Add, V128 { .lanes = { ~0ULL, 1 } }, V128 { .lanes = { 1, 1 } })
          == V128 { .lanes = { 0, 2 } });
    CHECK(applySimdBinary(Opcode::I8x16Sub, filled(0x00), filled(0x01)) == filled(0xFF));
    CHECK(applySimdBinary(Opcode::I16x8Sub, every<uint16_t>(0), every<uint16_t>(1))
          == every<uint16_t>(0xFFFF));
    CHECK(applySimdBinary(Opcode::I32x4Sub, i32x4({ 0, 5, 0, 0 }), i32x4({ 1, 2, 0, 0 }))
          == i32x4({ 0xFFFF'FFFF, 3, 0, 0 }));
    CHECK(applySimdBinary(Opcode::I64x2Sub, V128 { .lanes = { 0, 5 } }, V128 { .lanes = { 1, 2 } })
          == V128 { .lanes = { ~0ULL, 3 } });
    CHECK(applySimdBinary(Opcode::I16x8Mul, every<uint16_t>(0xFFFF), every<uint16_t>(0xFFFF))
          == every<uint16_t>(1));
    CHECK(applySimdBinary(Opcode::I32x4Mul,
                          i32x4({ 0xFFFF'FFFF, 0x1'0000, 3, 0 }),
                          i32x4({ 0xFFFF'FFFF, 0x1'0000, 4, 9 }))
          == i32x4({ 1, 0, 12, 0 }));
    CHECK(applySimdBinary(Opcode::I64x2Mul,
                          V128 { .lanes = { ~0ULL, 1ULL << 32U } },
                          V128 { .lanes = { ~0ULL, 1ULL << 32U } })
          == V128 { .lanes = { 1, 0 } });

    CHECK(applySimdUnary(Opcode::I8x16Neg, filled(0x01)) == filled(0xFF));
    CHECK(applySimdUnary(Opcode::I8x16Neg, filled(0x80)) == filled(0x80));
    CHECK(applySimdUnary(Opcode::I16x8Neg, every<uint16_t>(2)) == every<uint16_t>(0xFFFE));
    CHECK(applySimdUnary(Opcode::I32x4Neg, i32x4({ 1, 0, 0x8000'0000, 0xFFFF'FFFF }))
          == i32x4({ 0xFFFF'FFFF, 0, 0x8000'0000, 1 }));
    CHECK(applySimdUnary(Opcode::I64x2Neg, V128 { .lanes = { 1, 0 } }) == V128 { .lanes = { ~0ULL, 0 } });

    CHECK(applySimdUnary(Opcode::I8x16Popcnt,
                         vectorOf<uint8_t>({ 0, 1, 3, 0xFF, 0x80, 0x55, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7 }))
          == vectorOf<uint8_t>({ 0, 1, 2, 8, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3 }));
}

TEST_CASE("Simd.minMaxAverage")
{
    auto const bytesA = filled(0x80);
    auto const bytesB = filled(0x01);
    CHECK(applySimdBinary(Opcode::I8x16MinS, bytesA, bytesB) == bytesA);
    CHECK(applySimdBinary(Opcode::I8x16MinU, bytesA, bytesB) == bytesB);
    CHECK(applySimdBinary(Opcode::I8x16MaxS, bytesA, bytesB) == bytesB);
    CHECK(applySimdBinary(Opcode::I8x16MaxU, bytesA, bytesB) == bytesA);
    auto const halvesA = every<uint16_t>(0xFFFF);
    auto const halvesB = every<uint16_t>(0x0002);
    CHECK(applySimdBinary(Opcode::I16x8MinS, halvesA, halvesB) == halvesA);
    CHECK(applySimdBinary(Opcode::I16x8MinU, halvesA, halvesB) == halvesB);
    CHECK(applySimdBinary(Opcode::I16x8MaxS, halvesA, halvesB) == halvesB);
    CHECK(applySimdBinary(Opcode::I16x8MaxU, halvesA, halvesB) == halvesA);
    auto const wordsA = i32x4({ 0x8000'0000, 7, 0, 0xFFFF'FFFF });
    auto const wordsB = i32x4({ 1, 3, 0, 0 });
    CHECK(applySimdBinary(Opcode::I32x4MinS, wordsA, wordsB) == i32x4({ 0x8000'0000, 3, 0, 0xFFFF'FFFF }));
    CHECK(applySimdBinary(Opcode::I32x4MinU, wordsA, wordsB) == i32x4({ 1, 3, 0, 0 }));
    CHECK(applySimdBinary(Opcode::I32x4MaxS, wordsA, wordsB) == i32x4({ 1, 7, 0, 0 }));
    CHECK(applySimdBinary(Opcode::I32x4MaxU, wordsA, wordsB) == i32x4({ 0x8000'0000, 7, 0, 0xFFFF'FFFF }));

    // avgr_u rounds up, and its sum does not overflow the lane.
    CHECK(applySimdBinary(Opcode::I8x16AvgrU, filled(0xFF), filled(0xFF)) == filled(0xFF));
    CHECK(applySimdBinary(Opcode::I8x16AvgrU, filled(0x00), filled(0x01)) == filled(0x01));
    CHECK(applySimdBinary(Opcode::I16x8AvgrU, every<uint16_t>(0xFFFF), every<uint16_t>(0xFFFE))
          == every<uint16_t>(0xFFFF));
    CHECK(applySimdBinary(Opcode::I16x8AvgrU, every<uint16_t>(2), every<uint16_t>(4)) == every<uint16_t>(3));
}

TEST_CASE("Simd.saturatingArithmetic")
{
    CHECK(applySimdBinary(Opcode::I8x16AddSatS, filled(0x7F), filled(0x01)) == filled(0x7F));
    CHECK(applySimdBinary(Opcode::I8x16AddSatS, filled(0x80), filled(0xFF)) == filled(0x80));
    CHECK(applySimdBinary(Opcode::I8x16AddSatS, filled(0x05), filled(0xFE)) == filled(0x03));
    CHECK(applySimdBinary(Opcode::I8x16AddSatU, filled(0xFF), filled(0x01)) == filled(0xFF));
    CHECK(applySimdBinary(Opcode::I8x16AddSatU, filled(0x10), filled(0x01)) == filled(0x11));
    CHECK(applySimdBinary(Opcode::I8x16SubSatS, filled(0x80), filled(0x01)) == filled(0x80));
    CHECK(applySimdBinary(Opcode::I8x16SubSatS, filled(0x7F), filled(0xFF)) == filled(0x7F));
    CHECK(applySimdBinary(Opcode::I8x16SubSatU, filled(0x00), filled(0x01)) == filled(0x00));
    CHECK(applySimdBinary(Opcode::I8x16SubSatU, filled(0x10), filled(0x01)) == filled(0x0F));

    CHECK(applySimdBinary(Opcode::I16x8AddSatS, every<uint16_t>(0x7FFF), every<uint16_t>(1))
          == every<uint16_t>(0x7FFF));
    CHECK(applySimdBinary(Opcode::I16x8AddSatU, every<uint16_t>(0xFFFF), every<uint16_t>(1))
          == every<uint16_t>(0xFFFF));
    CHECK(applySimdBinary(Opcode::I16x8SubSatS, every<uint16_t>(0x8000), every<uint16_t>(1))
          == every<uint16_t>(0x8000));
    CHECK(applySimdBinary(Opcode::I16x8SubSatU, every<uint16_t>(1), every<uint16_t>(2))
          == every<uint16_t>(0));
}

TEST_CASE("Simd.comparisons")
{
    // A true comparison sets every bit of its lane; 0x80 is -128 signed and 128 unsigned.
    auto const bytesA = vectorOf<uint8_t>({ 0x80, 1, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
    auto const bytesB = vectorOf<uint8_t>({ 0x01, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
    auto const bytesOf = [](uint8_t first, uint8_t second, uint8_t third, uint8_t rest) {
        return vectorOf<uint8_t>({ first,
                                   second,
                                   third,
                                   rest,
                                   rest,
                                   rest,
                                   rest,
                                   rest,
                                   rest,
                                   rest,
                                   rest,
                                   rest,
                                   rest,
                                   rest,
                                   rest,
                                   rest });
    };
    CHECK(applySimdBinary(Opcode::I8x16Eq, bytesA, bytesB) == bytesOf(0, 0xFF, 0, 0xFF));
    CHECK(applySimdBinary(Opcode::I8x16Ne, bytesA, bytesB) == bytesOf(0xFF, 0, 0xFF, 0));
    CHECK(applySimdBinary(Opcode::I8x16LtS, bytesA, bytesB) == bytesOf(0xFF, 0, 0, 0));
    CHECK(applySimdBinary(Opcode::I8x16LtU, bytesA, bytesB) == bytesOf(0, 0, 0, 0));
    CHECK(applySimdBinary(Opcode::I8x16GtS, bytesA, bytesB) == bytesOf(0, 0, 0xFF, 0));
    CHECK(applySimdBinary(Opcode::I8x16GtU, bytesA, bytesB) == bytesOf(0xFF, 0, 0xFF, 0));
    CHECK(applySimdBinary(Opcode::I8x16LeS, bytesA, bytesB) == bytesOf(0xFF, 0xFF, 0, 0xFF));
    CHECK(applySimdBinary(Opcode::I8x16LeU, bytesA, bytesB) == bytesOf(0, 0xFF, 0, 0xFF));
    CHECK(applySimdBinary(Opcode::I8x16GeS, bytesA, bytesB) == bytesOf(0, 0xFF, 0xFF, 0xFF));
    CHECK(applySimdBinary(Opcode::I8x16GeU, bytesA, bytesB) == bytesOf(0xFF, 0xFF, 0xFF, 0xFF));

    auto const halvesA = vectorOf<uint16_t>({ 0x8000, 1, 5, 0, 0, 0, 0, 0 });
    auto const halvesB = vectorOf<uint16_t>({ 0x0001, 1, 4, 0, 0, 0, 0, 0 });
    auto const halvesOf = [](uint16_t first, uint16_t second, uint16_t third, uint16_t rest) {
        return vectorOf<uint16_t>({ first, second, third, rest, rest, rest, rest, rest });
    };
    CHECK(applySimdBinary(Opcode::I16x8Eq, halvesA, halvesB) == halvesOf(0, 0xFFFF, 0, 0xFFFF));
    CHECK(applySimdBinary(Opcode::I16x8Ne, halvesA, halvesB) == halvesOf(0xFFFF, 0, 0xFFFF, 0));
    CHECK(applySimdBinary(Opcode::I16x8LtS, halvesA, halvesB) == halvesOf(0xFFFF, 0, 0, 0));
    CHECK(applySimdBinary(Opcode::I16x8LtU, halvesA, halvesB) == halvesOf(0, 0, 0, 0));
    CHECK(applySimdBinary(Opcode::I16x8GtS, halvesA, halvesB) == halvesOf(0, 0, 0xFFFF, 0));
    CHECK(applySimdBinary(Opcode::I16x8GtU, halvesA, halvesB) == halvesOf(0xFFFF, 0, 0xFFFF, 0));
    CHECK(applySimdBinary(Opcode::I16x8LeS, halvesA, halvesB) == halvesOf(0xFFFF, 0xFFFF, 0, 0xFFFF));
    CHECK(applySimdBinary(Opcode::I16x8LeU, halvesA, halvesB) == halvesOf(0, 0xFFFF, 0, 0xFFFF));
    CHECK(applySimdBinary(Opcode::I16x8GeS, halvesA, halvesB) == halvesOf(0, 0xFFFF, 0xFFFF, 0xFFFF));
    CHECK(applySimdBinary(Opcode::I16x8GeU, halvesA, halvesB) == halvesOf(0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF));

    auto const wordsA = i32x4({ 0x8000'0000, 1, 5, 0 });
    auto const wordsB = i32x4({ 1, 1, 4, 0 });
    constexpr auto T = uint32_t { 0xFFFF'FFFF };
    CHECK(applySimdBinary(Opcode::I32x4Eq, wordsA, wordsB) == i32x4({ 0, T, 0, T }));
    CHECK(applySimdBinary(Opcode::I32x4Ne, wordsA, wordsB) == i32x4({ T, 0, T, 0 }));
    CHECK(applySimdBinary(Opcode::I32x4LtS, wordsA, wordsB) == i32x4({ T, 0, 0, 0 }));
    CHECK(applySimdBinary(Opcode::I32x4LtU, wordsA, wordsB) == i32x4({ 0, 0, 0, 0 }));
    CHECK(applySimdBinary(Opcode::I32x4GtS, wordsA, wordsB) == i32x4({ 0, 0, T, 0 }));
    CHECK(applySimdBinary(Opcode::I32x4GtU, wordsA, wordsB) == i32x4({ T, 0, T, 0 }));
    CHECK(applySimdBinary(Opcode::I32x4LeS, wordsA, wordsB) == i32x4({ T, T, 0, T }));
    CHECK(applySimdBinary(Opcode::I32x4LeU, wordsA, wordsB) == i32x4({ 0, T, 0, T }));
    CHECK(applySimdBinary(Opcode::I32x4GeS, wordsA, wordsB) == i32x4({ 0, T, T, T }));
    CHECK(applySimdBinary(Opcode::I32x4GeU, wordsA, wordsB) == i32x4({ T, T, T, T }));

    // i64x2 compares signed only.
    auto const longsA = V128 { .lanes = { 1ULL << 63U, 7 } };
    auto const longsB = V128 { .lanes = { 1, 7 } };
    CHECK(applySimdBinary(Opcode::I64x2Eq, longsA, longsB) == V128 { .lanes = { 0, ~0ULL } });
    CHECK(applySimdBinary(Opcode::I64x2Ne, longsA, longsB) == V128 { .lanes = { ~0ULL, 0 } });
    CHECK(applySimdBinary(Opcode::I64x2LtS, longsA, longsB) == V128 { .lanes = { ~0ULL, 0 } });
    CHECK(applySimdBinary(Opcode::I64x2GtS, longsA, longsB) == V128 { .lanes = { 0, 0 } });
    // 1 is above INT64_MIN signed, though below it unsigned.
    CHECK(applySimdBinary(Opcode::I64x2GtS, longsB, longsA) == V128 { .lanes = { ~0ULL, 0 } });
    CHECK(applySimdBinary(Opcode::I64x2LeS, longsA, longsB) == V128 { .lanes = { ~0ULL, ~0ULL } });
    CHECK(applySimdBinary(Opcode::I64x2GeS, longsA, longsB) == V128 { .lanes = { 0, ~0ULL } });
}

TEST_CASE("Simd.allTrue")
{
    CHECK(applySimdTest(Opcode::I8x16AllTrue, filled(0x01)) == I32 { 1 });
    auto oneZeroByte = filled(0xFF);
    setLane<uint8_t>(oneZeroByte, 9, 0);
    CHECK(applySimdTest(Opcode::I8x16AllTrue, oneZeroByte) == I32 { 0 });
    // 0x0100 is a true i16 lane but holds a zero byte.
    CHECK(applySimdTest(Opcode::I16x8AllTrue, every<uint16_t>(0x0100)) == I32 { 1 });
    CHECK(applySimdTest(Opcode::I16x8AllTrue, vectorOf<uint16_t>({ 1, 1, 1, 1, 1, 1, 1, 0 })) == I32 { 0 });
    CHECK(applySimdTest(Opcode::I32x4AllTrue, i32x4({ 1, 0x100, 0x1'0000, 0x100'0000 })) == I32 { 1 });
    CHECK(applySimdTest(Opcode::I32x4AllTrue, i32x4({ 1, 1, 0, 1 })) == I32 { 0 });
    CHECK(applySimdTest(Opcode::I64x2AllTrue, V128 { .lanes = { 1ULL << 63U, 1 } }) == I32 { 1 });
    CHECK(applySimdTest(Opcode::I64x2AllTrue, V128 { .lanes = { 0, 1 } }) == I32 { 0 });
}

TEST_CASE("Simd.extend")
{
    auto const bytes = vectorOf<uint8_t>({ 0x80, 0x7F, 0xFF, 1, 2, 3, 4, 5, 0xFE, 6, 7, 8, 9, 10, 11, 0x81 });
    CHECK(applySimdUnary(Opcode::I16x8ExtendLowI8x16S, bytes)
          == vectorOf<uint16_t>({ 0xFF80, 0x7F, 0xFFFF, 1, 2, 3, 4, 5 }));
    CHECK(applySimdUnary(Opcode::I16x8ExtendLowI8x16U, bytes)
          == vectorOf<uint16_t>({ 0x80, 0x7F, 0xFF, 1, 2, 3, 4, 5 }));
    CHECK(applySimdUnary(Opcode::I16x8ExtendHighI8x16S, bytes)
          == vectorOf<uint16_t>({ 0xFFFE, 6, 7, 8, 9, 10, 11, 0xFF81 }));
    CHECK(applySimdUnary(Opcode::I16x8ExtendHighI8x16U, bytes)
          == vectorOf<uint16_t>({ 0xFE, 6, 7, 8, 9, 10, 11, 0x81 }));

    auto const halves = vectorOf<uint16_t>({ 0x8000, 1, 2, 3, 4, 5, 6, 0xFFFF });
    CHECK(applySimdUnary(Opcode::I32x4ExtendLowI16x8S, halves) == i32x4({ 0xFFFF'8000, 1, 2, 3 }));
    CHECK(applySimdUnary(Opcode::I32x4ExtendLowI16x8U, halves) == i32x4({ 0x8000, 1, 2, 3 }));
    CHECK(applySimdUnary(Opcode::I32x4ExtendHighI16x8S, halves) == i32x4({ 4, 5, 6, 0xFFFF'FFFF }));
    CHECK(applySimdUnary(Opcode::I32x4ExtendHighI16x8U, halves) == i32x4({ 4, 5, 6, 0xFFFF }));

    auto const words = i32x4({ 0x8000'0000, 1, 2, 0xFFFF'FFFF });
    CHECK(applySimdUnary(Opcode::I64x2ExtendLowI32x4S, words)
          == V128 { .lanes = { 0xFFFF'FFFF'8000'0000, 1 } });
    CHECK(applySimdUnary(Opcode::I64x2ExtendLowI32x4U, words) == V128 { .lanes = { 0x8000'0000, 1 } });
    CHECK(applySimdUnary(Opcode::I64x2ExtendHighI32x4S, words) == V128 { .lanes = { 2, ~0ULL } });
    CHECK(applySimdUnary(Opcode::I64x2ExtendHighI32x4U, words) == V128 { .lanes = { 2, 0xFFFF'FFFF } });
}

TEST_CASE("Simd.extmul")
{
    auto const bytes = vectorOf<uint8_t>({ 0x80, 0xFF, 2, 3, 4, 5, 6, 7, 0xFF, 9, 10, 11, 12, 13, 14, 0x80 });
    CHECK(applySimdBinary(Opcode::I16x8ExtmulLowI8x16S, bytes, bytes)
          == vectorOf<uint16_t>({ 0x4000, 1, 4, 9, 16, 25, 36, 49 }));
    CHECK(applySimdBinary(Opcode::I16x8ExtmulLowI8x16U, bytes, bytes)
          == vectorOf<uint16_t>({ 0x4000, 0xFE01, 4, 9, 16, 25, 36, 49 }));
    CHECK(applySimdBinary(Opcode::I16x8ExtmulHighI8x16S, bytes, filled(0x01))
          == vectorOf<uint16_t>({ 0xFFFF, 9, 10, 11, 12, 13, 14, 0xFF80 }));
    CHECK(applySimdBinary(Opcode::I16x8ExtmulHighI8x16U, bytes, bytes)
          == vectorOf<uint16_t>({ 0xFE01, 81, 100, 121, 144, 169, 196, 0x4000 }));

    auto const halves = vectorOf<uint16_t>({ 0x8000, 0xFFFF, 2, 3, 0xFFFF, 5, 6, 0x8000 });
    CHECK(applySimdBinary(Opcode::I32x4ExtmulLowI16x8S, halves, halves) == i32x4({ 0x4000'0000, 1, 4, 9 }));
    CHECK(applySimdBinary(Opcode::I32x4ExtmulLowI16x8U, halves, halves)
          == i32x4({ 0x4000'0000, 0xFFFE'0001, 4, 9 }));
    CHECK(applySimdBinary(Opcode::I32x4ExtmulHighI16x8S, halves, every<uint16_t>(1))
          == i32x4({ 0xFFFF'FFFF, 5, 6, 0xFFFF'8000 }));
    CHECK(applySimdBinary(Opcode::I32x4ExtmulHighI16x8U, halves, halves)
          == i32x4({ 0xFFFE'0001, 25, 36, 0x4000'0000 }));

    auto const words = i32x4({ 0xFFFF'FFFF, 0x8000'0000, 3, 0xFFFF'FFFF });
    CHECK(applySimdBinary(Opcode::I64x2ExtmulLowI32x4S, words, words)
          == V128 { .lanes = { 1, 1ULL << 62U } });
    CHECK(applySimdBinary(Opcode::I64x2ExtmulLowI32x4U, words, words)
          == V128 { .lanes = { 0xFFFF'FFFE'0000'0001, 1ULL << 62U } });
    CHECK(applySimdBinary(Opcode::I64x2ExtmulHighI32x4S, words, i32x4({ 0, 0, 2, 2 }))
          == V128 { .lanes = { 6, ~1ULL } });
    CHECK(applySimdBinary(Opcode::I64x2ExtmulHighI32x4U, words, i32x4({ 0, 0, 2, 2 }))
          == V128 { .lanes = { 6, 0x1'FFFF'FFFE } });
}

TEST_CASE("Simd.extaddPairwise")
{
    auto const bytes =
        vectorOf<uint8_t>({ 0x80, 0x80, 0xFF, 0xFF, 1, 2, 0x7F, 0x7F, 0, 0, 0xFF, 1, 0, 0, 0, 0 });
    CHECK(applySimdUnary(Opcode::I16x8ExtaddPairwiseI8x16S, bytes)
          == vectorOf<uint16_t>({ 0xFF00, 0xFFFE, 3, 0xFE, 0, 0, 0, 0 }));
    CHECK(applySimdUnary(Opcode::I16x8ExtaddPairwiseI8x16U, bytes)
          == vectorOf<uint16_t>({ 0x100, 0x1FE, 3, 0xFE, 0, 0x100, 0, 0 }));
    auto const halves = vectorOf<uint16_t>({ 0x8000, 0x8000, 0xFFFF, 0xFFFF, 1, 2, 0xFFFF, 1 });
    CHECK(applySimdUnary(Opcode::I32x4ExtaddPairwiseI16x8S, halves)
          == i32x4({ 0xFFFF'0000, 0xFFFF'FFFE, 3, 0 }));
    CHECK(applySimdUnary(Opcode::I32x4ExtaddPairwiseI16x8U, halves)
          == i32x4({ 0x1'0000, 0x1'FFFE, 3, 0x1'0000 }));
}

TEST_CASE("Simd.dot")
{
    // Each product fits in 32 bits, but the sum of two (-32768)^2 is 2^31, which wraps.
    CHECK(applySimdBinary(Opcode::I32x4DotI16x8S, every<uint16_t>(0x8000), every<uint16_t>(0x8000))
          == i32x4({ 0x8000'0000, 0x8000'0000, 0x8000'0000, 0x8000'0000 }));
    CHECK(applySimdBinary(Opcode::I32x4DotI16x8S,
                          vectorOf<uint16_t>({ 1, 2, 0xFFFF, 3, 0, 0, 7, 0 }),
                          vectorOf<uint16_t>({ 3, 4, 5, 6, 9, 9, 2, 0xFFFF }))
          == i32x4({ 11, 13, 0, 14 }));
}

TEST_CASE("Simd.minNanIsCanonical")
{
    // A signalling NaN operand gives the positive canonical NaN, not the operand's bits.
    CHECK(applySimdBinary(Opcode::F32x4Min, every<uint32_t>(0x7FA0'0000), every<uint32_t>(f32Bits(1.0F)))
          == every<uint32_t>(0x7FC0'0000));
    CHECK(applySimdBinary(Opcode::F32x4Max, every<uint32_t>(f32Bits(1.0F)), every<uint32_t>(0xFFA0'0000))
          == every<uint32_t>(0x7FC0'0000));
    CHECK(applySimdBinary(Opcode::F64x2Min, every<uint64_t>(0x7FF4'0000'0000'0000), every<uint64_t>(0))
          == every<uint64_t>(0x7FF8'0000'0000'0000));
    CHECK(applySimdBinary(
              Opcode::F64x2Max, every<uint64_t>(f64Bits(-1.0)), every<uint64_t>(0x7FF8'0000'0000'0001))
          == every<uint64_t>(0x7FF8'0000'0000'0000));
}

TEST_CASE("Simd.minNegativeZero")
{
    auto const negativeZero = every<uint32_t>(0x8000'0000);
    auto const positiveZero = every<uint32_t>(0);
    CHECK(applySimdBinary(Opcode::F32x4Min, negativeZero, positiveZero) == negativeZero);
    CHECK(applySimdBinary(Opcode::F32x4Min, positiveZero, negativeZero) == negativeZero);
    CHECK(applySimdBinary(Opcode::F32x4Max, negativeZero, positiveZero) == positiveZero);
    CHECK(applySimdBinary(Opcode::F32x4Max, positiveZero, negativeZero) == positiveZero);
    CHECK(applySimdBinary(Opcode::F64x2Min, every<uint64_t>(0), every<uint64_t>(1ULL << 63U))
          == every<uint64_t>(1ULL << 63U));
}

TEST_CASE("Simd.pminReturnsOperand")
{
    // pmin is `rhs < lhs ? rhs : lhs`: with a NaN no comparison holds, so it is lhs, bits intact.
    auto const nan = every<uint32_t>(0x7FA0'0000);
    auto const one = every<uint32_t>(f32Bits(1.0F));
    CHECK(applySimdBinary(Opcode::F32x4Pmin, nan, one) == nan);
    CHECK(applySimdBinary(Opcode::F32x4Pmin, one, nan) == one);
    CHECK(applySimdBinary(Opcode::F32x4Pmax, nan, one) == nan);
    CHECK(applySimdBinary(Opcode::F32x4Pmax, one, nan) == one);
    // The zeros compare equal, so pmin and pmax both return lhs, unlike min and max.
    auto const negativeZero = every<uint32_t>(0x8000'0000);
    auto const positiveZero = every<uint32_t>(0);
    CHECK(applySimdBinary(Opcode::F32x4Pmin, positiveZero, negativeZero) == positiveZero);
    CHECK(applySimdBinary(Opcode::F32x4Pmax, negativeZero, positiveZero) == negativeZero);
    auto const lanes = vectorOf<uint32_t>({ f32Bits(1.0F), f32Bits(5.0F), f32Bits(-2.0F), f32Bits(3.0F) });
    auto const others = vectorOf<uint32_t>({ f32Bits(2.0F), f32Bits(4.0F), f32Bits(-3.0F), f32Bits(3.0F) });
    CHECK(applySimdBinary(Opcode::F32x4Pmin, lanes, others)
          == vectorOf<uint32_t>({ f32Bits(1.0F), f32Bits(4.0F), f32Bits(-3.0F), f32Bits(3.0F) }));
    CHECK(applySimdBinary(Opcode::F32x4Pmax, lanes, others)
          == vectorOf<uint32_t>({ f32Bits(2.0F), f32Bits(5.0F), f32Bits(-2.0F), f32Bits(3.0F) }));
    auto const nan64 = every<uint64_t>(0x7FF4'0000'0000'0000);
    auto const two = every<uint64_t>(f64Bits(2.0));
    CHECK(applySimdBinary(Opcode::F64x2Pmin, nan64, two) == nan64);
    CHECK(applySimdBinary(Opcode::F64x2Pmax, two, nan64) == two);
    CHECK(applySimdBinary(Opcode::F64x2Pmin, two, every<uint64_t>(f64Bits(1.0)))
          == every<uint64_t>(f64Bits(1.0)));
    CHECK(applySimdBinary(Opcode::F64x2Pmax, every<uint64_t>(f64Bits(1.0)), two) == two);
}

TEST_CASE("Simd.truncSat")
{
    auto const input = vectorOf<uint32_t>({ 0x7FC0'0000, f32Bits(3e9F), f32Bits(-3e9F), f32Bits(-7.9F) });
    CHECK(applySimdUnary(Opcode::I32x4TruncSatF32x4S, input)
          == vectorOf<uint32_t>({ 0, 0x7FFF'FFFF, 0x8000'0000, static_cast<uint32_t>(-7) }));
    CHECK(applySimdUnary(Opcode::I32x4TruncSatF32x4U, input)
          == vectorOf<uint32_t>({ 0, 3'000'000'000U, 0, 0 }));
    CHECK(applySimdUnary(Opcode::I32x4TruncSatF32x4U, every<uint32_t>(f32Bits(5e9F)))
          == every<uint32_t>(~0U));
    // The f64x2 forms fill lanes 0 and 1, and zero the upper two.
    auto const wide = V128 { .lanes = { f64Bits(-1e10), f64Bits(2.7) } };
    CHECK(applySimdUnary(Opcode::I32x4TruncSatF64x2SZero, wide)
          == vectorOf<uint32_t>({ 0x8000'0000, 2, 0, 0 }));
    CHECK(applySimdUnary(Opcode::I32x4TruncSatF64x2UZero, wide) == vectorOf<uint32_t>({ 0, 2, 0, 0 }));
    CHECK(applySimdUnary(Opcode::I32x4TruncSatF64x2UZero,
                         V128 { .lanes = { f64Bits(1e10), 0x7FF8'0000'0000'0000 } })
          == vectorOf<uint32_t>({ ~0U, 0, 0, 0 }));
    // The exact edges: 2^31 is one past i32's maximum but within u32; the infinities clamp.
    auto const edges = vectorOf<uint32_t>({ 0x4F00'0000, 0x7F80'0000, 0xFF80'0000, 0xCF00'0000 });
    CHECK(applySimdUnary(Opcode::I32x4TruncSatF32x4S, edges)
          == vectorOf<uint32_t>({ 0x7FFF'FFFF, 0x7FFF'FFFF, 0x8000'0000, 0x8000'0000 }));
    CHECK(applySimdUnary(Opcode::I32x4TruncSatF32x4U, edges)
          == vectorOf<uint32_t>({ 0x8000'0000, ~0U, 0, 0 }));
    // -2147483649.0 is one below i32's minimum, which f64 holds exactly.
    CHECK(applySimdUnary(Opcode::I32x4TruncSatF64x2SZero,
                         V128 { .lanes = { f64Bits(-2147483649.0), f64Bits(-2147483648.0) } })
          == vectorOf<uint32_t>({ 0x8000'0000, 0x8000'0000, 0, 0 }));
}

TEST_CASE("Simd.demoteZero")
{
    auto const wide = V128 { .lanes = { f64Bits(1.5), f64Bits(-2.0) } };
    CHECK(applySimdUnary(Opcode::F32x4DemoteF64x2Zero, wide)
          == vectorOf<uint32_t>({ f32Bits(1.5F), f32Bits(-2.0F), 0, 0 }));
    // A NaN demotes to the canonical f32 NaN; a value beyond f32's range to infinity.
    CHECK(applySimdUnary(Opcode::F32x4DemoteF64x2Zero,
                         V128 { .lanes = { 0x7FF4'0000'0000'0000, f64Bits(1e300) } })
          == vectorOf<uint32_t>({ 0x7FC0'0000, 0x7F80'0000, 0, 0 }));
    // promote_low reads lanes 0 and 1 only.
    auto const narrow = vectorOf<uint32_t>({ f32Bits(1.5F), 0x7FA0'0000, f32Bits(9.0F), f32Bits(9.0F) });
    CHECK(applySimdUnary(Opcode::F64x2PromoteLowF32x4, narrow)
          == V128 { .lanes = { f64Bits(1.5), 0x7FF8'0000'0000'0000 } });
}

TEST_CASE("Simd.nearestTiesToEven")
{
    CHECK(applySimdUnary(Opcode::F32x4Nearest, every<uint32_t>(f32Bits(2.5F)))
          == every<uint32_t>(f32Bits(2.0F)));
    CHECK(applySimdUnary(Opcode::F32x4Nearest, every<uint32_t>(f32Bits(3.5F)))
          == every<uint32_t>(f32Bits(4.0F)));
    CHECK(applySimdUnary(Opcode::F64x2Nearest, every<uint64_t>(f64Bits(-2.5)))
          == every<uint64_t>(f64Bits(-2.0)));
}

TEST_CASE("Simd.rounding")
{
    struct RoundingRow
    {
        Opcode f32x4;
        Opcode f64x2;
        double expected; ///< The operator's result for -1.5.
    };
    auto const rows = std::to_array<RoundingRow>({
        { .f32x4 = Opcode::F32x4Ceil, .f64x2 = Opcode::F64x2Ceil, .expected = -1.0 },
        { .f32x4 = Opcode::F32x4Floor, .f64x2 = Opcode::F64x2Floor, .expected = -2.0 },
        { .f32x4 = Opcode::F32x4Trunc, .f64x2 = Opcode::F64x2Trunc, .expected = -1.0 },
        { .f32x4 = Opcode::F32x4Nearest, .f64x2 = Opcode::F64x2Nearest, .expected = -2.0 },
    });
    for (auto const& row: rows)
    {
        INFO(isoline::binary::opcodeInfo(row.f32x4).text);
        CHECK(applySimdUnary(row.f32x4, every<uint32_t>(f32Bits(-1.5F)))
              == every<uint32_t>(f32Bits(static_cast<float>(row.expected))));
        CHECK(applySimdUnary(row.f64x2, every<uint64_t>(f64Bits(-1.5)))
              == every<uint64_t>(f64Bits(row.expected)));
        // A NaN rounds to the canonical NaN.
        CHECK(applySimdUnary(row.f32x4, every<uint32_t>(0x7FA0'0000)) == every<uint32_t>(0x7FC0'0000));
        CHECK(applySimdUnary(row.f64x2, every<uint64_t>(0xFFF4'0000'0000'0000))
              == every<uint64_t>(0x7FF8'0000'0000'0000));
    }
}

TEST_CASE("Simd.floatArithmetic")
{
    auto const lanes = vectorOf<uint32_t>({ f32Bits(1.5F), f32Bits(-4.0F), f32Bits(9.0F), 0x7FA0'0000 });
    auto const others = vectorOf<uint32_t>({ f32Bits(0.5F), f32Bits(2.0F), f32Bits(3.0F), f32Bits(1.0F) });
    CHECK(applySimdBinary(Opcode::F32x4Add, lanes, others)
          == vectorOf<uint32_t>({ f32Bits(2.0F), f32Bits(-2.0F), f32Bits(12.0F), 0x7FC0'0000 }));
    CHECK(applySimdBinary(Opcode::F32x4Sub, lanes, others)
          == vectorOf<uint32_t>({ f32Bits(1.0F), f32Bits(-6.0F), f32Bits(6.0F), 0x7FC0'0000 }));
    CHECK(applySimdBinary(Opcode::F32x4Mul, lanes, others)
          == vectorOf<uint32_t>({ f32Bits(0.75F), f32Bits(-8.0F), f32Bits(27.0F), 0x7FC0'0000 }));
    CHECK(applySimdBinary(Opcode::F32x4Div, lanes, others)
          == vectorOf<uint32_t>({ f32Bits(3.0F), f32Bits(-2.0F), f32Bits(3.0F), 0x7FC0'0000 }));
    CHECK(applySimdUnary(Opcode::F32x4Sqrt, lanes)
          == vectorOf<uint32_t>({ 0x3F9C'C471, 0x7FC0'0000, f32Bits(3.0F), 0x7FC0'0000 }));
    // abs and neg work on the sign bit alone: a NaN keeps its payload.
    CHECK(applySimdUnary(Opcode::F32x4Abs, lanes)
          == vectorOf<uint32_t>({ f32Bits(1.5F), f32Bits(4.0F), f32Bits(9.0F), 0x7FA0'0000 }));
    CHECK(applySimdUnary(Opcode::F32x4Neg, lanes)
          == vectorOf<uint32_t>({ f32Bits(-1.5F), f32Bits(4.0F), f32Bits(-9.0F), 0xFFA0'0000 }));

    auto const longs = V128 { .lanes = { f64Bits(6.0), f64Bits(-0.0) } };
    auto const otherLongs = V128 { .lanes = { f64Bits(4.0), f64Bits(0.0) } };
    CHECK(applySimdBinary(Opcode::F64x2Add, longs, otherLongs)
          == V128 { .lanes = { f64Bits(10.0), f64Bits(0.0) } });
    CHECK(applySimdBinary(Opcode::F64x2Sub, longs, otherLongs)
          == V128 { .lanes = { f64Bits(2.0), f64Bits(-0.0) } });
    CHECK(applySimdBinary(Opcode::F64x2Mul, longs, otherLongs)
          == V128 { .lanes = { f64Bits(24.0), f64Bits(-0.0) } });
    // 0 / 0 is a NaN, which is canonical.
    CHECK(applySimdBinary(Opcode::F64x2Div, longs, otherLongs)
          == V128 { .lanes = { f64Bits(1.5), 0x7FF8'0000'0000'0000 } });
    CHECK(applySimdUnary(Opcode::F64x2Sqrt, V128 { .lanes = { f64Bits(16.0), f64Bits(-1.0) } })
          == V128 { .lanes = { f64Bits(4.0), 0x7FF8'0000'0000'0000 } });
    CHECK(applySimdUnary(Opcode::F64x2Abs, longs) == V128 { .lanes = { f64Bits(6.0), f64Bits(0.0) } });
    CHECK(applySimdUnary(Opcode::F64x2Neg, longs) == V128 { .lanes = { f64Bits(-6.0), f64Bits(0.0) } });
}

TEST_CASE("Simd.floatComparisons")
{
    // A true comparison sets every bit of its lane; a NaN makes every comparison but ne false.
    struct ComparisonRow
    {
        Opcode f32x4;
        Opcode f64x2;
        std::array<bool, 4> holds; ///< Whether it holds of 1 and 2, 2 and 2, 2 and 1, and NaN and 1.
    };
    auto const rows = std::to_array<ComparisonRow>({
        { .f32x4 = Opcode::F32x4Eq, .f64x2 = Opcode::F64x2Eq, .holds = { false, true, false, false } },
        { .f32x4 = Opcode::F32x4Ne, .f64x2 = Opcode::F64x2Ne, .holds = { true, false, true, true } },
        { .f32x4 = Opcode::F32x4Lt, .f64x2 = Opcode::F64x2Lt, .holds = { true, false, false, false } },
        { .f32x4 = Opcode::F32x4Gt, .f64x2 = Opcode::F64x2Gt, .holds = { false, false, true, false } },
        { .f32x4 = Opcode::F32x4Le, .f64x2 = Opcode::F64x2Le, .holds = { true, true, false, false } },
        { .f32x4 = Opcode::F32x4Ge, .f64x2 = Opcode::F64x2Ge, .holds = { false, true, true, false } },
    });
    auto const lanes = vectorOf<uint32_t>({ f32Bits(1.0F), f32Bits(2.0F), f32Bits(2.0F), 0x7FC0'0000 });
    auto const others = vectorOf<uint32_t>({ f32Bits(2.0F), f32Bits(2.0F), f32Bits(1.0F), f32Bits(1.0F) });
    auto const mask = [](bool holds) {
        return holds ? ~0U : 0U;
    };
    for (auto const& row: rows)
    {
        INFO(isoline::binary::opcodeInfo(row.f32x4).text);
        auto const [first, second, third, fourth] = row.holds;
        CHECK(applySimdBinary(row.f32x4, lanes, others)
              == vectorOf<uint32_t>({ mask(first), mask(second), mask(third), mask(fourth) }));
        auto const wideMask = [](bool holds) {
            return holds ? ~0ULL : 0ULL;
        };
        CHECK(applySimdBinary(row.f64x2,
                              V128 { .lanes = { f64Bits(1.0), f64Bits(2.0) } },
                              V128 { .lanes = { f64Bits(2.0), f64Bits(2.0) } })
              == V128 { .lanes = { wideMask(first), wideMask(second) } });
        CHECK(applySimdBinary(row.f64x2,
                              V128 { .lanes = { f64Bits(2.0), 0x7FF8'0000'0000'0000 } },
                              V128 { .lanes = { f64Bits(1.0), f64Bits(1.0) } })
              == V128 { .lanes = { wideMask(third), wideMask(fourth) } });
    }
}

TEST_CASE("Simd.integerToFloat")
{
    auto const integers = vectorOf<uint32_t>({ static_cast<uint32_t>(-1), 7, 0x8000'0000, 16'777'217 });
    // 2^24 + 1 is not an f32; it rounds to even, 2^24.
    CHECK(applySimdUnary(Opcode::F32x4ConvertI32x4S, integers)
          == vectorOf<uint32_t>(
              { f32Bits(-1.0F), f32Bits(7.0F), f32Bits(-2147483648.0F), f32Bits(16777216.0F) }));
    CHECK(applySimdUnary(Opcode::F32x4ConvertI32x4U, integers)
          == vectorOf<uint32_t>(
              { f32Bits(4294967296.0F), f32Bits(7.0F), f32Bits(2147483648.0F), f32Bits(16777216.0F) }));
    // The f64x2 forms read lanes 0 and 1 only.
    CHECK(applySimdUnary(Opcode::F64x2ConvertLowI32x4S, integers)
          == V128 { .lanes = { f64Bits(-1.0), f64Bits(7.0) } });
    CHECK(applySimdUnary(Opcode::F64x2ConvertLowI32x4U, integers)
          == V128 { .lanes = { f64Bits(4294967295.0), f64Bits(7.0) } });
}

TEST_CASE("Simd.everyFixedSignatureSimdOpcodeHasARow")
{
    // execute() runs an instruction by its Numerics or Simd row, its memory access row, or a case of
    // its own; it has no fallback, so a SIMD instruction validation admits without one would abort.
    // This walks every 0xFD-prefixed row, whatever its proposal, so a SIMD instruction added later
    // (relaxed SIMD's included) lands with its row. The table holds no relaxed SIMD row yet: until
    // it does, the decoder rejects their encodings as illegal opcodes, so validation admits none.
    for (auto const& row: OpcodeTable)
    {
        if (row.prefix != 0xFD)
            continue;
        INFO(row.text);
        if (row.signature == OpcodeSignature::Special)
        {
            // Loads and stores: the shared memory access table types and executes them.
            CHECK(isoline::binary::memoryAccessOf(row.opcode) != nullptr);
            CHECK(simdShape(row.opcode) == SimdShape::None);
            continue;
        }
        switch (row.opcode)
        {
            // Their immediates are no operator's: the interpreter executes them itself, v128.const as
            // a constant and i8x16.shuffle by its own case.
            case Opcode::V128Const:
            case Opcode::I8x16Shuffle: CHECK(simdShape(row.opcode) == SimdShape::None); break;
            default: CHECK(simdShape(row.opcode) != SimdShape::None); break;
        }
    }
}
