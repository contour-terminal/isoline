// SPDX-License-Identifier: Apache-2.0
#include "ProcessRunner.hpp"

#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace isoline::differential
{

ProcessRunner::ProcessRunner(std::filesystem::path scratchDirectory):
    _scratchDirectory { std::move(scratchDirectory) }
{
}

std::expected<ProcessResult, ProcessError> readOutputs(std::filesystem::path const& outputPath,
                                                       std::filesystem::path const& errorPath,
                                                       int exitCode)
{
    auto standardOutput = readWholeFile(outputPath);
    auto standardError = readWholeFile(errorPath);
    removeOutputs(outputPath, errorPath);
    if (!standardOutput)
        return std::unexpected { ProcessError { .kind = ProcessErrorKind::Output,
                                                .detail = standardOutput.error() } };
    if (!standardError)
        return std::unexpected { ProcessError { .kind = ProcessErrorKind::Output,
                                                .detail = standardError.error() } };
    return ProcessResult { .exitCode = exitCode,
                           .standardOutput = std::move(*standardOutput),
                           .standardError = std::move(*standardError) };
}

void removeOutputs(std::filesystem::path const& outputPath, std::filesystem::path const& errorPath) noexcept
{
    auto ignored = std::error_code {};
    std::filesystem::remove(outputPath, ignored);
    std::filesystem::remove(errorPath, ignored);
}

std::expected<std::string, std::string> readWholeFile(std::filesystem::path const& path)
{
    // Sized from the file, then read in one go: GCC 14 reports a potential null dereference of the stream
    // buffer inside an istreambuf_iterator slurp.
    auto error = std::error_code {};
    auto const size = std::filesystem::file_size(path, error);
    auto stream = std::ifstream { path, std::ios::binary };
    if (error || !stream)
        return std::unexpected { std::format("cannot open {}", path.string()) };
    auto text = std::string(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(size));
    if (stream.bad())
        return std::unexpected { std::format("cannot read {}", path.string()) };
    text.resize(static_cast<std::size_t>(stream.gcount()));
    return text;
}

} // namespace isoline::differential
