// SPDX-License-Identifier: Apache-2.0
#include "ProcessRunner.hpp"

#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace isoline::differential
{

ProcessRunner::ProcessRunner(std::filesystem::path scratchDirectory):
    _scratchDirectory { std::move(scratchDirectory) }
{
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
