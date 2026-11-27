// SPDX-License-Identifier: Apache-2.0
#pragma once

// The machine layer's register vocabulary, target-independent: the register classes, a physical register as
// a class and a number within it, sets of them, and what an instruction asks of each operand. A target
// names its registers by these numbers (x86-64's Registers.hpp, P6's arm64 one); nothing here knows any
// instruction set. Data only, so that MachineFunction and the targets' tables need nothing of ITarget.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace isoline::compiler::machine
{

/// A register file: which registers a value of a type may live in.
enum class RegisterClass : uint8_t
{
    General, ///< Integer and pointer registers: I32, I64, Ref and Pointer values.
    Float,   ///< Floating-point registers: F32 and F64 values.
};

/// The last RegisterClass enumerator.
inline constexpr auto LastRegisterClass = RegisterClass::Float;

/// How many RegisterClass enumerators there are.
inline constexpr auto RegisterClassCount = static_cast<std::size_t>(LastRegisterClass) + 1;

/// The most registers one class of a target may have: what a RegisterSet holds per class.
inline constexpr auto MaximumRegistersPerClass = uint8_t { 64 };

/// A register of the machine: a class, and its number within the class as the target's register file
/// numbers it (x86-64's rax is General 0, xmm0 is Float 0).
struct PhysicalRegister
{
    RegisterClass registerClass = RegisterClass::General; ///< Its register file.
    uint8_t number = 0; ///< Its number within the file; below MaximumRegistersPerClass.

    /// Whether both name the same register.
    [[nodiscard]] constexpr bool operator==(PhysicalRegister const&) const noexcept = default;
};

/// A set of physical registers: one bit per register of each class. What a convention saves for its
/// caller, and what a frame saves in its prologue.
class RegisterSet
{
  public:
    /// The empty set.
    constexpr RegisterSet() noexcept = default;

    /// The set of @p registers.
    /// @param registers The members; repeats are harmless.
    /// @return The set.
    [[nodiscard]] static constexpr RegisterSet of(std::initializer_list<PhysicalRegister> registers) noexcept
    {
        auto set = RegisterSet {};
        for (auto const reg: registers)
            set = set.with(reg);
        return set;
    }

    /// This set and @p reg.
    /// @param reg The register to add; its number below MaximumRegistersPerClass.
    /// @return The new set.
    [[nodiscard]] constexpr RegisterSet with(PhysicalRegister reg) const noexcept
    {
        auto copy = *this;
        copy._bits[classIndex(reg.registerClass)] |= bitOf(reg.number);
        return copy;
    }

    /// Whether @p reg is in the set.
    /// @param reg The register.
    /// @return Whether it is.
    [[nodiscard]] constexpr bool contains(PhysicalRegister reg) const noexcept
    {
        return (_bits[classIndex(reg.registerClass)] & bitOf(reg.number)) != 0;
    }

    /// How many registers of @p registerClass the set holds.
    /// @param registerClass The class.
    /// @return The count.
    [[nodiscard]] constexpr uint32_t countIn(RegisterClass registerClass) const noexcept
    {
        return static_cast<uint32_t>(std::popcount(_bits[classIndex(registerClass)]));
    }

    /// Whether both hold the same registers.
    [[nodiscard]] constexpr bool operator==(RegisterSet const&) const noexcept = default;

  private:
    [[nodiscard]] static constexpr std::size_t classIndex(RegisterClass registerClass) noexcept
    {
        return static_cast<std::size_t>(registerClass);
    }

    [[nodiscard]] static constexpr uint64_t bitOf(uint8_t number) noexcept
    {
        return number < MaximumRegistersPerClass ? uint64_t { 1 } << number : 0;
    }

    std::array<uint64_t, RegisterClassCount> _bits {};
};

/// What an instruction does with a register operand, and when.
///
/// Positions are those of machine::computeLiveIntervals(): an instruction at position p reads its uses at
/// p and writes its definitions from p on, so a definition may take the register of a use that dies there.
enum class OperandRole : uint8_t
{
    Use,    ///< Read at the instruction.
    Def,    ///< Written by the instruction.
    UseDef, ///< Read and written, in one register: the target's two-address forms (a tied operand).
    Early,  ///< Written before the instruction's uses are read (early clobber): it shares no register with
            ///< them.
};

/// Whether an operand position may take a memory operand (a spill slot) in place of a register.
enum class AllowsMemory : uint8_t
{
    No,  ///< It must be a register.
    Yes, ///< A register or a memory operand.
};

/// What an instruction asks of one operand: its role, its register class, a fixed register if the
/// encoding demands one (x86-64's div in rdx:rax, a shift count in rcx), and whether a memory operand
/// will do. A target's constraints() returns one row per operand position.
struct OperandConstraint
{
    OperandRole role = OperandRole::Use;                  ///< What the instruction does with it.
    RegisterClass registerClass = RegisterClass::General; ///< The class of its register.
    std::optional<uint8_t> fixed {};        ///< The register's number within the class, when it is fixed.
    AllowsMemory memory = AllowsMemory::No; ///< Whether a memory operand will do.

    /// Whether both ask the same.
    [[nodiscard]] constexpr bool operator==(OperandConstraint const&) const noexcept = default;
};

} // namespace isoline::compiler::machine
