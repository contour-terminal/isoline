// SPDX-License-Identifier: Apache-2.0
#include "ProcessRunner.hpp"

#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
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
    auto stream = std::ifstream { path, std::ios::binary };
    if (!stream)
        return std::unexpected { std::format("cannot open {}", path.string()) };
    auto text = std::string { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
    if (stream.bad())
        return std::unexpected { std::format("cannot read {}", path.string()) };
    return text;
}

} // namespace isoline::differential
