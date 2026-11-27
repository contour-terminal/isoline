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

/// How a convention returns a call's results.
enum class AggregateResults : uint8_t
{
    /// Value by value: each takes the next result register of its class, and once a class's run out, the
    /// next slot of the caller's result area (Isoline's WasmInternal conventions).
    SplitAcrossRegisters,
    /// As one aggregate of stackSlotBytes words (the host-call boundary passes only 32- and 64-bit integers
    /// and pointers, P5 Decision "Calling conventions are rows"): in the result registers, each value in the
    /// next of its class, when the aggregate takes at most aggregateRegisterBytes; otherwise wholly in
    /// memory, through a pointer to the caller's area passed in resultAreaPointer, or as a hidden first
    /// argument when the row has none (which shifts the arguments by one), and given back in the first
    /// integer result register when pointerReturned says so. System V AMD64: 16 bytes, rax and rdx or xmm0
    /// and xmm1, the pointer in rdi and back in rax. Microsoft x64: 8 bytes in rax, whatever the member's
    /// type (a struct of one double returns in rax, not xmm0; only a scalar double returns in xmm0), the
    /// pointer in rcx and back in rax. AAPCS64: 16 bytes, the pointer in x8, not given back.
    ///
    /// The rule holds for what the host-call boundary uses (P5 amendment I8): non-member functions returning
    /// a trivially copyable struct of 64-bit integer words, or one such word. It does not describe Microsoft
    /// x64's member functions, which return any aggregate through a hidden pointer passed in rdx after
    /// `this`, nor any non-trivially-copyable type, which every ABI returns through the hidden pointer; so no
    /// stub is a member function or returns one. As the boundary returns integer words only, a host row's
    /// float result registers are never used for an aggregate.
    HiddenPointerBeyond,
};

/// Whether a callee gives the hidden result pointer back.
enum class PointerReturned : uint8_t
{
    No,  ///< It does not (AAPCS64's x8).
    Yes, ///< In the first integer result register (System V and Microsoft x64's rax).
};

/// One calling convention of a target. Every field is required: a row names each, so that no convention
/// inherits a value meant for another.
struct ConventionRow
{
    Convention convention;                              ///< Which convention this is.
    std::span<PhysicalRegister const> integerArguments; ///< General-class argument registers, in order.
    std::span<PhysicalRegister const> floatArguments;   ///< Float-class argument registers, in order.
    std::span<PhysicalRegister const> integerResults;   ///< General-class result registers, in order.
    std::span<PhysicalRegister const> floatResults;     ///< Float-class result registers, in order.
    /// The registers a callee preserves; every other one a call clobbers. A set names registers, not widths:
    /// how many bytes a save takes is the target's (Microsoft x64 preserves xmm6–15 whole, 128 bits; AAPCS64
    /// only the low 64 bits of v8–v15, so a V128 value lives across no call in one, P5b's F9).
    RegisterSet calleeSaved;
    ArgumentSlots slots;       ///< How the classes share argument registers.
    uint32_t shadowSpaceBytes; ///< Stack the caller reserves at the bottom of its frame for the callee.
    uint32_t stackSlotBytes;   ///< The bytes one stack argument or result-area value takes.
    /// The stack pointer's alignment wherever the convention requires it: at every call on x86-64 (the frame
    /// may be misaligned between calls), at every instruction boundary on arm64, where the hardware checks
    /// sp. A frame keeps it by its size (checkFrame()), since every call leaves from the frame's bottom.
    uint32_t stackAlignment;
    AggregateResults aggregateResults; ///< How the results are returned.
    /// HiddenPointerBeyond: the most bytes of results returned in registers; unused otherwise.
    uint32_t aggregateRegisterBytes;
    /// Where the pointer to the result area is passed (arm64's x8); none: as a hidden first argument.
    std::optional<PhysicalRegister> resultAreaPointer;
    PointerReturned pointerReturned; ///< HiddenPointerBeyond: whether the callee gives the pointer back.
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

/// Where a call's results go beyond registers.
enum class ResultArea : uint8_t
{
    None,          ///< Every result is in a register.
    CallerArea,    ///< SplitAcrossRegisters: the results past the registers, in the caller's area.
    HiddenPointer, ///< HiddenPointerBeyond: every result, in memory through the hidden pointer.
};

/// Where a convention places a call's arguments, or its results.
struct Assignment
{
    std::pmr::vector<ValueLocation> locations; ///< Per value, in order, where it goes.
    /// The bytes of memory the values take beyond the registers: for arguments the outgoing argument area
    /// (shadow space included, rounded to the stack alignment); for results the result area (0 when every
    /// result is in a register).
    uint32_t memoryBytes = 0;
    ResultArea area = ResultArea::None; ///< For results: where they go beyond registers.
};

/// Where a convention places a whole call: its results, and its arguments with the hidden result pointer,
/// when there is one, as the first.
struct CallAssignment
{
    Assignment arguments; ///< The arguments; the hidden pointer's location first when the results take one.
    Assignment results;   ///< The results.
    /// Where the pointer to the result area is passed, when the results go through one (CallerArea or
    /// HiddenPointer): the row's resultAreaPointer, or the first argument's place.
    std::optional<ValueLocation> resultPointer;
    /// Where the callee gives the hidden pointer back, when it does.
    std::optional<PhysicalRegister> returnedPointer;
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

/// Places the results of @p types under @p row, as its AggregateResults says.
/// @param row The convention.
/// @param types The results' types, in order.
/// @param memory What the assignment allocates from.
/// @return Each result's place, the result area's size (0 when all are in registers) and its kind.
[[nodiscard]] Assignment assignResults(ConventionRow const& row,
                                       std::span<intermediate::Type const> types,
                                       std::pmr::memory_resource& memory);

/// Places a whole call under @p row: its results, then its arguments, the hidden result pointer first when
/// the results go through one and the row passes it as an argument (Microsoft x64 shifts the arguments by
/// one; System V by one integer register; a row with resultAreaPointer shifts nothing).
/// @param row The convention.
/// @param parameters The arguments' types, in order, without the hidden pointer.
/// @param results The results' types, in order.
/// @param memory What the assignment allocates from.
[[nodiscard]] CallAssignment assignCall(ConventionRow const& row,
                                        std::span<intermediate::Type const> parameters,
                                        std::span<intermediate::Type const> results,
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
