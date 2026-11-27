// SPDX-License-Identifier: Apache-2.0
#include "Arguments.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::differential
{

namespace
{
    /// SplitMix64's increment, and its finalizer's multipliers.
    constexpr auto Golden = uint64_t { 0x9e37'79b9'7f4a'7c15 };
    constexpr auto MixFirst = uint64_t { 0xbf58'476d'1ce4'e5b9 };
    constexpr auto MixSecond = uint64_t { 0x94d0'49bb'1331'11eb };

    [[nodiscard]] constexpr uint64_t mix(uint64_t value) noexcept
    {
        value = (value ^ (value >> 30U)) * MixFirst;
        value = (value ^ (value >> 27U)) * MixSecond;
        return value ^ (value >> 31U);
    }

    /// The edge values of each number type: zero and one, the signed and unsigned extremes, the shift
    /// counts' boundaries, and memory's page size, as bits.
    constexpr auto I32Edges = std::to_array<uint32_t>({
        0,           1,           2,           7,           8,           31,          32,
        33,          63,          64,          0xFFFF,      0x1'0000,    0x1'0001,    0x7FFF'FFFF,
        0x8000'0000, 0x8000'0001, 0xFFFF'FFFE, 0xFFFF'FFFF, 0xFFFF'FFF0, 0xFFFF'0000,
    });
    constexpr auto I64Edges = std::to_array<uint64_t>({
        0,
        1,
        2,
        63,
        64,
        65,
        0xFFFF'FFFF,
        0x1'0000'0000,
        0x7FFF'FFFF'FFFF'FFFF,
        0x8000'0000'0000'0000,
        0x8000'0000'0000'0001,
        0xFFFF'FFFF'FFFF'FFFE,
        0xFFFF'FFFF'FFFF'FFFF,
        0xFFFF'FFFF'0000'0000,
        0x0000'FFFF'FFFF'FFFF,
    });
    /// Floats: ±0, ±1, ±infinity, the canonical NaN of each sign, NaNs with a payload (quiet and signalling),
    /// the smallest subnormal, the largest finite value, and the bounds of the truncations to 32 and 64 bits.
    constexpr auto F32Edges = std::to_array<uint32_t>({
        0x0000'0000, 0x8000'0000, 0x3F80'0000, 0xBF80'0000, 0x7F80'0000, 0xFF80'0000, 0x7FC0'0000,
        0xFFC0'0000, 0x7FA0'0001, 0x7F80'0001, 0xFFFF'FFFF, 0x0000'0001, 0x7F7F'FFFF, 0x4F00'0000,
        0xCF00'0000, 0xCF00'0001, 0x4F80'0000, 0x5F00'0000, 0xDF00'0000, 0x3F00'0000,
    });
    constexpr auto F64Edges = std::to_array<uint64_t>({
        0x0000'0000'0000'0000,
        0x8000'0000'0000'0000,
        0x3FF0'0000'0000'0000,
        0xBFF0'0000'0000'0000,
        0x7FF0'0000'0000'0000,
        0xFFF0'0000'0000'0000,
        0x7FF8'0000'0000'0000,
        0xFFF8'0000'0000'0000,
        0x7FF4'0000'0000'0001,
        0x7FF0'0000'0000'0001,
        0x0000'0000'0000'0001,
        0x7FEF'FFFF'FFFF'FFFF,
        0x41E0'0000'0000'0000,
        0xC1E0'0000'0020'0000,
        0x41F0'0000'0000'0000,
        0x43E0'0000'0000'0000,
        0xC3E0'0000'0000'0000,
        0x3FE0'0000'0000'0000,
    });
    /// Counts a recursion or a loop may take from an argument: below, at and past the store's call depth
    /// (FuzzLimits), and enough to spend its steps.
    constexpr auto Counts =
        std::to_array<uint64_t>({ 0, 1, 2, 3, 10, 100, 998, 999, 1'000, 1'001, 5'000, 100'000 });
    /// How far below memory 0's end an address aims: the widths of every access, and past it.
    constexpr auto BoundDistances = std::to_array<uint64_t>({ 0, 1, 2, 3, 4, 7, 8, 15, 16 });

    /// Where a number argument comes from, chosen per parameter.
    enum class Source : uint8_t
    {
        Random,
        Edge,
        NearBound,
        Count,
    };
    constexpr auto SourceCount = uint64_t { 4 };

    template <typename T, std::size_t N>
    [[nodiscard]] T pick(std::array<T, N> const& table, uint64_t number) noexcept
    {
        return table[number % N];
    }

    /// The bits of a number argument of @p kind (a vector takes two I64s).
    [[nodiscard]] uint64_t numberBits(ArgumentStream& stream,
                                      binary::ValueKind kind,
                                      ArgumentContext const& context) noexcept
    {
        auto const choice = stream.next();
        auto const number = stream.next();
        switch (static_cast<Source>(choice % SourceCount))
        {
            case Source::Random: return number;
            case Source::Edge:
                switch (kind)
                {
                    case binary::ValueKind::I32: return pick(I32Edges, number);
                    case binary::ValueKind::I64: return pick(I64Edges, number);
                    case binary::ValueKind::F32: return pick(F32Edges, number);
                    default: return pick(F64Edges, number);
                }
            case Source::NearBound:
                if (kind == binary::ValueKind::I32 || kind == binary::ValueKind::I64)
                    return context.memoryBytes - pick(BoundDistances, number);
                return number;
            case Source::Count: return pick(Counts, number);
        }
        return number;
    }
} // namespace

ArgumentStream::ArgumentStream(std::span<std::byte const> seed, uint64_t ordinal) noexcept:
    _state { mix(ordinal + Golden) }
{
    for (auto const byte: seed)
        _state = mix(_state ^ std::to_integer<uint64_t>(byte));
}

uint64_t ArgumentStream::next() noexcept
{
    _state += Golden;
    return mix(_state);
}

std::vector<runtime::Value> argumentsFor(ArgumentStream& stream,
                                         std::span<binary::ValueType const> params,
                                         ArgumentContext const& context)
{
    auto arguments = std::vector<runtime::Value> {};
    arguments.reserve(params.size());
    for (auto const& param: params)
    {
        switch (param.kind)
        {
            case binary::ValueKind::I32:
                arguments.emplace_back(
                    runtime::I32 { static_cast<uint32_t>(numberBits(stream, param.kind, context)) });
                break;
            case binary::ValueKind::I64:
                arguments.emplace_back(runtime::I64 { numberBits(stream, param.kind, context) });
                break;
            case binary::ValueKind::F32:
                arguments.emplace_back(
                    runtime::F32 { static_cast<uint32_t>(numberBits(stream, param.kind, context)) });
                break;
            case binary::ValueKind::F64:
                arguments.emplace_back(runtime::F64 { numberBits(stream, param.kind, context) });
                break;
            case binary::ValueKind::V128: {
                auto const low = numberBits(stream, binary::ValueKind::I64, context);
                auto const high = numberBits(stream, binary::ValueKind::I64, context);
                arguments.emplace_back(runtime::V128 { .lanes = { low, high } });
                break;
            }
            case binary::ValueKind::Ref:
                arguments.emplace_back(runtime::Ref { .kind = runtime::RefKind::Null, .heap = param.heap });
                break;
        }
    }
    return arguments;
}

} // namespace isoline::differential
