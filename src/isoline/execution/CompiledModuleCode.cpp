// SPDX-License-Identifier: Apache-2.0
#include <isoline/execution/CompiledModuleCode.hpp>

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace isoline::execution
{

std::expected<std::shared_ptr<CompiledModuleCode const>, CodeMemoryError> CompiledModuleCode::create(
    std::shared_ptr<compiler::CompiledCode const> code, ICodeMemory& memory)
{
    Require(code != nullptr);
    auto sealed = memory.seal(code->code);
    if (!sealed)
        return std::unexpected { sealed.error() };
    return std::make_shared<CompiledModuleCode const>(std::move(code), std::move(*sealed));
}

CompiledModuleCode::CompiledModuleCode(std::shared_ptr<compiler::CompiledCode const> code,
                                       std::unique_ptr<ISealedCode> sealed):
    _code { std::move(code) }, _sealed { std::move(sealed) }
{
    Require(_code != nullptr && _sealed != nullptr);
}

runtime::ExecutionEngine CompiledModuleCode::engine() const noexcept
{
    return runtime::ExecutionEngine::Compiled;
}

compiler::EntryTrampoline CompiledModuleCode::trampoline() const noexcept
{
    return reinterpret_cast<compiler::EntryTrampoline>(
        reinterpret_cast<std::uintptr_t>(at(_code->trampolineOffset)));
}

void const* CompiledModuleCode::function(uint32_t codeIndex) const
{
    Require(codeIndex < _code->functionOffsets.size());
    return at(_code->functionOffsets[codeIndex]);
}

void const* CompiledModuleCode::adapter(uint32_t codeIndex) const
{
    Require(codeIndex < _code->adapterOffsets.size());
    return at(_code->adapterOffsets[codeIndex]);
}

void const* CompiledModuleCode::at(uint64_t offset) const
{
    auto const code = _sealed->code();
    Require(offset < code.size());
    return code.subspan(static_cast<std::size_t>(offset)).data();
}

} // namespace isoline::execution
