// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/CompileError.hpp>

#include <format>
#include <iterator>
#include <string>
#include <utility>

namespace isoline::compiler
{

std::string describe(CompileError const& error)
{
    auto text = std::string { compileErrorRow(error.code).text };
    if (error.feature)
        std::format_to(std::back_inserter(text), ": {}", featureRow(*error.feature).name);
    if (error.function)
        std::format_to(std::back_inserter(text), " in function {}", *error.function);
    if (error.requested != 0 || error.limit != 0)
        std::format_to(std::back_inserter(text), " ({} requested, limit {})", error.requested, error.limit);
    if (!error.pass.empty())
        std::format_to(std::back_inserter(text), " after pass {}", error.pass);
    if (error.rule)
        std::format_to(std::back_inserter(text),
                       ": {}",
                       intermediate::VerifierErrorTable[std::to_underlying(*error.rule)].text);
    return text;
}

} // namespace isoline::compiler
