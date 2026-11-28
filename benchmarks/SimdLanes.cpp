// SPDX-License-Identifier: Apache-2.0
//
// isoline-benchmark-simd-lanes <operation>
//
// Calls one SIMD lane kernel of the reference engine (reference/Simd.hpp) Iterations times on pseudo-random
// vectors, so that callgrind counts the instructions of the operation alone:
//
//   valgrind --tool=callgrind --toggle-collect='*applySimd*' isoline-benchmark-simd-lanes f32x4.add
//
// gives Ir per operation as the summary over Iterations, and Ir per lane as that over the row's lanes. This
// is how P4 Task 3 measured the lane kernels rebuilt on the typed scalar kernels; scripts/benchmark.py's
// `simd-lanes` command runs it per row. Prints the XOR of every result, so no call is dead.
#include <array>
#include <cstdint>
#include <cstdio>
#include <format>
#include <iostream>
#include <ranges>
#include <span>
#include <string_view>

#include <isoline/binary/Opcode.hpp>
#include <isoline/reference/Simd.hpp>
#include <isoline/runtime/Value.hpp>

namespace
{

using isoline::binary::Opcode;
using isoline::runtime::V128;

/// How many times each operation runs.
constexpr auto Iterations = uint32_t { 100'000 };

/// How many operands an operation takes.
enum class Arity : uint8_t
{
    Binary,
    Ternary,
};

/// One operation the driver measures: its name on the command line, its opcode, its operands and lanes.
struct SimdLaneRow
{
    std::string_view name; ///< As the command line and the report spell it.
    Opcode opcode;         ///< The operation.
    Arity arity;           ///< Its operands.
    uint32_t lanes;        ///< Its lanes, by which the report divides.
};

/// The operations measured: the ones P4 Task 3's callgrind numbers were taken of.
constexpr auto SimdLaneRows = std::to_array<SimdLaneRow>({
    { .name = "f32x4.add", .opcode = Opcode::F32x4Add, .arity = Arity::Binary, .lanes = 4 },
    { .name = "i8x16.add", .opcode = Opcode::I8x16Add, .arity = Arity::Binary, .lanes = 16 },
    { .name = "f64x2.mul", .opcode = Opcode::F64x2Mul, .arity = Arity::Binary, .lanes = 2 },
    { .name = "i16x8.relaxed_dot",
      .opcode = Opcode::I16x8RelaxedDotI8x16I7x16S,
      .arity = Arity::Binary,
      .lanes = 8 },
    { .name = "i32x4.relaxed_dot_add",
      .opcode = Opcode::I32x4RelaxedDotI8x16I7x16AddS,
      .arity = Arity::Ternary,
      .lanes = 4 },
});

/// An xorshift64 generator of the operands: the same sequence on every run.
class Operands
{
  public:
    /// The next pseudo-random vector.
    [[nodiscard]] V128 next() noexcept { return V128 { .lanes = { step(), step() } }; }

  private:
    uint64_t step() noexcept
    {
        _state ^= _state << 13U;
        _state ^= _state >> 7U;
        _state ^= _state << 17U;
        return _state;
    }

    uint64_t _state = 0x9E37'79B9'7F4A'7C15ULL; ///< The generator's state.
};

/// Runs @p row Iterations times.
/// @return The XOR of every result's lanes.
uint64_t measure(SimdLaneRow const& row)
{
    auto operands = Operands {};
    auto sink = uint64_t { 0 };
    for ([[maybe_unused]] auto const iteration: std::views::iota(uint32_t { 0 }, Iterations))
    {
        auto const result =
            row.arity == Arity::Binary
                ? isoline::reference::applySimdBinary(row.opcode, operands.next(), operands.next())
                : isoline::reference::applySimdTernary(
                      row.opcode, operands.next(), operands.next(), operands.next());
        sink ^= result.lanes[0] ^ result.lanes[1];
    }
    return sink;
}

} // namespace

int main(int argc, char const* argv[])
{
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    auto const which = arguments.size() == 2 ? std::string_view { arguments[1] } : std::string_view {};
    for (auto const& row: SimdLaneRows)
    {
        if (row.name != which)
            continue;
        std::cout << std::format(
            "{{\"operation\": \"{}\", \"iterations\": {}, \"lanes\": {}, \"sink\": {}}}\n",
            row.name,
            Iterations,
            row.lanes,
            measure(row));
        return 0;
    }
    std::cerr << "usage: isoline-benchmark-simd-lanes <operation>; the operations are:";
    for (auto const& row: SimdLaneRows)
        std::cerr << ' ' << row.name;
    std::cerr << '\n';
    return 2;
}
