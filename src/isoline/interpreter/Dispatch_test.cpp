// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/Code.hpp>
#include <isoline/interpreter/Dispatch.hpp>
#include <isoline/interpreter/DispatchAvailability.hpp>
#include <isoline/interpreter/Listing.hpp>
#include <isoline/interpreter/Lowering.hpp>
#include <isoline/interpreter/testing/Engines.hpp>
#include <isoline/interpreter/testing/Lowering.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/SmallStackThread.hpp>

using isoline::binary::ExternKind;
using isoline::binary::FuncType;
using isoline::binary::testing::code;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::ModuleBuilder;
using isoline::interpreter::availableDispatches;
using isoline::interpreter::bestDispatch;
using isoline::interpreter::describe;
using isoline::interpreter::Dispatch;
using isoline::interpreter::familyRow;
using isoline::interpreter::headerWord;
using isoline::interpreter::isAvailable;
using isoline::interpreter::LoweringConfig;
using isoline::interpreter::OperationCount;
using isoline::interpreter::OperationFamilyTable;
using isoline::interpreter::operationOfHeader;
using isoline::interpreter::OperationTable;
using isoline::interpreter::Word;
using isoline::interpreter::testing::functionOf;
using isoline::interpreter::testing::Harness;
using isoline::interpreter::testing::interpreterUnder;
using isoline::interpreter::testing::lowered;
using isoline::interpreter::testing::op;
using isoline::interpreter::testing::validated;
using isoline::runtime::Abrupt;
using isoline::runtime::ExternValue;
using isoline::runtime::HostContext;
using isoline::runtime::I32;
using isoline::runtime::makeHostFunction;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::testing::runOnSmallStack;
using isoline::runtime::testing::SmallStackBytes;

namespace binary = isoline::binary;
namespace configured = isoline::interpreter::configured;

namespace
{

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

TEST_CASE("Dispatch.namesSpellEveryDispatch")
{
    // Every name spells its dispatch, compiled or not; no other name spells one.
    CHECK(isoline::interpreter::dispatchNamed("switch") == Dispatch::Switch);
    CHECK(isoline::interpreter::dispatchNamed("tailcall") == Dispatch::TailCall);
    CHECK_FALSE(isoline::interpreter::dispatchNamed("Switch").has_value());
    CHECK_FALSE(isoline::interpreter::dispatchNamed("").has_value());
    // One past the last Dispatch: no build compiles it.
    CHECK_FALSE(isAvailable(static_cast<Dispatch>(std::to_underlying(Dispatch::TailCall) + 1)));
    CHECK(isAvailable(Dispatch::Switch));
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
    // GCC has had its row's spelling since GCC 15; only a sanitizer keeps the handler's locals addressable
    // across the tail call, which GCC refuses.
    auto const major = configured::CompilerVersion.substr(0, configured::CompilerVersion.find('.'));
    auto const gccWithTailCalls = configured::CompilerFamily == "GNU" && major.size() >= 2 && major >= "15";
    if (configured::TailCallSetting == "AUTO" && gccWithTailCalls && configured::Sanitizers.empty())
        CHECK(passed == "gnu::musttail");
    // A compiler no row names, MSVC's, is not probed at all.
    if (!std::ranges::any_of(configured::TailCallProbeFamilies, [](std::string_view families) {
            return familiesOf(families).contains(configured::CompilerFamily);
        }))
        CHECK(passed.empty());
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

TEST_CASE("Dispatch.nestedInvocationsFitASmallThread")
{
    // f(n) = n == 0 ? 0 : reenter(n - 1) + 1, the host invoking f again: every level nests an invocation on
    // the host stack, StoreLimits::maxNestedInvocations of them at the most, and they fit a small thread
    // (SmallStackBytes) on every dispatch. One level more traps, on the same thread.
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto module = ModuleBuilder {};
    module.type(FuncType { .params = { binary::I32 }, .results = { binary::I32 } });
    module.importFunction("env", "reenter", 0);
    module.function(0,
                    {},
                    code({ localGet(0),
                           { 0x45, 0x04, 0x7F },
                           i32Const(0),
                           { 0x05 },
                           localGet(0),
                           i32Const(1),
                           op(0x6B),
                           op(0x10, 0),
                           i32Const(1),
                           op(0x6A),
                           { 0x0B } }));
    Harness* self = nullptr;
    auto harness =
        Harness { module, interpreterUnder(dispatch), [&self](Store& store) {
                     auto const host = store.addHostFunction(
                         FuncType { .params = { binary::I32 }, .results = { binary::I32 } },
                         makeHostFunction(
                             [&self](HostContext& context,
                                     std::span<Value const> args,
                                     std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
                                 return context.invoke(self->function(1), args, results);
                             }));
                     REQUIRE(host.has_value());
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = *host } };
                 } };
    self = &harness;
    auto const levels = static_cast<int32_t>(StoreLimits {}.maxNestedInvocations) - 1;
    auto deepest = std::expected<std::vector<Value>, Abrupt> {};
    auto beyond = std::expected<std::vector<Value>, Abrupt> {};
    auto const ran = runOnSmallStack(SmallStackBytes, [&] {
        deepest = harness.outcome(1, { I32 { static_cast<uint32_t>(levels) } });
        beyond = harness.outcome(1, { I32 { static_cast<uint32_t>(levels + 1) } });
    });
    REQUIRE(ran.has_value());
    CHECK(deepest == std::vector<Value> { I32 { static_cast<uint32_t>(levels) } });
    CHECK(beyond
          == std::expected<std::vector<Value>, Abrupt> {
              std::unexpected { Abrupt { Trap { TrapCode::CallStackExhausted } } } });
    CHECK(harness.store().chargedStackValues() == 0);
}
