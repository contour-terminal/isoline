// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "FuzzTargets.hpp"
#include "Seeds.hpp"
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/Lowering.hpp>

using namespace isoline;
using isoline::fuzz::LoadVerdict;

namespace
{

/// @p text's bytes.
[[nodiscard]] std::vector<std::byte> bytesOf(std::string_view text)
{
    auto const bytes = std::as_bytes(std::span { text });
    return { bytes.begin(), bytes.end() };
}

/// A target's committed inputs, and what runs one: whether it passes.
struct RegressionRow
{
    std::string_view directory;                                     ///< Under tests/fuzz/regressions/.
    std::function<std::string(std::span<std::byte const>)> failure; ///< Empty when the input passes.
};

/// Each target's regressions, one row per target.
[[nodiscard]] std::vector<RegressionRow> regressionRows()
{
    auto const describeLoad = [](std::expected<LoadVerdict, std::string> const& verdict) {
        return verdict ? std::string {} : verdict.error();
    };
    return {
        { .directory = "differential",
          .failure =
              [](std::span<std::byte const> input) {
                  auto const outcome = fuzz::runDifferential(input, fuzz::LaneSelection::Active);
                  return outcome ? std::string {}
                                 : outcome.error().participant + " in " + outcome.error().field;
              } },
        { .directory = "load",
          .failure =
              [=](std::span<std::byte const> input) {
                  return describeLoad(fuzz::runLoad(input, fuzz::LoadCompileBudget));
              } },
        { .directory = "text",
          .failure = [=](std::span<std::byte const> input) { return describeLoad(fuzz::runText(input)); } },
    };
}

/// The bytes of @p path.
[[nodiscard]] std::vector<std::byte> fileBytes(std::filesystem::path const& path)
{
    auto file = std::ifstream { path, std::ios::binary };
    REQUIRE(file.good());
    auto bytes = std::vector<std::byte> {};
    for (auto const character: std::vector<char> { std::istreambuf_iterator<char> { file }, {} })
        bytes.push_back(static_cast<std::byte>(character));
    return bytes;
}

} // namespace

TEST_CASE("LoadFuzzer.hostileSeedsStayInBudget", "[memory]")
{
    for (auto const& seed: fuzz::hostileSeeds(fuzz::SeedScale::Full))
    {
        INFO(seed.name);
        auto const verdict = fuzz::runLoad(seed.bytes, fuzz::LoadCompileBudget);
        if (!verdict)
            UNSCOPED_INFO(verdict.error());
        REQUIRE(verdict.has_value());
        CHECK(*verdict == seed.expected);
    }
}

TEST_CASE("LoadFuzzer.judgesEveryBound")
{
    auto const within = fuzz::LoweringMeasures { .peak = 100,
                                                 .budget = 100,
                                                 .refusal = std::nullopt,
                                                 .functions = { { .words = 10, .bound = 10 } },
                                                 .work = isoline::interpreter::WorkPerBodyUnit * 4,
                                                 .bodyUnits = 4 };
    auto const lowered = fuzz::judgeLowering(within);
    REQUIRE(lowered.has_value());
    CHECK(*lowered == LoadVerdict::Lowered);

    SECTION("the arena's peak within its budget")
    {
        auto measures = within;
        measures.peak = measures.budget + 1;
        auto const verdict = fuzz::judgeLowering(measures);
        REQUIRE_FALSE(verdict.has_value());
        CHECK(verdict.error().contains("peak"));
    }
    SECTION("the compile budget the only refusal")
    {
        auto measures = within;
        measures.refusal = isoline::interpreter::LoweringFailure::Budget;
        auto const overBudget = fuzz::judgeLowering(measures);
        REQUIRE(overBudget.has_value());
        CHECK(*overBudget == LoadVerdict::OverBudget);
        for (auto const reason: { isoline::interpreter::LoweringFailure::HostMemory,
                                  isoline::interpreter::LoweringFailure::FrameSlots,
                                  isoline::interpreter::LoweringFailure::CodeWords })
        {
            measures.refusal = reason;
            auto const verdict = fuzz::judgeLowering(measures);
            REQUIRE_FALSE(verdict.has_value());
            CHECK(verdict.error().contains("another reason"));
        }
    }
    SECTION("every function's code within its bound")
    {
        auto measures = within;
        measures.functions.push_back({ .words = 11, .bound = 10 });
        auto const verdict = fuzz::judgeLowering(measures);
        REQUIRE_FALSE(verdict.has_value());
        CHECK(verdict.error().contains("function 1"));
    }
    SECTION("the work within WorkPerBodyUnit per body unit")
    {
        auto measures = within;
        ++measures.work;
        auto const verdict = fuzz::judgeLowering(measures);
        REQUIRE_FALSE(verdict.has_value());
        CHECK(verdict.error().contains("work"));
    }
}

TEST_CASE("LoadFuzzer.reportsEachVerdict")
{
    auto builder = binary::testing::ModuleBuilder {};
    builder.type(binary::FuncType {}).function(0, {}, { 0x01 }); // nop
    auto const module = builder.build();

    auto const lowered = fuzz::runLoad(module, fuzz::LoadCompileBudget);
    REQUIRE(lowered.has_value());
    CHECK(*lowered == LoadVerdict::Lowered);

    auto const refused =
        fuzz::runLoad(std::span { module }.first(module.size() - 1), fuzz::LoadCompileBudget);
    REQUIRE(refused.has_value());
    CHECK(*refused == LoadVerdict::Refused);

    auto const overBudget = fuzz::runLoad(module, 16);
    REQUIRE(overBudget.has_value());
    CHECK(*overBudget == LoadVerdict::OverBudget);
}

TEST_CASE("TextFuzzer.compilesThenLoads")
{
    auto const lowered = fuzz::runText(bytesOf("(module (func (export \"f\") (result i32) i32.const 7))"));
    REQUIRE(lowered.has_value());
    CHECK(*lowered == LoadVerdict::Lowered);

    auto const unbalanced = fuzz::runText(bytesOf("(module (func"));
    REQUIRE(unbalanced.has_value());
    CHECK(*unbalanced == LoadVerdict::Refused);

    // Past the text format's nesting limit: refused, not a deep recursion.
    auto deep = std::string(20'000, '(');
    deep.append(20'000, ')');
    auto const nested = fuzz::runText(bytesOf(deep));
    REQUIRE(nested.has_value());
    CHECK(*nested == LoadVerdict::Refused);

    // Text that compiles but does not validate.
    auto const invalid = fuzz::runText(bytesOf("(module (func (result i32)))"));
    REQUIRE(invalid.has_value());
    CHECK(*invalid == LoadVerdict::Refused);
}

TEST_CASE("DifferentialFuzzer.splitsTheSeedFromTheEntropy")
{
    auto const input = std::to_array<std::byte>({ std::byte { 0x11 },
                                                  std::byte { 0x22 },
                                                  std::byte { 0x00 },
                                                  std::byte { 0x00 },
                                                  std::byte { 0x00 },
                                                  std::byte { 0x00 },
                                                  std::byte { 0x00 },
                                                  std::byte { 0x01 },
                                                  std::byte { 0xAB } });
    auto const split = fuzz::splitDifferentialInput(input);
    CHECK(split.seed == 0x0100'0000'0000'2211U);
    REQUIRE(split.moduleEntropy.size() == 1);
    CHECK(split.moduleEntropy.front() == std::byte { 0xAB });

    auto const shortInput = fuzz::splitDifferentialInput(std::span { input }.first(2));
    CHECK(shortInput.seed == 0x2211U);
    CHECK(shortInput.moduleEntropy.empty());
}

TEST_CASE("DifferentialFuzzer.enginesAgreeOnGeneratedModules")
{
    constexpr auto Inputs = 24;
    for (auto const index: std::views::iota(0, Inputs))
    {
        auto input = std::vector<std::byte>(static_cast<std::size_t>(64 + (index * 37)));
        for (auto const offset: std::views::iota(std::size_t { 0 }, input.size()))
            input[offset] =
                static_cast<std::byte>(((offset * 131U) + (static_cast<std::size_t>(index) * 17U)) & 0xFFU);
        auto const outcome = fuzz::runDifferential(input, fuzz::LaneSelection::Active);
        if (!outcome)
            UNSCOPED_INFO("input " << index << ": " << outcome.error().participant << " in "
                                   << outcome.error().field << "\n  expected " << outcome.error().expected
                                   << "\n  actual   " << outcome.error().actual);
        CHECK(outcome.has_value());
    }
}

TEST_CASE("FuzzRegressions.everyInputPasses")
{
    auto const root = std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "fuzz" / "regressions";
    for (auto const& row: regressionRows())
    {
        auto inputs = 0;
        for (auto const& entry: std::filesystem::directory_iterator { root / row.directory })
        {
            INFO(entry.path().string());
            CHECK(row.failure(fileBytes(entry.path())).empty());
            ++inputs;
        }
        INFO(row.directory);
        CHECK(inputs > 0);
    }
}
