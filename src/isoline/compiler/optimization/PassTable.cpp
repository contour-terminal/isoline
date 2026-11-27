// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/optimization/PassTable.hpp>

#include <expected>
#include <span>
#include <string_view>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>

namespace isoline::compiler::optimization
{

namespace
{
    /// Verifies @p function, naming @p pass in the error when it is ill-formed.
    [[nodiscard]] PassOutcome verified(intermediate::Function const& function,
                                       PassContext const& context,
                                       std::string_view pass)
    {
        return intermediate::verify(function).transform_error([&](intermediate::VerifierError const& error) {
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

PassOutcome runPasses(intermediate::Function& function, PassContext& context)
{
    return runPasses(function, context, PassTable);
}

} // namespace isoline::compiler::optimization
