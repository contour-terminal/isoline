// SPDX-License-Identifier: Apache-2.0
#pragma once

// Calling conventions as rows. A convention is what a call passes where: the argument and result registers
// of each class, how the two classes share the argument registers, the registers a callee preserves, the
// shadow space and the stack's alignment. Each target owns the rows of its conventions (x86-64's in its
// Registers.hpp, P6's arm64 ones in its own), in these shapes; assignArguments() and assignResults() read
// any of them alike. Which convention the host uses (the runtime stubs, the entry trampoline) is a row
// lookup per (architecture, ABI), hostConvention(), so that a cross-compiled artifact names its host's
// convention without the building host's CMake selection (amended for P6, 2026-11-26: F2).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <variant>
#include <vector>

#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/machine/Registers.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>

namespace isoline::compiler::machine
{

/// A calling convention.
enum class Convention : uint8_t
{
    WasmInternal, ///< Between compiled Wasm functions: the target's own.
    SystemV,      ///< The System V AMD64 ABI: Linux, the BSDs, macOS on x86-64.
    Windows64,    ///< The Microsoft x64 convention: shadow space, xmm6–15 and rdi/rsi callee-saved.
};

/// The last Convention enumerator.
inline constexpr auto LastConvention = Convention::Windows64;

/// How a convention hands out argument registers to the two classes.
enum class ArgumentSlots : uint8_t
{
    /// Each class counts on its own: the n-th integer argument takes the n-th integer register, whatever
    /// floats came before it (System V, AAPCS64, Isoline's internal conventions).
    Independent,
    /// The classes share one count: the n-th argument takes the n-th register of its own class, and a
    /// stack slot once n passes either class's registers (the Microsoft x64 convention).
    Shared,
};

/// One calling convention of a target.
struct ConventionRow
{
    Convention convention = Convention::WasmInternal;      ///< Which convention this is.
    std::span<PhysicalRegister const> integerArguments {}; ///< General-class argument registers, in order.
    std::span<PhysicalRegister const> floatArguments {};   ///< Float-class argument registers, in order.
    std::span<PhysicalRegister const> integerResults {};   ///< General-class result registers, in order.
    std::span<PhysicalRegister const> floatResults {};     ///< Float-class result registers, in order.
    RegisterSet calleeSaved {}; ///< The registers a callee preserves; every other one a call clobbers.
    ArgumentSlots slots = ArgumentSlots::Independent; ///< How the classes share argument registers.
    uint32_t shadowSpaceBytes = 0; ///< Stack the caller reserves below the arguments for the callee.
    uint32_t stackSlotBytes = 8;   ///< The bytes one stack argument or stacked result takes.
    uint32_t stackAlignment = 16;  ///< The stack pointer's alignment at a call.
    /// Where the pointer to a caller-provided result area is passed, when the results outnumber the result
    /// registers (arm64's x8); none: as a hidden first argument, a Pointer the caller prepends to the
    /// argument types.
    std::optional<PhysicalRegister> resultAreaPointer {};
};

/// Where a convention places one argument or result.
struct StackOffset
{
    uint32_t bytes = 0; ///< Its offset in the stack argument area, or in the result area.

    /// Whether both name the same place.
    [[nodiscard]] constexpr bool operator==(StackOffset const&) const noexcept = default;
};

/// Where one argument or result goes: a register, or a place in memory.
using ValueLocation = std::variant<PhysicalRegister, StackOffset>;

/// Where a convention places a call's arguments, or its results.
struct Assignment
{
    std::pmr::vector<ValueLocation> locations; ///< Per value, in order, where it goes.
    /// The bytes of memory the values take beyond the registers: for arguments the outgoing argument area
    /// (shadow space included, rounded to the stack alignment); for results the result area (0 when every
    /// result is in a register).
    uint32_t memoryBytes = 0;
};

/// The register class of a value of @p type: General for I32, I64, Ref and Pointer, Float for F32 and F64.
/// @param type The type.
/// @return Its class.
[[nodiscard]] RegisterClass registerClassOf(intermediate::Type type) noexcept;

/// Places the arguments of @p types under @p row: registers of each value's class in the row's order, as
/// its ArgumentSlots says, then the stack in stackSlotBytes slots above the shadow space. A Ref is
/// integer class.
/// @param row The convention.
/// @param types The arguments' types, in order.
/// @param memory What the assignment allocates from.
/// @return Each argument's place, and the outgoing argument area's size.
[[nodiscard]] Assignment assignArguments(ConventionRow const& row,
                                         std::span<intermediate::Type const> types,
                                         std::pmr::memory_resource& memory);

/// Places the results of @p types under @p row: the result registers of each value's class in order, and
/// once a class's registers run out, the result area in stackSlotBytes slots.
/// @param row The convention.
/// @param types The results' types, in order.
/// @param memory What the assignment allocates from.
/// @return Each result's place, and the result area's size (0 when all are in registers).
[[nodiscard]] Assignment assignResults(ConventionRow const& row,
                                       std::span<intermediate::Type const> types,
                                       std::pmr::memory_resource& memory);

/// Which convention the host of one (architecture, ABI) uses.
struct HostConventionRow
{
    Architecture architecture = Architecture::X64;        ///< The instruction set.
    OperatingSystemAbi abi = OperatingSystemAbi::SystemV; ///< The operating-system ABI.
    Convention convention = Convention::SystemV;          ///< The host's convention there.
};

/// The host convention of every (architecture, ABI) with a backend, sorted by architecture, then ABI.
/// P6 appends arm64's rows (Convention::Aapcs64 for every ABI).
inline constexpr auto HostConventionTable = std::to_array<HostConventionRow>({
    { .architecture = Architecture::X64,
      .abi = OperatingSystemAbi::SystemV,
      .convention = Convention::SystemV },
    { .architecture = Architecture::X64,
      .abi = OperatingSystemAbi::Windows,
      .convention = Convention::Windows64 },
    // macOS on x86-64 uses the System V convention; it is an ABI row of its own for its platform rules.
    { .architecture = Architecture::X64,
      .abi = OperatingSystemAbi::Apple,
      .convention = Convention::SystemV },
});

static_assert(
    [] {
        auto const key = [](HostConventionRow const& row) {
            return (static_cast<uint32_t>(row.architecture) << 8U) | static_cast<uint32_t>(row.abi);
        };
        for (auto const index: std::views::iota(std::size_t { 1 }, HostConventionTable.size()))
            if (key(HostConventionTable[index - 1]) >= key(HostConventionTable[index]))
                return false;
        return true;
    }(),
    "HostConventionTable must be sorted by (architecture, ABI), one row each");

/// Whether @p target's (architecture, ABI) has a row of HostConventionTable.
/// @param target The target.
/// @return Whether it has.
[[nodiscard]] bool hasHostConvention(TargetDescription const& target) noexcept;

/// The convention the host of @p target uses: what the runtime stubs and the entry trampoline are called
/// with. A row lookup in HostConventionTable.
/// @param target A target whose (architecture, ABI) has a row (hasHostConvention(); Require): every target
///               with a backend.
/// @return Its host convention.
[[nodiscard]] Convention hostConvention(TargetDescription const& target) noexcept;

} // namespace isoline::compiler::machine
