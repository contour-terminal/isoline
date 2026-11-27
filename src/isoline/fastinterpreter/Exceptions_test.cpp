// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/FastInterpreter.hpp>
#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/fastinterpreter/testing/Engines.hpp>
#include <isoline/fastinterpreter/testing/HostileModules.hpp>
#include <isoline/fastinterpreter/testing/Lowering.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>

// The fast interpreter's exceptions against the reference interpreter's: what a throw allocates and where
// it is caught, the polls and steps its unwinding and its sweeps take, and what the sweeps keep.

using isoline::binary::CatchKind;
using isoline::binary::ExternKind;
using isoline::binary::FuncType;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::i64Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::localSet;
using isoline::binary::testing::ModuleBuilder;
using isoline::fastinterpreter::LoweredModule;
using isoline::fastinterpreter::OperationFamily;
using isoline::fastinterpreter::OperationTable;
using isoline::fastinterpreter::recordBounds;
using isoline::fastinterpreter::testing::CountingResource;
using isoline::fastinterpreter::testing::EngineUnderTest;
using isoline::fastinterpreter::testing::fastEngineOfEachDispatch;
using isoline::fastinterpreter::testing::Harness;
using isoline::fastinterpreter::testing::loweredWithinTheBounds;
using isoline::fastinterpreter::testing::op;
using isoline::fastinterpreter::testing::operationsOf;
using isoline::fastinterpreter::testing::Reference;
using isoline::runtime::Abrupt;
using isoline::runtime::ExternValue;
using isoline::runtime::FuncAddr;
using isoline::runtime::HostContext;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Limit;
using isoline::runtime::makeHostFunction;
using isoline::runtime::PollSite;
using isoline::runtime::PollSiteTable;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::Thrown;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::UnwindPollItems;
using isoline::runtime::Value;
using isoline::runtime::testing::PollCounter;

namespace binary = isoline::binary;

namespace
{

/// What an invocation ended with.
using Outcome = std::expected<std::vector<Value>, Abrupt>;

/// What a host function returns.
using HostOutcome = std::expected<void, Abrupt>;

/// Block types' bytes: none, i32, exnref; a type index below 64 is its own byte.
constexpr auto NoResult = uint8_t { 0x40 };
constexpr auto I32Result = uint8_t { 0x7F };
constexpr auto ExnRefResult = uint8_t { 0x69 };

/// One catch clause, as a try_table's immediate spells it.
struct Clause
{
    CatchKind kind = CatchKind::CatchAll; ///< What it catches and hands its label.
    uint32_t tag = 0;                     ///< Its tag; Catch and CatchRef only.
    uint32_t label = 0;                   ///< Its label, counted from outside the try_table.
};

/// The opening of a try_table of block type @p blockType with @p clauses.
[[nodiscard]] Bytes tryTable(uint8_t blockType, std::vector<Clause> const& clauses)
{
    auto bytes = code({ { 0x1F, blockType }, encodeU32(static_cast<uint32_t>(clauses.size())) });
    for (auto const& clause: clauses)
    {
        bytes.push_back(static_cast<uint8_t>(clause.kind));
        if (binary::catchesByTag(clause.kind))
            bytes = code({ bytes, encodeU32(clause.tag) });
        bytes = code({ bytes, encodeU32(clause.label) });
    }
    return bytes;
}

/// throw @p tag.
[[nodiscard]] Bytes throwTag(uint32_t tag)
{
    return code({ { 0x08 }, encodeU32(tag) });
}

/// throw_ref.
[[nodiscard]] Bytes throwRef()
{
    return { 0x0A };
}

/// end.
[[nodiscard]] Bytes end()
{
    return { 0x0B };
}

/// @p inner in @p depth blocks of no result.
[[nodiscard]] Bytes inBlocks(uint32_t depth, Bytes const& inner)
{
    auto bytes = Bytes {};
    bytes.reserve((4 * std::size_t { depth }) + inner.size());
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, depth))
        bytes.insert(bytes.end(), { 0x02, NoResult });
    bytes.insert(bytes.end(), inner.begin(), inner.end());
    bytes.insert(bytes.end(), depth, 0x0B);
    return bytes;
}

[[nodiscard]] Value i32(int32_t value)
{
    return I32 { static_cast<uint32_t>(value) };
}

/// Adds a host function of @p type implemented by @p function to @p store.
template <typename F>
[[nodiscard]] FuncAddr addHost(Store& store, FuncType type, F function)
{
    auto const address = store.addHostFunction(std::move(type), makeHostFunction(std::move(function)));
    REQUIRE(address.has_value());
    return *address;
}

/// The imports of one host function, of @p type, implemented by @p function.
template <typename F>
[[nodiscard]] Harness::Imports hostImport(FuncType const& type, F const& function)
{
    return [type, function](Store& store) {
        return std::vector { ExternValue { .kind = ExternKind::Func,
                                           .address = addHost(store, type, function) } };
    };
}

/// A host function that throws an exception of tag @p tag of its caller's instance, with payload @p payload.
[[nodiscard]] auto throwingHost(uint32_t tag, std::vector<Value> const& payload)
{
    return [tag,
            payload](HostContext& context, std::span<Value const>, std::span<Value>) noexcept -> HostOutcome {
        auto const caller = context.callerInstance();
        if (!caller)
            return std::unexpected { Abrupt { Trap { TrapCode::Unreachable } } };
        auto const address = context.store().moduleInstance(*caller).tags.at(tag);
        auto const exception = context.addException(address, payload);
        if (!exception)
            return std::unexpected { exception.error() };
        return std::unexpected { Abrupt { Thrown { .exception = *exception } } };
    };
}

/// One invocation of a sequence: the function and its arguments.
struct Call
{
    uint32_t function = 0;   ///< The function's index in the module.
    std::vector<Value> args; ///< Its arguments.
};

/// What one engine did over a sequence of invocations on one store, after each of them.
struct Observation
{
    std::vector<Outcome> outcomes;            ///< How each ended.
    std::vector<uint64_t> steps;              ///< The store's used steps after each.
    std::vector<std::size_t> live;            ///< The store's live exceptions after each.
    std::vector<uint64_t> charged;            ///< The store's charged stack values after each.
    std::vector<std::vector<uint64_t>> polls; ///< The polls per site after each; empty unobserved.

    /// Whether both did the same.
    [[nodiscard]] bool operator==(Observation const&) const = default;
};

/// Whether an Observation counts the store's polls.
enum class Observed : uint8_t
{
    No,  ///< No observer: the poll's fast path.
    Yes, ///< A PollCounter: every poll goes through the store.
};

/// Runs @p calls on @p module, instantiated by @p engine on one store of @p limits whose imports @p imports
/// adds, and records what each did.
[[nodiscard]] Observation observe(ModuleBuilder const& module,
                                  EngineUnderTest engine,
                                  std::vector<Call> const& calls,
                                  StoreLimits limits = {},
                                  Harness::Imports const& imports = {},
                                  Observed observed = Observed::Yes)
{
    auto counter = PollCounter {};
    auto harness =
        Harness { module, engine, imports, limits, observed == Observed::Yes ? &counter : nullptr };
    auto observation = Observation {};
    for (auto const& call: calls)
    {
        observation.outcomes.push_back(harness.outcome(call.function, call.args));
        observation.steps.push_back(harness.store().used(Limit::Steps));
        observation.live.push_back(harness.store().liveExceptions());
        observation.charged.push_back(harness.store().chargedStackValues());
        if (observed == Observed::Yes)
        {
            auto& polls = observation.polls.emplace_back();
            for (auto const& row: PollSiteTable)
                polls.push_back(counter.count(row.site));
        }
    }
    return observation;
}

/// Runs @p calls on the reference interpreter and on @p engine, observed and unobserved: each must do what
/// the reference does, invocation by invocation, and give back every stack charge.
/// @return The reference's observed run.
Observation checkAgainstReference(ModuleBuilder const& module,
                                  EngineUnderTest engine,
                                  std::vector<Call> const& calls,
                                  StoreLimits limits = {},
                                  Harness::Imports const& imports = {})
{
    auto const reference = observe(module, Reference, calls, limits, imports);
    CHECK(observe(module, engine, calls, limits, imports) == reference);
    CHECK(observe(module, engine, calls, limits, imports, Observed::No)
          == observe(module, Reference, calls, limits, imports, Observed::No));
    CHECK(std::ranges::all_of(reference.charged, [](uint64_t charged) { return charged == 0; }));
    return reference;
}

/// The polls @p observation counted at @p site by its last invocation.
[[nodiscard]] uint64_t pollsAt(Observation const& observation, PollSite site)
{
    REQUIRE_FALSE(observation.polls.empty());
    return observation.polls.back()[static_cast<std::size_t>(site)];
}

/// The exception @p outcome ended with, which must be an uncaught one.
[[nodiscard]] Thrown thrownOf(Outcome const& outcome)
{
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(std::holds_alternative<Thrown>(outcome.error()));
    return std::get<Thrown>(outcome.error());
}

/// The type indices the modules below share.
enum TypeIndex : uint8_t
{
    Nullary = 0,       ///< [] -> []
    UnaryI32 = 1,      ///< [i32] -> []
    ToI32 = 2,         ///< [] -> [i32]
    ExnToI32 = 3,      ///< [exnref] -> [i32]
    FromExn = 4,       ///< [exnref] -> []
    ArgumentsType = 5, ///< [i32 i64 exnref] -> []
    ToI32AndExn = 6,   ///< [] -> [i32 exnref]
    I32ToI32 = 7,      ///< [i32] -> [i32]
};

/// A module of the shared types (TypeIndex) and two tags: 0 of [i32], 1 of [].
[[nodiscard]] ModuleBuilder withTypesAndTags()
{
    auto module = ModuleBuilder {};
    module.type(FuncType {});
    module.type(FuncType { .params = { binary::I32 }, .results = {} });
    module.type(FuncType { .params = {}, .results = { binary::I32 } });
    module.type(FuncType { .params = { binary::ExnRef }, .results = { binary::I32 } });
    module.type(FuncType { .params = { binary::ExnRef }, .results = {} });
    module.type(FuncType { .params = { binary::I32, binary::I64, binary::ExnRef }, .results = {} });
    module.type(FuncType { .params = {}, .results = { binary::I32, binary::ExnRef } });
    module.type(FuncType { .params = { binary::I32 }, .results = { binary::I32 } });
    module.tag(UnaryI32);
    module.tag(Nullary);
    return module;
}

/// The body of a () -> () function that throws tag 1 and catches it @p rounds times, each round allocating
/// an exception nothing keeps; local 0 counts the rounds.
[[nodiscard]] Bytes throwAndCatchRounds(int32_t rounds)
{
    return code({ { 0x03, NoResult, 0x02, NoResult },
                  tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                  throwTag(1),
                  end(),
                  end(),
                  localGet(0),
                  i32Const(1),
                  { 0x6A, 0x22, 0x00 }, // i32.add local.tee 0
                  i32Const(rounds),
                  { 0x49, 0x0D, 0x00 }, // i32.lt_u br_if 0
                  end() });
}

} // namespace

TEST_CASE("Exceptions.catchToLoopPolls")
{
    // Review Focus 2: loop $l try_table (catch_all $l) ... throw end end, 100 rounds: each catch goes to the
    // loop's label, a back-edge, which polls as the reference does.
    auto const engine = fastEngineOfEachDispatch();
    auto module = withTypesAndTags();
    module.function(ToI32,
                    { binary::I32 },
                    code({ { 0x03, NoResult },
                           tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                           localGet(0),
                           i32Const(1),
                           { 0x6A, 0x22, 0x00 }, // i32.add local.tee 0
                           i32Const(100),
                           { 0x49, 0x04, NoResult }, // i32.lt_u if
                           throwTag(1),
                           end(),
                           end(),
                           end(),
                           localGet(0) }));
    auto const reference = checkAgainstReference(module, engine, { { .function = 0, .args = {} } });
    CHECK(reference.outcomes.front() == Outcome { std::vector { i32(100) } });
    CHECK(pollsAt(reference, PollSite::LoopBackEdge) == 99);
}

TEST_CASE("Exceptions.unwindPollsMatchReference")
{
    auto const engine = fastEngineOfEachDispatch();
    SECTION("past three frames of 2'000 labels each")
    {
        // f0 throws inside 2'000 blocks; f1 calls it inside 1'997 blocks, a try_table of three clauses of tag
        // 1, which do not match, and a block; f2 calls f1 inside 2'000 blocks, uncaught, and f3 inside 1'999
        // and a try_table whose catch_all goes to the function's label. 6'005 items either way: one Unwind
        // poll.
        auto module = withTypesAndTags();
        module.function(Nullary, {}, inBlocks(2'000, code({ i32Const(5), throwTag(0) })));
        module.function(Nullary,
                        {},
                        code({ { 0x02, NoResult },
                               tryTable(NoResult,
                                        { { .kind = CatchKind::Catch, .tag = 1, .label = 0 },
                                          { .kind = CatchKind::Catch, .tag = 1, .label = 0 },
                                          { .kind = CatchKind::Catch, .tag = 1, .label = 0 } }),
                               inBlocks(1'997, op(0x10, 0)),
                               end(),
                               end() }));
        module.function(Nullary, {}, inBlocks(2'000, op(0x10, 1)));
        module.function(Nullary,
                        {},
                        code({ tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                               inBlocks(1'999, op(0x10, 1)),
                               end() }));
        auto const uncaught = checkAgainstReference(module, engine, { { .function = 2, .args = {} } });
        CHECK(thrownOf(uncaught.outcomes.front()).exception == 0);
        CHECK(pollsAt(uncaught, PollSite::Unwind) == 1);
        auto const caught = checkAgainstReference(module, engine, { { .function = 3, .args = {} } });
        CHECK(caught.outcomes.front() == Outcome { std::vector<Value> {} });
        CHECK(pollsAt(caught, PollSite::Unwind) == 1);
    }
    SECTION("a host's exception past 4'095, 4'096 and 4'097 items")
    {
        // f1 calls the throwing host inside k blocks, uncaught: k labels and its frame. f2 calls it inside a
        // try_table of one catch_all to the function's label and k - 1 blocks: k labels and the clause.
        for (auto const items: { uint32_t { 4'095 }, uint32_t { 4'096 }, uint32_t { 4'097 } })
        {
            INFO(items << " items");
            auto const blocks = items - 1;
            auto module = withTypesAndTags();
            module.importFunction("env", "throw", Nullary);
            module.function(Nullary, {}, inBlocks(blocks, op(0x10, 0)));
            module.function(Nullary,
                            {},
                            code({ tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                                   inBlocks(blocks - 1, op(0x10, 0)),
                                   end() }));
            auto const imports = hostImport(FuncType {}, throwingHost(0, { i32(3) }));
            auto const expected = items >= UnwindPollItems ? 1U : 0U;
            for (auto const function: { 1U, 2U })
            {
                INFO("function " << function);
                auto const reference = checkAgainstReference(
                    module, engine, { { .function = function, .args = {} } }, {}, imports);
                CHECK(pollsAt(reference, PollSite::Unwind) == expected);
            }
        }
    }
}

namespace
{

/// The order of a try_table's two catch-alls, of which only the first can match.
enum class CatchAllOrder : uint8_t
{
    ToTheLoopFirst,     ///< catch_all, then catch_all_ref.
    ToTheFunctionFirst, ///< catch_all_ref, then catch_all.
};

/// A module whose function 0, (i32 selector) -> exnref, runs one try_table of the four kinds of clause, each
/// to a label of its own: catch of tag 0 hands its payload to a block, catch_ref of tag 2 ([i64 i32]) the
/// payload and the exnref to a block, catch_all nothing to the loop around, catch_all_ref the exnref to the
/// function's label, which returns it; the catch-alls in @p order. Selector 0 throws tag 0 with 7, which
/// comes back as tag 0 with 1'007, thrown on uncaught; 1 throws tag 2, rethrown by its exnref uncaught; 2
/// throws tag 1 until local 1 counts 3, then returns null; 3 throws tag 1.
[[nodiscard]] ModuleBuilder everyClauseKind(CatchAllOrder order)
{
    auto module = withTypesAndTags();
    module.type(FuncType { .params = { binary::I64, binary::I32 }, .results = {} });                 // 8
    module.type(FuncType { .params = {}, .results = { binary::I64, binary::I32, binary::ExnRef } }); // 9
    module.type(FuncType { .params = { binary::I32 }, .results = { binary::ExnRef } });              // 10
    module.tag(8);
    auto const toTheLoop = Clause { .kind = CatchKind::CatchAll, .label = 3 };
    auto const toTheFunction = Clause { .kind = CatchKind::CatchAllRef, .label = 4 };
    auto clauses = std::vector<Clause> { { .kind = CatchKind::Catch, .tag = 0, .label = 2 },
                                         { .kind = CatchKind::CatchRef, .tag = 2, .label = 1 } };
    if (order == CatchAllOrder::ToTheLoopFirst)
        clauses.insert(clauses.end(), { toTheLoop, toTheFunction });
    else
        clauses.insert(clauses.end(), { toTheFunction, toTheLoop });
    module.function(10,
                    { binary::I32 },
                    code({ { 0x03, I32Result }, // loop $again (label 3 to the clauses): [] -> [i32]
                           { 0x02, I32Result }, // block $payload (label 2)
                           { 0x02, 0x09 },      // block $both (type 9, label 1)
                           { 0x02, NoResult },  // block $none (label 0)
                           tryTable(NoResult, clauses),
                           localGet(0),
                           { 0x45, 0x04, NoResult }, // i32.eqz if
                           i32Const(7),
                           throwTag(0),
                           end(),
                           localGet(0),
                           i32Const(1),
                           { 0x46, 0x04, NoResult }, // i32.eq if
                           i64Const(11),
                           i32Const(13),
                           throwTag(2),
                           end(),
                           localGet(0),
                           i32Const(2),
                           { 0x46, 0x04, NoResult }, // i32.eq if
                           localGet(1),
                           i32Const(1),
                           { 0x6A, 0x22, 0x01 }, // i32.add local.tee 1
                           i32Const(3),
                           { 0x49, 0x04, NoResult }, // i32.lt_u if
                           throwTag(1),
                           end(),
                           { 0xD0, 0x69, 0x0F }, // ref.null exn return
                           end(),
                           throwTag(1),
                           end(),    // try_table
                           end(),    // $none
                           { 0x00 }, // unreachable
                           end(),    // $both: i64 i32 exnref
                           throwRef(),
                           end(), // $payload: the i32
                           end(), // the loop
                           i32Const(1'000),
                           { 0x6A },
                           throwTag(0) }));
    return module;
}

} // namespace

TEST_CASE("Exceptions.everyCatchKindAndLabel")
{
    // A try_table whose clauses are of every kind, to a block, a block of several values, a loop and the
    // function's label: what each hands and where it goes, the clauses tried before the one that matches,
    // and the back-edge a catch to the loop polls, as the reference does.
    auto const engine = fastEngineOfEachDispatch();
    auto const loopFirst = everyClauseKind(CatchAllOrder::ToTheLoopFirst);
    auto const reference = checkAgainstReference(loopFirst,
                                                 engine,
                                                 { { .function = 0, .args = { i32(0) } },
                                                   { .function = 0, .args = { i32(1) } },
                                                   { .function = 0, .args = { i32(2) } } });
    auto harness = Harness { loopFirst, engine };
    auto const thrown = thrownOf(harness.outcome(0, { i32(0) }));
    CHECK(harness.store().exception(thrown.exception).payload == std::vector { i32(1'007) });
    CHECK(thrown == thrownOf(reference.outcomes[0]));
    auto const rethrown = thrownOf(harness.outcome(0, { i32(1) }));
    CHECK(harness.store().exception(rethrown.exception).payload
          == std::vector<Value> { I64 { 11 }, i32(13) });
    REQUIRE(reference.outcomes[2].has_value());
    CHECK(std::get<Ref>(reference.outcomes[2]->front()).kind == RefKind::Null);
    CHECK(pollsAt(reference, PollSite::LoopBackEdge) == 2);

    auto const functionFirst = checkAgainstReference(
        everyClauseKind(CatchAllOrder::ToTheFunctionFirst),
        engine,
        { { .function = 0, .args = { i32(2) } }, { .function = 0, .args = { i32(3) } } });
    for (auto const& outcome: functionFirst.outcomes)
    {
        REQUIRE(outcome.has_value());
        CHECK(std::get<Ref>(outcome->front()).kind == RefKind::Exn);
    }
}

TEST_CASE("Exceptions.slotHeldExnrefSurvivesSweep")
{
    // Review Focus 6: an exnref caught by catch_ref and held only in a local, or only in an operand below a
    // call (the local it came from cleared after a first call found the operand there), across 40
    // throw-and-catch rounds of a callee in a store of 16 exceptions, whose sweeps reclaim the rounds'
    // exceptions: rethrown, it is caught with its payload, 42.
    auto const engine = fastEngineOfEachDispatch();
    auto module = withTypesAndTags();
    module.function(Nullary, { binary::I32 }, throwAndCatchRounds(40));
    // block (result i32 exnref) try_table (catch_ref 0 0) i32.const 42 throw 0 end unreachable end: the
    // payload and the exnref, of which local 0 keeps the exnref.
    auto const caughtByRef =
        code({ { 0x02, ToI32AndExn },
               tryTable(NoResult, { { .kind = CatchKind::CatchRef, .tag = 0, .label = 0 } }),
               i32Const(42),
               throwTag(0),
               end(),
               { 0x00 },
               end(),
               localSet(0),
               { 0x1A } });
    // In a local.
    module.function(ToI32,
                    { binary::ExnRef },
                    code({ caughtByRef,
                           op(0x10, 0),
                           { 0x02, I32Result },
                           tryTable(NoResult, { { .kind = CatchKind::Catch, .tag = 0, .label = 0 } }),
                           localGet(0),
                           throwRef(),
                           end(),
                           { 0x00 },
                           end() }));
    // In an operand.
    module.function(ToI32,
                    { binary::ExnRef },
                    code({ caughtByRef,
                           localGet(0),
                           op(0x10, 0),    // a site that finds the operand while the local still holds it
                           { 0xD0, 0x69 }, // ref.null exn
                           localSet(0),
                           op(0x10, 0),
                           { 0x02, ExnToI32 },
                           tryTable(FromExn, { { .kind = CatchKind::Catch, .tag = 0, .label = 0 } }),
                           throwRef(),
                           end(),
                           { 0x00 },
                           end() }));
    auto const limits = StoreLimits { .maxExceptions = 16 };
    for (auto const function: { 1U, 2U })
    {
        INFO("function " << function);
        auto const reference = checkAgainstReference(
            module,
            engine,
            { { .function = function, .args = {} }, { .function = function, .args = {} } },
            limits);
        CHECK(reference.outcomes.front() == Outcome { std::vector { i32(42) } });
        CHECK(reference.outcomes.back() == Outcome { std::vector { i32(42) } });
        CHECK(pollsAt(reference, PollSite::Sweep) > 0);
    }
}

namespace
{

/// A module whose function 1, () -> (), holds an exnref in local 3, pushes @p operands operands of mixed
/// types, forwarded locals and the exnref among them, and calls function 0 with three arguments (i32, i64,
/// exnref); function 0 throws and catches 10 times. Function 1 then drops them all.
[[nodiscard]] ModuleBuilder wideStackBelowAThrowingCall(uint32_t operands)
{
    auto module = withTypesAndTags();
    module.function(ArgumentsType,
                    { binary::I32 },
                    code({ { 0x03, NoResult, 0x02, NoResult },
                           tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                           throwTag(1),
                           end(),
                           end(),
                           localGet(3),
                           i32Const(1),
                           { 0x6A, 0x22, 0x03 },
                           i32Const(10),
                           { 0x49, 0x0D, 0x00 },
                           end() }));
    auto body = code({ { 0x02, ExnRefResult },
                       tryTable(NoResult, { { .kind = CatchKind::CatchAllRef, .label = 0 } }),
                       throwTag(1),
                       end(),
                       { 0x00 },
                       end(),
                       localSet(3) });
    // The operands, in a cycle of every kind the frame holds them as.
    auto const pieces = std::to_array<Bytes>({
        i32Const(7),
        i64Const(-3),
        { 0x43, 0x00, 0x00, 0x80, 0x3F },       // f32.const 1
        { 0x44, 0, 0, 0, 0, 0, 0, 0xF0, 0x3F }, // f64.const 1
        localGet(0),
        localGet(1),
        localGet(3),
        { 0xD0, 0x70 }, // ref.null func
    });
    for (auto const index: std::views::iota(uint32_t { 0 }, operands))
    {
        auto const& piece = pieces[index % pieces.size()];
        body.insert(body.end(), piece.begin(), piece.end());
    }
    auto const call = code({ i32Const(1), i64Const(2), localGet(3), op(0x10, 0) });
    body.insert(body.end(), call.begin(), call.end());
    body.insert(body.end(), operands, 0x1A); // drop
    module.function(Nullary, { binary::I32, binary::I64, binary::F64, binary::ExnRef }, body);
    return module;
}

} // namespace

TEST_CASE("Exceptions.sweepChargesLikeReference", "[memory]")
{
    // Review Focus 6: 100'000 operands of mixed types below a throwing call in a store of 8 exceptions: every
    // sweep visits as many values as the reference's, so it polls as often and charges as many steps. A
    // count off by any number shows at the operand count where the reference's sweeps start to poll once
    // more, which the reference itself finds: there, and one below it.
    auto const engine = fastEngineOfEachDispatch();
    auto const limits = StoreLimits { .maxExceptions = 8 };
    auto const calls = std::vector<Call> { { .function = 1, .args = {} },
                                           { .function = 1, .args = {} },
                                           { .function = 1, .args = {} } };
    auto const sweepPolls = [&](uint32_t operands) {
        return pollsAt(observe(wideStackBelowAThrowingCall(operands), Reference, calls, limits),
                       PollSite::Sweep);
    };
    constexpr auto Base = uint32_t { 100'000 };
    auto const basePolls = sweepPolls(Base);
    REQUIRE(basePolls > 0);
    // The first count that polls once more, within one poll granule above the base.
    auto low = Base;
    auto high = Base + static_cast<uint32_t>(isoline::runtime::SweepPollItems);
    REQUIRE(sweepPolls(high) > basePolls);
    while (high - low > 1)
    {
        auto const middle = low + ((high - low) / 2);
        if (sweepPolls(middle) > basePolls)
            high = middle;
        else
            low = middle;
    }
    for (auto const operands: { high - 1, high })
    {
        INFO(operands << " operands");
        auto const reference =
            checkAgainstReference(wideStackBelowAThrowingCall(operands), engine, calls, limits);
        CHECK(reference.outcomes.back() == Outcome { std::vector<Value> {} });
    }
}

TEST_CASE("Exceptions.sweepReadsRunsOfResultsAndParams")
{
    // f1 returns an i32 and an exnref; f2 keeps both, a run of a call's results, below a call of f0, whose
    // throws and catches sweep a store of 16 exceptions; f3 hands both to an if as its params, and calls f0
    // in either arm, the else's holding them as a run of params. Each sweep reads them as the reference's
    // operand stack holds them.
    auto const engine = fastEngineOfEachDispatch();
    auto module = withTypesAndTags();
    module.type(FuncType { .params = { binary::I32, binary::ExnRef }, .results = {} }); // 8
    module.function(Nullary, { binary::I32 }, throwAndCatchRounds(40));
    module.function(ToI32AndExn,
                    {},
                    code({ { 0x02, ToI32AndExn },
                           tryTable(NoResult, { { .kind = CatchKind::CatchRef, .tag = 0, .label = 0 } }),
                           i32Const(42),
                           throwTag(0),
                           end(),
                           { 0x00 },
                           end() }));
    module.function(Nullary, {}, code({ op(0x10, 1), op(0x10, 0), { 0x1A, 0x1A } }));
    module.function(UnaryI32,
                    {},
                    code({ op(0x10, 1),
                           localGet(0),
                           { 0x04, 0x08 }, // if (param i32 exnref)
                           op(0x10, 0),
                           { 0x1A, 0x1A, 0x05 }, // drop drop else
                           op(0x10, 0),
                           { 0x1A, 0x1A }, // drop drop
                           end() }));
    for (auto const& call: std::to_array<Call>({ { .function = 2, .args = {} },
                                                 { .function = 3, .args = { i32(1) } },
                                                 { .function = 3, .args = { i32(0) } } }))
    {
        INFO("function " << call.function);
        auto const reference = checkAgainstReference(module, engine, { call }, { .maxExceptions = 16 });
        CHECK(reference.outcomes.front() == Outcome { std::vector<Value> {} });
        CHECK(pollsAt(reference, PollSite::Sweep) > 0);
    }
}

TEST_CASE("Exceptions.unwoundFramesHoldNoRoots")
{
    // A callee catches an exception into its local and throws another out; its caller catches that, then
    // throws and catches until the store of 4 exceptions sweeps. The callee's frame is gone: its local's
    // exnref is no root any more, so the sweep reclaims what the reference's does, and as many.
    auto const engine = fastEngineOfEachDispatch();
    auto module = withTypesAndTags();
    module.function(Nullary, { binary::I32 }, throwAndCatchRounds(12));
    module.function(Nullary,
                    { binary::ExnRef },
                    code({ { 0x02, ExnRefResult },
                           tryTable(NoResult, { { .kind = CatchKind::CatchAllRef, .label = 0 } }),
                           throwTag(1),
                           end(),
                           { 0x00 },
                           end(),
                           localSet(0),
                           i32Const(9),
                           throwTag(0) }));
    module.function(Nullary,
                    {},
                    code({ { 0x02, NoResult },
                           tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                           op(0x10, 1),
                           end(),
                           end(),
                           op(0x10, 0) }));
    auto const reference =
        checkAgainstReference(module,
                              engine,
                              { { .function = 2, .args = {} }, { .function = 2, .args = {} } },
                              { .maxExceptions = 4 });
    CHECK(pollsAt(reference, PollSite::Sweep) > 0);
}

TEST_CASE("Exceptions.rootsScratchComesFromTheSlotMemory")
{
    // One frame throws and catches 40 times in a store of 16 exceptions: the slot memory gives the frame its
    // one chunk, and its sweeps the roots' scratch buffer. With room for every exception, nothing sweeps, and
    // the chunk is all it gives.
    auto const engine = fastEngineOfEachDispatch();
    auto module = withTypesAndTags();
    module.function(Nullary, { binary::I32 }, throwAndCatchRounds(40));
    auto const allocationsWith = [&](StoreLimits limits) {
        auto memory = CountingResource {};
        auto under = engine;
        under.slotMemory = &memory;
        auto harness = Harness { module, under, limits };
        CHECK(harness.outcome(0) == Outcome { std::vector<Value> {} });
        return memory.allocations();
    };
    CHECK(allocationsWith(StoreLimits {}) == 1);
    CHECK(allocationsWith(StoreLimits { .maxExceptions = 16 }) > 1);
}

TEST_CASE("Exceptions.uncaughtReachesTheEmbedder")
{
    // A guest's exception no frame catches ends the invocation as Thrown, of the same exception as the
    // reference's, with its payload. A host's exception goes on in its caller: caught there, uncaught to the
    // embedder, and through a host that invoked the guest again; also through a tail call to the host, whose
    // caller's caller catches it.
    auto const engine = fastEngineOfEachDispatch();
    auto module = withTypesAndTags();
    module.importFunction("env", "host", UnaryI32);
    // f1: throws tag 0 with 5. f2: calls the host with its i32, uncaught. f3: calls f2 with its i32, caught,
    // returning the payload. f4: tail-calls the host. f5: calls f4 with its i32 in a try_table, caught.
    module.function(Nullary, {}, code({ i32Const(5), throwTag(0) }));
    module.function(UnaryI32, {}, code({ localGet(0), op(0x10, 0) }));
    auto const caught = [](uint32_t callee) {
        return code({ { 0x02, I32Result },
                      tryTable(NoResult, { { .kind = CatchKind::Catch, .tag = 0, .label = 0 } }),
                      localGet(0),
                      op(0x10, callee),
                      end(),
                      i32Const(-1),
                      { 0x0F },
                      end() });
    };
    module.function(I32ToI32, {}, caught(2));
    module.function(UnaryI32, {}, code({ localGet(0), op(0x12, 0) }));
    module.function(I32ToI32, {}, caught(4));
    // The host: 0 throws its own exception, tag 0 with 9; 1 invokes f1, whose exception it returns.
    auto const imports = hostImport(
        FuncType { .params = { binary::I32 }, .results = {} },
        [](HostContext& context, std::span<Value const> args, std::span<Value>) noexcept -> HostOutcome {
            auto const caller = context.callerInstance();
            if (!caller)
                return std::unexpected { Abrupt { Trap { TrapCode::Unreachable } } };
            auto const& instance = context.store().moduleInstance(*caller);
            if (std::get<I32>(args.front()).bits == 1)
                return context.invoke(instance.functions.at(1), {}, {});
            auto const exception = context.addException(instance.tags.at(0), { i32(9) });
            if (!exception)
                return std::unexpected { exception.error() };
            return std::unexpected { Abrupt { Thrown { .exception = *exception } } };
        });
    auto const calls = std::vector<Call> {
        { .function = 1, .args = {} },         { .function = 2, .args = { i32(0) } },
        { .function = 2, .args = { i32(1) } }, { .function = 3, .args = { i32(0) } },
        { .function = 3, .args = { i32(1) } }, { .function = 4, .args = { i32(0) } },
        { .function = 5, .args = { i32(0) } }, { .function = 5, .args = { i32(1) } },
    };
    auto const reference = checkAgainstReference(module, engine, calls, {}, imports);
    auto harness = Harness { module, engine, imports };
    auto const payloadOf = [&](Outcome const& outcome) {
        return harness.store().exception(thrownOf(outcome).exception).payload;
    };
    CHECK(payloadOf(harness.outcome(1)) == std::vector { i32(5) });
    CHECK(payloadOf(harness.outcome(2, { i32(0) })) == std::vector { i32(9) });
    CHECK(payloadOf(harness.outcome(2, { i32(1) })) == std::vector { i32(5) });
    CHECK(payloadOf(harness.outcome(4, { i32(0) })) == std::vector { i32(9) });
    CHECK(reference.outcomes[3] == Outcome { std::vector { i32(9) } });
    CHECK(reference.outcomes[4] == Outcome { std::vector { i32(5) } });
    CHECK(reference.outcomes[6] == Outcome { std::vector { i32(9) } });
    CHECK(reference.outcomes[7] == Outcome { std::vector { i32(5) } });
}

TEST_CASE("Exceptions.catchInAnotherInstanceRestoresMemory0")
{
    // Instance A stores 0xAAAA at 0 of its memory 0 and calls B's function in a try_table; B stores 0xBBBB at
    // 0 of its own memory 0 and throws. A catches it and loads its memory 0 again: its own, not B's, which
    // the registers held while B ran.
    auto const engine = fastEngineOfEachDispatch();
    auto exporter = ModuleBuilder {};
    exporter.type(FuncType {});
    exporter.memory(binary::Limits { .min = 1, .max = 1, .addressType = binary::AddressType::I32 });
    exporter.tag(0);
    exporter.function(0, {}, code({ i32Const(0), i32Const(0xBBBB), { 0x36, 0x02, 0x00 }, throwTag(0) }));
    auto importer = ModuleBuilder {};
    importer.type(FuncType {});
    importer.type(FuncType { .params = {}, .results = { binary::I32 } });
    importer.importFunction("b", "f", 0);
    importer.memory(binary::Limits { .min = 1, .max = 1, .addressType = binary::AddressType::I32 });
    importer.function(1,
                      {},
                      code({ i32Const(0),
                             i32Const(0xAAAA),
                             { 0x36, 0x02, 0x00 }, // i32.store
                             { 0x02, NoResult },
                             tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                             op(0x10, 0),
                             end(),
                             end(),
                             i32Const(0),
                             { 0x28, 0x02, 0x00 } })); // i32.load
    auto const exporting = [&exporter](EngineUnderTest under) -> Harness::Imports {
        return [&exporter, under](Store& store) {
            auto const module = isoline::fastinterpreter::testing::validated(exporter);
            auto code = std::shared_ptr<LoweredModule const> {};
            auto executor = std::unique_ptr<isoline::runtime::ICodeExecutor> {};
            if (under.dispatch)
            {
                code = isoline::fastinterpreter::testing::lowered(
                    module, isoline::fastinterpreter::LoweringConfig { .dispatch = *under.dispatch });
                executor = std::make_unique<isoline::fastinterpreter::FastInterpreter>(*under.dispatch);
            }
            else
                executor = std::make_unique<isoline::interpreter::ReferenceInterpreter>();
            auto const instance = isoline::runtime::instantiate(store, *executor, module, code, {});
            REQUIRE(instance.has_value());
            return std::vector { ExternValue { .kind = ExternKind::Func,
                                               .address = store.moduleInstance(*instance).functions.at(0) } };
        };
    };
    auto reference = Harness { importer, Reference, exporting(Reference) };
    auto harness = Harness { importer, engine, exporting(engine) };
    CHECK(reference.outcome(1) == Outcome { std::vector { i32(0xAAAA) } });
    CHECK(harness.outcome(1) == Outcome { std::vector { i32(0xAAAA) } });
    CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
}

TEST_CASE("Exceptions.nullThrowRefTraps")
{
    auto const engine = fastEngineOfEachDispatch();
    auto module = withTypesAndTags();
    module.function(Nullary, {}, code({ { 0xD0, 0x69 }, throwRef() }));
    auto const reference = checkAgainstReference(module, engine, { { .function = 0, .args = {} } });
    CHECK(reference.outcomes.front()
          == Outcome { std::unexpected { Abrupt { Trap { TrapCode::NullExceptionReference } } } });
}

TEST_CASE("Exceptions.limitsHitMidUnwind")
{
    // A throw past 8'200 labels polls twice at Unwind, which charges no step: an interrupt at either poll
    // ends the invocation. A catch to a loop polls its back-edge right after the unwinding, which charges
    // one: a step budget that runs out there, or at the next round's entry, ends it. An arena too full for a
    // throw (ExceptionLimitExceeded), or swept with a step budget that runs out at its Sweep poll, ends it;
    // and a catch is followed by a descent to the depth limit. Each as the reference's does, at the same
    // step, every frame's depth and charge given back.
    auto const engine = fastEngineOfEachDispatch();
    auto module = withTypesAndTags();
    module.function(Nullary, {}, inBlocks(4'100, code({ i32Const(1), throwTag(0) })));
    module.function(Nullary,
                    {},
                    code({ tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                           inBlocks(4'099, op(0x10, 0)),
                           end() }));
    // Function 2: loop try_table (catch_all to the loop) count; if below 3 call 0 end end end.
    module.function(Nullary,
                    { binary::I32 },
                    code({ { 0x03, NoResult },
                           tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                           localGet(0),
                           i32Const(1),
                           { 0x6A, 0x22, 0x00 }, // i32.add local.tee 0
                           i32Const(3),
                           { 0x49, 0x04, NoResult }, // i32.lt_u if
                           op(0x10, 0),
                           end(),
                           end(),
                           end() }));
    SECTION("a step budget at the back-edge after an unwinding")
    {
        REQUIRE(pollsAt(observe(module, Reference, { { .function = 1, .args = {} } }), PollSite::Unwind)
                == 2);
        auto const unlimited = observe(module, Reference, { { .function = 2, .args = {} } });
        REQUIRE(pollsAt(unlimited, PollSite::LoopBackEdge) == 2);
        auto const steps = unlimited.steps.front();
        for (auto const budget: { steps - 2, steps - 1 })
        {
            INFO("steps " << budget);
            auto const reference = checkAgainstReference(
                module, engine, { { .function = 2, .args = {} } }, { .maxSteps = budget });
            CHECK(reference.outcomes.front()
                  == Outcome { std::unexpected { Abrupt { Trap { TrapCode::StepLimitExceeded } } } });
        }
    }
    SECTION("a step budget exactly the reference's, past an Unwind poll")
    {
        // 1'000 frames, each one's call or throw inside four labels: the innermost throws after the last
        // FunctionEntry, and the outermost catches, past 5'000 items and one Unwind poll, which charges no
        // step. A budget of exactly the steps the reference takes runs it through, every step of it used; one
        // fewer runs out.
        auto chain = withTypesAndTags();
        chain.function(UnaryI32,
                       {},
                       inBlocks(4,
                                code({ localGet(0),
                                       { 0x45, 0x04, NoResult }, // i32.eqz if
                                       i32Const(1),
                                       throwTag(0),
                                       end(),
                                       localGet(0),
                                       i32Const(1),
                                       { 0x6B }, // i32.sub
                                       op(0x10, 0) })));
        chain.function(UnaryI32,
                       {},
                       code({ { 0x02, NoResult },
                              tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                              inBlocks(2, code({ localGet(0), op(0x10, 0) })),
                              end(),
                              end() }));
        auto const calls = std::vector<Call> { { .function = 1, .args = { i32(998) } } };
        auto const unlimited = observe(chain, Reference, calls);
        REQUIRE(unlimited.outcomes.front() == Outcome { std::vector<Value> {} });
        REQUIRE(pollsAt(unlimited, PollSite::FunctionEntry) == 1'000);
        REQUIRE(pollsAt(unlimited, PollSite::Unwind) == 1);
        auto const steps = unlimited.steps.front();
        auto const exact = checkAgainstReference(chain, engine, calls, { .maxSteps = steps });
        CHECK(exact.outcomes.front() == Outcome { std::vector<Value> {} });
        CHECK(exact.steps.front() == steps);
        auto const oneShort = checkAgainstReference(chain, engine, calls, { .maxSteps = steps - 1 });
        CHECK(oneShort.outcomes.front()
              == Outcome { std::unexpected { Abrupt { Trap { TrapCode::StepLimitExceeded } } } });
    }
    SECTION("an interrupt at each Unwind poll")
    {
        for (auto const ordinal: { uint64_t { 1 }, uint64_t { 2 } })
        {
            INFO("poll " << ordinal);
            auto const interrupted = [&](EngineUnderTest under) {
                auto counter = PollCounter { isoline::runtime::testing::PollTrigger {
                    .site = PollSite::Unwind, .ordinal = ordinal } };
                auto harness = Harness { module, under, StoreLimits {}, &counter };
                counter.interruptAtTrigger(harness.store().interrupter());
                auto const outcome = harness.outcome(1);
                CHECK(harness.store().chargedStackValues() == 0);
                return std::pair { outcome, harness.store().used(Limit::Steps) };
            };
            auto const reference = interrupted(Reference);
            CHECK(reference.first
                  == Outcome { std::unexpected { Abrupt { Trap { TrapCode::Interrupted } } } });
            CHECK(interrupted(engine) == reference);
        }
    }
    SECTION("an interrupt at an Unwind poll among a try_table's clauses")
    {
        // f1 calls the thrower inside a try_table of eight clauses of tag 1, which do not match, inside
        // blocks: over these block counts the 4'096th item, where the Unwind poll comes, is each clause in
        // turn, and an interrupt there ends the invocation as the reference's does.
        for (auto const blocks: std::views::iota(uint32_t { 4'086 }, uint32_t { 4'096 }))
        {
            INFO(blocks << " blocks");
            auto clauses = withTypesAndTags();
            clauses.function(Nullary, {}, code({ i32Const(1), throwTag(0) }));
            auto const notMatching = Clause { .kind = CatchKind::Catch, .tag = 1, .label = 0 };
            clauses.function(Nullary,
                             {},
                             code({ tryTable(NoResult, std::vector<Clause>(8, notMatching)),
                                    inBlocks(blocks, op(0x10, 0)),
                                    end() }));
            auto const interrupted = [&](EngineUnderTest under) {
                auto counter = PollCounter { isoline::runtime::testing::PollTrigger {
                    .site = PollSite::Unwind, .ordinal = 1 } };
                auto harness = Harness { clauses, under, StoreLimits {}, &counter };
                counter.interruptAtTrigger(harness.store().interrupter());
                auto const outcome = harness.outcome(1);
                CHECK(harness.store().chargedStackValues() == 0);
                return std::pair { outcome, harness.store().used(Limit::Steps) };
            };
            auto const reference = interrupted(Reference);
            CHECK(reference.first
                  == Outcome { std::unexpected { Abrupt { Trap { TrapCode::Interrupted } } } });
            CHECK(interrupted(engine) == reference);
        }
    }
    SECTION("the arena")
    {
        // f1 holds two exceptions, one in its local and one passed to f0 as its argument, which fill a store
        // of two: f0's throw sweeps, keeps both, and traps; with too few steps, its Sweep poll ends it first.
        auto const caughtInto = [](uint32_t local) {
            return code({ { 0x02, ExnRefResult },
                          tryTable(NoResult, { { .kind = CatchKind::CatchAllRef, .label = 0 } }),
                          throwTag(1),
                          end(),
                          { 0x00 },
                          end(),
                          localSet(local) });
        };
        auto full = withTypesAndTags();
        full.function(FromExn, {}, throwTag(1));
        full.function(Nullary,
                      { binary::ExnRef, binary::ExnRef },
                      code({ caughtInto(0), caughtInto(1), localGet(1), op(0x10, 0) }));
        auto const limits = StoreLimits { .maxExceptions = 2 };
        auto const reference = checkAgainstReference(full, engine, { { .function = 1, .args = {} } }, limits);
        auto const trapped = reference.outcomes.front();
        CHECK(trapped
              == Outcome { std::unexpected { Abrupt { Trap { TrapCode::ExceptionLimitExceeded } } } });
        auto const stepsToTrap = reference.steps.front();
        auto const shortOfSteps =
            checkAgainstReference(full,
                                  engine,
                                  { { .function = 1, .args = {} } },
                                  StoreLimits { .maxExceptions = 2, .maxSteps = stepsToTrap - 1 });
        CHECK(shortOfSteps.outcomes.front()
              == Outcome { std::unexpected { Abrupt { Trap { TrapCode::StepLimitExceeded } } } });
    }
    SECTION("the stack after a catch")
    {
        // f0(n) recurses n deep and throws there; f2(n) catches that, then recurses n deep again through f1,
        // returning: every unwound frame gave its depth and charge back, or the second descent would run out
        // first.
        auto deep = withTypesAndTags();
        deep.function(UnaryI32,
                      {},
                      code({ localGet(0),
                             { 0x45, 0x04, NoResult },
                             i32Const(1),
                             throwTag(0),
                             end(),
                             localGet(0),
                             i32Const(1),
                             { 0x6B },
                             op(0x10, 0) }));
        deep.function(
            UnaryI32,
            {},
            code(
                { localGet(0), { 0x04, NoResult }, localGet(0), i32Const(1), { 0x6B }, op(0x10, 1), end() }));
        deep.function(UnaryI32,
                      {},
                      code({ { 0x02, NoResult },
                             tryTable(NoResult, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                             localGet(0),
                             op(0x10, 0),
                             end(),
                             end(),
                             localGet(0),
                             op(0x10, 1) }));
        auto const limits = StoreLimits { .maxCallDepth = 1'002 };
        auto const reference =
            checkAgainstReference(deep, engine, { { .function = 2, .args = { i32(1'000) } } }, limits);
        CHECK(reference.outcomes.front() == Outcome { std::vector<Value> {} });
        auto const tooDeep =
            checkAgainstReference(deep, engine, { { .function = 2, .args = { i32(1'001) } } }, limits);
        CHECK(tooDeep.outcomes.front()
              == Outcome { std::unexpected { Abrupt { Trap { TrapCode::CallStackExhausted } } } });
    }
}

TEST_CASE("Exceptions.everyExceptionOperationRuns")
{
    // Every operation of the exceptions family runs: throw, and throw_ref of the exnref its catch_all_ref
    // handed a block, which FastInterpreter.everyOperationAgreesWithReference leaves to this file.
    auto module = withTypesAndTags();
    module.function(Nullary,
                    {},
                    code({ { 0x02, ExnRefResult },
                           tryTable(NoResult, { { .kind = CatchKind::CatchAllRef, .label = 0 } }),
                           i32Const(1),
                           throwTag(0),
                           end(),
                           { 0x00 },
                           end(),
                           { 0x02, FromExn },
                           tryTable(FromExn, { { .kind = CatchKind::CatchAll, .label = 0 } }),
                           throwRef(),
                           end(),
                           end() }));
    auto const engine = fastEngineOfEachDispatch();
    auto harness = Harness { module, engine };
    CHECK(harness.outcome(0) == Outcome { std::vector<Value> {} });
    auto const executed = operationsOf(*harness.code());
    for (auto const& row: OperationTable)
    {
        if (row.family != OperationFamily::Exceptions)
            continue;
        INFO(row.name);
        CHECK(executed.contains(row.id));
    }
}

namespace
{

/// Checks that every function of @p lowered, lowered from @p module, made no more records than its bounds
/// (recordBounds()), which lowering reserved.
void checkRecordBounds(ModuleBuilder const& module, LoweredModule const& lowered)
{
    auto const source = isoline::fastinterpreter::testing::validated(module);
    for (auto const& function: lowered.functions())
    {
        auto const bounds = recordBounds(source->module().code[function.codeIndex]);
        CHECK(function.sites.size() <= bounds.sites);
        CHECK(function.handlers.size() <= bounds.handlers);
        CHECK(function.catches.size() <= bounds.catches);
        CHECK(function.operands.size() <= bounds.operands);
        CHECK(function.constants.size() <= bounds.constants);
    }
}

} // namespace

TEST_CASE("Exceptions.siteOperandsCountTheStack")
{
    // A site's operand tree holds exactly the values below its call: a call's 1'000 results, whole and then
    // with 400 dropped (a run that shrank after a site made its node), an else branch's params (a run of
    // params), a forwarded local and a constant, and a call's results kept whole. The roots hand the store as
    // many values as the reference interpreter's stack holds there.
    auto module = withTypesAndTags();
    module.type(
        FuncType { .params = {}, .results = std::vector<binary::ValueType>(1'000, binary::I32) }); // 8
    module.type(
        FuncType { .params = { binary::I32, binary::I32 }, .results = { binary::I32, binary::I32 } }); // 9
    module.function(Nullary, {}, {});
    auto many = Bytes {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, 1'000))
        many.insert(many.end(), { 0x41, 0x00 });
    module.function(8, {}, many);
    auto body = code({ op(0x10, 1), op(0x10, 0) }); // site 0 on an empty stack, site 1 over the whole run
    body.insert(body.end(), 400, 0x1A);
    body = code({ body,
                  op(0x10, 0), // site 2: 600
                  localGet(0),
                  i32Const(5),
                  op(0x10, 0), // site 3: 602
                  i32Const(1),
                  i32Const(2),
                  localGet(0),
                  { 0x04, 0x09 }, // if (type 9)
                  op(0x10, 0),    // site 4: 604, the params as they were
                  { 0x05 },       // else
                  op(0x10, 0),    // site 5: 604, the params as a run
                  end(),
                  op(0x10, 0) }); // site 6: 604
    body.insert(body.end(), 604, 0x1A);
    module.function(Nullary, { binary::I32 }, body);
    auto const lowered =
        isoline::fastinterpreter::testing::lowered(isoline::fastinterpreter::testing::validated(module));
    auto const& function = lowered->functions()[2];
    REQUIRE(function.sites.size() == 7);
    auto const heights = std::to_array<uint64_t>({ 0, 1'000, 600, 602, 604, 604, 604 });
    for (auto const index: std::views::iota(std::size_t { 0 }, heights.size()))
    {
        INFO("site " << index);
        auto values = uint64_t { 0 };
        auto kinds = std::vector<isoline::fastinterpreter::OperandNodeKind> {};
        for (auto node = function.sites[index].operands; node != isoline::fastinterpreter::NoNode;
             node = function.operands[node].parent)
        {
            values += function.operands[node].count;
            kinds.push_back(function.operands[node].kind);
        }
        CHECK(values == heights[index]);
        if (index == 5)
            CHECK(kinds.front() == isoline::fastinterpreter::OperandNodeKind::Params);
        if (index == 3)
            CHECK(std::ranges::find(kinds, isoline::fastinterpreter::OperandNodeKind::Local) != kinds.end());
    }
    CHECK(function.constants.size() == 1);
}

TEST_CASE("Exceptions.hostileShapesStayLinear", "[memory]")
{
    // Review Focus 5, lowered within the bounds, never run: 50'000 exnref locals around 3'000'000 call sites,
    // and 50'000 nested try_tables around 1'000'000 call sites (testing/HostileModules.hpp). Every site
    // records its operands, handler and label depth in O(1), never a copy of the exnref locals or the
    // handlers around it.
    SECTION("exnref locals")
    {
        constexpr auto Calls = uint32_t { 3'000'000 };
        auto const module = isoline::fastinterpreter::testing::exnrefLocalsAroundCalls(50'000, Calls);
        auto const lowered = loweredWithinTheBounds(module, uint64_t { 64 } << 20U);
        auto const& function = lowered->functions()[1];
        CHECK(function.sites.size() == Calls);
        CHECK(function.operands.empty());
        CHECK(function.handlers.empty());
        checkRecordBounds(module, *lowered);
    }
    SECTION("nested try_tables")
    {
        constexpr auto Depth = uint32_t { 50'000 };
        constexpr auto Calls = uint32_t { 1'000'000 };
        auto const module = isoline::fastinterpreter::testing::nestedTryTablesAroundCalls(Depth, Calls);
        auto const lowered = loweredWithinTheBounds(module, uint64_t { 64 } << 20U);
        auto const& function = lowered->functions()[1];
        CHECK(function.sites.size() == Calls);
        CHECK(function.handlers.size() == Depth);
        CHECK(function.catches.size() == Depth);
        CHECK(std::ranges::all_of(function.sites, [](auto const& site) {
            return site.innermostHandler == Depth - 1 && site.labelDepth == Depth;
        }));
        checkRecordBounds(module, *lowered);
    }
}
