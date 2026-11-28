// SPDX-License-Identifier: Apache-2.0
// isoline-fuzz-seeds <load|text> <directory> <max-len>
//
// Writes a fuzz target's generated seeds into <directory>: for the load target, linear lowering's hostile
// shapes scaled down (Seeds.hpp) and modules of the generator; for the text target, the generator's modules
// in the text format. <max-len> is the target's -max_len, from CI's target table: a seed longer than it would
// never be mutated whole, so writing one is an error, which keeps the seeds and the table in step. Exits with
// 0, or 2 on a usage or file error or an overlong seed.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "Seeds.hpp"
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/text/ModuleWriter.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace
{

constexpr auto ExitWritten = 0;
constexpr auto ExitUsage = 2;

/// The generated seeds of each target.
constexpr auto GeneratedSeedCount = uint32_t { 64 };

/// Writes @p bytes to @p path, refusing more than @p maxLength bytes.
[[nodiscard]] std::expected<void, std::string> writeSeed(std::filesystem::path const& path,
                                                         std::span<std::byte const> bytes,
                                                         std::size_t maxLength)
{
    if (bytes.size() > maxLength)
        return std::unexpected { std::format(
            "{} holds {} bytes, past -max_len {}", path.string(), bytes.size(), maxLength) };
    auto output = std::ofstream { path, std::ios::binary };
    output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output)
        return std::unexpected { std::format("cannot write {}", path.string()) };
    return {};
}

/// The load target's seeds: the hostile shapes at corpus scale, then the generator's modules.
[[nodiscard]] std::expected<void, std::string> writeLoadSeeds(std::filesystem::path const& directory,
                                                              std::size_t maxLength)
{
    auto index = 0;
    for (auto const& seed: isoline::fuzz::hostileSeeds(isoline::fuzz::SeedScale::Corpus))
        if (auto const written =
                writeSeed(directory / std::format("hostile-{}.wasm", index++), seed.bytes, maxLength);
            !written)
            return written;
    index = 0;
    for (auto const& module:
         isoline::fuzz::generatedSeeds(GeneratedSeedCount, isoline::fuzz::GeneratorMode::Harness))
        if (auto const written =
                writeSeed(directory / std::format("generated-{}.wasm", index++), module, maxLength);
            !written)
            return written;
    return {};
}

/// The text target's seeds: the generator's modules, written in the text format.
[[nodiscard]] std::expected<void, std::string> writeTextSeeds(std::filesystem::path const& directory,
                                                              std::size_t maxLength)
{
    auto const writer = isoline::text::ModuleWriter { isoline::text::WriterOptions {} };
    auto index = 0;
    for (auto const& module:
         isoline::fuzz::generatedSeeds(GeneratedSeedCount, isoline::fuzz::GeneratorMode::Harness))
    {
        auto const loaded = isoline::validate::ValidatedModule::load(
            module, isoline::fuzz::GeneratorFeatures, isoline::binary::DecodeLimits {});
        if (!loaded)
            return std::unexpected { std::format("generated seed {} does not validate", index) };
        auto const text = writer.write((*loaded)->module());
        if (!text)
            return std::unexpected { std::format("generated seed {} has no text form", index) };
        if (auto const written = writeSeed(directory / std::format("generated-{}.wat", index++),
                                           std::as_bytes(std::span { *text }),
                                           maxLength);
            !written)
            return written;
    }
    return {};
}

/// What writes a target's seeds, one row per target.
struct SeedWriter
{
    std::string_view target;
    std::expected<void, std::string> (*write)(std::filesystem::path const&, std::size_t);
};

constexpr auto SeedWriters = std::to_array<SeedWriter>({
    { .target = "load", .write = &writeLoadSeeds },
    { .target = "text", .write = &writeTextSeeds },
});

} // namespace

int main(int argc, char const* argv[])
{
    auto const arguments = std::span<char const* const> { argv, static_cast<std::size_t>(argc) }.subspan(1);
    auto maxLength = std::size_t { 0 };
    auto const usage = [] {
        std::println(stderr, "usage: isoline-fuzz-seeds <load|text> <directory> <max-len>");
        return ExitUsage;
    };
    if (arguments.size() != 3)
        return usage();
    auto const length = std::string_view { arguments[2] };
    if (auto const [end, error] = std::from_chars(length.data(), length.data() + length.size(), maxLength);
        error != std::errc {} || end != length.data() + length.size())
        return usage();
    auto const target = std::string_view { arguments[0] };
    auto const directory = std::filesystem::path { arguments[1] };
    std::filesystem::create_directories(directory);
    auto const row = std::ranges::find(SeedWriters, target, &SeedWriter::target);
    if (row == SeedWriters.end())
        return usage();
    auto const written = row->write(directory, maxLength);
    if (!written)
    {
        std::println(stderr, "isoline-fuzz-seeds: {}", written.error());
        return ExitUsage;
    }
    return ExitWritten;
}
