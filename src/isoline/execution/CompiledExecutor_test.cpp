// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/compiler/Compiler.hpp>
#include <isoline/compiler/registerallocation/LinearScan.hpp>
#include <isoline/compiler/registerallocation/testing/AssignmentChecker.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>
#include <isoline/compiler/x86_64/Target.hpp>
#include <isoline/execution/CompiledExecutor.hpp>
#include <isoline/execution/StackBounds.hpp>
#include <isoline/execution/testing/CompileOne.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>
#include <isoline/runtime/testing/SmallStackThread.hpp>

using isoline::execution::CompiledExecutor;
using isoline::execution::CompiledExecutorConfig;
using isoline::execution::ExecutorError;
using isoline::execution::testing::CompiledInstance;
using isoline::execution::testing::HarnessLimits;
using isoline::execution::testing::hostConfig;
using isoline::execution::testing::hostRunsCompiledCode;
using isoline::execution::testing::InstanceOptions;
using isoline::execution::testing::moduleOf;
using isoline::execution::testing::runReference;
using isoline::runtime::Abrupt;
using isoline::runtime::ExecutionMode;
using isoline::runtime::ExternValue;
using isoline::runtime::FuncAddr;
using isoline::runtime::HostContext;
using isoline::runtime::I32;
using isoline::runtime::Limit;
using isoline::runtime::PollSite;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::testing::PollCounter;

namespace
{

constexpr auto Deterministic = ExecutionMode::Deterministic;
constexpr auto Production = ExecutionMode::Production;

/// How a host function ends.
using HostOutcome = std::expected<void, Abrupt>;

/// What a host function of a test does.
using HostBody = std::function<HostOutcome(HostContext&, std::span<Value const>, std::span<Value>)>;

/// An i32 argument.
[[nodiscard]] Value i32(uint32_t bits) noexcept
{
    return I32 { .bits = bits };
}

/// The trap @p outcome ended with.
template <typename Outcome>
[[nodiscard]] std::optional<TrapCode> trapOf(Outcome const& outcome)
{
    if (outcome.has_value())
        return std::nullopt;
    auto const* trap = std::get_if<Trap>(&outcome.error());
    return trap != nullptr ? std::optional { trap->code } : std::nullopt;
}

/// Imports of one host function "host" "f" of @p type running @p body.
[[nodiscard]] std::function<std::vector<ExternValue>(Store&)> hostImport(isoline::binary::FuncType type,
                                                                         HostBody body)
{
    return [type = std::move(type), body = std::move(body)](Store& store) {
        auto const address = store.addHostFunction(
            type,
            isoline::runtime::makeHostFunction([body](HostContext& context,
                                                      std::span<Value const> arguments,
                                                      std::span<Value> results) noexcept -> HostOutcome {
                return body(context, arguments, results);
            }));
        REQUIRE(address.has_value());
        return std::vector { ExternValue { .kind = isoline::binary::ExternKind::Func, .address = *address } };
    };
}

/// A compiled instance of @p wat in @p mode (Require: created).
[[nodiscard]] std::unique_ptr<CompiledInstance> compiled(std::string_view wat,
                                                         InstanceOptions options = {},
                                                         ExecutionMode mode = Deterministic)
{
    auto created = CompiledInstance::create(moduleOf(wat), hostConfig(mode, {}), std::move(options));
    if (!created)
        FAIL(isoline::compiler::describe(created.error()));
    return std::move(*created);
}

/// An observer that records every poll's site, in order.
class PollRecorder final: public isoline::runtime::IPollObserver
{
  public:
    PollRecorder() { _sites.reserve(MaximumSites); }

    void onPoll(PollSite site) noexcept override
    {
        if (_sites.size() < MaximumSites)
            _sites.push_back(site);
    }

    [[nodiscard]] std::vector<PollSite> const& sites() const noexcept { return _sites; }

  private:
    static constexpr auto MaximumSites = std::size_t { 1 } << 20;
    std::vector<PollSite> _sites;
};

/// One program of the contract corpus: a module, the function a test calls and its arguments.
struct ContractProgram
{
    std::string_view name;
    std::string_view wat;
    uint32_t codeIndex = 0;
    std::vector<Value> arguments;
};

/// The contract corpus: every PollSiteTable site a compiled function reaches without a host, in programs
/// whose control flow differs.
std::vector<ContractProgram> const& contractPrograms()
{
    static auto const programs = std::vector<ContractProgram> {
        { "loop",
          R"((module (func (param i32) (result i32)
  (loop $l (local.set 0 (i32.sub (local.get 0) (i32.const 1))) (br_if $l (local.get 0)))
  (local.get 0))))",
          0,
          { i32(20) } },
        { "recursion",
          R"((module (func $fib (param i32) (result i32)
  (if (result i32) (i32.lt_u (local.get 0) (i32.const 2))
    (then (local.get 0))
    (else (i32.add (call $fib (i32.sub (local.get 0) (i32.const 1)))
                   (call $fib (i32.sub (local.get 0) (i32.const 2)))))))))",
          0,
          { i32(10) } },
        { "br_table loop",
          R"((module (func (param i32) (result i32) (local i32)
  (block $done (loop $l
    (local.set 1 (i32.add (local.get 1) (i32.const 3)))
    (local.set 0 (i32.sub (local.get 0) (i32.const 1)))
    (br_table $l $done (i32.eqz (local.get 0)))))
  (local.get 1))))",
          0,
          { i32(9) } },
        { "call_indirect",
          R"((module
  (type $t (func (param i32) (result i32)))
  (table 2 funcref) (elem (i32.const 0) $double $inc)
  (func $double (type $t) (i32.mul (local.get 0) (i32.const 2)))
  (func $inc (type $t) (i32.add (local.get 0) (i32.const 1)))
  (func (param i32) (result i32)
    (call_indirect (type $t) (call_indirect (type $t) (local.get 0) (i32.const 0)) (i32.const 1)))))",
          2,
          { i32(5) } },
        { "memory.fill",
          R"((module (memory 4)
  (func (param i32) (result i32)
    (memory.fill (i32.const 16) (i32.const 7) (local.get 0))
    (i32.load8_u (i32.const 100)))))",
          0,
          { i32(200'000) } },
        { "memory.grow",
          R"((module (memory 1)
  (func (param i32) (result i32)
    (drop (memory.grow (local.get 0)))
    (i32.store (i32.const 70000) (i32.const 11))
    (i32.add (memory.size) (i32.load (i32.const 70000))))))",
          0,
          { i32(2) } },
        { "blocks",
          R"((module (func (param i32) (result i32)
  (block $a (result i32) (block $b (result i32)
    (br_if $a (i32.const 1) (i32.eqz (local.get 0)))
    (br $b (i32.const 2)))
    (i32.const 3) (i32.add)))))",
          0,
          { i32(4) } },
        { "if in a loop",
          R"((module (func (param i32) (result i32) (local i32)
  (loop $l
    (if (i32.and (local.get 0) (i32.const 1))
      (then (local.set 1 (i32.add (local.get 1) (local.get 0))))
      (else (local.set 1 (i32.sub (local.get 1) (i32.const 1)))))
    (br_if $l (local.tee 0 (i32.sub (local.get 0) (i32.const 1)))))
  (local.get 1))))",
          0,
          { i32(15) } },
        { "calls",
          R"((module
  (func $c (param i32) (result i32) (i32.mul (local.get 0) (local.get 0)))
  (func $b (param i32) (result i32) (i32.add (call $c (local.get 0)) (i32.const 1)))
  (func (param i32) (result i32) (call $b (call $b (local.get 0))))))",
          2,
          { i32(3) } },
        { "trap",
          R"((module (memory 1) (func (param i32) (result i32)
  (loop $l (br_if $l (local.tee 0 (i32.sub (local.get 0) (i32.const 1)))))
  (i32.load (i32.const 70000)))))",
          0,
          { i32(6) } },
    };
    return programs;
}

/// A module whose function 0 counts down from its argument through calls, one per step, the stack and
/// the depth charged per frame: what the exhaustion rows cut off.
constexpr auto Countdown = std::string_view { R"((module
  (func $down (param i32) (result i32) (local i64 i64)
    (if (result i32) (local.get 0)
      (then (i32.add (i32.const 1) (call $down (i32.sub (local.get 0) (i32.const 1)))))
      (else (i32.const 0)))))
)" };

/// The same compiled and in the reference: outcome, steps and the stack left charged.
void checkAgainstTheReference(std::string_view wat,
                              uint32_t codeIndex,
                              std::vector<Value> const& arguments,
                              InstanceOptions const& options)
{
    auto const reference = runReference(moduleOf(wat), codeIndex, arguments, options);
    auto instance = compiled(wat, options);
    auto const called = instance->call(codeIndex, arguments);
    CHECK(called == reference.outcome);
    CHECK(instance->store().used(Limit::Steps) == reference.steps);
    CHECK(instance->store().chargedStackValues() == 0);
    CHECK(instance->store().budgetState() == isoline::runtime::BudgetState::Settled);
}

/// Stack bounds that end @p below bytes under the stack's address where it is made.
class BoundsBelowHere final: public isoline::execution::IStackBounds
{
  public:
    explicit BoundsBelowHere(uint64_t below) noexcept:
        _lowest { isoline::execution::currentStackAddress() - static_cast<std::uintptr_t>(below) }
    {
    }

    [[nodiscard]] std::expected<std::uintptr_t, isoline::execution::StackBoundsError> lowestUsableAddress()
        const noexcept override
    {
        return _lowest;
    }

  private:
    std::uintptr_t _lowest;
};

/// The text of a function type of @p count params of @p type, and as many results when @p results.
[[nodiscard]] std::string repeated(std::string_view word, std::size_t count)
{
    auto text = std::string {};
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, count))
        text.append(" ").append(word);
    return text;
}

} // namespace

TEST_CASE("Contract.pollsMatchTheReference")
{
    // With an observer every poll goes through the store: the compiled code's sequence of sites is the
    // reference's, as are its results and steps.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    for (auto const& program: contractPrograms())
    {
        INFO(program.name);
        auto referencePolls = PollRecorder {};
        auto const reference = runReference(moduleOf(program.wat),
                                            program.codeIndex,
                                            program.arguments,
                                            InstanceOptions { .observer = &referencePolls });
        auto polls = PollRecorder {};
        auto instance = compiled(program.wat, InstanceOptions { .observer = &polls });
        auto const called = instance->call(program.codeIndex, program.arguments);
        CHECK(called == reference.outcome);
        CHECK(polls.sites() == referencePolls.sites());
        CHECK(instance->store().used(Limit::Steps) == reference.steps);
        CHECK(instance->store().chargedStackValues() == 0);
    }
}

TEST_CASE("Contract.exhaustsAtTheSamePointAsTheReference")
{
    // Review Focus 1: every limit cuts off where the reference's does, ties included: a call refused for its
    // stack or its depth in the same poll that spends the last step.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    struct Row
    {
        std::string_view name;
        StoreLimits limits;
    };
    auto rows = std::vector<Row> {};
    auto const with = [](auto const& adjust) {
        auto limits = HarnessLimits;
        adjust(limits);
        return limits;
    };
    rows.push_back(
        { "maxStackValues 1'000", with([](StoreLimits& limits) { limits.maxStackValues = 1'000; }) });
    rows.push_back({ "maxSteps 777", with([](StoreLimits& limits) { limits.maxSteps = 777; }) });
    rows.push_back({ "depth 50", with([](StoreLimits& limits) { limits.maxCallDepth = 50; }) });
    // Ties: every step budget around where the stack and the depth run out.
    for (auto const steps: std::views::iota(uint64_t { 1 }, uint64_t { 160 }))
        rows.push_back({ "ties", with([steps](StoreLimits& limits) {
                             limits.maxSteps = steps;
                             limits.maxStackValues = 200;
                             limits.maxCallDepth = 45;
                         }) });
    for (auto const& row: rows)
    {
        INFO(row.name << " (steps " << row.limits.maxSteps << ")");
        checkAgainstTheReference(Countdown, 0, { i32(400) }, InstanceOptions { .limits = row.limits });
    }
}

TEST_CASE("Contract.depthSpansNestedInvocations")
{
    // maxCallDepth counts the Wasm frames of every invocation in progress: a recursion through a host import
    // that invokes the guest again ends at the reference's depth and step, not once per invocation.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    constexpr auto Reentering = std::string_view { R"((module
  (import "host" "f" (func $reenter (param i32)))
  (func $f (param i32 i32)
    (if (local.get 1)
      (then (call $f (local.get 0) (i32.sub (local.get 1) (i32.const 1))))
      (else (call $reenter (local.get 0))))))
)" };
    // The host invokes $f (the store's function after the import) again, one nesting fewer.
    auto const reenter =
        [](HostContext& context, std::span<Value const> arguments, std::span<Value>) -> HostOutcome {
        auto const nestings = std::get<I32>(arguments[0]).bits;
        if (nestings == 0)
            return {};
        auto const& caller = context.store().moduleInstance(*context.callerInstance());
        auto const again = std::vector<Value> { i32(nestings - 1), i32(9) };
        return context.invoke(caller.functions[1], again, {});
    };
    auto limits = HarnessLimits;
    limits.maxCallDepth = 50;
    limits.maxNestedInvocations = 8;
    auto const options = InstanceOptions {
        .limits = limits,
        .imports = hostImport(isoline::binary::FuncType { .params = { isoline::binary::I32 }, .results = {} },
                              reenter)
    };
    auto const ran = isoline::runtime::testing::runOnSmallStack(std::size_t { 8 } << 20, [&] {
        auto const reference =
            runReference(moduleOf(Reentering), 0, std::vector { i32(10), i32(9) }, options);
        REQUIRE(trapOf(reference.outcome) == TrapCode::CallStackExhausted);
        auto instance = compiled(Reentering, options);
        auto const called = instance->call(0, std::vector { i32(10), i32(9) });
        CHECK(called == reference.outcome);
        CHECK(instance->store().used(Limit::Steps) == reference.steps);
        CHECK(instance->store().chargedStackValues() == 0);
        // The depth given back: a shallow call runs again.
        CHECK(instance->call(0, std::vector { i32(0), i32(3) }).has_value());
    });
    REQUIRE(ran.has_value());
}

TEST_CASE("Contract.outerTrapAfterNestedTrap")
{
    // A → host → B traps deep in its frames; the host absorbs it and returns; A then traps itself. The outer
    // trap exit resets to A's own entry, and the depth and stack B's dropped frames held are given back.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    constexpr auto Nesting = std::string_view { R"((module
  (import "host" "f" (func $nested))
  (func $a (call $nested) unreachable)
  (func $b (drop (call $deep (i32.const 6))))
  (func $deep (param i32) (result i32)
    (if (result i32) (local.get 0)
      (then (call $deep (i32.sub (local.get 0) (i32.const 1))))
      (else (i32.div_u (i32.const 1) (local.get 0)))))
  (func $ok (result i32) (i32.const 7)))
)" };
    auto nestedTraps = std::make_shared<std::vector<std::optional<TrapCode>>>();
    auto const nested =
        [nestedTraps](HostContext& context, std::span<Value const>, std::span<Value>) -> HostOutcome {
        auto const& caller = context.store().moduleInstance(*context.callerInstance());
        auto const outcome = context.invoke(caller.functions[2], {}, {});
        nestedTraps->push_back(trapOf(outcome));
        return {};
    };
    auto const options = InstanceOptions { .imports = hostImport(isoline::binary::FuncType {}, nested) };
    auto const reference = runReference(moduleOf(Nesting), 0, {}, options);
    REQUIRE(trapOf(reference.outcome) == TrapCode::Unreachable);
    nestedTraps->clear();
    auto instance = compiled(Nesting, options);
    for ([[maybe_unused]] auto const attempt: { 1, 2 })
    {
        auto const called = instance->call(0, {});
        CHECK(trapOf(called) == TrapCode::Unreachable);
        CHECK(instance->store().chargedStackValues() == 0);
    }
    CHECK(*nestedTraps == std::vector<std::optional<TrapCode>>(2, TrapCode::IntegerDivideByZero));
    // One call's steps are the reference's, and the instance runs on.
    auto fresh = compiled(Nesting, options);
    CHECK(fresh->call(0, {}) == reference.outcome);
    CHECK(fresh->store().used(Limit::Steps) == reference.steps);
    CHECK(instance->call(3, {}) == std::expected<std::vector<Value>, Abrupt> { std::vector { i32(7) } });
}

TEST_CASE("Contract.nativeStackBoundHolds")
{
    // Under a grant of requiredNativeStack() the static limits always fire first; under a small Production
    // grant the native limit does, and traps rather than crashes.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    SECTION("Deterministic, grant = requiredNativeStack: the reference's limit, at its step")
    {
        auto limits = HarnessLimits;
        limits.maxCallDepth = 3'000;
        limits.maxStackValues = uint64_t { 1 } << 16;
        auto const grant =
            isoline::execution::requiredNativeStack(isoline::compiler::x86_64::X64FrameRule, limits);
        auto const options = InstanceOptions { .limits = limits, .nativeStackBytes = grant };
        auto const ran = isoline::runtime::testing::runOnSmallStack(
            static_cast<std::size_t>(grant) + (std::size_t { 2 } << 20),
            [&] { checkAgainstTheReference(Countdown, 0, { i32(10'000) }, options); });
        REQUIRE(ran.has_value());
    }
    SECTION("Production, a 64 KiB grant: CallStackExhausted, no crash")
    {
        auto limits = StoreLimits {};
        limits.maxNestedInvocations = 2;
        auto const options = InstanceOptions { .limits = limits, .nativeStackBytes = uint64_t { 64 } * 1024 };
        auto const ran = isoline::runtime::testing::runOnSmallStack(std::size_t { 1 } << 20, [&] {
            auto instance = compiled(Countdown, options, Production);
            CHECK(trapOf(instance->call(0, std::vector { i32(50'000) })) == TrapCode::CallStackExhausted);
            CHECK(instance->store().chargedStackValues() == 0);
            CHECK(instance->call(0, std::vector { i32(10) })
                  == std::expected<std::vector<Value>, Abrupt> { std::vector { i32(10) } });
        });
        REQUIRE(ran.has_value());
    }
}

TEST_CASE("Contract.deterministicRefusesASmallGrant")
{
    auto const bounds = isoline::execution::ThreadStackBounds {};
    auto const required =
        isoline::execution::requiredNativeStack(isoline::compiler::x86_64::X64FrameRule, HarnessLimits);
    if (!isoline::execution::hostFrameRule())
    {
        auto const created = CompiledExecutor::create(
            CompiledExecutorConfig { Deterministic, required }, HarnessLimits, bounds);
        REQUIRE_FALSE(created.has_value());
        CHECK(created.error() == ExecutorError::NoBackendForHost);
        return;
    }
    auto const small = CompiledExecutor::create(
        CompiledExecutorConfig { Deterministic, required - 1 }, HarnessLimits, bounds);
    REQUIRE_FALSE(small.has_value());
    CHECK(small.error() == ExecutorError::GrantBelowRequired);
    CHECK(CompiledExecutor::create(CompiledExecutorConfig { Deterministic, required }, HarnessLimits, bounds)
              .has_value());
    // Production promises no order between the native limit and the static ones.
    CHECK(CompiledExecutor::create(CompiledExecutorConfig { Production, 1 }, HarnessLimits, bounds)
              .has_value());
}

TEST_CASE("Contract.deterministicRefusesAThreadWithoutTheGrant")
{
    // An outermost Deterministic invocation on a thread with less than its grant left is refused before any
    // poll: otherwise the native limit could fire before a static one.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    auto const grant =
        isoline::execution::requiredNativeStack(isoline::compiler::x86_64::X64FrameRule, HarnessLimits);
    auto const tooLittle = BoundsBelowHere { grant / 2 };
    auto polls = PollCounter {};
    auto instance = compiled("(module (func))", InstanceOptions { .observer = &polls, .bounds = &tooLittle });
    CHECK(trapOf(instance->call(0, {})) == TrapCode::CallStackExhausted);
    CHECK(polls.count(PollSite::FunctionEntry) == 0);
    auto const enough = BoundsBelowHere { grant + (uint64_t { 256 } * 1024) };
    auto roomy = compiled("(module (func))", InstanceOptions { .bounds = &enough });
    CHECK(roomy->call(0, {}).has_value());
}

TEST_CASE("Contract.entryChecksTheNativeLimit")
{
    // Before the trampoline, an entry checks that the trampoline's saves, its adapter's area and the stub
    // reserve fit above the native limit: a wide signature's adapter would otherwise run past it before the
    // first prologue checks. Refused before any poll.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    constexpr auto Values = std::size_t { 1'000 };
    auto const wide = "(module (func (param" + repeated("i64", Values) + ") (result" + repeated("i64", Values)
                      + ")" + repeated("(local.get 0)", Values) + ") (func))";
    auto const area = isoline::compiler::x86_64::X64Target::create(hostConfig(Production, {}).target)
                          ->entryAdapterAreaBytes(moduleOf(wide)->module().types[0]);
    REQUIRE(area >= Values * 8);
    // Room for a narrow entry and the stub reserve twice over, not for the wide adapter's area.
    auto const room =
        (2 * isoline::execution::StubReserve) + isoline::execution::EntryOverheadBytes + (area / 2);
    auto const bounds = BoundsBelowHere { room };
    auto polls = PollCounter {};
    // A wide signature's code is large for its module: past the default artifact floor.
    auto config = hostConfig(Production, {});
    config.limits.artifactBytesFloor = uint64_t { 1 } << 20;
    auto created = CompiledInstance::create(
        moduleOf(wide), config, InstanceOptions { .observer = &polls, .bounds = &bounds });
    REQUIRE(created.has_value());
    auto& instance = *created;
    auto const arguments = std::vector<Value>(Values, isoline::runtime::I64 { .bits = 1 });
    CHECK(trapOf(instance->call(0, arguments)) == TrapCode::CallStackExhausted);
    CHECK(polls.count(PollSite::FunctionEntry) == 0);
    CHECK(instance->store().budgetState() == isoline::runtime::BudgetState::Settled);
    CHECK(instance->call(1, {}).has_value());
    CHECK(polls.count(PollSite::FunctionEntry) == 1);
}

TEST_CASE("Contract.interruptStopsCompiledLoops", "[threading]")
{
    // Review Focus 4: an endless loop, a br_table loop and a recursion in a loop each stop within one poll of
    // an interrupt from another thread.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    struct Row
    {
        std::string_view name;
        std::string_view wat;
    };
    auto const rows = std::vector<Row> {
        { "(loop br 0)", "(module (func (loop $l (br $l))))" },
        { "br_table loop", "(module (func (local i32) (loop $l (br_table $l $l (local.get 0)))))" },
        { "self-recursion",
          R"((module (func $rec (param i32)
  (if (local.get 0) (then (call $rec (i32.sub (local.get 0) (i32.const 1))))))
  (func (loop $l (call $rec (i32.const 40)) (br $l)))))" },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto limits = HarnessLimits;
        // A bound past which a missing poll ends the guest with another trap rather than a hang.
        limits.maxSteps = uint64_t { 4'000'000'000 };
        auto instance = compiled(row.wat, InstanceOptions { .limits = limits });
        auto const interrupter = instance->store().interrupter();
        auto const codeIndex = static_cast<uint32_t>(moduleOf(row.wat)->module().code.size() - 1);
        auto started = std::chrono::steady_clock::time_point {};
        auto thread = std::thread { [interrupter] {
            std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
            interrupter->interrupt();
        } };
        started = std::chrono::steady_clock::now();
        auto const called = instance->call(codeIndex, {});
        auto const elapsed = std::chrono::steady_clock::now() - started;
        thread.join();
        CHECK(trapOf(called) == TrapCode::Interrupted);
        CHECK(elapsed < std::chrono::seconds { 1 });
    }
}

TEST_CASE("Contract.growthOutsideTheExecutorIsSeen")
{
    // Review Focus 3: a memory grown by Store::growMemory between two compiled calls, or by another instance,
    // is seen by the next access: the new page reads, one past it traps.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    constexpr auto Reader = std::string_view { R"((module (memory (export "m") 1)
  (func (param i32) (result i32) (i32.load (local.get 0))))
)" };
    constexpr auto Grower = std::string_view { R"((module (import "a" "m" (memory 1))
  (func (param i32) (result i32) (memory.grow (local.get 0))))
)" };
    auto instance = compiled(Reader);
    auto& store = instance->store();
    auto const memory = store.moduleInstance(instance->context().moduleInstance).memories[0];
    auto const page = uint32_t { 65'536 };
    CHECK(trapOf(instance->call(0, std::vector { i32(page) })) == TrapCode::OutOfBoundsMemoryAccess);
    SECTION("by Store::growMemory")
    {
        REQUIRE(store.growMemory(memory, 1).has_value());
        store.memory(memory).bytes()[page] = std::byte { 9 };
        CHECK(instance->call(0, std::vector { i32(page) })
              == std::expected<std::vector<Value>, Abrupt> { std::vector { i32(9) } });
        CHECK(trapOf(instance->call(0, std::vector { i32((2 * page) - 3) }))
              == TrapCode::OutOfBoundsMemoryAccess);
    }
    SECTION("by another instance")
    {
        auto const code =
            isoline::execution::testing::compileModule(*moduleOf(Grower), hostConfig(Deterministic, {}));
        REQUIRE(code.has_value());
        auto const imports =
            std::vector { ExternValue { .kind = isoline::binary::ExternKind::Memory, .address = memory } };
        auto const other =
            isoline::runtime::instantiate(store, instance->executor(), moduleOf(Grower), *code, imports);
        REQUIRE(other.has_value());
        auto results = std::vector<Value>(1);
        auto const grown = isoline::runtime::invokeChecked(store,
                                                           instance->executor(),
                                                           store.moduleInstance(*other).functions[0],
                                                           std::vector { i32(2) },
                                                           results);
        REQUIRE(grown.outcome.has_value());
        CHECK(results[0] == i32(1));
        CHECK(instance->call(0, std::vector { i32((3 * page) - 4) })
              == std::expected<std::vector<Value>, Abrupt> { std::vector { i32(0) } });
        CHECK(trapOf(instance->call(0, std::vector { i32((3 * page) - 3) }))
              == TrapCode::OutOfBoundsMemoryAccess);
    }
}

TEST_CASE("Contract.hostInstantiatesMidCall")
{
    // A host function instantiates a module while compiled code waits on it and puts its function into the
    // caller's table: the caller's call_indirect reaches it through the store's dispatch rows.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    constexpr auto Caller = std::string_view { R"((module
  (import "host" "f" (func $make))
  (type $t (func (result i32)))
  (table 2 funcref)
  (func (result i32) (call $make) (call_indirect (type $t) (i32.const 1))))
)" };
    constexpr auto Callee = std::string_view { "(module (func (result i32) (i32.const 42)))" };
    auto* self = static_cast<CompiledInstance*>(nullptr);
    auto const make =
        [&self, Callee](HostContext& context, std::span<Value const>, std::span<Value>) -> HostOutcome {
        auto& store = self->store();
        auto const validated = moduleOf(Callee);
        auto const code =
            isoline::execution::testing::compileModule(*validated, hostConfig(Deterministic, {}));
        REQUIRE(code.has_value());
        auto const instance = isoline::runtime::instantiate(store, self->executor(), validated, *code, {});
        REQUIRE(instance.has_value());
        auto const function = store.moduleInstance(*instance).functions[0];
        auto const table = store.moduleInstance(*context.callerInstance()).tables[0];
        store.table(table).elements()[1] =
            isoline::runtime::Ref { .kind = isoline::runtime::RefKind::Func, .address = function };
        return {};
    };
    auto instance =
        compiled(Caller, InstanceOptions { .imports = hostImport(isoline::binary::FuncType {}, make) });
    self = instance.get();
    CHECK(instance->call(0, {}) == std::expected<std::vector<Value>, Abrupt> { std::vector { i32(42) } });
    CHECK(instance->store().chargedStackValues() == 0);
}

TEST_CASE("Contract.hostCallsReachAWiderHostFunction")
{
    // A host function adds a host function wider than any the store held: the host-call scratch grows to it
    // before generated code marshals the next call, and a call of more values than the stub keeps inline
    // takes its own buffer.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    constexpr auto Values = std::size_t { 24 };
    auto const wat =
        R"((module (import "host" "f" (func $add)) (type $wide (func (param)" + repeated("i32", Values)
        + ") (result i32))) (table 1 funcref) (func (result i32) (call $add) (call_indirect (type $wide)"
        + repeated("(i32.const 2)", Values) + " (i32.const 0))))";
    auto* self = static_cast<CompiledInstance*>(nullptr);
    auto const add = [&self](HostContext& context, std::span<Value const>, std::span<Value>) -> HostOutcome {
        auto& store = self->store();
        auto type = isoline::binary::FuncType { .params = std::vector(Values, isoline::binary::I32),
                                                .results = { isoline::binary::I32 } };
        auto const sum = store.addHostFunction(
            type,
            isoline::runtime::makeHostFunction([](HostContext&,
                                                  std::span<Value const> arguments,
                                                  std::span<Value> results) noexcept -> HostOutcome {
                auto total = uint32_t { 0 };
                for (auto const& argument: arguments)
                    total += std::get<I32>(argument).bits;
                results[0] = i32(total);
                return {};
            }));
        REQUIRE(sum.has_value());
        auto const table = store.moduleInstance(*context.callerInstance()).tables[0];
        store.table(table).elements()[0] =
            isoline::runtime::Ref { .kind = isoline::runtime::RefKind::Func, .address = *sum };
        return {};
    };
    auto instance =
        compiled(wat, InstanceOptions { .imports = hostImport(isoline::binary::FuncType {}, add) });
    self = instance.get();
    CHECK(instance->call(0, {})
          == std::expected<std::vector<Value>, Abrupt> { std::vector { i32(2 * Values) } });
}

TEST_CASE("Contract.indirectCallsHostAndGuest")
{
    // call_indirect reaches a guest function and a host function alike, and traps as the reference does on a
    // type mismatch, a null element and an index past the table.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    constexpr auto Indirect = std::string_view { R"((module
  (import "host" "f" (func $host (param i32) (result i32)))
  (type $t (func (param i32) (result i32)))
  (table 5 funcref) (elem (i32.const 0) $host $guest $other)
  (func $guest (type $t) (i32.add (local.get 0) (i32.const 100)))
  (func $other (result i32) (i32.const 0))
  (func (param i32) (result i32) (call_indirect (type $t) (i32.const 5) (local.get 0))))
)" };
    auto const host =
        [](HostContext&, std::span<Value const> arguments, std::span<Value> results) -> HostOutcome {
        results[0] = i32(std::get<I32>(arguments[0]).bits * 3);
        return {};
    };
    auto const options =
        InstanceOptions { .imports =
                              hostImport(isoline::binary::FuncType { .params = { isoline::binary::I32 },
                                                                     .results = { isoline::binary::I32 } },
                                         host) };
    for (auto const element: { 0U, 1U, 2U, 3U, 7U })
    {
        INFO("element " << element);
        checkAgainstTheReference(Indirect, 2, { i32(element) }, options);
    }
}

TEST_CASE("Contract.invokesAHostFunctionDirectly")
{
    // The embedder invokes an imported host function through the compiled executor: the callee's
    // FunctionEntry poll first, then the host frame, as every engine does.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    auto calls = std::make_shared<int>(0);
    auto const host = [calls](HostContext&, std::span<Value const>, std::span<Value> results) -> HostOutcome {
        ++*calls;
        results[0] = i32(5);
        return {};
    };
    auto const imports =
        hostImport(isoline::binary::FuncType { .params = {}, .results = { isoline::binary::I32 } }, host);
    constexpr auto Importing = std::string_view { R"((module (import "host" "f" (func (result i32)))))" };
    SECTION("observed: the entry poll, then the host")
    {
        auto polls = PollCounter {};
        auto instance = compiled(Importing, InstanceOptions { .observer = &polls, .imports = imports });
        auto const address =
            instance->store().moduleInstance(instance->context().moduleInstance).functions[0];
        CHECK(instance->invoke(address, {})
              == std::expected<std::vector<Value>, Abrupt> { std::vector { i32(5) } });
        CHECK(polls.count(PollSite::FunctionEntry) == 1);
        CHECK(polls.count(PollSite::HostReturn) == 1);
        CHECK(*calls == 1);
    }
    SECTION("interrupted: the entry poll ends it before the host runs")
    {
        auto instance =
            compiled(Importing,
                     InstanceOptions { .imports = imports,
                                       .interruption = isoline::execution::testing::Interruption::Pending });
        auto const address =
            instance->store().moduleInstance(instance->context().moduleInstance).functions[0];
        CHECK(trapOf(instance->invoke(address, {})) == TrapCode::Interrupted);
        CHECK(*calls == 0);
    }
}

TEST_CASE("Contract.noSweepSeesACompiledFrame")
{
    // A host function creates exceptions up to the store's cap while a compiled frame is active: no sweep
    // runs under a host call, so none charges a step or visits a frame the compiler keeps no roots of.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    constexpr auto Calling = std::string_view { R"((module
  (import "host" "f" (func $make))
  (func (result i32) (local i32) (call $make) (local.set 0 (i32.const 3)) (call $make) (local.get 0)))
)" };
    auto created = std::make_shared<std::vector<std::optional<TrapCode>>>();
    auto const options = [&] {
        auto limits = HarnessLimits;
        limits.maxExceptions = 4;
        auto imports = [created](Store& store) {
            auto const tagType = store.internType(isoline::binary::FuncType {});
            auto const tag = store.addTag(tagType);
            REQUIRE(tag.has_value());
            return hostImport(isoline::binary::FuncType {},
                              [created, tag = *tag](HostContext& context,
                                                    std::span<Value const>,
                                                    std::span<Value>) -> HostOutcome {
                                  for ([[maybe_unused]] auto const attempt: std::views::iota(0, 6))
                                  {
                                      auto const exception = context.addException(tag, {});
                                      if (!exception)
                                      {
                                          created->push_back(trapOf(exception));
                                          return {};
                                      }
                                  }
                                  created->emplace_back(std::nullopt);
                                  return {};
                              })(store);
        };
        return InstanceOptions { .limits = limits, .imports = imports };
    }();
    auto referencePolls = PollCounter {};
    auto referenceOptions = options;
    referenceOptions.observer = &referencePolls;
    auto const reference = runReference(moduleOf(Calling), 0, {}, referenceOptions);
    created->clear();
    auto polls = PollCounter {};
    auto compiledOptions = options;
    compiledOptions.observer = &polls;
    auto instance = compiled(Calling, compiledOptions);
    auto const called = instance->call(0, {});
    CHECK(called == reference.outcome);
    CHECK(called == std::expected<std::vector<Value>, Abrupt> { std::vector { i32(3) } });
    CHECK(polls.count(PollSite::Sweep) == 0);
    CHECK(instance->store().used(Limit::Steps) == reference.steps);
    CHECK(*created == std::vector<std::optional<TrapCode>>(2, TrapCode::ExceptionLimitExceeded));
}

TEST_CASE("Contract.assignmentsCheckOnWholeModules")
{
    // Every function of the contract corpus, as Compiler::compile() selects and allocates it, is a correct
    // allocation of the function as selected (AssignmentChecker), with the module's other functions around
    // it.
    auto const config = hostConfig(Deterministic, {});
    auto const target = isoline::compiler::x86_64::X64Target::create(config.target);
    REQUIRE(target.has_value());
    auto const cancellation = isoline::compiler::CompileCancellation {};
    auto const allocator = isoline::compiler::registerallocation::LinearScanAllocator { config.limits };
    auto checked = 0;
    for (auto const& program: contractPrograms())
    {
        INFO(program.name);
        auto memory = std::pmr::monotonic_buffer_resource {};
        auto const validated = moduleOf(program.wat);
        auto const index = isoline::compiler::translation::ModuleIndex { validated->module(), memory };
        auto const minimums = isoline::compiler::optimization::memoryMinimumBytesOf(*validated, memory);
        for (auto const codeIndex:
             std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(validated->module().code.size())))
        {
            auto const input = isoline::compiler::FunctionInput {
                .module = *validated,
                .index = index,
                .config = config,
                .target = *target,
                .cancellation = cancellation,
                .memoryMinimumBytes = minimums,
                .verification = isoline::compiler::optimization::Verification::AfterEachPass,
                .importedFunctions = 0,
                .codeIndex = codeIndex
            };
            auto const before = isoline::compiler::selectFunction(input, memory);
            auto after = isoline::compiler::selectFunction(input, memory);
            REQUIRE(before.has_value());
            REQUIRE(after.has_value());
            auto const frameCost = validated->frameCost(codeIndex);
            auto const frame = allocator.allocate(*after, *target, frameCost, cancellation, memory);
            REQUIRE(frame.has_value());
            auto const assignment = isoline::compiler::registerallocation::testing::checkAssignment(
                *before, *after, *target, *frame, frameCost, memory);
            CHECK(assignment.has_value());
            ++checked;
        }
    }
    CHECK(checked > 10);
}
