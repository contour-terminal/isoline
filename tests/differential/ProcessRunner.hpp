// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>

namespace isoline::differential
{

/// How a process ended, and what it wrote.
struct ProcessResult
{
    int exitCode = 0;           ///< Its exit status: what it returned, or the signal that ended it plus 128.
    std::string standardOutput; ///< Everything it wrote to standard output.
    std::string standardError;  ///< Everything it wrote to standard error.
};

/// Runs a program to its end: the seam the Wasmtime oracle runs Wasmtime through, so that its tests replay
/// recorded outputs instead.
class IProcessRunner
{
  public:
    virtual ~IProcessRunner() = default;

    /// Runs @p program with @p arguments, its standard input empty, and waits for it to end.
    /// @param program The executable.
    /// @param arguments Its arguments, each passed as it is (no shell reads them).
    /// @return How it ended, or why it could not be started.
    [[nodiscard]] virtual std::expected<ProcessResult, std::string> run(
        std::filesystem::path const& program, std::span<std::string const> arguments) = 0;
};

/// The operating system's processes: fork and execv, or CreateProcessW on Windows (one source file per
/// platform, chosen by CMake). Each output goes to a file of its own in the scratch directory, read back and
/// removed when the process ends, so that neither pipe can fill and block it.
class ProcessRunner final: public IProcessRunner
{
  public:
    /// A runner keeping the outputs of the processes it runs in @p scratchDirectory while they run.
    /// @param scratchDirectory An existing directory the runner may create and remove files in.
    explicit ProcessRunner(std::filesystem::path scratchDirectory);

    /// @copydoc IProcessRunner::run
    [[nodiscard]] std::expected<ProcessResult, std::string> run(
        std::filesystem::path const& program, std::span<std::string const> arguments) override;

  private:
    std::filesystem::path _scratchDirectory;
    uint64_t _runs = 0; ///< The processes run so far, which names each one's output files.
};

/// Reads a whole file.
/// @param path The file.
/// @return Its bytes as text, or why it could not be read.
[[nodiscard]] std::expected<std::string, std::string> readWholeFile(std::filesystem::path const& path);

} // namespace isoline::differential
