// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/Code.hpp>
#include <isoline/interpreter/Dispatch.hpp>
#include <isoline/interpreter/Listing.hpp>
#include <isoline/interpreter/LoweringTable.hpp>
#include <isoline/interpreter/Operations.hpp>
#include <isoline/interpreter/testing/Engines.hpp>
#include <isoline/interpreter/testing/Lowering.hpp>
#include <isoline/interpreter/testing/VectorOperations.hpp>
#include <isoline/numerics/SimdKernels.hpp>
#include <isoline/numerics/SimdLanes.hpp>
#include <isoline/numerics/SimdShape.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

// The vector instructions, relaxed SIMD and wide arithmetic on the interpreter, against the reference
// interpreter: every SIMD row of binary::OpcodeTable, the memory accesses included, over fixed-seed vectors,
// under every dispatch and in both execution modes, bit for bit. Both engines evaluate an operator with the
// same numerics kernel, so what these tests check is the interpreter's own part: lowering, slots, immediates,
// the kernel each operation resolves to, and the memory accesses' bounds and byte order.

using isoline::binary::AddressType;
using isoline::binary::ImmediateKind;
using isoline::binary::Limits;
using isoline::binary::MemoryAccess;
using isoline::binary::MemoryAccessKind;
using isoline::binary::MemoryAccessTable;
using isoline::binary::MemoryType;
using isoline::binary::Opcode;
using isoline::binary::OpcodeSignature;
using isoline::binary::OpcodeTable;
using isoline::binary::ValueKind;
using isoline::binary::VectorShape;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::i64Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::ModuleBuilder;
using isoline::interpreter::availableDispatches;
using isoline::interpreter::OperationId;
using isoline::interpreter::operationRow;
using isoline::interpreter::OperationTable;
using isoline::interpreter::SimdLaneTable;
using isoline::interpreter::WideArithmeticTable;
using isoline::interpreter::testing::encodingOf;
using isoline::interpreter::testing::EngineUnderTest;
using isoline::interpreter::testing::functionOf;
using isoline::interpreter::testing::Harness;
using isoline::interpreter::testing::isVectorOperation;
using isoline::interpreter::testing::memoryInstruction;
using isoline::interpreter::testing::operationsOf;
using isoline::interpreter::testing::Reference;
using isoline::interpreter::testing::typesOf;
using isoline::runtime::ExecutionMode;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Limit;
using isoline::runtime::V128;
using isoline::runtime::Value;

namespace binary = isoline::binary;
namespace numerics = isoline::numerics;

namespace
{

/// The bytes of a 64 KiB page.
constexpr auto PageBytes = uint64_t { 65'536 };

/// The interpreter under each available dispatch, in each execution mode, one per run of the calling
/// test case (a Catch2 generator: call it once, at the test case's top level). P4 lowers both modes alike
/// (Decision C2): the canonical NaNs and the deterministic relaxed-SIMD profile in both.
[[nodiscard]] EngineUnderTest interpreterOfEachDispatchAndMode()
{
    auto engines = std::vector<EngineUnderTest> {};
    for (auto const dispatch: availableDispatches())
        for (auto const mode: { ExecutionMode::Production, ExecutionMode::Deterministic })
            engines.push_back(EngineUnderTest { .dispatch = dispatch, .mode = mode });
    return GENERATE_COPY(from_range(engines));
}

/// splitmix64 from a fixed seed: the same sequence on every host and every run.
class Sequence
{
  public:
    explicit constexpr Sequence(uint64_t seed) noexcept: _state { seed } {}

    /// The next 64 bits.
    [[nodiscard]] constexpr uint64_t next() noexcept
    {
        _state += 0x9E37'79B9'7F4A'7C15;
        auto mixed = _state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EB;
        return mixed ^ (mixed >> 31U);
    }

    /// One of @p pool's values.
    template <typename T, std::size_t Size>
    [[nodiscard]] constexpr T pick(std::array<T, Size> const& pool) noexcept
    {
        return pool[next() % Size];
    }

  private:
    uint64_t _state;
};

/// f32 lanes every float kernel must agree on: zeros, ones, infinities, the canonical NaN and NaNs with
/// payloads and either sign, a denormal, halves (rounding ties), and the edges of the integer conversions.
constexpr auto F32Lanes = std::to_array<uint32_t>({
    0x0000'0000, 0x8000'0000, 0x3F80'0000, 0xBF80'0000, 0x7F80'0000, 0xFF80'0000, 0x7FC0'0000,
    0xFFC0'0000, 0x7FA0'0001, 0xFF80'0001, 0x0000'0001, 0x807F'FFFF, 0x4F00'0000, 0x4F80'0000,
    0xCF00'0000, 0x3F00'0000, 0x3FC0'0000, 0x4020'0000, 0x7F7F'FFFF, 0xC020'0000,
});

/// f64 lanes alike.
constexpr auto F64Lanes = std::to_array<uint64_t>({
    0x0000'0000'0000'0000,
    0x8000'0000'0000'0000,
    0x3FF0'0000'0000'0000,
    0xBFF0'0000'0000'0000,
    0x7FF0'0000'0000'0000,
    0xFFF0'0000'0000'0000,
    0x7FF8'0000'0000'0000,
    0xFFF8'0000'0000'0000,
    0x7FF4'0000'0000'0001,
    0x0000'0000'0000'0001,
    0x41E0'0000'0000'0000,
    0x41F0'0000'0000'0000,
    0xC1E0'0000'0000'0000,
    0x3FE0'0000'0000'0000,
    0x3FF8'0000'0000'0000,
    0x4004'0000'0000'0000,
    0x43E0'0000'0000'0000,
    0x7FEF'FFFF'FFFF'FFFF,
});

/// i8 lanes at the edges of saturation, sign and the swizzle's index range.
constexpr auto I8Lanes =
    std::to_array<uint8_t>({ 0x00, 0x01, 0x0F, 0x10, 0x11, 0x40, 0x7F, 0x80, 0x81, 0xFE, 0xFF });

/// i16 lanes alike, the Q15 product's edges among them.
constexpr auto I16Lanes = std::to_array<uint16_t>(
    { 0x0000, 0x0001, 0x00FF, 0x0100, 0x4000, 0x7FFF, 0x8000, 0x8001, 0xC000, 0xFFFF });

/// The vector of @p Lane lanes each drawn from @p pool.
template <std::unsigned_integral Lane, std::size_t Size>
[[nodiscard]] V128 vectorFrom(Sequence& sequence, std::array<Lane, Size> const& pool)
{
    auto vector = V128 {};
    for (auto const index: std::views::iota(0U, numerics::LaneCount<Lane>))
        numerics::setLane<Lane>(vector, index, sequence.pick(pool));
    return vector;
}

/// The 64 vectors every operator runs over, from a fixed seed: random bits, and vectors of f32, f64, i8 and
/// i16 lanes drawn from the pools above.
[[nodiscard]] std::vector<V128> sampleVectors()
{
    auto sequence = Sequence { 0x1505'1A4E };
    auto vectors = std::vector<V128> {};
    for (auto const index: std::views::iota(0U, 64U))
    {
        switch (index % 5)
        {
            case 0: vectors.push_back(V128 { .lanes = { sequence.next(), sequence.next() } }); break;
            case 1: vectors.push_back(vectorFrom(sequence, F32Lanes)); break;
            case 2: vectors.push_back(vectorFrom(sequence, F64Lanes)); break;
            case 3: vectors.push_back(vectorFrom(sequence, I8Lanes)); break;
            default: vectors.push_back(vectorFrom(sequence, I16Lanes)); break;
        }
    }
    return vectors;
}

/// The scalar of @p kind for argument set @p index: shift counts at and past every lane width, the integer
/// edges, the float pools, then random bits.
[[nodiscard]] Value scalarSample(ValueKind kind, std::size_t index, Sequence& sequence)
{
    constexpr auto I32Samples = std::to_array<uint32_t>({ 0,
                                                          1,
                                                          7,
                                                          8,
                                                          9,
                                                          15,
                                                          16,
                                                          17,
                                                          31,
                                                          32,
                                                          33,
                                                          63,
                                                          64,
                                                          65,
                                                          127,
                                                          128,
                                                          0x7FFF'FFFF,
                                                          0x8000'0000,
                                                          0xFFFF'FFFF });
    constexpr auto I64Samples = std::to_array<uint64_t>({ 0,
                                                          1,
                                                          0x7F,
                                                          0x80,
                                                          0xFFFF,
                                                          0x1'0000,
                                                          0x7FFF'FFFF'FFFF'FFFF,
                                                          0x8000'0000'0000'0000,
                                                          0xFFFF'FFFF'FFFF'FFFF });
    switch (kind)
    {
        case ValueKind::I32:
            return I32 { index < I32Samples.size() ? I32Samples[index]
                                                   : static_cast<uint32_t>(sequence.next()) };
        case ValueKind::I64: return I64 { index < I64Samples.size() ? I64Samples[index] : sequence.next() };
        case ValueKind::F32:
            return F32 { index < F32Lanes.size() ? F32Lanes[index] : static_cast<uint32_t>(sequence.next()) };
        default:
            REQUIRE(kind == ValueKind::F64);
            return F64 { index < F64Lanes.size() ? F64Lanes[index] : sequence.next() };
    }
}

/// The 64 argument sets of a signature's @p params: vectors from sampleVectors(), each parameter's from its
/// own offset, so that no two operands of a set coincide; scalars from scalarSample().
[[nodiscard]] std::vector<std::vector<Value>> argumentSetsOf(std::span<ValueKind const> params)
{
    auto const vectors = sampleVectors();
    auto sequence = Sequence { 0x5EED'0F'5CA1 };
    auto sets = std::vector<std::vector<Value>> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, vectors.size()))
    {
        auto set = std::vector<Value> {};
        for (auto const param: std::views::iota(std::size_t { 0 }, params.size()))
        {
            if (params[param] == ValueKind::V128)
                set.emplace_back(vectors[(index + (23 * param)) % vectors.size()]);
            else
                set.push_back(scalarSample(params[param], index, sequence));
        }
        sets.push_back(std::move(set));
    }
    return sets;
}

/// What a test runs on both engines: each module on the reference interpreter and on the production one, the
/// operations of its lowering counted as executed.
class Agreement
{
  public:
    explicit Agreement(EngineUnderTest engine): _engine { engine } {}

    /// Runs @p module's function 0 on both engines with each of @p argumentSets: the outcomes, the steps
    /// they took and the bytes of every memory must be equal.
    void check(ModuleBuilder const& module, std::vector<std::vector<Value>> const& argumentSets)
    {
        auto reference = Harness { module, Reference };
        auto harness = Harness { module, _engine };
        _executed.merge(operationsOf(*harness.code()));
        for (auto const& args: argumentSets)
        {
            CHECK(harness.outcome(0, args) == reference.outcome(0, args));
            CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
            CHECK(harness.store().chargedStackValues() == 0);
            for (auto const index: std::views::iota(std::size_t { 0 }, reference.instance().memories.size()))
                CHECK(std::ranges::equal(bytesOf(harness, index), bytesOf(reference, index)));
        }
    }

    /// The operations every module checked so far was lowered to.
    [[nodiscard]] std::set<OperationId> const& executed() const noexcept { return _executed; }

  private:
    [[nodiscard]] static std::span<std::byte const> bytesOf(Harness& harness, std::size_t memory)
    {
        return std::as_const(harness.store()).memory(harness.instance().memories.at(memory)).bytes();
    }

    EngineUnderTest _engine;
    std::set<OperationId> _executed;
};

/// The lane count of a lane operation's vector, by its opcode's SimdLaneTable row.
[[nodiscard]] uint32_t laneCountOf(Opcode opcode)
{
    auto const* row = isoline::interpreter::simdLaneRowOf(opcode);
    REQUIRE(row != nullptr);
    return 16U / row->laneBytes;
}

/// The 16 bytes of @p vector, as v128.const and i8x16.shuffle encode them.
[[nodiscard]] Bytes bytesOfVector(V128 vector)
{
    auto bytes = Bytes {};
    for (auto const byte: isoline::runtime::v128ToBytes(vector))
        bytes.push_back(std::to_integer<uint8_t>(byte));
    return bytes;
}

/// v128.const of @p vector.
[[nodiscard]] Bytes v128Const(V128 vector)
{
    return code({ encodingOf(Opcode::V128Const), bytesOfVector(vector) });
}

/// Where an operator finds its operands.
enum class Operands : uint8_t
{
    Forwarded, ///< In the params, forwarded by their local.get: the operation reads the locals' slots.
    Homed,     ///< In their home slots, where a block's entry moved them.
};

/// Both placements of an operator's operands.
constexpr auto BothPlacements = std::to_array<Operands>({ Operands::Forwarded, Operands::Homed });

/// The function (params) -> (results) that pushes each param and runs @p instruction on them; Homed, it runs
/// it inside a block of the function's own type, whose entry moves every forwarded param to its home slot.
[[nodiscard]] ModuleBuilder operatorModule(std::span<ValueKind const> params,
                                           std::span<ValueKind const> results,
                                           Bytes const& instruction,
                                           Operands operands)
{
    auto body = Bytes {};
    for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(params.size())))
        body = code({ body, localGet(index) });
    if (operands == Operands::Homed)
        body = code({ body, { 0x02, 0x00 }, instruction, { 0x0B } });
    else
        body = code({ body, instruction });
    return functionOf(typesOf(params), typesOf(results), {}, body);
}

/// The lane index patterns i8x16.shuffle runs with: the identity, the second operand reversed, every byte
/// the last, the operands interleaved, and two from the seed.
[[nodiscard]] std::vector<V128> shufflePatterns()
{
    auto patterns = std::vector<V128> {};
    auto const pattern = [&patterns](auto const& laneOf) {
        auto vector = V128 {};
        for (auto const index: std::views::iota(0U, 16U))
            numerics::setLane<uint8_t>(vector, index, static_cast<uint8_t>(laneOf(index)));
        patterns.push_back(vector);
    };
    pattern([](unsigned index) { return index; });
    pattern([](unsigned index) { return 31 - index; });
    pattern([](unsigned) { return 31U; });
    pattern([](unsigned index) { return (index % 2 == 0 ? 0U : 16U) + (index / 2); });
    auto sequence = Sequence { 0x5AFF'1E };
    for ([[maybe_unused]] auto const round: std::views::iota(0, 2))
        pattern([&sequence](unsigned) { return sequence.next() % 32; });
    return patterns;
}

/// Runs every non-memory SIMD row of OpcodeTable on both engines: an operator over the 64 argument sets,
/// with its operands forwarded and homed; a lane operation at every lane; v128.const and i8x16.shuffle over
/// several immediates.
void checkOperators(Agreement& agreement)
{
    for (auto const& row: OpcodeTable)
    {
        if (row.prefix != 0xFD || row.signature == OpcodeSignature::Special)
            continue;
        INFO(row.text);
        auto const& signature = binary::signatureInfo(row.signature);
        auto const params = signature.paramKinds();
        auto const results = signature.resultKinds();
        auto const sets = argumentSetsOf(params);
        auto const run = [&](Bytes const& instruction) {
            for (auto const placement: BothPlacements)
                agreement.check(operatorModule(params, results, instruction, placement), sets);
        };
        switch (row.immediate)
        {
            case ImmediateKind::V128Const:
                for (auto const& vector: sampleVectors() | std::views::take(8))
                    agreement.check(operatorModule(params, results, v128Const(vector), Operands::Forwarded),
                                    { {} });
                break;
            case ImmediateKind::Shuffle:
                for (auto const& lanes: shufflePatterns())
                    run(code({ encodingOf(row.opcode), bytesOfVector(lanes) }));
                break;
            case ImmediateKind::LaneIndex:
                for (auto const lane: std::views::iota(0U, laneCountOf(row.opcode)))
                    run(code({ encodingOf(row.opcode), { static_cast<uint8_t>(lane) } }));
                break;
            default: run(encodingOf(row.opcode)); break;
        }
    }
}

/// The bytes vector accesses find in memory: distinct, both signs of every lane width among them.
[[nodiscard]] Bytes memoryPattern(std::size_t size)
{
    auto sequence = Sequence { 0xB17E'5 };
    auto bytes = Bytes {};
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, size))
        bytes.push_back(static_cast<uint8_t>(sequence.next()));
    return bytes;
}

/// An active data segment writing @p bytes to memory @p memory, of address type @p type, at @p offset.
[[nodiscard]] Bytes dataAt(uint32_t memory, AddressType type, uint64_t offset, Bytes const& bytes)
{
    auto const offsetCode = type == AddressType::I64 ? i64Const(static_cast<int64_t>(offset))
                                                     : i32Const(static_cast<int32_t>(offset));
    return code({ { 0x02 },
                  encodeU32(memory),
                  offsetCode,
                  { 0x0B },
                  encodeU32(static_cast<uint32_t>(bytes.size())),
                  bytes });
}

/// The vector access @p access of memory @p memory with the static offset @p offset, at lane @p lane where
/// it has one.
[[nodiscard]] Bytes vectorAccess(MemoryAccess const& access, uint64_t offset, uint32_t memory, uint8_t lane)
{
    auto instruction = memoryInstruction(access.opcode, offset, memory);
    if (access.shape == VectorShape::Lane)
        instruction.push_back(lane);
    return instruction;
}

/// The function of @p access to memory @p memory, of @p addressType addresses and @p pages pages of
/// 2^@p pageSizeLog2 bytes, after a memory 0 of one page where @p memory is 1, its bytes memoryPattern() at
/// its start and its end: (address) -> (v128) for a load, (address v128) -> (v128) for a lane load,
/// (address v128) -> () for a store.
[[nodiscard]] ModuleBuilder accessModule(MemoryAccess const& access,
                                         AddressType addressType,
                                         uint32_t memory,
                                         uint64_t offset,
                                         uint8_t lane,
                                         uint64_t pages = 1,
                                         uint8_t pageSizeLog2 = 16)
{
    auto const addressKind = addressType == AddressType::I64 ? ValueKind::I64 : ValueKind::I32;
    auto const load = access.access == MemoryAccessKind::Load;
    auto params = std::vector { addressKind };
    if (!load || access.shape == VectorShape::Lane)
        params.push_back(ValueKind::V128);
    auto const results = load ? std::vector { ValueKind::V128 } : std::vector<ValueKind> {};
    auto module =
        operatorModule(params, results, vectorAccess(access, offset, memory, lane), Operands::Forwarded);
    if (memory != 0)
        module.memory(MemoryType {
            .limits = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 } });
    module.memory(
        MemoryType { .limits = Limits { .min = pages, .max = std::nullopt, .addressType = addressType },
                     .pageSizeLog2 = pageSizeLog2 });
    auto const size = pages << pageSizeLog2;
    auto const pattern = std::min<uint64_t>(size, 64);
    if (pattern > 0)
    {
        module.data(dataAt(memory, addressType, 0, memoryPattern(pattern)));
        module.data(dataAt(memory, addressType, size - pattern, memoryPattern(pattern)));
    }
    return module;
}

/// The address argument @p value of a memory of @p addressType addresses.
[[nodiscard]] Value addressValue(AddressType addressType, uint64_t value)
{
    if (addressType == AddressType::I64)
        return I64 { value };
    return I32 { static_cast<uint32_t>(value) };
}

/// The argument sets of an access of @p width bytes to a memory of @p size bytes with the static offset
/// @p offset: addresses in bounds, at the last that fits and the first that does not, and far past; with
/// two vectors where the access takes one.
[[nodiscard]] std::vector<std::vector<Value>> accessArguments(MemoryAccess const& access,
                                                              AddressType addressType,
                                                              uint64_t size,
                                                              uint64_t offset)
{
    auto const width = uint64_t { access.width };
    auto const last = size - width - offset;
    auto const vectors = sampleVectors();
    auto sets = std::vector<std::vector<Value>> {};
    for (auto const address: { uint64_t { 0 },
                               uint64_t { 3 },
                               last - 1,
                               last,
                               last + 1,
                               size,
                               uint64_t { 0xFFFF'FFFF },
                               std::numeric_limits<uint64_t>::max() })
    {
        auto const addressArgument = addressValue(addressType, address);
        if (access.access == MemoryAccessKind::Store || access.shape == VectorShape::Lane)
        {
            for (auto const& vector: { vectors[1], vectors[3] })
            {
                auto& withVector = sets.emplace_back();
                withVector.push_back(addressArgument);
                withVector.emplace_back(vector);
            }
        }
        else
            sets.emplace_back().push_back(addressArgument);
    }
    return sets;
}

/// Runs every vector load and store on both engines: to memory 0 of either address type and to memory 1,
/// with no static offset and with one, at its first and its last lane, in bounds, at the edges and past them.
void checkAccesses(Agreement& agreement)
{
    for (auto const& access: MemoryAccessTable)
    {
        if (access.value != ValueKind::V128)
            continue;
        auto const lanes = access.shape == VectorShape::Lane ? 16U / access.width : 1U;
        for (auto const addressType: { AddressType::I32, AddressType::I64 })
        {
            for (auto const memory: { 0U, 1U })
            {
                for (auto const offset: { uint64_t { 0 }, uint64_t { 13 } })
                {
                    for (auto const lane: { 0U, lanes - 1 })
                    {
                        INFO(binary::opcodeInfo(access.opcode).text
                             << " of memory " << memory << ", "
                             << (addressType == AddressType::I64 ? "i64" : "i32") << " addresses, offset "
                             << offset << ", lane " << lane);
                        agreement.check(
                            accessModule(access, addressType, memory, offset, static_cast<uint8_t>(lane)),
                            accessArguments(access, addressType, PageBytes, offset));
                    }
                }
            }
        }
    }
}

} // namespace

TEST_CASE("Simd.everySimdOpcodeAgreesWithReference")
{
    // Every 0xFD row of OpcodeTable, relaxed SIMD's included, and every vector access, over the same inputs
    // on both engines: the same results bit for bit, NaN payloads and signs included, the same traps, the
    // same steps, the same memory. Together the modules run every vector operation lowering emits.
    auto agreement = Agreement { interpreterOfEachDispatchAndMode() };
    checkOperators(agreement);
    checkAccesses(agreement);
    for (auto const& row: OperationTable)
    {
        if (!isVectorOperation(row.id)
            || operationRow(row.id).family == isoline::interpreter::OperationFamily::Numeric)
            continue;
        INFO(row.name);
        CHECK(agreement.executed().contains(row.id));
    }
}

TEST_CASE("Simd.lowersEachOpcodeToItsKernelOnce")
{
    // An operator of a kernel shape lowers to the generic operation of its shape, whose last word is its
    // kernel, numerics' own; a lane operation, i32x4.add and f32x4.mul to their dedicated operations.
    for (auto const& row: OpcodeTable)
    {
        if (row.prefix != 0xFD || row.signature == OpcodeSignature::Special
            || row.immediate != ImmediateKind::None)
            continue;
        INFO(row.text);
        auto const& signature = binary::signatureInfo(row.signature);
        auto const module = isoline::interpreter::testing::lowered(
            isoline::interpreter::testing::validated(operatorModule(signature.paramKinds(),
                                                                    signature.resultKinds(),
                                                                    encodingOf(row.opcode),
                                                                    Operands::Forwarded)));
        auto const& lowered = module->functions().front();
        auto const words = std::span { lowered.code };
        auto const operation = isoline::interpreter::operationOfHeader(words.front());
        auto const shape = numerics::simdShape(row.opcode);
        auto const kernel = [&]() -> uint64_t {
            switch (shape)
            {
                case numerics::SimdShape::Unary:
                    return std::bit_cast<uint64_t>(numerics::simdUnaryKernel(row.opcode));
                case numerics::SimdShape::Binary:
                    return std::bit_cast<uint64_t>(numerics::simdBinaryKernel(row.opcode));
                case numerics::SimdShape::Ternary:
                    return std::bit_cast<uint64_t>(numerics::simdTernaryKernel(row.opcode));
                case numerics::SimdShape::Test:
                    return std::bit_cast<uint64_t>(numerics::simdTestKernel(row.opcode));
                case numerics::SimdShape::Shift:
                    return std::bit_cast<uint64_t>(numerics::simdShiftKernel(row.opcode));
                default: return 0;
            }
        }();
        auto const dedicated = isoline::interpreter::loweringRow(row.opcode).form
                               != isoline::interpreter::OperationForm::Special;
        if (dedicated)
        {
            CHECK(operation == isoline::interpreter::loweringRow(row.opcode).operation);
            CHECK(operationRow(operation).text == row.text);
            continue;
        }
        if (auto const* lane = isoline::interpreter::simdLaneRowOf(row.opcode))
        {
            CHECK(operation == lane->operation);
            continue;
        }
        REQUIRE(kernel != 0);
        CHECK(operation == isoline::interpreter::simdKernelOperationOf(shape));
        auto const& info = operationRow(operation);
        CHECK(words[info.words - 1] == kernel);
        // The listing names the kernel by the first instruction that has it, not by its address: a relaxed
        // operator the deterministic profile maps onto its plain twin is listed with the twin's kernel.
        auto const first = std::ranges::find_if(OpcodeTable, [&](auto const& other) {
            return isoline::interpreter::simdKernelOperationOf(numerics::simdShape(other.opcode)) == operation
                   && isoline::interpreter::simdKernelWordOf(other.opcode) == kernel;
        });
        REQUIRE(first != OpcodeTable.end());
        auto const listing = isoline::interpreter::describe(lowered);
        CHECK(listing.starts_with(std::format("{} ", info.text)));
        CHECK(listing.contains(std::format(", {}\n", first->text)));
        CHECK_FALSE(listing.contains('#'));
    }
}

TEST_CASE("Simd.vectorRowChecksDecide")
{
    // The check the static_assert runs over the lane and wide-arithmetic tables runs here too: it accepts
    // them, and refuses a row naming another instruction's operation, or an instruction a LoweringRows row
    // lowers.
    namespace detail = isoline::interpreter::detail;
    using isoline::interpreter::SimdLaneRow;
    using isoline::interpreter::WideArithmeticRow;
    CHECK(detail::vectorRowsNameTheirOperations(SimdLaneTable, WideArithmeticTable));
    auto other = SimdLaneTable[0];
    other.operation = OperationId::I16x8Splat;
    CHECK_FALSE(detail::vectorRowsNameTheirOperations(std::array { other }, {}));
    auto const lowered = SimdLaneRow { .opcode = Opcode::I32x4Add,
                                       .operation = OperationId::I32x4Add,
                                       .shape = numerics::SimdShape::Splat,
                                       .laneBytes = 4,
                                       .extension = binary::Extension::Zero };
    CHECK_FALSE(detail::vectorRowsNameTheirOperations(std::array { lowered }, {}));
    auto const swapped =
        WideArithmeticRow { .opcode = Opcode::I64Add128, .operation = OperationId::I64MulWideS };
    CHECK_FALSE(detail::vectorRowsNameTheirOperations({}, std::array { swapped }));
    auto const family = WideArithmeticRow { .opcode = Opcode::I32x4Add, .operation = OperationId::I32x4Add };
    CHECK_FALSE(detail::vectorRowsNameTheirOperations({}, std::array { family }));
    // Only the five kernel shapes have a generic operation.
    for (auto const shape: { numerics::SimdShape::None,
                             numerics::SimdShape::Splat,
                             numerics::SimdShape::ExtractLane,
                             numerics::SimdShape::ReplaceLane })
        CHECK(isoline::interpreter::simdKernelOperationOf(shape) == OperationId::Nop);
    // And only their instructions a kernel word.
    for (auto const opcode: { Opcode::I32Add, Opcode::I8x16Splat, Opcode::V128Load, Opcode::I8x16Shuffle })
        CHECK(isoline::interpreter::simdKernelWordOf(opcode) == 0);
}

TEST_CASE("Simd.dedicatedRowsRunTheTablesKernels")
{
    // i32x4.add and f32x4.mul inline their kernels rather than calling through a word: the same functions
    // numerics' table holds, so that both engines compute them alike.
    CHECK(isoline::interpreter::SimdInlineKernel<Opcode::I32x4Add>
          == numerics::simdBinaryKernel(Opcode::I32x4Add));
    CHECK(isoline::interpreter::SimdInlineKernel<Opcode::F32x4Mul>
          == numerics::simdBinaryKernel(Opcode::F32x4Mul));
}

TEST_CASE("Simd.everyLaneRowNamesItsLanes")
{
    // A lane operation's row names the lane width its instruction's shape has, and the operation of its name.
    for (auto const& row: SimdLaneTable)
    {
        auto const text = binary::opcodeInfo(row.opcode).text;
        INFO(text);
        CHECK(operationRow(row.operation).text == text);
        CHECK(numerics::simdShape(row.opcode) == row.shape);
        auto const prefix = text.substr(0, text.find('.'));
        auto const lanes = std::stoul(std::string { prefix.substr(prefix.find('x') + 1) });
        CHECK(lanes * row.laneBytes == 16);
        auto const sign = text.ends_with("_s");
        CHECK((row.extension == binary::Extension::Sign) == sign);
    }
}

TEST_CASE("Simd.smallMemoriesKeepAVectorsTail")
{
    // A v128 access is the widest: a masked one at the base of a memory of fewer than 16 bytes reads past its
    // end, 16 bytes past a memory of none, which every memory keeps, the heap's as its storage reports them.
    auto widest = uint64_t { 0 };
    for (auto const& access: MemoryAccessTable)
        widest = std::max<uint64_t>(widest, access.width);
    CHECK(widest == 16);
    CHECK(isoline::runtime::MaxAccessWidth == widest);
    CHECK(isoline::runtime::requiredTailBytesOf(0) == 16);
    CHECK(isoline::runtime::requiredTailBytesOf(1) == 15);
    CHECK(isoline::runtime::requiredTailBytesOf(16) == 0);
    auto heap = isoline::runtime::HeapMemoryAllocator {};
    for (auto const pageSizeLog2: { uint8_t { 0 }, uint8_t { 16 } })
    {
        INFO("pages of 2^" << unsigned { pageSizeLog2 } << " bytes");
        auto const type =
            MemoryType { .limits = Limits { .min = 0, .max = 1, .addressType = AddressType::I32 },
                         .pageSizeLog2 = pageSizeLog2 };
        auto const request =
            isoline::runtime::MemoryRequest { .type = type,
                                              .initialBytes = 0,
                                              .maximumBytes = isoline::binary::pageSize(type),
                                              .requiredGuardBytes = 0,
                                              .requiredTailBytes = isoline::runtime::requiredTailBytesOf(0) };
        auto const memory = heap.allocate(request);
        REQUIRE(memory.has_value());
        CHECK(std::as_const(**memory).bytes().empty());
        // A masked access of 16 bytes at the base stays within the storage.
        CHECK((*memory)->tailBytes() + (*memory)->guardBytes() >= 16);
    }
}

TEST_CASE("Simd.relaxedUsesTheDeterministicProfile")
{
    // The relaxed operators' results are the Wasmtime-deterministic profile's (numerics/SimdKernels.hpp), the
    // same on every host and in both modes: the reference's, and the values that profile names.
    auto const engine = interpreterOfEachDispatchAndMode();
    struct Row
    {
        Opcode opcode;
        std::vector<V128> operands;
        V128 expected;
    };
    auto const every32 = [](uint32_t lane) {
        return V128 { .lanes = { lane | (uint64_t { lane } << 32U), lane | (uint64_t { lane } << 32U) } };
    };
    auto const every64 = [](uint64_t lane) {
        return V128 { .lanes = { lane, lane } };
    };
    auto const every16 = [&every32](uint16_t lane) {
        return every32(lane | (uint32_t { lane } << 16U));
    };
    auto const filled = [&every16](uint8_t byte) {
        return every16(static_cast<uint16_t>(byte | (byte << 8U)));
    };
    auto bytes = V128 {};
    auto indices = V128 {};
    auto swizzled = V128 {};
    constexpr auto Indices = std::to_array<uint8_t>(
        { 0x10, 0x80, 0x00, 0x0F, 0xFF, 0x8F, 0x01, 0x11, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 });
    for (auto const index: std::views::iota(0U, 16U))
    {
        numerics::setLane<uint8_t>(bytes, index, static_cast<uint8_t>(0x40 + index));
        numerics::setLane<uint8_t>(indices, index, Indices[index]);
        numerics::setLane<uint8_t>(
            swizzled, index, Indices[index] < 16 ? static_cast<uint8_t>(0x40 + Indices[index]) : 0);
    }
    auto const rows = std::vector<Row> {
        // An index of 16 or more gives 0, its top bit set or not: swizzle, not pshufb.
        { .opcode = Opcode::I8x16RelaxedSwizzle, .operands = { bytes, indices }, .expected = swizzled },
        // Fused, rounded once: (1 + 2^-23)^2 - (1 + 2^-22) is 2^-46, and the tie only 2^-60 breaks upwards.
        { .opcode = Opcode::F32x4RelaxedMadd,
          .operands = { every32(0x3F80'0001), every32(0x3F80'0001), every32(0xBF80'0002) },
          .expected = every32(0x2880'0000) },
        { .opcode = Opcode::F32x4RelaxedMadd,
          .operands = { every32(0x3F80'0800), every32(0x3F80'0800), every32(0x2180'0000) },
          .expected = every32(0x3F80'1001) },
        { .opcode = Opcode::F32x4RelaxedNmadd,
          .operands = { every32(0x3F80'0001), every32(0x3F80'0001), every32(0x3F80'0002) },
          .expected = every32(0xA880'0000) },
        { .opcode = Opcode::F64x2RelaxedMadd,
          .operands = { every64(0x3FF0'0000'0000'0001),
                        every64(0x3FF0'0000'0000'0001),
                        every64(0xBFF0'0000'0000'0002) },
          .expected = every64(0x3970'0000'0000'0000) },
        { .opcode = Opcode::F64x2RelaxedNmadd,
          .operands = { every64(0x3FF0'0000'0000'0001),
                        every64(0x3FF0'0000'0000'0001),
                        every64(0x3FF0'0000'0000'0002) },
          .expected = every64(0xB970'0000'0000'0000) },
        // A NaN, or infinity times zero, gives the positive canonical NaN.
        { .opcode = Opcode::F32x4RelaxedMadd,
          .operands = { every32(0x7F80'0000), every32(0), every32(0x3F80'0000) },
          .expected = every32(0x7FC0'0000) },
        { .opcode = Opcode::F64x2RelaxedNmadd,
          .operands = { every64(0x3FF0'0000'0000'0000),
                        every64(0xFFF4'0000'0000'0000),
                        every64(0x3FF0'0000'0000'0000) },
          .expected = every64(0x7FF8'0000'0000'0000) },
        // laneselect is bitselect: bit by bit, not by each lane's top bit.
        { .opcode = Opcode::I8x16RelaxedLaneselect,
          .operands = { filled(0xA5), filled(0x5A), filled(0x0F) },
          .expected = filled(0x55) },
        { .opcode = Opcode::I16x8RelaxedLaneselect,
          .operands = { filled(0xFF), filled(0x00), every16(0x000F) },
          .expected = every16(0x000F) },
        { .opcode = Opcode::I32x4RelaxedLaneselect,
          .operands = { filled(0xFF), filled(0x00), every32(0x8000'000F) },
          .expected = every32(0x8000'000F) },
        { .opcode = Opcode::I64x2RelaxedLaneselect,
          .operands = { filled(0xFF), filled(0x00), every64(0x0F) },
          .expected = every64(0x0F) },
        // trunc saturates, and a NaN gives 0.
        { .opcode = Opcode::I32x4RelaxedTruncF32x4S,
          .operands = { V128 { .lanes = { 0x4F80'0000'FFC0'0000, 0xC0FC'CCCD'DF80'0000 } } },
          .expected = V128 { .lanes = { 0x7FFF'FFFF'0000'0000, 0xFFFF'FFF9'8000'0000 } } },
        { .opcode = Opcode::I32x4RelaxedTruncF32x4U,
          .operands = { V128 { .lanes = { 0x4F80'0000'7FC0'0000, 0x40FC'CCCD'BF80'0000 } } },
          .expected = V128 { .lanes = { 0xFFFF'FFFF'0000'0000, 0x0000'0007'0000'0000 } } },
        { .opcode = Opcode::I32x4RelaxedTruncF64x2SZero,
          .operands = { V128 { .lanes = { 0x7FF8'0000'0000'0000, 0xC202'A05F'2000'0000 } } },
          .expected = V128 { .lanes = { 0x8000'0000'0000'0000, 0 } } },
        { .opcode = Opcode::I32x4RelaxedTruncF64x2UZero,
          .operands = { V128 { .lanes = { 0x4202'A05F'2000'0000, 0xC202'A05F'2000'0000 } } },
          .expected = V128 { .lanes = { 0x0000'0000'FFFF'FFFF, 0 } } },
        // -1 times -1 in Q15 saturates to 0x7FFF.
        { .opcode = Opcode::I16x8RelaxedQ15mulrS,
          .operands = { every16(0x8000), every16(0x8000) },
          .expected = every16(0x7FFF) },
        // Both operands signed: two products of 16384 wrap at 16 bits to 0x8000; four sum exactly in 32 bits.
        { .opcode = Opcode::I16x8RelaxedDotI8x16I7x16S,
          .operands = { filled(0x80), filled(0x80) },
          .expected = every16(0x8000) },
        { .opcode = Opcode::I32x4RelaxedDotI8x16I7x16AddS,
          .operands = { filled(0x80),
                        filled(0x80),
                        V128 { .lanes = { 0x0000'0000'0000'0001, 0x7FFF'FFFF'FFFF'FFFF } } },
          .expected = V128 { .lanes = { 0x0001'0000'0001'0001, 0x8000'FFFF'0000'FFFF } } },
        // min and max: -0 below +0, a NaN canonical.
        { .opcode = Opcode::F32x4RelaxedMin,
          .operands = { every32(0), every32(0x8000'0000) },
          .expected = every32(0x8000'0000) },
        { .opcode = Opcode::F32x4RelaxedMax,
          .operands = { every32(0x3F80'0000), every32(0xFFC0'0000) },
          .expected = every32(0x7FC0'0000) },
        { .opcode = Opcode::F64x2RelaxedMin,
          .operands = { every64(0), every64(uint64_t { 1 } << 63U) },
          .expected = every64(uint64_t { 1 } << 63U) },
        { .opcode = Opcode::F64x2RelaxedMax,
          .operands = { every64(0x7FF4'0000'0000'0000), every64(0) },
          .expected = every64(0x7FF8'0000'0000'0000) },
    };
    for (auto const& row: rows)
    {
        auto const& info = binary::opcodeInfo(row.opcode);
        INFO(info.text);
        auto const& signature = binary::signatureInfo(info.signature);
        auto args = std::vector<Value> {};
        for (auto const& operand: row.operands)
            args.emplace_back(operand);
        for (auto const placement: BothPlacements)
        {
            auto const module = operatorModule(
                signature.paramKinds(), signature.resultKinds(), encodingOf(row.opcode), placement);
            auto reference = Harness { module, Reference };
            auto interpreted = Harness { module, engine };
            CHECK(reference.result(0, args) == Value { row.expected });
            CHECK(interpreted.result(0, args) == Value { row.expected });
        }
    }
}

TEST_CASE("Simd.wideArithmeticAgrees")
{
    // i64.add128, i64.sub128, i64.mul_wide_s and i64.mul_wide_u at the edges of their carries, borrows and
    // signs, their operands forwarded, homed or constants: the reference's two results, low then high.
    auto const engine = interpreterOfEachDispatchAndMode();
    auto agreement = Agreement { engine };
    constexpr auto Max = std::numeric_limits<uint64_t>::max();
    constexpr auto Top = uint64_t { 1 } << 63U;
    constexpr auto Edges = std::to_array<uint64_t>(
        { 0, 1, 2, Max, Max - 1, Top, Top - 1, Top + 1, 0xFFFF'FFFF, 0x1'0000'0000, 0x1234'5678'9ABC'DEF0 });
    for (auto const& row: WideArithmeticTable)
    {
        auto const& info = binary::opcodeInfo(row.opcode);
        INFO(info.text);
        auto const& signature = binary::signatureInfo(info.signature);
        auto const params = signature.paramKinds();
        auto sets = std::vector<std::vector<Value>> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, Edges.size() * Edges.size()))
        {
            auto const lhs = index / Edges.size();
            auto const rhs = index % Edges.size();
            if (params.size() == 2)
                sets.push_back({ I64 { Edges[lhs] }, I64 { Edges[rhs] } });
            else
                sets.push_back({ I64 { Edges[lhs] },
                                 I64 { Edges[(lhs + 3) % Edges.size()] },
                                 I64 { Edges[rhs] },
                                 I64 { Edges[(rhs + 5) % Edges.size()] } });
        }
        for (auto const placement: BothPlacements)
            agreement.check(
                operatorModule(params, signature.resultKinds(), encodingOf(row.opcode), placement), sets);
        // Constant operands, materialized into their home slots.
        auto constants = Bytes {};
        for (auto const index: std::views::iota(std::size_t { 0 }, params.size()))
            constants =
                code({ constants, i64Const(static_cast<int64_t>(Edges[((index * 4) + 3) % Edges.size()])) });
        agreement.check(
            functionOf({}, typesOf(signature.resultKinds()), {}, code({ constants, encodingOf(row.opcode) })),
            { {} });
    }
    for (auto const& row: WideArithmeticTable)
    {
        INFO(operationRow(row.operation).text);
        CHECK(agreement.executed().contains(row.operation));
    }
    // The results the proposal names, on the interpreter: the carry and the borrow cross the halves, and the
    // signed product's high half differs from the unsigned one's.
    struct Expected
    {
        Opcode opcode;
        std::vector<uint64_t> operands;
        std::array<uint64_t, 2> results;
    };
    auto const expectations = std::to_array<Expected>({
        { .opcode = Opcode::I64Add128, .operands = { Max, 0, 1, 0 }, .results = { 0, 1 } },
        { .opcode = Opcode::I64Add128, .operands = { Max, Max, 1, 0 }, .results = { 0, 0 } },
        { .opcode = Opcode::I64Sub128, .operands = { 0, 0, 1, 0 }, .results = { Max, Max } },
        { .opcode = Opcode::I64Sub128, .operands = { 0, 1, 1, 0 }, .results = { Max, 0 } },
        { .opcode = Opcode::I64MulWideU, .operands = { Max, Max }, .results = { 1, Max - 1 } },
        { .opcode = Opcode::I64MulWideS, .operands = { Max, Max }, .results = { 1, 0 } },
        { .opcode = Opcode::I64MulWideS, .operands = { Top, 2 }, .results = { 0, Max } },
        { .opcode = Opcode::I64MulWideU, .operands = { Top, 2 }, .results = { 0, 1 } },
    });
    for (auto const& expected: expectations)
    {
        auto const& signature = binary::signatureInfo(binary::opcodeInfo(expected.opcode).signature);
        INFO(binary::opcodeInfo(expected.opcode).text);
        auto harness = Harness { operatorModule(signature.paramKinds(),
                                                signature.resultKinds(),
                                                encodingOf(expected.opcode),
                                                Operands::Forwarded),
                                 engine };
        auto args = std::vector<Value> {};
        for (auto const operand: expected.operands)
            args.emplace_back(I64 { operand });
        CHECK(harness.outcome(0, args)
              == std::vector<Value> { I64 { expected.results[0] }, I64 { expected.results[1] } });
    }
}
