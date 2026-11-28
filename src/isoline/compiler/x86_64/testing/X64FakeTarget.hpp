// SPDX-License-Identifier: Apache-2.0
#pragma once

// The fake opcodes (machine/testing/FakeTarget.hpp) on x86-64's real register file: its twelve allocatable
// General and sixteen Float registers in the allocator's order, its internal convention, a host convention
// and its stack rule. The register allocator's tests run on it beside FakeTarget's two registers per class,
// so that the allocator is checked against the file it will allocate for, before the x86-64 selector exists.

#include <isoline/compiler/machine/testing/FakeTarget.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>

namespace isoline::compiler::x86_64::testing
{

/// x86-64's register file under a System V host: what Linux, the BSDs and macOS run.
inline constexpr auto X64RegisterFile = machine::testing::FakeRegisterFile {
    .general = AllocatableGeneral,
    .floats = AllocatableFloat,
    .internal = &WasmInternalConvention,
    .savingInternal = &WasmInternalConvention,
    .host = &SystemVConvention,
    .stack = X64StackRule,
    .abi = machine::OperatingSystemAbi::SystemV,
};

/// x86-64's register file under a Microsoft x64 host: 32 bytes of shadow space, xmm6–xmm15 preserved.
inline constexpr auto X64WindowsRegisterFile = machine::testing::FakeRegisterFile {
    .general = AllocatableGeneral,
    .floats = AllocatableFloat,
    .internal = &WasmInternalConvention,
    .savingInternal = &WasmInternalConvention,
    .host = &Windows64Convention,
    .stack = X64StackRule,
    .abi = machine::OperatingSystemAbi::Windows,
};

static_assert(machine::testing::FakeConstraints<X64RegisterFile>::isComplete()
                  && machine::testing::FakeConstraints<X64WindowsRegisterFile>::isComplete(),
              "the fake opcodes' rows must be complete on x86-64's register file");

/// The fake opcodes on x86-64's register file, under a System V host.
using X64FakeTarget = machine::testing::BasicFakeTarget<X64RegisterFile>;

/// The fake opcodes on x86-64's register file, under a Microsoft x64 host.
using X64WindowsFakeTarget = machine::testing::BasicFakeTarget<X64WindowsRegisterFile>;

} // namespace isoline::compiler::x86_64::testing
