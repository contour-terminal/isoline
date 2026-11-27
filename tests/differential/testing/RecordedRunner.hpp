// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ProcessRunner.hpp"
#include "WasmtimeOracle.hpp"

/// Test doubles of the oracle's seams: a process runner that replays a recorded run, and module files that
/// are never written.
namespace isoline::differential::testing
{

/// Replays a recorded run of Wasmtime for every call, and keeps the arguments of the last.
class RecordedRunner final: public IProcessRunner
{
  public:
    /// A runner answering every run with @p recorded.
    /// @param recorded The run's result, or its error.
    explicit RecordedRunner(std::expected<ProcessResult, ProcessError> recorded):
        _recorded { std::move(recorded) }
    {
    }

    /// @copydoc IProcessRunner::run
    [[nodiscard]] std::expected<ProcessResult, ProcessError> run(std::filesystem::path const& program,
                                                                 std::span<std::string const> arguments,
                                                                 std::chrono::milliseconds timeout) override
    {
        _program = program;
        _arguments.assign(arguments.begin(), arguments.end());
        _timeout = timeout;
        return _recorded;
    }

    /// The program of the last run.
    [[nodiscard]] std::filesystem::path const& program() const noexcept { return _program; }
    /// The arguments of the last run.
    [[nodiscard]] std::vector<std::string> const& arguments() const noexcept { return _arguments; }
    /// The deadline of the last run.
    [[nodiscard]] std::chrono::milliseconds timeout() const noexcept { return _timeout; }

  private:
    std::expected<ProcessResult, ProcessError> _recorded;
    std::chrono::milliseconds _timeout {};
    std::filesystem::path _program;
    std::vector<std::string> _arguments;
};

/// Module files that are never written: each module is named /modules/module.wasm.
class NamedModuleFiles final: public IModuleFiles
{
  public:
    /// @copydoc IModuleFiles::write
    [[nodiscard]] std::expected<std::filesystem::path, std::string> write(
        std::span<std::byte const> /*module*/) override
    {
        return std::filesystem::path { "/modules/module.wasm" };
    }
};

} // namespace isoline::differential::testing
