// SPDX-License-Identifier: Apache-2.0
#include <isoline/api/ExecutionEngines.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

#include <isoline/fastinterpreter/FastInterpreter.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/CountingArena.hpp>

namespace isoline::api::engines
{

namespace
{
    constexpr auto Rows = std::to_array<EngineRow>({
        { .engine = runtime::ExecutionEngine::Reference,
          .name = "reference",
          .dispatch = DispatchUse::None,
          .code = [](validate::ValidatedModule const& /*module*/,
                     CodeRequest const& /*request*/) -> CodeOutcome { return nullptr; },
          .executor = [](fastinterpreter::Dispatch /*dispatch*/) -> std::unique_ptr<runtime::ICodeExecutor> {
              return std::make_unique<interpreter::ReferenceInterpreter>();
          } },
        { .engine = runtime::ExecutionEngine::Fast,
          .name = "fast",
          .dispatch = DispatchUse::Lowered,
          .code = [](validate::ValidatedModule const& module, CodeRequest const& request) -> CodeOutcome {
              return fastinterpreter::lower(
                         module,
                         fastinterpreter::LoweringConfig { .mode = request.mode,
                                                           .dispatch = request.dispatch },
                         std::make_shared<runtime::CountingArena>(request.budget, request.memory))
                  .transform([](std::shared_ptr<fastinterpreter::LoweredModule const> lowered) {
                      return std::shared_ptr<runtime::ModuleCode const> { std::move(lowered) };
                  });
          },
          .executor = [](fastinterpreter::Dispatch dispatch) -> std::unique_ptr<runtime::ICodeExecutor> {
              return std::make_unique<fastinterpreter::FastInterpreter>(dispatch);
          } },
    });

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, Rows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(Rows[index].engine) == index;
                                      }),
                  "the rows are in ExecutionEngine's order");
} // namespace

EngineRow const* rowOf(runtime::ExecutionEngine engine) noexcept
{
    auto const index = static_cast<std::size_t>(std::to_underlying(engine));
    return index < Rows.size() ? &Rows[index] : nullptr;
}

EngineRow const* rowNamed(std::string_view name) noexcept
{
    auto const found = std::ranges::find(Rows, name, &EngineRow::name);
    return found == Rows.end() ? nullptr : &*found;
}

} // namespace isoline::api::engines
