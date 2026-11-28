// SPDX-License-Identifier: Apache-2.0
// isoline-fuzz-module <input> <directory>
//
// Writes the module a differential fuzzer input generates as <directory>/<seed>.wasm, where <seed> is the
// input's argument seed: the form of tests/differential/regressions/, which isoline-differential-replay and
// CorpusReplay.everyRegressionAgrees replay with that seed. Prints the path written. Never overwrites a file.
// Exits with 0, or 2 on a usage or file error, or when the file exists.
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <print>
#include <span>
#include <string>
#include <vector>

#include "FuzzTargets.hpp"
#include "ModuleGenerator.hpp"

namespace
{

constexpr auto ExitWritten = 0;
constexpr auto ExitUsage = 2;

} // namespace

int main(int argc, char const* argv[])
{
    auto const arguments = std::span<char const* const> { argv, static_cast<std::size_t>(argc) }.subspan(1);
    if (arguments.size() != 2)
    {
        std::println(stderr, "usage: isoline-fuzz-module <input> <directory>");
        return ExitUsage;
    }
    auto file = std::ifstream { arguments[0], std::ios::binary };
    if (!file)
    {
        std::println(stderr, "isoline-fuzz-module: cannot read '{}'", arguments[0]);
        return ExitUsage;
    }
    auto input = std::vector<std::byte> {};
    for (auto const character: std::vector<char> { std::istreambuf_iterator<char> { file }, {} })
        input.push_back(static_cast<std::byte>(character));
    auto const split = isoline::fuzz::splitDifferentialInput(input);
    auto const module =
        isoline::fuzz::generateModule(split.moduleEntropy, isoline::fuzz::DifferentialGeneratorLimits);
    auto const path = std::filesystem::path { arguments[1] } / std::format("{}.wasm", split.seed);
    if (std::filesystem::exists(path))
    {
        std::println(
            stderr, "isoline-fuzz-module: '{}' exists already; it is not overwritten", path.string());
        return ExitUsage;
    }
    auto output = std::ofstream { path, std::ios::binary };
    output.write(reinterpret_cast<char const*>(module.data()), static_cast<std::streamsize>(module.size()));
    if (!output)
    {
        std::println(stderr, "isoline-fuzz-module: cannot write '{}'", path.string());
        return ExitUsage;
    }
    std::println("{}", path.string());
    return ExitWritten;
}
