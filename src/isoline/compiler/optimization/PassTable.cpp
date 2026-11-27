// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/optimization/PassTable.hpp>

#include <core/Assert.hpp>

#include <expected>
#include <format>
#include <iostream>
#include <span>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>

namespace isoline::compiler::optimization
{

PassOutcome runPasses(intermediate::Function& function, PassContext& context, std::span<PassRow const> passes)
{
    for (auto const& row: passes)
    {
        if (context.cancellation.state() == CancellationState::Cancelled)
            return std::unexpected { CompileError { .code = CompileErrorCode::Cancelled,
                                                    .function = context.codeIndex } };
        if (auto ran = row.run(function, context); !ran)
            return ran;
        if (context.verify == VerifyAfterEachPass::No)
            continue;
        auto const verified = intermediate::verify(function);
        if (!verified)
            std::cerr << std::format("pass {} left function {} ill-formed: {}\n",
                                     row.name,
                                     context.codeIndex,
                                     describe(verified.error()));
        Require(verified.has_value());
    }
    function.compact();
    return {};
}

PassOutcome runPasses(intermediate::Function& function, PassContext& context)
{
    return runPasses(function, context, PassTable);
}

} // namespace isoline::compiler::optimization
