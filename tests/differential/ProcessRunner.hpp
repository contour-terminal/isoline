// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
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
    int exitCode = 0;           ///< The status it exited with.
    std::string standardOutput; ///< Everything it wrote to standard output.
    std::string standardError;  ///< Everything it wrote to standard error.
};

/// Why a process gave no result.
enum class ProcessErrorKind : uint8_t
{
    NotStarted, ///< It could not be started: no such program, not executable, no file for its output.
    TimedOut,   ///< It ran past its deadline, and the runner killed it.
    Killed,     ///< It ended abnormally: a signal (POSIX), an NTSTATUS error status (Windows).
    Output,     ///< It ended, but its output could not be read back.
};

/// Why a process gave no result, and what the system said.
struct ProcessError
{
    ProcessErrorKind kind = ProcessErrorKind::NotStarted; ///< Why.
    std::string detail;                                   ///< The program, and the system's words.
};

/// Runs a program to its end: the seam the Wasmtime oracle runs Wasmtime through, so that its tests replay
/// recorded outputs instead.
class IProcessRunner
{
  public:
    virtual ~IProcessRunner() = default;

    /// Runs @p program with @p arguments, its standard input empty, and waits for it to end, at most
    /// @p timeout: past it, the program is killed.
    /// @param program The executable.
    /// @param arguments Its arguments, each passed as it is (no shell reads them).
    /// @param timeout How long it may run.
    /// @return How it ended, or why it gave no result.
    [[nodiscard]] virtual std::expected<ProcessResult, ProcessError> run(
        std::filesystem::path const& program,
        std::span<std::string const> arguments,
        std::chrono::milliseconds timeout) = 0;
};

/// The operating system's processes: fork and execv, or CreateProcessW on Windows (one source file per
/// platform, chosen by CMake). Each output goes to a file of its own in the scratch directory, read back and
/// removed when the process ends, so that neither pipe can fill and block it. The deadline is measured on
/// the steady clock: this is the adapter to the system, the one place that reads it.
class ProcessRunner final: public IProcessRunner
{
  public:
    /// A runner keeping the outputs of the processes it runs in @p scratchDirectory while they run.
    /// @param scratchDirectory An existing directory the runner may create and remove files in.
    explicit ProcessRunner(std::filesystem::path scratchDirectory);

    /// @copydoc IProcessRunner::run
    [[nodiscard]] std::expected<ProcessResult, ProcessError> run(std::filesystem::path const& program,
                                                                 std::span<std::string const> arguments,
                                                                 std::chrono::milliseconds timeout) override;

  private:
    std::filesystem::path _scratchDirectory;
    uint64_t _runs = 0; ///< The processes run so far, which names each one's output files.
};

/// Reads a whole file.
/// @param path The file.
/// @return Its bytes as text, or why it could not be read.
[[nodiscard]] std::expected<std::string, std::string> readWholeFile(std::filesystem::path const& path);

/// What a platform's ProcessRunner::run() does once its process ended: reads both outputs back, and removes
/// their files.
/// @param outputPath Standard output's file.
/// @param errorPath Standard error's file.
/// @param exitCode The status the process exited with.
/// @return The result, or ProcessErrorKind::Output.
[[nodiscard]] std::expected<ProcessResult, ProcessError> readOutputs(std::filesystem::path const& outputPath,
                                                                     std::filesystem::path const& errorPath,
                                                                     int exitCode);

/// Removes both output files, as far as they exist.
/// @param outputPath Standard output's file.
/// @param errorPath Standard error's file.
void removeOutputs(std::filesystem::path const& outputPath, std::filesystem::path const& errorPath) noexcept;

} // namespace isoline::differential
