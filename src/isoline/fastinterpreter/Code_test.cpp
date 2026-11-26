// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>

using isoline::fastinterpreter::availableDispatches;
using isoline::fastinterpreter::bestDispatch;
using isoline::fastinterpreter::bodyUnits;
using isoline::fastinterpreter::Dispatch;
using isoline::fastinterpreter::LoweredFunction;
using isoline::fastinterpreter::LoweredModule;
using isoline::fastinterpreter::LoweringStats;
using isoline::fastinterpreter::MaxWordsPerBodyUnit;
using isoline::fastinterpreter::OperationCount;
using isoline::fastinterpreter::OperationForm;
using isoline::fastinterpreter::OperationId;
using isoline::fastinterpreter::operationRow;
using isoline::fastinterpreter::OperationTable;
using isoline::fastinterpreter::operationWords;
using isoline::fastinterpreter::Word;
using isoline::runtime::ExecutionEngine;
using isoline::runtime::ExecutionMode;

namespace binary = isoline::binary;

namespace
{

/// A function of @p resource whose code is @p words header words.
[[nodiscard]] LoweredFunction functionOf(std::pmr::memory_resource& resource,
                                         uint32_t codeIndex,
                                         std::size_t words)
{
    auto function = LoweredFunction(&resource);
    function.codeIndex = codeIndex;
    function.code.assign(words, Word { std::to_underlying(OperationId::Nop) });
    return function;
}

} // namespace

TEST_CASE("OperationTable.oneRowPerIdInOrder")
{
    // Mirrors the header's static_assert at run time, so its rows count as covered.
    for (auto const index: std::views::iota(std::size_t { 0 }, OperationTable.size()))
    {
        auto const& row = OperationTable[index];
        CHECK(std::to_underlying(row.id) == index);
        CHECK(&operationRow(row.id) == &row);
        CHECK_FALSE(row.name.empty());
        CHECK_FALSE(row.text.empty());
        CHECK(row.words == operationWords(row.slots, row.immediates));
        CHECK(row.words <= MaxWordsPerBodyUnit);
    }
    auto names = std::set<std::string_view> {};
    auto texts = std::set<std::string_view> {};
    for (auto const& row: OperationTable)
    {
        CHECK(names.insert(row.name).second);
        CHECK(texts.insert(row.text).second);
    }
    CHECK(OperationCount == OperationTable.size());

    // The operations the code model starts with, before lowering adds its own.
    auto const first = std::to_array<std::tuple<std::string_view, OperationForm, uint32_t>>({
        { "Unreachable", OperationForm::Nullary, 1 },
        { "Nop", OperationForm::Nullary, 1 },
        { "Move64", OperationForm::Move, 2 },
        { "Move128", OperationForm::Move, 2 },
        { "MoveRange", OperationForm::Move, 3 },
        { "Const64", OperationForm::Move, 3 },
        { "Const128", OperationForm::Move, 4 },
        { "Return", OperationForm::Special, 2 },
    });
    REQUIRE(OperationTable.size() >= first.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, first.size()))
    {
        auto const& [name, form, words] = first[index];
        CHECK(OperationTable[index].name == name);
        CHECK(OperationTable[index].form == form);
        CHECK(OperationTable[index].words == words);
    }
}

TEST_CASE("Dispatch.switchIsAlwaysAvailable")
{
    auto const available = availableDispatches();
    REQUIRE_FALSE(available.empty());
    CHECK(available.front() == Dispatch::Switch);
    CHECK(std::ranges::find(available, bestDispatch()) != available.end());
    auto const tailCall = std::ranges::find(available, Dispatch::TailCall) != available.end();
    CHECK((bestDispatch() == Dispatch::TailCall) == tailCall);
}

TEST_CASE("Code.bodyUnitsCountEveryDecodedPart")
{
    // A decoded body unit is an instruction, a br_table label, a catch clause, a typed select's type
    // or a local run: each took at least one byte of the body, so a bound per unit is one per byte.
    auto function = binary::Function {};
    CHECK(bodyUnits(function) == 0);
    function.body.resize(5);
    function.brTableLabels.resize(7);
    function.catchClauses.resize(2);
    function.selectTypes.resize(3);
    function.locals.resize(11);
    CHECK(bodyUnits(function) == 5 + 7 + 2 + 3 + 11);
}

TEST_CASE("Code.loweredModuleKeepsItsArena")
{
    // The module holds the resource its containers allocate from, so the code outlives the caller's
    // handle on it.
    auto arena = std::make_shared<std::pmr::monotonic_buffer_resource>();
    auto functions = std::pmr::vector<LoweredFunction>(arena.get());
    functions.push_back(functionOf(*arena, 0, 3));
    functions.push_back(functionOf(*arena, 1, 5));
    auto const* const resource = arena.get();
    auto const module = LoweredModule { std::move(arena),
                                        std::move(functions),
                                        Dispatch::Switch,
                                        ExecutionMode::Deterministic,
                                        1'234,
                                        LoweringStats { .bodyUnits = 5, .work = 7 } };

    CHECK(module.engine() == ExecutionEngine::Fast);
    CHECK(module.dispatch() == Dispatch::Switch);
    CHECK(module.mode() == ExecutionMode::Deterministic);
    CHECK(module.loweredBytes() == 1'234);
    CHECK(module.stats() == LoweringStats { .bodyUnits = 5, .work = 7 });
    REQUIRE(module.functions().size() == 2);
    CHECK(module.functions()[1].codeIndex == 1);
    CHECK(module.functions()[1].code.size() == 5);
    CHECK(module.functions()[1].code[4] == Word { std::to_underlying(OperationId::Nop) });
    CHECK(module.functions()[1].code.get_allocator().resource() == resource);
}

// The contract tests below end the process at a failed Require, so they are hidden from the unit run:
// tests/CMakeLists.txt runs each in a process of its own and expects the Require's text
// (isoline::runtime::testing::endAtContractViolation()).

TEST_CASE("Contract.Code.everyContainerUsesTheArena", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto arena = std::make_shared<std::pmr::monotonic_buffer_resource>();
    auto other = std::pmr::monotonic_buffer_resource {};
    auto functions = std::pmr::vector<LoweredFunction>(arena.get());
    functions.push_back(functionOf(*arena, 0, 1));
    functions.push_back(functionOf(*arena, 1, 1));
    // One container of one function, the last the check reads, allocates from elsewhere.
    auto& catches = functions.back().catches;
    std::destroy_at(&catches);
    std::construct_at(&catches, &other);
    std::ignore = LoweredModule {
        std::move(arena), std::move(functions), Dispatch::Switch, ExecutionMode::Production, 0,
        LoweringStats {}
    };
    FAIL("SURVIVED: a function allocating from another resource outlives the arena check");
}
