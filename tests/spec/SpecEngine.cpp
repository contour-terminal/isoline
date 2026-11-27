// SPDX-License-Identifier: Apache-2.0
#include "SpecEngine.hpp"

#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include "SpecFeatures.hpp"
#include <isoline/api/ExecutionEngines.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>

namespace isoline::spectest
{

std::expected<RunOptions, std::string> parseOptions(std::span<char const* const> arguments)
{
    auto features = std::optional<FeatureSet> {};
    auto const* row = static_cast<api::engines::EngineRow const*>(nullptr);
    auto dispatch = std::optional<fastinterpreter::Dispatch> {};
    if (arguments.size() % 2 != 0)
        return std::unexpected { std::string { "every option takes one value" } };
    for (auto const pair: std::views::iota(std::size_t { 0 }, arguments.size() / 2))
    {
        auto const name = std::string_view { arguments[2 * pair] };
        auto const value = std::string_view { arguments[(2 * pair) + 1] };
        auto const repeated = std::format("{} is given twice", name);
        if (name == "--features")
        {
            if (features)
                return std::unexpected { repeated };
            features = featureSetNamed(value);
            if (!features)
                return std::unexpected { std::format("no feature set is named '{}'", value) };
        }
        else if (name == "--engine")
        {
            if (row != nullptr)
                return std::unexpected { repeated };
            row = api::engines::rowNamed(value);
            if (row == nullptr)
                return std::unexpected { std::format("no engine is named '{}'", value) };
        }
        else if (name == "--dispatch")
        {
            if (dispatch)
                return std::unexpected { repeated };
            dispatch = fastinterpreter::dispatchNamed(value);
            if (!dispatch)
                return std::unexpected { std::format("no dispatch is named '{}'", value) };
            if (!fastinterpreter::isAvailable(*dispatch))
                return std::unexpected { std::format("this build did not compile the {} dispatch", value) };
        }
        else
            return std::unexpected { std::format("unknown option '{}'", name) };
    }
    if (!features)
        return std::unexpected { std::string { "--features is required" } };
    if (row == nullptr)
        row = api::engines::rowOf(runtime::ExecutionEngine::Reference);
    if (row->dispatch == api::engines::DispatchUse::None && dispatch)
        return std::unexpected { std::string { "--dispatch names the fast engine's dispatch" } };
    return RunOptions { .features = *features,
                        .engine = SpecEngine {
                            .row = row, .dispatch = dispatch.value_or(fastinterpreter::bestDispatch()) } };
}

} // namespace isoline::spectest
