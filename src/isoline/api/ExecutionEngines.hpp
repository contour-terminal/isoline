// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <string_view>

#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/validate/ValidatedModule.hpp>

/// Internal to the API and its test tools: what the API does for each runtime::ExecutionEngine, one row each,
/// so that an engine is added as a row rather than as a case in every place that chooses one.
namespace isoline::api::engines
{

/// What an engine is asked to make of a module: how to lower it, within which budget, from which memory.
struct CodeRequest
{
    runtime::ExecutionMode mode = runtime::ExecutionMode::Production;       ///< The execution mode.
    fastinterpreter::Dispatch dispatch = fastinterpreter::Dispatch::Switch; ///< One of availableDispatches().
    uint64_t budget = 0;                                                    ///< The compile budget, in bytes.
    /// What code is allocated from; the global heap when null.
    std::shared_ptr<std::pmr::memory_resource> memory = nullptr;
};

/// What a module's code is, or why the engine refused to make it.
using CodeOutcome = std::expected<std::shared_ptr<runtime::ModuleCode const>, fastinterpreter::LoweringError>;

/// Whether an engine's code depends on a fastinterpreter::Dispatch.
enum class DispatchUse : uint8_t
{
    None = 0, ///< It lowers nothing, and ignores the dispatch.
    Lowered,  ///< It lowers for the dispatch and runs with it: part of its identity, and one this build
              ///< compiled.
};

/// One engine: its name, whether it takes a dispatch, the code it makes of a module, and its executor.
struct EngineRow
{
    runtime::ExecutionEngine engine; ///< The engine.
    std::string_view name;           ///< How command lines name it: reference or fast.
    DispatchUse dispatch;            ///< Whether its code depends on the dispatch.
    /// The code the engine runs @p module with; null for an engine that runs the validated module itself.
    CodeOutcome (*code)(validate::ValidatedModule const& module, CodeRequest const& request);
    /// An executor of the engine, running code lowered for @p dispatch (one of availableDispatches()) where
    /// the engine lowers.
    std::unique_ptr<runtime::ICodeExecutor> (*executor)(fastinterpreter::Dispatch dispatch);
};

/// The row of @p engine.
/// @param engine Any value of the type.
/// @return Its row, or null for a value that is no enumerator.
[[nodiscard]] EngineRow const* rowOf(runtime::ExecutionEngine engine) noexcept;

/// The row named @p name.
/// @param name A name, as EngineRow::name spells it.
/// @return Its row, or null when no engine has that name.
[[nodiscard]] EngineRow const* rowNamed(std::string_view name) noexcept;

} // namespace isoline::api::engines
