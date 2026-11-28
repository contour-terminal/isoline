// SPDX-License-Identifier: Apache-2.0
#pragma once

// x86-64's register file and its calling conventions, as rows. A register is the machine layer's
// PhysicalRegister numbered as the hardware encodes it (rax is General 0, r15 General 15, xmm0 Float 0), so
// that the encoder writes the number it is given into ModRM, SIB and REX. RegisterTable names each register
// and the role it has in generated code: the allocator may assign it, or it is pinned (the stack pointer,
// the frame pointer, r15 = ExecutionState*, r14 = InstanceContext*; P5 Decision "Pinned registers"). The
// three ConventionRows are the internal convention between compiled Wasm functions and the two host
// conventions (System V AMD64 and Microsoft x64), which the runtime stubs and the entry trampoline use.

#include <core/Assert.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/compiler/machine/Registers.hpp>

namespace isoline::compiler::x86_64
{

/// A general-purpose register, by its hardware number.
enum class Gpr : uint8_t
{
    Rax,
    Rcx,
    Rdx,
    Rbx,
    Rsp,
    Rbp,
    Rsi,
    Rdi,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
};

/// An SSE register, by its hardware number.
enum class Xmm : uint8_t
{
    Xmm0,
    Xmm1,
    Xmm2,
    Xmm3,
    Xmm4,
    Xmm5,
    Xmm6,
    Xmm7,
    Xmm8,
    Xmm9,
    Xmm10,
    Xmm11,
    Xmm12,
    Xmm13,
    Xmm14,
    Xmm15,
};

/// How many registers each of x86-64's files has (without APX).
inline constexpr auto RegistersPerFile = std::size_t { 16 };

/// @p reg as the machine layer's register: General, its hardware number.
[[nodiscard]] constexpr machine::PhysicalRegister physical(Gpr reg) noexcept
{
    return { .registerClass = machine::RegisterClass::General, .number = static_cast<uint8_t>(reg) };
}

/// @p reg as the machine layer's register: Float, its hardware number.
[[nodiscard]] constexpr machine::PhysicalRegister physical(Xmm reg) noexcept
{
    return { .registerClass = machine::RegisterClass::Float, .number = static_cast<uint8_t>(reg) };
}

/// What a register is for in generated code.
enum class RegisterRole : uint8_t
{
    Allocatable,     ///< The register allocator may assign it.
    StackPointer,    ///< rsp.
    FramePointer,    ///< rbp: kept as a frame pointer for unwinding and profilers (P7).
    ExecutionState,  ///< r15: the ExecutionState* of the invocation.
    InstanceContext, ///< r14: the InstanceContext* of the running function's instance.
};

/// One register of x86-64: the register, its name (the 64-bit one for a General register), its role.
struct RegisterRow
{
    machine::PhysicalRegister reg; ///< The register.
    std::string_view name;         ///< Its name, as Intel syntax spells it.
    RegisterRole role;             ///< What generated code uses it for.
};

/// Every register of x86-64: the General ones by number, then the Float ones by number.
inline constexpr auto RegisterTable = std::to_array<RegisterRow>({
    { .reg = physical(Gpr::Rax), .name = "rax", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::Rcx), .name = "rcx", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::Rdx), .name = "rdx", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::Rbx), .name = "rbx", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::Rsp), .name = "rsp", .role = RegisterRole::StackPointer },
    { .reg = physical(Gpr::Rbp), .name = "rbp", .role = RegisterRole::FramePointer },
    { .reg = physical(Gpr::Rsi), .name = "rsi", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::Rdi), .name = "rdi", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::R8), .name = "r8", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::R9), .name = "r9", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::R10), .name = "r10", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::R11), .name = "r11", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::R12), .name = "r12", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::R13), .name = "r13", .role = RegisterRole::Allocatable },
    { .reg = physical(Gpr::R14), .name = "r14", .role = RegisterRole::InstanceContext },
    { .reg = physical(Gpr::R15), .name = "r15", .role = RegisterRole::ExecutionState },
    { .reg = physical(Xmm::Xmm0), .name = "xmm0", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm1), .name = "xmm1", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm2), .name = "xmm2", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm3), .name = "xmm3", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm4), .name = "xmm4", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm5), .name = "xmm5", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm6), .name = "xmm6", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm7), .name = "xmm7", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm8), .name = "xmm8", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm9), .name = "xmm9", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm10), .name = "xmm10", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm11), .name = "xmm11", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm12), .name = "xmm12", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm13), .name = "xmm13", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm14), .name = "xmm14", .role = RegisterRole::Allocatable },
    { .reg = physical(Xmm::Xmm15), .name = "xmm15", .role = RegisterRole::Allocatable },
});

static_assert(
    [] {
        for (auto const index: std::views::iota(std::size_t { 0 }, RegisterTable.size()))
        {
            auto const& row = RegisterTable[index];
            auto const registerClass =
                index < RegistersPerFile ? machine::RegisterClass::General : machine::RegisterClass::Float;
            if (row.reg.registerClass != registerClass || row.reg.number != index % RegistersPerFile
                || row.name.empty())
                return false;
        }
        return RegisterTable.size() == 2 * RegistersPerFile;
    }(),
    "RegisterTable must hold every General register by number, then every Float register by number");

/// The row of @p reg.
/// @param reg A register of x86-64 (number below RegistersPerFile; Require).
[[nodiscard]] constexpr RegisterRow const& registerRow(machine::PhysicalRegister reg) noexcept
{
    Require(reg.number < RegistersPerFile);
    auto const offset = reg.registerClass == machine::RegisterClass::General ? 0 : RegistersPerFile;
    return RegisterTable[offset + reg.number];
}

/// The General registers the allocator assigns, in its order of preference: the ones every convention lets
/// a call clobber first, so that a short-lived value costs no prologue save, then the ones the internal
/// convention preserves. Exactly the Allocatable rows of RegisterTable.
inline constexpr auto AllocatableGeneral = std::to_array<machine::PhysicalRegister>({
    physical(Gpr::Rax),
    physical(Gpr::Rcx),
    physical(Gpr::Rdx),
    physical(Gpr::Rsi),
    physical(Gpr::Rdi),
    physical(Gpr::R8),
    physical(Gpr::R9),
    physical(Gpr::R10),
    physical(Gpr::R11),
    physical(Gpr::Rbx),
    physical(Gpr::R12),
    physical(Gpr::R13),
});

/// The Float registers the allocator assigns, in its order of preference: all sixteen, by number.
inline constexpr auto AllocatableFloat = std::to_array<machine::PhysicalRegister>({
    physical(Xmm::Xmm0),
    physical(Xmm::Xmm1),
    physical(Xmm::Xmm2),
    physical(Xmm::Xmm3),
    physical(Xmm::Xmm4),
    physical(Xmm::Xmm5),
    physical(Xmm::Xmm6),
    physical(Xmm::Xmm7),
    physical(Xmm::Xmm8),
    physical(Xmm::Xmm9),
    physical(Xmm::Xmm10),
    physical(Xmm::Xmm11),
    physical(Xmm::Xmm12),
    physical(Xmm::Xmm13),
    physical(Xmm::Xmm14),
    physical(Xmm::Xmm15),
});

/// Whether @p order lists exactly the Allocatable registers of @p registerClass in RegisterTable, each once.
[[nodiscard]] constexpr bool isAllocationOrderOf(std::span<machine::PhysicalRegister const> order,
                                                 machine::RegisterClass registerClass) noexcept
{
    auto listed = machine::RegisterSet {};
    for (auto const reg: order)
    {
        if (reg.registerClass != registerClass || reg.number >= RegistersPerFile || listed.contains(reg)
            || registerRow(reg).role != RegisterRole::Allocatable)
            return false;
        listed = listed.with(reg);
    }
    auto allocatable = std::size_t { 0 };
    for (auto const& row: RegisterTable)
        if (row.reg.registerClass == registerClass && row.role == RegisterRole::Allocatable)
            ++allocatable;
    return allocatable == order.size();
}

static_assert(isAllocationOrderOf(AllocatableGeneral, machine::RegisterClass::General)
                  && AllocatableGeneral.size() == 12,
              "AllocatableGeneral must list the 12 Allocatable General registers, each once");
static_assert(isAllocationOrderOf(AllocatableFloat, machine::RegisterClass::Float)
                  && AllocatableFloat.size() == 16,
              "AllocatableFloat must list the 16 Allocatable Float registers, each once");

/// The integer argument registers of the internal and the System V convention: rdi, rsi, rdx, rcx, r8, r9.
inline constexpr auto SystemVIntegerArguments = std::to_array<machine::PhysicalRegister>({
    physical(Gpr::Rdi),
    physical(Gpr::Rsi),
    physical(Gpr::Rdx),
    physical(Gpr::Rcx),
    physical(Gpr::R8),
    physical(Gpr::R9),
});

/// The float argument registers of the internal and the System V convention: xmm0–xmm7.
inline constexpr auto SystemVFloatArguments = std::to_array<machine::PhysicalRegister>({
    physical(Xmm::Xmm0),
    physical(Xmm::Xmm1),
    physical(Xmm::Xmm2),
    physical(Xmm::Xmm3),
    physical(Xmm::Xmm4),
    physical(Xmm::Xmm5),
    physical(Xmm::Xmm6),
    physical(Xmm::Xmm7),
});

/// The integer result registers of the internal and the System V convention: rax, rdx.
inline constexpr auto SystemVIntegerResults = std::to_array<machine::PhysicalRegister>({
    physical(Gpr::Rax),
    physical(Gpr::Rdx),
});

/// The float result registers of the internal and the System V convention: xmm0, xmm1.
inline constexpr auto SystemVFloatResults = std::to_array<machine::PhysicalRegister>({
    physical(Xmm::Xmm0),
    physical(Xmm::Xmm1),
});

/// The integer argument registers of the Microsoft x64 convention: rcx, rdx, r8, r9.
inline constexpr auto Windows64IntegerArguments = std::to_array<machine::PhysicalRegister>({
    physical(Gpr::Rcx),
    physical(Gpr::Rdx),
    physical(Gpr::R8),
    physical(Gpr::R9),
});

/// The float argument registers of the Microsoft x64 convention: xmm0–xmm3.
inline constexpr auto Windows64FloatArguments = std::to_array<machine::PhysicalRegister>({
    physical(Xmm::Xmm0),
    physical(Xmm::Xmm1),
    physical(Xmm::Xmm2),
    physical(Xmm::Xmm3),
});

/// The integer result register of the Microsoft x64 convention: rax.
inline constexpr auto Windows64IntegerResults =
    std::to_array<machine::PhysicalRegister>({ physical(Gpr::Rax) });

/// The float result register of the Microsoft x64 convention: xmm0.
inline constexpr auto Windows64FloatResults =
    std::to_array<machine::PhysicalRegister>({ physical(Xmm::Xmm0) });

/// What System V AMD64 preserves across a call: rbx, rbp and r12–r15.
inline constexpr auto SystemVCalleeSaved = machine::RegisterSet::of({
    physical(Gpr::Rbx),
    physical(Gpr::Rbp),
    physical(Gpr::R12),
    physical(Gpr::R13),
    physical(Gpr::R14),
    physical(Gpr::R15),
});

/// The internal convention between compiled Wasm functions (P5 Decision "Calling conventions are rows"):
/// System V's argument and result registers, value by value, the results past rax, rdx / xmm0, xmm1 in the
/// caller's area, whose pointer is a hidden first argument. It preserves what System V does, the pinned r14
/// and r15 among them, so that a callee keeps the caller's context registers and a value may live across a
/// call in rbx, r12 or r13.
inline constexpr auto WasmInternalConvention = machine::ConventionRow {
    .convention = machine::Convention::WasmInternal,
    .integerArguments = SystemVIntegerArguments,
    .floatArguments = SystemVFloatArguments,
    .integerResults = SystemVIntegerResults,
    .floatResults = SystemVFloatResults,
    .calleeSaved = SystemVCalleeSaved,
    .slots = machine::ArgumentSlots::Independent,
    .shadowSpaceBytes = 0,
    .stackSlotBytes = 8,
    .stackAlignment = 16,
    .aggregateResults = machine::AggregateResults::SplitAcrossRegisters,
    .aggregateRegisterBytes = 0,
    .resultAreaPointer = std::nullopt,
    .pointerReturned = machine::PointerReturned::No,
};

/// The System V AMD64 convention (Linux, the BSDs, macOS): 16 bytes of results in registers, a larger
/// aggregate through a hidden pointer in rdi, given back in rax.
inline constexpr auto SystemVConvention = machine::ConventionRow {
    .convention = machine::Convention::SystemV,
    .integerArguments = SystemVIntegerArguments,
    .floatArguments = SystemVFloatArguments,
    .integerResults = SystemVIntegerResults,
    .floatResults = SystemVFloatResults,
    .calleeSaved = SystemVCalleeSaved,
    .slots = machine::ArgumentSlots::Independent,
    .shadowSpaceBytes = 0,
    .stackSlotBytes = 8,
    .stackAlignment = 16,
    .aggregateResults = machine::AggregateResults::HiddenPointerBeyond,
    .aggregateRegisterBytes = 16,
    .resultAreaPointer = std::nullopt,
    .pointerReturned = machine::PointerReturned::Yes,
};

/// The Microsoft x64 convention (Windows): shared argument positions, 32 bytes of shadow space, rbx, rbp,
/// rdi, rsi, r12–r15 and xmm6–xmm15 preserved; 8 bytes of results in rax, a larger aggregate through a
/// hidden pointer in rcx, given back in rax.
inline constexpr auto Windows64Convention = machine::ConventionRow {
    .convention = machine::Convention::Windows64,
    .integerArguments = Windows64IntegerArguments,
    .floatArguments = Windows64FloatArguments,
    .integerResults = Windows64IntegerResults,
    .floatResults = Windows64FloatResults,
    .calleeSaved = machine::RegisterSet::of({
        physical(Gpr::Rbx),
        physical(Gpr::Rbp),
        physical(Gpr::Rdi),
        physical(Gpr::Rsi),
        physical(Gpr::R12),
        physical(Gpr::R13),
        physical(Gpr::R14),
        physical(Gpr::R15),
        physical(Xmm::Xmm6),
        physical(Xmm::Xmm7),
        physical(Xmm::Xmm8),
        physical(Xmm::Xmm9),
        physical(Xmm::Xmm10),
        physical(Xmm::Xmm11),
        physical(Xmm::Xmm12),
        physical(Xmm::Xmm13),
        physical(Xmm::Xmm14),
        physical(Xmm::Xmm15),
    }),
    .slots = machine::ArgumentSlots::Shared,
    .shadowSpaceBytes = 32,
    .stackSlotBytes = 8,
    .stackAlignment = 16,
    .aggregateResults = machine::AggregateResults::HiddenPointerBeyond,
    .aggregateRegisterBytes = 8,
    .resultAreaPointer = std::nullopt,
    .pointerReturned = machine::PointerReturned::Yes,
};

/// x86-64's conventions, one row per machine::Convention, in enumerator order.
inline constexpr auto ConventionTable = std::to_array<machine::ConventionRow const*>({
    &WasmInternalConvention,
    &SystemVConvention,
    &Windows64Convention,
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const* row: ConventionTable)
        {
            if (static_cast<std::size_t>(row->convention) != index)
                return false;
            // Every convention preserves the pinned registers: a call never changes a context register.
            for (auto const& reg: RegisterTable)
                if (reg.role != RegisterRole::Allocatable && reg.role != RegisterRole::StackPointer
                    && !row->calleeSaved.contains(reg.reg))
                    return false;
            ++index;
        }
        return index == static_cast<std::size_t>(machine::LastConvention) + 1;
    }(),
    "ConventionTable must hold one row per Convention, at the index of its enumerator, each preserving the "
    "pinned registers");

/// x86-64's row of @p convention.
[[nodiscard]] constexpr machine::ConventionRow const& conventionRow(machine::Convention convention) noexcept
{
    return *ConventionTable[static_cast<std::size_t>(convention)];
}

/// x86-64's frame rule (P5 Decision "Native frames are bounded by a published per-target rule"): per Wasm
/// stack value at most 8 B of spill, 8 B of outgoing argument, 8 B of result area and 8 B of slack; 256 B for
/// the saved registers, alignment, the return address, rbp and the three context values.
inline constexpr auto X64FrameRule = machine::FrameRule { .bytesPerStackValue = 32, .frameOverhead = 256 };

/// x86-64's stack rule: 16-byte alignment at every call, an 8-byte return address, a General register saved
/// in 8 bytes and a Float one in 16 (Microsoft x64 preserves xmm6–xmm15 whole), and the prologue's
/// `push rbp`.
inline constexpr auto X64StackRule = machine::StackRule {
    .alignment = 16, .returnAddressBytes = 8, .savedRegisterBytes = { 8, 16 }, .framePointerBytes = 8
};

/// Whether @p T may cross the host-call boundary as a parameter (P5 Decision "Calling conventions are rows",
/// amendment I8): a 32- or 64-bit integer, or a pointer.
template <typename T>
concept HostBoundaryArgument = std::is_pointer_v<T> || std::same_as<T, uint32_t> || std::same_as<T, int32_t>
                               || std::same_as<T, uint64_t> || std::same_as<T, int64_t>;

namespace detail
{
    /// Whether @p T is an aggregate initialized from exactly sizeof...(Words) uint64_t words without a
    /// narrowing conversion: a struct whose members are all uint64_t (a double, a narrower integer or a
    /// pointer member makes the braced initialization ill-formed).
    template <typename T, std::size_t... Words>
    constexpr bool isMadeOfWords(std::index_sequence<Words...> /*words*/) noexcept
    {
        return requires(uint64_t word) { T { (static_cast<void>(Words), word)... }; };
    }
} // namespace detail

/// Whether @p T may be what a runtime stub or the trampoline returns: a 32- or 64-bit integer, a pointer, or
/// a trivially copyable aggregate of uint64_t words (so AggregateResults::HiddenPointerBeyond describes where
/// it goes). A floating-point value or a struct holding one is no such word: Microsoft x64 returns a struct
/// of one double in rax, a scalar double in xmm0.
template <typename T>
concept HostBoundaryResult =
    HostBoundaryArgument<T>
    || (std::is_class_v<T> && std::is_aggregate_v<T> && std::is_trivially_copyable_v<T> && sizeof(T) % 8 == 0
        && detail::isMadeOfWords<T>(std::make_index_sequence<sizeof(T) / 8> {}));

namespace detail
{
    template <typename Function>
    struct HostBoundaryFunctionTraits
    {
        static constexpr bool Allowed = false;
    };

    template <typename Result, typename... Parameters>
    struct HostBoundaryFunctionTraits<Result (*)(Parameters...) noexcept>
    {
        static constexpr bool Allowed =
            HostBoundaryResult<Result> && (HostBoundaryArgument<Parameters> && ...);
    };

    template <typename Result, typename... Parameters>
    struct HostBoundaryFunctionTraits<Result (*)(Parameters...)>
    {
        static constexpr bool Allowed =
            HostBoundaryResult<Result> && (HostBoundaryArgument<Parameters> && ...);
    };
} // namespace detail

/// Whether @p Function, the type of a pointer to a runtime stub or the trampoline's host-facing entry, keeps
/// the host-call boundary's rule (ledger carry to T11): a non-member function, so that no `this` shifts its
/// hidden result pointer (Microsoft x64 member functions take it in rdx), taking only HostBoundaryArgument
/// parameters and returning a HostBoundaryResult. The stub table static_asserts it of each stub (P5 Task 14).
template <typename Function>
concept HostBoundaryFunction = detail::HostBoundaryFunctionTraits<Function>::Allowed;

} // namespace isoline::compiler::x86_64
