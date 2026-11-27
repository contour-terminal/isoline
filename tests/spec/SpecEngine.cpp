// SPDX-License-Identifier: Apache-2.0
#include "SpecEngine.hpp"

#include <core/Assert.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "SpecFeatures.hpp"
#include <isoline/api/Engine.hpp>
#include <isoline/fastinterpreter/FastInterpreter.hpp>
#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/CountingArena.hpp>

namespace isoline::spectest
{

namespace
{
    /// The row of @p table whose name is @p name.
    /// @return The row, or null.
    template <typename Row, std::size_t Count>
    [[nodiscard]] Row const* rowNamed(std::array<Row, Count> const& table, std::string_view name)
    {
        auto const found = std::ranges::find(table, name, &Row::name);
        return found == table.end() ? nullptr : &*found;
    }
} // namespace

std::expected<RunOptions, std::string> parseOptions(std::span<char const* const> arguments)
{
    auto features = std::optional<FeatureSet> {};
    auto engine = std::optional<runtime::ExecutionEngine> {};
    auto dispatch = std::optional<fastinterpreter::Dispatch> {};
    if (arguments.size() % 2 != 0)
        return std::unexpected { std::string { "every option takes one value" } };
    for (auto const pair: std::views::iota(std::size_t { 0 }, arguments.size() / 2))
    {
        struct
        {
            std::string_view name;
            std::string_view value;
        } const option { .name = arguments[2 * pair], .value = arguments[(2 * pair) + 1] };
        auto const repeated = std::format("{} is given twice", option.name);
        if (option.name == "--features")
        {
            if (features)
                return std::unexpected { repeated };
            features = featureSetNamed(option.value);
            if (!features)
                return std::unexpected { std::format("no feature set is named '{}'", option.value) };
        }
        else if (option.name == "--engine")
        {
            if (engine)
                return std::unexpected { repeated };
            auto const* const row = rowNamed(Engines, option.value);
            if (row == nullptr)
                return std::unexpected { std::format("no engine is named '{}'", option.value) };
            engine = row->engine;
        }
        else if (option.name == "--dispatch")
        {
            if (dispatch)
                return std::unexpected { repeated };
            auto const* const row = rowNamed(Dispatches, option.value);
            if (row == nullptr)
                return std::unexpected { std::format("no dispatch is named '{}'", option.value) };
            if (std::ranges::find(fastinterpreter::availableDispatches(), row->dispatch)
                == fastinterpreter::availableDispatches().end())
                return std::unexpected { std::format("this build did not compile the {} dispatch",
                                                     option.value) };
            dispatch = row->dispatch;
        }
        else
            return std::unexpected { std::format("unknown option '{}'", option.name) };
    }
    if (!features)
        return std::unexpected { std::string { "--features is required" } };
    auto const chosen = engine.value_or(runtime::ExecutionEngine::Reference);
    if (chosen == runtime::ExecutionEngine::Reference && dispatch)
        return std::unexpected { std::string { "--dispatch names the fast engine's dispatch" } };
    if (chosen == runtime::ExecutionEngine::Fast && !dispatch)
        dispatch = fastinterpreter::bestDispatch();
    return RunOptions { .features = *features,
                        .engine = SpecEngine { .engine = chosen, .dispatch = dispatch } };
}

std::unique_ptr<runtime::ICodeExecutor> makeExecutor(SpecEngine const& engine)
{
    switch (engine.engine)
    {
        case runtime::ExecutionEngine::Reference:
            return std::make_unique<interpreter::ReferenceInterpreter>();
        case runtime::ExecutionEngine::Fast:
            return std::make_unique<fastinterpreter::FastInterpreter>(
                engine.dispatch.value_or(fastinterpreter::bestDispatch()));
    }
    // Every ExecutionEngine is a case above.
    core::unreachable();
}

std::expected<std::shared_ptr<runtime::ModuleCode const>, std::string> codeFor(
    SpecEngine const& engine, validate::ValidatedModule const& module)
{
    if (engine.engine == runtime::ExecutionEngine::Reference)
        return nullptr;
    auto arena = std::make_shared<runtime::CountingArena>(api::LoadLimits {}.maxCompileBytes,
                                                          *std::pmr::new_delete_resource());
    return fastinterpreter::lower(module,
                                  fastinterpreter::LoweringConfig {
                                      .dispatch = engine.dispatch.value_or(fastinterpreter::bestDispatch()) },
                                  std::move(arena))
        .transform([](std::shared_ptr<fastinterpreter::LoweredModule const> lowered) {
            return std::shared_ptr<runtime::ModuleCode const> { std::move(lowered) };
        })
        .transform_error([](fastinterpreter::LoweringError const& error) {
            return std::format("lowering refused the module at function {}: {} of {}",
                               error.codeIndex,
                               error.requested,
                               error.limit);
        });
}

} // namespace isoline::spectest
