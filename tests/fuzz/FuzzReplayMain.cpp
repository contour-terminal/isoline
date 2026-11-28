// SPDX-License-Identifier: Apache-2.0
// The main of a fuzz target in a build without libFuzzer (every Clang build but the clang-fuzz presets): it
// runs the target on each file its command line names, and on every file of each directory it names, in
// name order, as libFuzzer runs a corpus. A target that finds a fault traps, as under libFuzzer. Exits with 0
// when every input passed, 2 on a usage or file error.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv);
extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, std::size_t size);

namespace
{

constexpr auto ExitPassed = 0;
constexpr auto ExitUsage = 2;

/// The inputs @p path names: itself, or a directory's files in name order.
[[nodiscard]] std::vector<std::filesystem::path> inputsOf(std::filesystem::path const& path)
{
    if (!std::filesystem::is_directory(path))
        return { path };
    auto files = std::vector<std::filesystem::path> {};
    for (auto const& entry: std::filesystem::directory_iterator { path })
        if (entry.is_regular_file())
            files.push_back(entry.path());
    std::ranges::sort(files);
    return files;
}

} // namespace

int main(int argc, char* argv[])
{
    LLVMFuzzerInitialize(&argc, &argv);
    auto const arguments = std::span<char* const> { argv, static_cast<std::size_t>(argc) }.subspan(1);
    if (arguments.empty())
    {
        std::println(stderr, "usage: {} <input|directory>...", argv[0]);
        return ExitUsage;
    }
    auto inputs = 0;
    for (auto const* argument: arguments)
    {
        for (auto const& path: inputsOf(argument))
        {
            auto file = std::ifstream { path, std::ios::binary };
            if (!file)
            {
                std::println(stderr, "cannot read '{}'", path.string());
                return ExitUsage;
            }
            auto const bytes = std::vector<char> { std::istreambuf_iterator<char> { file }, {} };
            auto const data = std::as_bytes(std::span { bytes });
            LLVMFuzzerTestOneInput(reinterpret_cast<uint8_t const*>(data.data()), data.size());
            ++inputs;
        }
    }
    std::println("{} inputs passed", inputs);
    return ExitPassed;
}
