// SPDX-License-Identifier: Apache-2.0
#include "Seeds.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <vector>

#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/testing/HostileModules.hpp>

namespace isoline::fuzz
{

namespace
{

    /// @p bytes as std::byte.
    [[nodiscard]] std::vector<std::byte> asBytes(std::vector<uint8_t> const& bytes)
    {
        auto result = std::vector<std::byte> {};
        result.reserve(bytes.size());
        for (auto const byte: bytes)
            result.push_back(static_cast<std::byte>(byte));
        return result;
    }

    /// A module whose type section claims @p entries entries, as many as bytes follow the count, each a func
    /// type cut short: a lying count, which a decoder that reserves by the count before reading amplifies.
    [[nodiscard]] std::vector<std::byte> lyingTypeCount(uint32_t entries)
    {
        auto bytes = std::vector<uint8_t> { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00, 0x01 };
        auto payload = binary::testing::encodeU32(entries);
        payload.insert(payload.end(), entries, 0x60);
        auto const size = binary::testing::encodeU32(static_cast<uint32_t>(payload.size()));
        bytes.insert(bytes.end(), size.begin(), size.end());
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return asBytes(bytes);
    }

    /// One hostile shape: how it is built at each scale, and its full-size verdict.
    struct HostileRow
    {
        std::string_view name;
        std::function<std::vector<std::byte>(SeedScale)> build;
        LoadVerdict expected;
    };

    /// @p full at SeedScale::Full, else @p corpus.
    [[nodiscard]] constexpr uint32_t sized(SeedScale scale, uint32_t full, uint32_t corpus) noexcept
    {
        return scale == SeedScale::Full ? full : corpus;
    }

    [[nodiscard]] std::vector<HostileRow> hostileRows()
    {
        namespace hostile = isoline::interpreter::testing;
        static constexpr auto Limits = binary::DecodeLimits {};
        auto rows = std::vector<HostileRow> {
            { .name = "50'000 locals over a forwarded stack",
              .build =
                  [](SeedScale scale) {
                      return hostile::forwardedStack(sized(scale, Limits.maxLocals - 2, 5'000),
                                                     sized(scale, 1'000, 200),
                                                     sized(scale, 1'000, 200))
                          .build();
                  },
              .expected = LoadVerdict::Lowered },
            { .name = "a 7 MB body",
              .build =
                  [](SeedScale scale) {
                      return hostile::constantsAndDrops(
                                 sized(scale, Limits.maxFunctionBodySize - 64, 200'000))
                          .build();
                  },
              .expected = LoadVerdict::OverBudget },
            { .name = "1'000-result blocks branched to",
              .build =
                  [](SeedScale scale) {
                      return hostile::branchesWithValues(sized(scale, Limits.maxResults, 1'000),
                                                         sized(scale, 10'000, 1'000))
                          .build();
                  },
              .expected = LoadVerdict::Lowered },
            { .name = "a br_table of the most labels at 1'000-result blocks",
              .build =
                  [](SeedScale scale) {
                      return hostile::branchTableWithValues(sized(scale, Limits.maxResults, 1'000),
                                                            sized(scale, Limits.maxBrTableSize, 10'000))
                          .build();
                  },
              .expected = LoadVerdict::Lowered },
            { .name = "1'000-value blocks re-entered",
              .build =
                  [](SeedScale scale) {
                      return hostile::reenteredBlocks(
                                 sized(scale, Limits.maxParams, 1'000), sized(scale, 2'000, 200), 0x04)
                          .build();
                  },
              .expected = LoadVerdict::Lowered },
            { .name = "exnref locals around calls",
              .build =
                  [](SeedScale scale) {
                      return hostile::exnrefLocalsAroundCalls(sized(scale, Limits.maxLocals, 5'000),
                                                              sized(scale, 10'000, 1'000))
                          .build();
                  },
              .expected = LoadVerdict::Lowered },
            { .name = "50'000 nested try_tables around calls",
              .build =
                  [](SeedScale scale) {
                      return hostile::nestedTryTablesAroundCalls(sized(scale, 50'000, 5'000),
                                                                 sized(scale, 1'000, 100))
                          .build();
                  },
              .expected = LoadVerdict::Lowered },
            { .name = "a lying type count",
              .build = [](SeedScale scale) { return lyingTypeCount(sized(scale, 1'000'000, 100'000)); },
              .expected = LoadVerdict::Refused },
        };
        for (auto const opcode: { uint8_t { 0x02 }, uint8_t { 0x03 }, uint8_t { 0x04 } })
            rows.push_back(
                { .name = "100'000 nested blocks, loops or ifs",
                  .build =
                      [opcode](SeedScale scale) {
                          return hostile::nestedBlocks(sized(scale, 100'000, 20'000), opcode).build();
                      },
                  .expected = LoadVerdict::OverBudget });
        return rows;
    }

    /// A SplitMix64 stream: the generated seeds' entropy, the same on every host.
    [[nodiscard]] uint64_t splitMix(uint64_t& state) noexcept
    {
        state += 0x9E37'79B9'7F4A'7C15U;
        auto mixed = state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9U;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBU;
        return mixed ^ (mixed >> 31U);
    }

    /// The entropy of each generated seed.
    constexpr auto SeedEntropyBytes = std::size_t { 2'048 };

} // namespace

std::vector<HostileSeed> hostileSeeds(SeedScale scale)
{
    auto seeds = std::vector<HostileSeed> {};
    for (auto const& row: hostileRows())
        seeds.push_back({ .name = row.name, .bytes = row.build(scale), .expected = row.expected });
    return seeds;
}

std::vector<std::vector<std::byte>> generatedSeeds(uint32_t count, GeneratorMode mode)
{
    auto seeds = std::vector<std::vector<std::byte>> {};
    auto state = uint64_t { 0 };
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
    {
        auto entropy = std::vector<std::byte> {};
        entropy.reserve(SeedEntropyBytes);
        for ([[maybe_unused]] auto const byte: std::views::iota(std::size_t { 0 }, SeedEntropyBytes))
            entropy.push_back(static_cast<std::byte>(splitMix(state) & 0xFFU));
        seeds.push_back(generateModule(entropy, GeneratorLimits {}, mode));
    }
    return seeds;
}

} // namespace isoline::fuzz
