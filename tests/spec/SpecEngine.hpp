// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <span>
#include <string>

#include <isoline/Features.hpp>
#include <isoline/api/ExecutionEngines.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>

namespace isoline::spectest
{

/// The engine a script runs on: an engine row (api::engines), and the dispatch its code is lowered for and
/// run with, one this build compiled.
struct SpecEngine
{
    api::engines::EngineRow const* row =
        api::engines::rowOf(runtime::ExecutionEngine::Reference);         ///< Never null.
    fastinterpreter::Dispatch dispatch = fastinterpreter::bestDispatch(); ///< The reference ignores it.

    /// Two engines are equal when row and dispatch are.
    bool operator==(SpecEngine const&) const = default;
};

/// What a command line asks of a run, beside the script's directory and its expected skips.
struct RunOptions
{
    FeatureSet features; ///< The features the decoder and the validator accept.
    SpecEngine engine;   ///< The engine the script runs on.
};

/// Reads the options of a command line: `--features <wasm2|wasm3|all>`, required, then
/// `--engine <name>` (an api::engines row; reference unless named) and, for the fast engine only,
/// `--dispatch <name>` (a fastinterpreter::dispatchNamed() name; this build's fastest unless named), each at
/// most once, in any order.
/// @param arguments The arguments after the directory and the expected skips.
/// @return The options, or why the command line names none: an unknown or repeated option, a value no
///         table names, a dispatch for the reference engine, or one this build did not compile.
[[nodiscard]] std::expected<RunOptions, std::string> parseOptions(std::span<char const* const> arguments);

} // namespace isoline::spectest
