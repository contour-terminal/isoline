// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>

namespace isoline::compiler::optimization
{

PassOutcome eliminateBoundsChecks([[maybe_unused]] intermediate::Function& function,
                                  [[maybe_unused]] PassContext& context)
{
    // P5 Task 8: the two rules of the plan's Decisions. Until then every check stays.
    return {};
}

} // namespace isoline::compiler::optimization
