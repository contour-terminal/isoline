// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/FastInterpreter.hpp>
#include <isoline/fastinterpreter/testing/Engines.hpp>
#include <isoline/fastinterpreter/testing/Lowering.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/runtime/NestedInvocationBytes.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>
#include <isoline/runtime/testing/SmallStackThread.hpp>
#include <isoline/runtime/testing/StackAddress.hpp>

using isoline::binary::AbstractHeapType;
using isoline::binary::ExternKind;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::binary::ValueType;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::ModuleBuilder;
using isoline::fastinterpreter::availableDispatches;
using isoline::fastinterpreter::FirstChunkSlots;
using isoline::fastinterpreter::LoweringConfig;
using isoline::fastinterpreter::MaximumGrownChunkSlots;
using isoline::fastinterpreter::testing::EngineUnderTest;
using isoline::fastinterpreter::testing::fast;
using isoline::fastinterpreter::testing::fastEngineOfEachDispatch;
using isoline::fastinterpreter::testing::Harness;
using isoline::fastinterpreter::testing::lowered;
using isoline::fastinterpreter::testing::op;
using isoline::fastinterpreter::testing::Reference;
using isoline::fastinterpreter::testing::validated;
using isoline::runtime::Abrupt;
using isoline::runtime::ExnAddr;
using isoline::runtime::ExternValue;
using isoline::runtime::FuncAddr;
using isoline::runtime::HostContext;
using isoline::runtime::I32;
using isoline::runtime::Limit;
using isoline::runtime::makeHostFunction;
using isoline::runtime::PollSite;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::testing::PollCounter;

namespace binary = isoline::binary;

namespace
{

/// What a host function returns.
using Outcome = std::expected<void, Abrupt>;

[[nodiscard]] Value i32(int32_t value)
{
    return I32 { static_cast<uint32_t>(value) };
}

/// The (i32) -> i32 type every recursive guest has.
[[nodiscard]] FuncType unaryI32()
{
    return FuncType { .params = { binary::I32 }, .results = { binary::I32 } };
}

/// Adds a host function of @p type implemented by @p function to @p store.
template <typename F>
[[nodiscard]] FuncAddr addHost(Store& store, FuncType type, F function)
{
    auto const address = store.addHostFunction(std::move(type), makeHostFunction(std::move(function)));
    REQUIRE(address.has_value());
    return *address;
}

/// How a recursive guest calls itself.
enum class Recursion : uint8_t
{
    Call,         ///< call.
    CallIndirect, ///< call_indirect through table element 0.
    CallRef,      ///< call_ref of a global's reference.
    ReturnCall,   ///< return_call.
    HostReentry,  ///< call of an imported host function, which invokes the guest again.
};

/// Every Recursion, with its name.
[[nodiscard]] std::vector<std::pair<Recursion, std::string>> recursions()
{
    return { { Recursion::Call, "call" },
             { Recursion::CallIndirect, "call_indirect" },
             { Recursion::CallRef, "call_ref" },
             { Recursion::ReturnCall, "return_call" },
             { Recursion::HostReentry, "host re-entry" } };
}

/// The function index of the recursive guest in recursiveGuest()'s module; its import "env" "reenter" is 0.
constexpr auto Guest = uint32_t { 1 };

/// A module whose function 1, f(n) of three declared i32 locals, counts its entries in global 0 and returns
/// n == 0 ? 0 : f(n - 1) + 1, recursing as @p recursion says (a tail call returns f(n - 1) itself, so 0).
/// Function 0 is an imported (i32) -> i32 host function, "env" "reenter"; table 0 holds f, and global 1 is a
/// (ref null 0) to it.
[[nodiscard]] ModuleBuilder recursiveGuest(Recursion recursion)
{
    auto recurse = Bytes {};
    switch (recursion)
    {
        case Recursion::Call: recurse = op(0x10, Guest); break;
        case Recursion::CallIndirect: recurse = code({ i32Const(0), { 0x11, 0x00, 0x00 } }); break;
        case Recursion::CallRef: recurse = code({ op(0x23, 1), { 0x14, 0x00 } }); break;
        case Recursion::ReturnCall: recurse = op(0x12, Guest); break;
        case Recursion::HostReentry: recurse = op(0x10, 0); break;
    }
    auto const tail = recursion == Recursion::ReturnCall;
    auto module = ModuleBuilder {};
    module.type(unaryI32());
    module.importFunction("env", "reenter", 0);
    module.table(
        TableType { .element = binary::FuncRef,
                    .limits = { .min = 1, .max = std::nullopt, .addressType = binary::AddressType::I32 } });
    module.global(GlobalType { .type = binary::I32, .mutability = Mutability::Var }, i32Const(0));
    module.global(
        GlobalType { .type = binary::concreteRef(0, binary::Nullable::Yes), .mutability = Mutability::Const },
        code({ { 0xD2 }, encodeU32(Guest) }));
    module.function(0,
                    { binary::I32, binary::I32, binary::I32 },
                    code({ op(0x23, 0),
                           i32Const(1),
                           op(0x6A),
                           op(0x24, 0),
                           localGet(0),
                           { 0x45, 0x04, 0x7F }, // i32.eqz; if (result i32)
                           i32Const(0),
                           { 0x05 },
                           localGet(0),
                           i32Const(1),
                           op(0x6B),
                           recurse,
                           tail ? Bytes {} : code({ i32Const(1), op(0x6A) }),
                           { 0x0B } }));
    module.element(code({ { 0x00 }, i32Const(0), { 0x0B, 0x01 }, encodeU32(Guest) }));
    return module;
}

/// A harness of recursiveGuest(), whose import invokes the guest again with its argument, through its
/// HostContext, and returns what it returned.
class Recursive
{
  public:
    Recursive(Recursion recursion,
              EngineUnderTest engine,
              StoreLimits limits,
              PollCounter* observer = nullptr):
        _harness { std::make_unique<Harness>(
            recursiveGuest(recursion),
            engine,
            [this](Store& store) {
                auto const host =
                    addHost(store,
                            unaryI32(),
                            [this](HostContext& context,
                                   std::span<Value const> args,
                                   std::span<Value> results) noexcept -> Outcome {
                                // Where this nesting's host frame is, on the real stack.
                                if (_hostStack.size() < _hostStack.capacity())
                                    _hostStack.push_back(isoline::runtime::testing::stackAddress());
                                return context.invoke(_harness->function(Guest), args, results);
                            });
                return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
            },
            limits,
            observer) }
    {
        _hostStack.reserve(256);
    }

    /// Invokes f(@p n).
    [[nodiscard]] std::expected<std::vector<Value>, Abrupt> run(int32_t n)
    {
        return _harness->outcome(Guest, { i32(n) });
    }

    /// The entries of f so far.
    [[nodiscard]] Value entries() { return store().global(_harness->instance().globals.at(0)).value(); }

    [[nodiscard]] Store& store() noexcept { return _harness->store(); }

    [[nodiscard]] Harness& harness() noexcept { return *_harness; }

    /// The most host stack one nesting of host re-entry took: the largest distance between the host frames of
    /// consecutive nestings (the stack grows down on every target, but the distance is taken either way).
    [[nodiscard]] std::size_t nestingStackBytes() const noexcept
    {
        auto most = std::uintptr_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 1 }, _hostStack.size()))
            most = std::max(most,
                            std::max(_hostStack[index - 1], _hostStack[index])
                                - std::min(_hostStack[index - 1], _hostStack[index]));
        return static_cast<std::size_t>(most);
    }

  private:
    std::unique_ptr<Harness> _harness;
    /// Each host call's frame address, in call order; reserved up front, so the host allocates nothing.
    std::vector<std::uintptr_t> _hostStack;
};

/// What one run of a recursive guest showed: how it ended, the steps, the entries and the stack left charged.
struct Observed
{
    std::expected<std::vector<Value>, Abrupt> outcome;
    uint64_t steps = 0;
    Value entries;
    uint64_t charged = 0;

    [[nodiscard]] bool operator==(Observed const&) const = default;
};

/// Runs f(@p n) of the guest recursing by @p recursion on @p engine in a store of @p limits.
[[nodiscard]] Observed observe(Recursion recursion, EngineUnderTest engine, StoreLimits limits, int32_t n)
{
    auto guest = Recursive { recursion, engine, limits };
    auto outcome = guest.run(n);
    return Observed { .outcome = std::move(outcome),
                      .steps = guest.store().used(Limit::Steps),
                      .entries = guest.entries(),
                      .charged = guest.store().chargedStackValues() };
}

/// The trap code of @p observed, which must have trapped.
[[nodiscard]] TrapCode trapOf(Observed const& observed)
{
    REQUIRE_FALSE(observed.outcome.has_value());
    REQUIRE(std::holds_alternative<Trap>(observed.outcome.error()));
    return std::get<Trap>(observed.outcome.error()).code;
}

/// Passes every allocation on to the global heap, and records the bytes held at once, now and at the most.
class PeakResource final: public std::pmr::memory_resource
{
  public:
    [[nodiscard]] uint64_t peak() const noexcept { return _peak; }

    [[nodiscard]] uint64_t held() const noexcept { return _held; }

    [[nodiscard]] uint64_t allocations() const noexcept { return _allocations; }

  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        ++_allocations;
        _held += bytes;
        _peak = std::max(_peak, _held);
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
    {
        _held -= bytes;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override
    {
        return this == &other;
    }

    uint64_t _held = 0;
    uint64_t _peak = 0;
    uint64_t _allocations = 0;
};

/// The most bytes one invocation's slot chunks may hold, its frames active at once taking @p activeSlots
/// slots: docs/fast-interpreter.md's bound, 2 × A + min(2 × A, MaximumGrownChunkSlots) + FirstChunkSlots
/// slots.
[[nodiscard]] uint64_t chunkBytesBound(uint64_t activeSlots) noexcept
{
    return ((2 * activeSlots) + std::min(2 * activeSlots, MaximumGrownChunkSlots) + FirstChunkSlots)
           * sizeof(isoline::fastinterpreter::Slot);
}

/// The slots of @p harness's function @p codeIndex's frame.
[[nodiscard]] uint64_t frameSlotsOf(Harness const& harness, uint32_t codeIndex)
{
    REQUIRE(harness.code() != nullptr);
    return harness.code()->functions()[codeIndex].frameSlots;
}

/// A function of (i32, i32) -> i32 whose operand stack rises @p height constants high, then returns
/// a + b + its one declared i32 local, with @p tail at its end: a call or a tail call of something of its
/// type.
[[nodiscard]] Bytes highFunction(std::size_t height, Bytes const& tail)
{
    auto body = code({ localGet(0), localGet(1), op(0x6A) });
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, height))
        body.insert(body.end(), { 0x41, 0x01 }); // i32.const 1
    body.insert(body.end(), height, 0x1A);       // drop
    return code({ body, localGet(2), op(0x6A), tail });
}

} // namespace

TEST_CASE("Calls.sameExhaustionPointAsReference")
{
    // Whichever limit runs out first, and however the guest recurses, both engines stop at the same entry:
    // the same trap (or the same result, where a tail call never deepens), after the same steps and entries,
    // with nothing left charged.
    auto const engine = fastEngineOfEachDispatch();
    auto const limits = std::vector<std::pair<std::string, StoreLimits>> {
        { "maxCallDepth 50", StoreLimits { .maxCallDepth = 50 } },
        { "maxStackValues 1'000", StoreLimits { .maxStackValues = 1'000 } },
        { "maxSteps 300", StoreLimits { .maxSteps = 300 } },
    };
    for (auto const& [recursion, kind]: recursions())
    {
        for (auto const& [name, limit]: limits)
        {
            INFO(kind << " under " << name);
            auto const reference = observe(recursion, Reference, limit, 10'000);
            auto const observed = observe(recursion, engine, limit, 10'000);
            CHECK(observed == reference);
            CHECK(observed.charged == 0);
            if (recursion != Recursion::ReturnCall || name == "maxSteps 300")
                CHECK(trapOf(observed) == trapOf(reference));
        }
    }
}

TEST_CASE("Calls.tiesFollowTheEntryOrder")
{
    // Where the steps run out at the very entry the depth or the stack budget refuses, the FunctionEntry poll
    // comes first: StepLimitExceeded, on both engines, after the same steps.
    auto const engine = fastEngineOfEachDispatch();
    SECTION("a tail call into a costlier callee")
    {
        // f0 return_calls f1, whose frame is past the stack budget f0's fits: f0's charge is given back, and
        // then the steps and the stack run out at f1's entry together.
        auto module = ModuleBuilder {};
        module.type(FuncType { .params = { binary::I32, binary::I32 }, .results = { binary::I32 } });
        module.function(0, { binary::I32 }, code({ localGet(0), localGet(1), op(0x12, 1) }));
        module.function(0, { binary::I32 }, highFunction(100, {}));
        auto const costs = validated(module);
        auto const limit = StoreLimits { .maxStackValues = costs->frameCost(0) };
        auto exhausted = Harness { module, Reference, limit };
        REQUIRE(exhausted.trap(0, { i32(1), i32(2) }) == TrapCode::CallStackExhausted);
        auto tie = limit;
        tie.maxSteps = exhausted.store().used(Limit::Steps) - 1;
        auto reference = Harness { module, Reference, tie };
        auto harness = Harness { module, engine, tie };
        CHECK(reference.trap(0, { i32(1), i32(2) }) == TrapCode::StepLimitExceeded);
        CHECK(harness.trap(0, { i32(1), i32(2) }) == TrapCode::StepLimitExceeded);
        CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
        CHECK(harness.store().chargedStackValues() == 0);
    }
    for (auto const& [recursion, kind]: recursions())
    {
        if (recursion == Recursion::ReturnCall)
            continue; // A tail call into its own kind deepens nothing; the section above ties one.
        for (auto const& limit: { StoreLimits { .maxCallDepth = 40 }, StoreLimits { .maxStackValues = 800 } })
        {
            INFO(kind << " at depth " << limit.maxCallDepth << ", stack " << limit.maxStackValues);
            auto const exhausted = observe(recursion, Reference, limit, 10'000);
            REQUIRE(trapOf(exhausted) == TrapCode::CallStackExhausted);
            // One step short: the poll of the entry the limit refused is the one that fails.
            auto tie = limit;
            tie.maxSteps = exhausted.steps - 1;
            auto const reference = observe(recursion, Reference, tie, 10'000);
            auto const observed = observe(recursion, engine, tie, 10'000);
            CHECK(trapOf(reference) == TrapCode::StepLimitExceeded);
            CHECK(trapOf(observed) == TrapCode::StepLimitExceeded);
            CHECK(observed == reference);
        }
    }
}

TEST_CASE("Calls.tailCallReleasesBeforeCharging")
{
    // A driver calls f(1'000'000), which return_calls itself down to 0, under a stack budget of exactly the
    // two frames: each tail call gives its frame back before the next is charged.
    auto module = ModuleBuilder {};
    module.type(unaryI32());
    module.type(FuncType { .params = {}, .results = { binary::I32 } });
    module.function(0,
                    { binary::I32 },
                    code({ localGet(0),
                           { 0x45, 0x04, 0x7F },
                           i32Const(0),
                           { 0x05 },
                           localGet(0),
                           i32Const(1),
                           op(0x6B),
                           op(0x12, 0),
                           { 0x0B } }));
    module.function(1, { binary::I32 }, code({ i32Const(1'000'000), op(0x10, 0) }));
    auto const module0 = validated(module);
    auto const limits = StoreLimits { .maxStackValues = module0->frameCost(0) + module0->frameCost(1) };
    auto reference = Harness { module, Reference, limits };
    auto harness = Harness { module, fastEngineOfEachDispatch(), limits };
    CHECK(reference.result(1) == i32(0));
    CHECK(harness.result(1) == i32(0));
    CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
    CHECK(harness.store().chargedStackValues() == 0);
}

TEST_CASE("Calls.hostReturnReplacesOutcomeOnInterrupt")
{
    // A host function that interrupts the store and then returns success: its HostReturn poll replaces the
    // outcome, whether it was called or tail-called.
    auto const engine = fastEngineOfEachDispatch();
    for (auto const opcode: { uint8_t { 0x10 }, uint8_t { 0x12 } })
    {
        INFO("opcode " << int { opcode });
        auto module = ModuleBuilder {};
        module.type(unaryI32());
        module.importFunction("env", "interrupt", 0);
        module.function(0, {}, code({ localGet(0), op(opcode, 0) }));
        auto const imports = [](Store& store) {
            auto const host = addHost(store,
                                      unaryI32(),
                                      [&store](HostContext&,
                                               std::span<Value const> args,
                                               std::span<Value> results) noexcept -> Outcome {
                                          store.interrupter()->interrupt();
                                          results[0] = args[0];
                                          return {};
                                      });
            return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
        };
        auto reference = Harness { module, Reference, imports };
        auto harness = Harness { module, engine, imports };
        CHECK(reference.trap(1, { i32(5) }) == TrapCode::Interrupted);
        CHECK(harness.trap(1, { i32(5) }) == TrapCode::Interrupted);
        CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
        CHECK(harness.store().chargedStackValues() == 0);
    }
}

TEST_CASE("Calls.hostReentryNestsAndSharesSteps")
{
    // Three levels of the guest, each the host's re-entry through HostContext::invoke: one step budget and
    // one depth count over all of them, as the reference counts them.
    auto const engine = fastEngineOfEachDispatch();
    for (auto const& limit: { StoreLimits {},
                              StoreLimits { .maxCallDepth = 2 },
                              StoreLimits { .maxSteps = 20 },
                              StoreLimits { .maxNestedInvocations = 3 } })
    {
        INFO("depth " << limit.maxCallDepth << ", steps " << limit.maxSteps << ", nesting "
                      << limit.maxNestedInvocations);
        auto const reference = observe(Recursion::HostReentry, Reference, limit, 3);
        auto const observed = observe(Recursion::HostReentry, engine, limit, 3);
        CHECK(observed == reference);
        CHECK(observed.charged == 0);
    }
    CHECK(observe(Recursion::HostReentry, engine, {}, 3).outcome == std::vector<Value> { i32(3) });
    SECTION("an instantiation from a host function shares the caller's budget")
    {
        // The host instantiates a module whose start function recurses through 10 frames: its steps are the
        // caller's, on both engines.
        auto started = ModuleBuilder {};
        started.type(FuncType {});
        started.type(unaryI32());
        started.function(1,
                         {},
                         code({ localGet(0),
                                { 0x45, 0x04, 0x7F },
                                i32Const(0),
                                { 0x05 },
                                localGet(0),
                                i32Const(1),
                                op(0x6B),
                                op(0x10, 0),
                                { 0x0B } }));
        started.function(0, {}, code({ i32Const(10), op(0x10, 0), op(0x1A) }));
        started.start(1);
        auto const startedModule = validated(started);
        auto guest = ModuleBuilder {};
        guest.type(FuncType {});
        guest.importFunction("env", "instantiate", 0);
        guest.function(0, {}, op(0x10, 0));
        auto const stepsOn = [&](EngineUnderTest under) {
            auto const code = under.dispatch
                                  ? lowered(startedModule, LoweringConfig { .dispatch = *under.dispatch })
                                  : nullptr;
            Harness* self = nullptr;
            auto harness =
                Harness { guest, under, [&](Store& store) {
                             auto const host = addHost(
                                 store,
                                 FuncType {},
                                 [&store, &self, code, startedModule](HostContext&,
                                                                      std::span<Value const>,
                                                                      std::span<Value>) noexcept -> Outcome {
                                     auto const instance = isoline::runtime::instantiate(
                                         store, self->executor(), startedModule, code, {});
                                     if (!instance)
                                         return std::unexpected { Abrupt { Trap { TrapCode::Unreachable } } };
                                     return {};
                                 });
                             return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                         } };
            self = &harness;
            REQUIRE(harness.outcome(1).has_value());
            CHECK(harness.store().chargedStackValues() == 0);
            return harness.store().used(Limit::Steps);
        };
        CHECK(stepsOn(engine) == stepsOn(Reference));
    }
}

TEST_CASE("Calls.hostArgumentsEscape")
{
    // An exnref the guest hands a host survives every later sweep: the host may keep it past its frame.
    auto const engine = fastEngineOfEachDispatch();
    auto module = ModuleBuilder {};
    module.type(FuncType { .params = { binary::ExnRef }, .results = {} });
    module.importFunction("env", "keep", 0);
    module.function(0, {}, code({ localGet(0), op(0x10, 0) }));
    auto const imports = [](Store& store) {
        auto const host = addHost(
            store,
            FuncType { .params = { binary::ExnRef }, .results = {} },
            [](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> Outcome { return {}; });
        return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
    };
    auto const survives = [&](EngineUnderTest under) {
        auto harness = Harness { module, under, imports, StoreLimits { .maxExceptions = 16 } };
        auto& store = harness.store();
        auto const tag =
            store.addTag(store.internType(FuncType { .params = { binary::I32 }, .results = {} })).value();
        auto const handed = store.addException(tag, { I32 { 42 } });
        REQUIRE(handed.has_value());
        auto const reference =
            Value { Ref { .kind = RefKind::Exn, .heap = binary::ExnRef.heap, .address = *handed } };
        REQUIRE(harness.outcome(1, { reference }).has_value());
        for ([[maybe_unused]] auto const round: std::views::iota(0, 20))
            REQUIRE(store.addException(tag, { I32 { 7 } }).has_value());
        // Its own payload: a reclaimed slot would be live again, holding a later exception.
        return store.isLiveException(*handed)
               && store.exception(*handed).payload == std::vector<Value> { I32 { 42 } };
    };
    CHECK(survives(Reference));
    CHECK(survives(engine));
}

TEST_CASE("Calls.indirectCallTraps")
{
    // An undefined, an uninitialized and a mistyped table element, and a null reference, trap before the
    // callee's FunctionEntry poll: the same trap and subject, the same steps and polls, as the reference.
    auto const engine = fastEngineOfEachDispatch();
    auto module = ModuleBuilder {};
    module.type(unaryI32());
    module.type(FuncType { .params = {}, .results = { binary::I32 } });
    module.type(FuncType { .params = { binary::concreteRef(0, binary::Nullable::Yes), binary::I32 },
                           .results = { binary::I32 } });
    module.table(
        TableType { .element = binary::FuncRef,
                    .limits = { .min = 3, .max = std::nullopt, .addressType = binary::AddressType::I32 } });
    // f0(i) calls table[i] as (i32) -> i32; f1 is its callee, f2 one of another type; f3(r, x) call_refs r;
    // f4 and f5 the tail twins of f0 and f3.
    module.function(0, {}, code({ i32Const(7), localGet(0), { 0x11, 0x00, 0x00 } }));
    module.function(0, {}, localGet(0));
    module.function(1, {}, i32Const(9));
    module.function(2, {}, code({ localGet(1), localGet(0), { 0x14, 0x00 } }));
    module.function(0, {}, code({ i32Const(7), localGet(0), { 0x13, 0x00, 0x00 } }));
    module.function(2, {}, code({ localGet(1), localGet(0), { 0x15, 0x00 } }));
    module.element(code({ { 0x00 }, i32Const(0), { 0x0B, 0x02, 0x01, 0x02 } }));
    auto const nullReference =
        Value { Ref { .kind = RefKind::Null, .heap = { .kind = AbstractHeapType::Func }, .address = 0 } };
    struct Row
    {
        uint32_t function = 0;
        std::vector<Value> args;
        Trap trap;
    };
    auto const rows = std::vector<Row> {
        { 0, { i32(1) }, Trap { TrapCode::IndirectCallTypeMismatch } },
        { 0, { i32(2) }, Trap { TrapCode::UninitializedElement, 2 } },
        { 0, { i32(5) }, Trap { TrapCode::UndefinedElement, 5 } },
        { 0, { i32(-1) }, Trap { TrapCode::UndefinedElement, 0xFFFF'FFFF } },
        { 3, { nullReference, i32(1) }, Trap { TrapCode::NullFunctionReference } },
        { 4, { i32(1) }, Trap { TrapCode::IndirectCallTypeMismatch } },
        { 4, { i32(2) }, Trap { TrapCode::UninitializedElement, 2 } },
        { 4, { i32(5) }, Trap { TrapCode::UndefinedElement, 5 } },
        { 5, { nullReference, i32(1) }, Trap { TrapCode::NullFunctionReference } },
    };
    for (auto const& row: rows)
    {
        INFO("function " << row.function << ", trap " << static_cast<int>(row.trap.code));
        auto referencePolls = PollCounter {};
        auto polls = PollCounter {};
        auto reference = Harness { module, Reference, {}, &referencePolls };
        auto harness = Harness { module, engine, {}, &polls };
        auto const expected =
            std::expected<std::vector<Value>, Abrupt> { std::unexpected { Abrupt { row.trap } } };
        CHECK(reference.outcome(row.function, row.args) == expected);
        CHECK(harness.outcome(row.function, row.args) == expected);
        CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
        // Only the caller's own entry polled.
        CHECK(polls.count(PollSite::FunctionEntry) == 1);
        CHECK(referencePolls.count(PollSite::FunctionEntry) == 1);
        CHECK(harness.store().liveExceptions() == reference.store().liveExceptions());
        CHECK(harness.store().chargedStackValues() == 0);
    }
    CHECK(Harness { module, engine }.result(0, { i32(0) }) == i32(7));
    SECTION("a table of 64-bit addresses")
    {
        // The index is the whole slot: 2^32 is past the table, not element 0.
        auto wide = ModuleBuilder {};
        wide.type(unaryI32());
        wide.type(FuncType { .params = { binary::I64 }, .results = { binary::I32 } });
        wide.table(TableType {
            .element = binary::FuncRef,
            .limits = { .min = 1, .max = std::nullopt, .addressType = binary::AddressType::I64 } });
        wide.function(1, {}, code({ i32Const(7), localGet(0), { 0x11, 0x00, 0x00 } }));
        wide.function(0, {}, localGet(0));
        wide.element(code({ { 0x02, 0x00 }, { 0x42, 0x00, 0x0B }, { 0x00, 0x01, 0x01 } }));
        auto const index = [](uint64_t value) {
            return Value { isoline::runtime::I64 { value } };
        };
        for (auto const value: { uint64_t { 0 }, uint64_t { 1 } << 32U, uint64_t { 1 } })
        {
            auto reference = Harness { wide, Reference };
            auto harness = Harness { wide, engine };
            CHECK(harness.outcome(0, { index(value) }) == reference.outcome(0, { index(value) }));
            CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
        }
        CHECK(Harness { wide, engine }.trap(0, { index(uint64_t { 1 } << 32U) })
              == TrapCode::UndefinedElement);
        CHECK(Harness { wide, engine }.result(0, { index(0) }) == i32(7));
    }
}

TEST_CASE("Calls.stackChargeIsReleasedOnTrap")
{
    // However a call traps, in its callee, at its entry, at its resolution, in a host function or in a tail
    // call, every frame's charge is given back.
    auto const engine = fastEngineOfEachDispatch();
    auto module = ModuleBuilder {};
    module.type(unaryI32());
    module.importFunction("env", "fail", 0);
    // f1(n) = n == 0 ? unreachable : f1(n - 1); f2 calls the failing host; f3 return_calls it; f4
    // return_calls f1; f5 calls table[n].
    module.table(
        TableType { .element = binary::FuncRef,
                    .limits = { .min = 1, .max = std::nullopt, .addressType = binary::AddressType::I32 } });
    module.function(0,
                    {},
                    code({ localGet(0),
                           { 0x45, 0x04, 0x7F },
                           { 0x00 },
                           { 0x05 },
                           localGet(0),
                           i32Const(1),
                           op(0x6B),
                           op(0x10, 1),
                           { 0x0B } }));
    module.function(0, {}, code({ localGet(0), op(0x10, 0) }));
    module.function(0, {}, code({ localGet(0), op(0x12, 0) }));
    module.function(0, {}, code({ localGet(0), op(0x12, 1) }));
    module.function(0, {}, code({ localGet(0), localGet(0), { 0x11, 0x00, 0x00 } }));
    auto const imports = [](Store& store) {
        auto const host =
            addHost(store,
                    unaryI32(),
                    [](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> Outcome {
                        return std::unexpected { Abrupt { Trap { TrapCode::IntegerOverflow } } };
                    });
        return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
    };
    struct Row
    {
        uint32_t function = 0;
        int32_t argument = 0;
        StoreLimits limits;
    };
    for (auto const& row: { Row { 1, 5, {} },
                            Row { 1, 100, StoreLimits { .maxCallDepth = 10 } },
                            Row { 1, 100, StoreLimits { .maxStackValues = 50 } },
                            Row { 1, 100, StoreLimits { .maxSteps = 20 } },
                            Row { 2, 1, {} },
                            Row { 3, 1, {} },
                            Row { 4, 5, {} },
                            Row { 5, 3, {} } })
    {
        INFO("function " << row.function << " of " << row.argument);
        auto reference = Harness { module, Reference, imports, row.limits };
        auto harness = Harness { module, engine, imports, row.limits };
        auto const outcome = harness.outcome(row.function, { i32(row.argument) });
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome == reference.outcome(row.function, { i32(row.argument) }));
        CHECK(harness.store().chargedStackValues() == 0);
        CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
    }
    // A tail call refused at its callee's entry: f4's frame fits the stack budget, f1's, which replaces it,
    // does not.
    auto const costs = validated(module);
    REQUIRE(costs->frameCost(3) < costs->frameCost(0));
    auto const tight = StoreLimits { .maxStackValues = costs->frameCost(3) };
    auto reference = Harness { module, Reference, imports, tight };
    auto harness = Harness { module, engine, imports, tight };
    CHECK(harness.trap(4, { i32(5) }) == TrapCode::CallStackExhausted);
    CHECK(reference.trap(4, { i32(5) }) == TrapCode::CallStackExhausted);
    CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
    CHECK(harness.store().chargedStackValues() == 0);
}

TEST_CASE("Calls.newChunkCopiesOnlyTheArguments")
{
    // f0(a, b) calls f1(a, b), whose operand stack, 66'000 constants high, does not fit the chunk f0's frame
    // is in: f1's frame starts a chunk of its own, its arguments copied there; f1 calls f2(a + b), whose
    // frame fits after its argument in f1's chunk. Every value is a number, one slot each, so the slot chunks
    // stay within twice the stack budget plus one chunk.
    // f3 is f1 at 68'000, and f4(a, b) calls f1, f3 and f1 again: f3's frame does not fit the chunk f1 left,
    // which is replaced; the second f1's fits the replacement, which is reused.
    auto module = ModuleBuilder {};
    module.type(FuncType { .params = { binary::I32, binary::I32 }, .results = { binary::I32 } });
    module.type(unaryI32());
    module.function(0, {}, code({ localGet(0), localGet(1), op(0x10, 1), i32Const(1), op(0x6A) }));
    module.function(0, { binary::I32 }, highFunction(66'000, op(0x10, 2)));
    module.function(1, { binary::I32 }, code({ localGet(0), i32Const(10), op(0x6A) }));
    module.function(0, { binary::I32 }, highFunction(68'000, op(0x10, 2)));
    module.function(0,
                    {},
                    code({ localGet(0),
                           localGet(1),
                           op(0x10, 1),
                           localGet(0),
                           localGet(1),
                           op(0x10, 3),
                           op(0x6A),
                           localGet(0),
                           localGet(1),
                           op(0x10, 1),
                           op(0x6A) }));
    auto const module0 = validated(module);
    auto const limits = StoreLimits { .maxStackValues = module0->frameCost(0) + module0->frameCost(1)
                                                        + module0->frameCost(2) };
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto memory = PeakResource {};
    auto harness =
        Harness { module, EngineUnderTest { .dispatch = dispatch, .slotMemory = &memory }, limits };
    auto reference = Harness { module, Reference, limits };
    CHECK(harness.result(0, { i32(3), i32(4) }) == i32(18));
    CHECK(harness.result(0, { i32(3), i32(4) }) == reference.result(0, { i32(3), i32(4) }));
    CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
    // f0's chunk and f1's, of f1's own size, for each of the two invocations: f2's frame begins at its
    // argument, low in f1's frame, and fits in f1's chunk.
    CHECK(memory.allocations() == 4);
    CHECK(memory.held() == 0);
    // f1's frame over the whole of f0's: a bound on the frames active at once.
    auto const active = frameSlotsOf(harness, 0) + frameSlotsOf(harness, 1) + frameSlotsOf(harness, 2);
    CHECK(memory.peak() <= chunkBytesBound(active));
    // Every value is a number, one slot each, so that is within twice the stack budget plus one grown chunk.
    CHECK(chunkBytesBound(active)
          <= ((2 * limits.maxStackValues) + MaximumGrownChunkSlots + FirstChunkSlots) * 8);
    auto replaced = PeakResource {};
    auto replacing = Harness { module, EngineUnderTest { .dispatch = dispatch, .slotMemory = &replaced } };
    CHECK(replacing.result(4, { i32(3), i32(4) }) == i32(51));
    CHECK(replacing.result(4, { i32(3), i32(4) })
          == Harness { module, Reference }.result(4, { i32(3), i32(4) }));
    // f4's chunk, f1's, and f3's in its place, in each of the two invocations; the second f1 allocates
    // nothing.
    CHECK(replaced.allocations() == 6);
    CHECK(replaced.held() == 0);
}

TEST_CASE("Calls.deepRecursionDoesNotRecurseOnTheHostStack", "[.memory]")
{
    // A million Wasm frames deep, under a 1 MiB main-thread stack on Linux: hidden from the unit run, it runs
    // alone under the memory caps, tests/stack/StackLimit.cmake's row. The depth limit binds, not the stack
    // budget, and the slot chunks stay within their bound.
    constexpr auto Depth = uint32_t { 1'000'000 };
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto const cost = validated(recursiveGuest(Recursion::Call))->frameCost(0);
    auto memory = PeakResource {};
    auto guest = Recursive { Recursion::Call,
                             EngineUnderTest { .dispatch = dispatch, .slotMemory = &memory },
                             StoreLimits { .maxCallDepth = Depth,
                                           .maxStackValues = (uint64_t { Depth } + 1) * cost } };
    auto const outcome = guest.run(2 * static_cast<int32_t>(Depth));
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error() == Abrupt { Trap { TrapCode::CallStackExhausted } });
    CHECK(guest.entries() == Value { I32 { Depth } });
    CHECK(guest.store().chargedStackValues() == 0);
    CHECK(memory.held() == 0);
    CHECK(memory.peak() <= chunkBytesBound(uint64_t { Depth } * frameSlotsOf(guest.harness(), 0)));
}

TEST_CASE("Calls.tailChainKeepsOneChunk")
{
    // f0 return_calls f1, f1 f2, and so on, each frame larger than the last and past the chunk the one before
    // left: each callee takes its predecessor's chunk, which no other frame holds, so the chain holds one
    // chunk (and, while it is replaced, the smaller one it replaces), not one per frame. Run alone, and
    // called by a driver whose frame stays in the first chunk.
    constexpr auto Steps = uint32_t { 6 };
    auto module = ModuleBuilder {};
    module.type(FuncType { .params = { binary::I32, binary::I32 }, .results = { binary::I32 } });
    for (auto const index: std::views::iota(uint32_t { 0 }, Steps))
    {
        auto const last = index + 1 == Steps;
        module.function(0,
                        { binary::I32 },
                        highFunction(66'000 + (std::size_t { index } * 10'000),
                                     last ? Bytes {} : code({ i32Const(1), op(0x12, index + 1) })));
    }
    module.function(0, {}, code({ localGet(0), localGet(1), op(0x10, 0), i32Const(1), op(0x6A) }));
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto reference = Harness { module, Reference };
    for (auto const& [entry, driverSlots]: { std::pair { 0U, false }, std::pair { Steps, true } })
    {
        INFO("entry " << entry);
        auto memory = PeakResource {};
        auto harness = Harness { module, EngineUnderTest { .dispatch = dispatch, .slotMemory = &memory } };
        CHECK(harness.result(entry, { i32(3), i32(4) }) == reference.result(entry, { i32(3), i32(4) }));
        // One chain frame at a time, the largest the last, beside the driver's.
        auto const active =
            frameSlotsOf(harness, Steps - 1) + (driverSlots ? frameSlotsOf(harness, Steps) : 0);
        CHECK(memory.peak() <= chunkBytesBound(active));
        CHECK(memory.held() == 0);
    }
}

TEST_CASE("Calls.tailCallOutOfTheFirstChunkKeepsItsResults")
{
    // The invocation's own f0 return_calls f1, whose frame is past the first chunk's FirstChunkSlots: f1
    // takes that chunk's place, a new chunk, and the invocation's results go to f1's own slots, not to the
    // freed chunk's. f1 then ends each way a frame can: a tail call of a host function, whose results are
    // written to the invocation's results; a Wasm return; a trap.
    struct Ending
    {
        std::string name;
        Bytes tail;
    };
    auto const endings = std::vector<Ending> {
        { .name = "a host tail call", .tail = code({ i32Const(1), op(0x12, 0) }) },
        { .name = "a Wasm return", .tail = {} },
        { .name = "a trap", .tail = { 0x00 } },
    };
    auto const imports = [](Store& store) {
        auto const host = addHost(
            store,
            FuncType { .params = { binary::I32, binary::I32 }, .results = { binary::I32 } },
            [](HostContext&, std::span<Value const> args, std::span<Value> results) noexcept -> Outcome {
                results[0] = I32 { std::get<I32>(args[0]).bits + std::get<I32>(args[1]).bits };
                return {};
            });
        return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
    };
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    for (auto const& ending: endings)
    {
        INFO(ending.name);
        auto module = ModuleBuilder {};
        module.type(FuncType { .params = { binary::I32, binary::I32 }, .results = { binary::I32 } });
        module.importFunction("env", "add", 0);
        module.function(0, {}, code({ localGet(0), localGet(1), op(0x12, 2) }));
        module.function(0, { binary::I32 }, highFunction(FirstChunkSlots + 100, ending.tail));
        auto memory = PeakResource {};
        auto harness =
            Harness { module, EngineUnderTest { .dispatch = dispatch, .slotMemory = &memory }, imports };
        auto reference = Harness { module, Reference, imports };
        REQUIRE(frameSlotsOf(harness, 1) > FirstChunkSlots);
        CHECK(harness.outcome(1, { i32(3), i32(4) }) == reference.outcome(1, { i32(3), i32(4) }));
        CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
        CHECK(harness.store().chargedStackValues() == 0);
        // The first chunk and f1's, which replaced it.
        CHECK(memory.allocations() == 2);
        CHECK(memory.held() == 0);
    }
}

TEST_CASE("Calls.framesHoldTheirResults")
{
    // A function that never pushes its results still receives them in its frame: here 300 of them, more than
    // FirstChunkSlots, from a host function it tail-calls. Its frame holds its results whatever its body
    // pushes: one whose whole body is the return_call; the same after a tail chain f0 → f1; and one whose
    // block's end, and then the function's, are dead code.
    constexpr auto Results = std::size_t { 300 };
    auto const many = FuncType { .params = {}, .results = std::vector<ValueType>(Results, binary::I32) };
    auto const imports = [&many](Store& store) {
        auto const host =
            addHost(store,
                    many,
                    [](HostContext&, std::span<Value const>, std::span<Value> results) noexcept -> Outcome {
                        for (auto const index: std::views::iota(std::size_t { 0 }, results.size()))
                            results[index] = I32 { static_cast<uint32_t>(index) };
                        return {};
                    });
        return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
    };
    struct Shape
    {
        std::string name;
        std::vector<Bytes> bodies; ///< Functions 1, 2, …; function 1 is invoked.
    };
    auto const shapes = std::vector<Shape> {
        { .name = "a return_call-only function", .bodies = { op(0x12, 0) } },
        { .name = "after a tail chain", .bodies = { op(0x12, 2), op(0x12, 0) } },
        { .name = "a dead-code end", .bodies = { code({ { 0x02, 0x40 }, op(0x12, 0), { 0x0B, 0x00 } }) } },
    };
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    for (auto const& shape: shapes)
    {
        INFO(shape.name);
        auto module = ModuleBuilder {};
        module.type(many);
        module.importFunction("env", "many", 0);
        for (auto const& body: shape.bodies)
            module.function(0, {}, body);
        auto harness = Harness { module, fast(dispatch), imports };
        auto reference = Harness { module, Reference, imports };
        for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(shape.bodies.size())))
            CHECK(harness.code()->functions()[index].frameSlots >= Results);
        auto const outcome = harness.outcome(1);
        REQUIRE(outcome.has_value());
        CHECK(outcome->size() == Results);
        CHECK(outcome->back() == i32(static_cast<int32_t>(Results) - 1));
        CHECK(outcome == reference.outcome(1));
        CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
        CHECK(harness.store().chargedStackValues() == 0);
    }
}

TEST_CASE("Calls.nestedInvocationsHoldSmallChunks")
{
    // StoreLimits::maxNestedInvocations levels of host re-entry, one small frame each: every nested
    // invocation's first chunk is FirstChunkSlots, not a grown chunk, so the slot memory is the bound of
    // each.
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto memory = PeakResource {};
    auto guest = Recursive { Recursion::HostReentry,
                             EngineUnderTest { .dispatch = dispatch, .slotMemory = &memory },
                             {} };
    auto const levels = StoreLimits {}.maxNestedInvocations;
    CHECK(guest.run(static_cast<int32_t>(levels) - 1)
          == std::vector<Value> { i32(static_cast<int32_t>(levels) - 1) });
    CHECK(memory.allocations() == levels);
    CHECK(memory.held() == 0);
    CHECK(memory.peak() <= levels * chunkBytesBound(frameSlotsOf(guest.harness(), 0)));
    CHECK(memory.peak() == levels * FirstChunkSlots * sizeof(isoline::fastinterpreter::Slot));
}

TEST_CASE("Calls.nestingStackCostIsPublished")
{
    // The host stack one nesting of host re-entry takes, measured between the host frames of consecutive
    // nestings, is within the engine's published row; and the rows of every engine, for maxNestedInvocations
    // nestings, fit the tests' small thread.
    auto const engines = std::vector<std::pair<isoline::runtime::ExecutionEngine, EngineUnderTest>> {
        { isoline::runtime::ExecutionEngine::Reference, Reference },
        { isoline::runtime::ExecutionEngine::Fast, fast(GENERATE(from_range(availableDispatches()))) },
    };
    for (auto const& [identity, engine]: engines)
    {
        auto guest = Recursive { Recursion::HostReentry, engine, {} };
        REQUIRE(guest.run(20).has_value());
        auto const measured = guest.nestingStackBytes();
        INFO("flavour " << isoline::runtime::NestedInvocationFlavour << ", measured " << measured);
        // Printed for harvesting from CI logs: the rows are raised to the most any toolchain measures.
        std::println("nesting stack ({}, engine {}): {} of {}",
                     isoline::runtime::NestedInvocationFlavour,
                     std::to_underlying(identity),
                     measured,
                     isoline::runtime::nestedInvocationBytes(identity));
        CHECK(measured > 0);
        CHECK(measured <= isoline::runtime::nestedInvocationBytes(identity));
        CHECK(isoline::runtime::nestedInvocationBytes(identity) * StoreLimits {}.maxNestedInvocations
              <= isoline::runtime::testing::SmallStackBytes);
    }
}

TEST_CASE("Calls.measureNestingStack", "[.measure]")
{
    // What src/isoline/CMakeLists.txt's host-stack rows are measured with: prints the host stack one nesting
    // takes on each engine of this build.
    auto measure = [](EngineUnderTest engine) {
        auto guest = Recursive { Recursion::HostReentry, engine, {} };
        REQUIRE(guest.run(20).has_value());
        return guest.nestingStackBytes();
    };
    std::print(
        "nesting stack ({}): reference {}", isoline::runtime::NestedInvocationFlavour, measure(Reference));
    for (auto const dispatch: availableDispatches())
        std::print(", fast {}", measure(fast(dispatch)));
    std::println("");
}

TEST_CASE("Calls.embedderCallsAHostFunctionDirectly")
{
    // The embedder invokes an exported host function: one FunctionEntry poll, then the host frame with no
    // caller instance, and nothing left charged.
    auto const engine = fastEngineOfEachDispatch();
    auto module = ModuleBuilder {};
    module.type(unaryI32());
    module.importFunction("env", "host", 0);
    // f1 calls the host, f2 tail-calls it, and f3 calls f2, which goes on in f3 after the tail-called host
    // returns: their instance is its caller.
    module.function(0, {}, code({ localGet(0), op(0x10, 0) }));
    module.function(0, {}, code({ localGet(0), op(0x12, 0) }));
    module.function(0, {}, code({ localGet(0), op(0x10, 2), i32Const(10), op(0x6A) }));
    auto caller = std::optional<std::optional<uint32_t>> {};
    auto const imports = [&caller](Store& store) {
        auto const host = addHost(store,
                                  unaryI32(),
                                  [&caller](HostContext& context,
                                            std::span<Value const> args,
                                            std::span<Value> results) noexcept -> Outcome {
                                      caller = context.callerInstance();
                                      results[0] = I32 { std::get<I32>(args[0]).bits + 1 };
                                      return {};
                                  });
        return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
    };
    auto polls = PollCounter {};
    auto referencePolls = PollCounter {};
    auto harness = Harness { module, engine, imports, {}, &polls };
    CHECK(harness.result(0, { i32(41) }) == i32(42));
    REQUIRE(caller.has_value());
    CHECK(*caller == std::nullopt);
    auto reference = Harness { module, Reference, imports, {}, &referencePolls };
    CHECK(reference.result(0, { i32(41) }) == i32(42));
    for (auto const site: { PollSite::FunctionEntry, PollSite::HostReturn })
        CHECK(polls.count(site) == referencePolls.count(site));
    CHECK(polls.count(PollSite::FunctionEntry) == 1);
    CHECK(harness.store().used(Limit::Steps) == reference.store().used(Limit::Steps));
    CHECK(harness.store().chargedStackValues() == 0);
    auto const instance = harness.store().function(harness.function(1)).moduleInstance;
    for (auto const& [function, result]: { std::pair { 1U, 2 }, std::pair { 2U, 2 }, std::pair { 3U, 12 } })
    {
        // Read before the reference runs the same host function, which records its own caller.
        caller.reset();
        CHECK(harness.result(function, { i32(1) }) == i32(result));
        REQUIRE(caller.has_value());
        CHECK(*caller == std::optional { instance });
        CHECK(reference.result(function, { i32(1) }) == i32(result));
        CHECK(harness.store().chargedStackValues() == 0);
    }
}
