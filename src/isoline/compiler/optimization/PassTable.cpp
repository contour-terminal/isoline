// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/optimization/PassTable.hpp>

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>
#include <string_view>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::compiler::optimization
{

namespace
{
    /// Verifies @p function, naming @p pass in the error when it is ill-formed.
    [[nodiscard]] PassOutcome verified(intermediate::Function const& function,
                                       PassContext const& context,
                                       std::string_view pass)
    {
        return intermediate::verify(function, context.memoryMinimumBytes)
            .transform_error([&](intermediate::VerifierError const& error) {
                return CompileError { .code = CompileErrorCode::InternalVerificationFailed,
                                      .function = context.codeIndex,
                                      .pass = pass,
                                      .rule = error.code };
            });
    }
} // namespace

PassOutcome runPasses(intermediate::Function& function, PassContext& context, std::span<PassRow const> passes)
{
    for (auto const& row: passes)
    {
        if (context.cancellation.state() == CancellationState::Cancelled)
            return std::unexpected { CompileError { .code = CompileErrorCode::Cancelled,
                                                    .function = context.codeIndex } };
        if (auto ran = row.run(function, context); !ran)
            return ran;
        if (context.verification == Verification::AfterEachPass)
            if (auto checked = verified(function, context, row.name); !checked)
                return checked;
    }
    if (context.verification == Verification::AtEnd)
        if (auto checked = verified(function, context, "end"); !checked)
            return checked;
    function.compact();
    return {};
}

std::pmr::vector<uint64_t> memoryMinimumBytesOf(validate::ValidatedModule const& module,
                                                std::pmr::memory_resource& memory)
{
    auto const& binary = module.module();
    auto minimums = std::pmr::vector<uint64_t> { &memory };
    auto const add = [&](binary::MemoryType const& type) {
        minimums.push_back(intermediate::minimumBytesOf(type.limits.min, type.pageSizeLog2));
    };
    for (auto const& import: binary.imports)
        if (import.kind == binary::ExternKind::Memory)
            add(import.memory);
    for (auto const& type: binary.memories)
        add(type);
    return minimums;
}

PassOutcome runPasses(intermediate::Function& function, PassContext& context)
{
    return runPasses(function, context, PassTable);
}

} // namespace isoline::compiler::optimization
