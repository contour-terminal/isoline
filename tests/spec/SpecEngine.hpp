// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <isoline/Features.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::spectest
{

/// The engine a script runs on: the reference interpreter, or the fast one under a dispatch.
struct SpecEngine
{
    runtime::ExecutionEngine engine = runtime::ExecutionEngine::Reference; ///< The engine.
    /// The fast engine's dispatch, one this build compiled; the reference engine has none.
    std::optional<fastinterpreter::Dispatch> dispatch;

    /// Two engines are equal when engine and dispatch are.
    bool operator==(SpecEngine const&) const = default;
};

/// An engine `--engine` can name.
struct NamedEngine
{
    std::string_view name;           ///< What `--engine` spells it as.
    runtime::ExecutionEngine engine; ///< The engine.
};

/// The engines `--engine` can name.
constexpr auto Engines = std::to_array<NamedEngine>({
    { .name = "reference", .engine = runtime::ExecutionEngine::Reference },
    { .name = "fast", .engine = runtime::ExecutionEngine::Fast },
});

/// A dispatch `--dispatch` can name.
struct NamedDispatch
{
    std::string_view name;              ///< What `--dispatch` spells it as.
    fastinterpreter::Dispatch dispatch; ///< The dispatch.
};

/// The dispatches `--dispatch` can name, whether or not this build compiled them.
constexpr auto Dispatches = std::to_array<NamedDispatch>({
    { .name = "switch", .dispatch = fastinterpreter::Dispatch::Switch },
    { .name = "tailcall", .dispatch = fastinterpreter::Dispatch::TailCall },
});

/// What a command line asks of a run, beside the script's directory and its expected skips.
struct RunOptions
{
    FeatureSet features; ///< The features the decoder and the validator accept.
    SpecEngine engine;   ///< The engine the script runs on.
};

/// Reads the options of a command line: `--features <wasm2|wasm3|all>`, required, then
/// `--engine <reference|fast>` (reference unless named) and, for the fast engine only,
/// `--dispatch <switch|tailcall>` (this build's fastest unless named), each at most once, in any order.
/// @param arguments The arguments after the directory and the expected skips.
/// @return The options, or why the command line names none: an unknown or repeated option, a value no
///         table names, a dispatch for the reference engine, or one this build did not compile.
[[nodiscard]] std::expected<RunOptions, std::string> parseOptions(std::span<char const* const> arguments);

/// An executor of @p engine: the reference interpreter, or the fast one under its dispatch.
/// @param engine The engine; a fast one names a dispatch this build compiled (checked by its constructor).
/// @return The executor.
[[nodiscard]] std::unique_ptr<runtime::ICodeExecutor> makeExecutor(SpecEngine const& engine);

/// The code @p engine runs @p module with: lowered for the fast engine, under the embedding API's default
/// compile budget; none for the reference engine.
/// @param engine The engine.
/// @param module The validated module.
/// @return The code (null for the reference engine), or why lowering refused the module.
[[nodiscard]] std::expected<std::shared_ptr<runtime::ModuleCode const>, std::string> codeFor(
    SpecEngine const& engine, validate::ValidatedModule const& module);

} // namespace isoline::spectest
