// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <isoline/compiler/CompileConfig.hpp>

namespace isoline::compiler
{

/// A module's compiled code: the bytes Compiler::compile() generated and the metadata that runs them, made
/// once per module and shared by every instance of it in any store.
///
/// It holds no pointer and no absolute address: the code is position independent (calls within it are
/// rel32, jump tables RIP-relative, runtime addresses come through ExecutionState and InstanceContext), so
/// the same bytes run wherever they are sealed, and P8 serializes the struct as it is. Its layout, in code
/// order: a pad no entry lies in, every function of the code section, the entry trampoline with its trap
/// exit, then one entry adapter per function. Equal input and configuration give an equal CompiledCode, byte
/// for byte (Compiler.isDeterministic).
struct CompiledCode
{
    CompileConfig config;                  ///< How it was compiled: its target, mode, guard and caps.
    std::vector<std::byte> code;           ///< The code.
    std::vector<uint32_t> functionOffsets; ///< Per code index, its function's offset into code.
    std::vector<uint32_t> adapterOffsets;  ///< Per code index, its entry adapter's offset into code.
    uint32_t trampolineOffset = 0;         ///< The entry trampoline's offset into code.
    std::vector<uint64_t> frameCosts;      ///< Per code index, its frame's cost in stack values (charged).
    /// Per imported function, in import order, the host-call scratch cells a call of it takes:
    /// max(params, results), the RawValue cells a CallHost of it marshals through
    /// (ExecutionState::hostScratch).
    std::vector<uint32_t> hostScratchValues;
    /// Per code index, the stack its entry adapter takes beyond the published entry overhead
    /// (machine::ITarget::entryAdapterAreaBytes()): what an entry into it checks against the native stack
    /// limit before it calls the trampoline.
    std::vector<uint64_t> adapterAreaBytes;

    /// Whether both are the same code with the same metadata.
    [[nodiscard]] bool operator==(CompiledCode const&) const = default;
};

} // namespace isoline::compiler
