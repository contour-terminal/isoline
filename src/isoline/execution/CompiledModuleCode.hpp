// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory>

#include <isoline/compiler/CompiledCode.hpp>
#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/execution/CodeMemory.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/ModuleCode.hpp>

namespace isoline::execution
{

/// A module's compiled code sealed for running: what the compiled engine's instances hold as their
/// runtime::ModuleCode (ModuleInstance::code), made once per module and shared by every instance of it in any
/// store, since the code is position independent.
///
/// It keeps the CompiledCode it was sealed from, for the metadata the executor reads (offsets, adapter
/// areas), and the sealed bytes, unmapped when the last instance lets go of it.
class CompiledModuleCode final: public runtime::ModuleCode
{
  public:
    /// Seals @p code through @p memory.
    /// Precondition (checked): @p code is not null.
    /// @param code The compiled code.
    /// @param memory What seals it.
    /// @return The module's code; or why the system refused to seal it.
    [[nodiscard]] static std::expected<std::shared_ptr<CompiledModuleCode const>, CodeMemoryError> create(
        std::shared_ptr<compiler::CompiledCode const> code, ICodeMemory& memory);

    /// The compiled engine, whose executor (CompiledExecutor) runs it.
    /// @return ExecutionEngine::Compiled.
    [[nodiscard]] runtime::ExecutionEngine engine() const noexcept override;

    /// The code and its metadata.
    [[nodiscard]] compiler::CompiledCode const& compiled() const noexcept { return *_code; }

    /// The execution mode it was compiled for, which its executor's must be.
    [[nodiscard]] runtime::ExecutionMode mode() const noexcept { return _code->config.mode; }

    /// The entry trampoline, as the host calls it.
    [[nodiscard]] compiler::EntryTrampoline trampoline() const noexcept;

    /// The function of code index @p codeIndex's entry (Require: one of the module's).
    [[nodiscard]] void const* function(uint32_t codeIndex) const;

    /// The entry adapter of the function of code index @p codeIndex (Require: one of the module's).
    [[nodiscard]] void const* adapter(uint32_t codeIndex) const;

    /// What lies at @p offset of the sealed code (Require: within it).
    [[nodiscard]] void const* at(uint64_t offset) const;

    CompiledModuleCode(std::shared_ptr<compiler::CompiledCode const> code,
                       std::unique_ptr<ISealedCode> sealed);

  private:
    std::shared_ptr<compiler::CompiledCode const> _code;
    std::unique_ptr<ISealedCode> _sealed;
};

} // namespace isoline::execution
