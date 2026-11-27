// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/DispatchAvailability.hpp>
#include <isoline/fastinterpreter/Listing.hpp>
#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/fastinterpreter/testing/Lowering.hpp>

using isoline::binary::testing::code;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::fastinterpreter::availableDispatches;
using isoline::fastinterpreter::bestDispatch;
using isoline::fastinterpreter::describe;
using isoline::fastinterpreter::Dispatch;
using isoline::fastinterpreter::familyRow;
using isoline::fastinterpreter::headerWord;
using isoline::fastinterpreter::LoweringConfig;
using isoline::fastinterpreter::OperationCount;
using isoline::fastinterpreter::OperationFamilyTable;
using isoline::fastinterpreter::operationOfHeader;
using isoline::fastinterpreter::OperationTable;
using isoline::fastinterpreter::Word;
using isoline::fastinterpreter::testing::functionOf;
using isoline::fastinterpreter::testing::lowered;
using isoline::fastinterpreter::testing::op;
using isoline::fastinterpreter::testing::validated;

namespace binary = isoline::binary;
namespace configured = isoline::fastinterpreter::configured;

namespace
{

/// Whether this build compiled @p dispatch.
[[nodiscard]] bool isAvailable(Dispatch dispatch)
{
    return std::ranges::find(availableDispatches(), dispatch) != availableDispatches().end();
}

/// The compiler families a probe row is that of, as the generated header lists them.
[[nodiscard]] std::set<std::string_view> familiesOf(std::string_view families)
{
    auto split = std::set<std::string_view> {};
    for (auto const family: families | std::views::split(' '))
        split.emplace(family.begin(), family.end());
    return split;
}

} // namespace

TEST_CASE("Dispatch.bestIsAvailable")
{
    auto const available = availableDispatches();
    REQUIRE_FALSE(available.empty());
    CHECK(std::ranges::find(available, bestDispatch()) != available.end());
    CHECK(available.front() == Dispatch::Switch);
    CHECK((bestDispatch() == Dispatch::TailCall) == isAvailable(Dispatch::TailCall));
}

TEST_CASE("Dispatch.probeRowsAreExercised")
{
    // Every compiler family that has a guaranteed tail call has a row, and every row names its families.
    REQUIRE(configured::TailCallProbeRows.size() == configured::TailCallProbeFamilies.size());
    auto covered = std::set<std::string_view> {};
    for (auto const families: configured::TailCallProbeFamilies)
        covered.merge(familiesOf(families));
    for (auto const family: { "Clang", "AppleClang", "GNU" })
        CHECK(covered.contains(family));

    // The row that passed is one of the rows, of this compiler, and is what compiled TailCall; none passed
    // where it is not compiled.
    auto const passed = configured::PassedProbeRow;
    CHECK(passed.empty() == !isAvailable(Dispatch::TailCall));
    if (!passed.empty())
    {
        auto const row = std::ranges::find(configured::TailCallProbeRows, passed);
        REQUIRE(row != configured::TailCallProbeRows.end());
        auto const families = configured::TailCallProbeFamilies[static_cast<std::size_t>(
            std::distance(configured::TailCallProbeRows.begin(), row))];
        CHECK(familiesOf(families).contains(configured::CompilerFamily));
    }

    // Clang has had its row's spelling since Clang 13: a probe that fails there has broken, not found the
    // compiler wanting, and would leave the build on Switch alone without a word.
    if (configured::TailCallSetting == "AUTO"
        && familiesOf("Clang AppleClang").contains(configured::CompilerFamily))
        CHECK(passed == "clang::musttail");
    if (configured::TailCallSetting == "OFF")
        CHECK(passed.empty());
}

TEST_CASE("Dispatch.everyOperationHasOneHeaderWord")
{
    // Switch code names an operation by its OperationId, TailCall code by its handler's address: one handler
    // per operation, distinct, and at no address an OperationId is, so a header names its operation under
    // either dispatch.
    for (auto const dispatch: availableDispatches())
    {
        auto words = std::set<Word> {};
        for (auto const& row: OperationTable)
        {
            INFO(row.name);
            auto const word = headerWord(dispatch, row.id);
            CHECK(operationOfHeader(word) == row.id);
            if (dispatch == Dispatch::Switch)
                CHECK(word == std::to_underlying(row.id));
            else
                CHECK(word >= OperationCount);
            words.insert(word);
        }
        CHECK(words.size() == OperationCount);
    }
}

TEST_CASE("Dispatch.everyFamilyHasARow")
{
    // Every operation's family is a row of the table, which names it once.
    auto names = std::set<std::string_view> {};
    for (auto const& row: OperationFamilyTable)
    {
        CHECK(familyRow(row.family).name == row.name);
        names.insert(row.name);
    }
    CHECK(names.size() == OperationFamilyTable.size());
    for (auto const& row: OperationTable)
        CHECK(familyRow(row.family).family == row.family);
}

TEST_CASE("Dispatch.threadedCodeListsAlike")
{
    // The same module lowered for every dispatch lists alike; only TailCall's header words differ.
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto const module = validated(functionOf({ binary::I32 },
                                             { binary::I32 },
                                             {},
                                             code({ { 0x02, 0x40 },
                                                    localGet(0),
                                                    i32Const(3),
                                                    op(0x48),
                                                    op(0x0D, 0),
                                                    localGet(0),
                                                    { 0x0E, 0x01, 0x00, 0x00 },
                                                    { 0x0B },
                                                    localGet(0) })));
    auto const switchCode = lowered(module, LoweringConfig { .dispatch = Dispatch::Switch });
    auto const threaded = lowered(module, LoweringConfig { .dispatch = dispatch });
    auto const& switchFunction = switchCode->functions().front();
    auto const& threadedFunction = threaded->functions().front();
    CHECK(describe(threadedFunction) == describe(switchFunction));
    CHECK(threaded->dispatch() == dispatch);
    REQUIRE(threadedFunction.code.size() == switchFunction.code.size());
    CHECK((threadedFunction.code == switchFunction.code) == (dispatch == Dispatch::Switch));
}
