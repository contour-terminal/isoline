// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/FloatEnvironment.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/FloatEnvironment.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>
#include <isoline/validate/ValidatedModule.hpp>
#include <isoline/validate/Validator.hpp>

using isoline::FeatureSet;
using isoline::binary::AddressType;
using isoline::binary::ExternKind;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::binary::ValueType;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::call;
using isoline::binary::testing::code;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::i64Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::memoryAccess;
using isoline::binary::testing::memoryFill;
using isoline::binary::testing::ModuleBuilder;
using isoline::interpreter::ReferenceInterpreter;
using isoline::runtime::Abrupt;
using isoline::runtime::ExnAddr;
using isoline::runtime::ExternValue;
using isoline::runtime::F32;
using isoline::runtime::FuncAddr;
using isoline::runtime::governedBits;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::HostContext;
using isoline::runtime::HostFailure;
using isoline::runtime::HostFailureKind;
using isoline::runtime::hostFloatControlLayout;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::IPollObserver;
using isoline::runtime::Limit;
using isoline::runtime::makeHostFunction;
using isoline::runtime::MismatchKind;
using isoline::runtime::PollSite;
using isoline::runtime::Ref;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::Thrown;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::ValueMismatch;
using isoline::runtime::testing::departFromIeeeDefaults;
using isoline::runtime::testing::HostFloatEnvironment;
using isoline::runtime::testing::InterruptWatchdog;
using isoline::runtime::testing::PollCounter;
using isoline::runtime::testing::PollTrigger;
using isoline::runtime::testing::TriggerWait;
using isoline::validate::FrameShape;

namespace
{

/// The default page size, 64 KiB.
constexpr auto PageSize = isoline::binary::pageSize(MemoryType {});

constexpr auto TypeI32 = isoline::binary::I32;
constexpr auto TypeI64 = isoline::binary::I64;
constexpr auto TypeF32 = isoline::binary::F32;

/// An active element segment for table 0 at @p offset, listing @p functions.
Bytes activeElements(uint8_t offset, Bytes const& functions)
{
    return code({ { 0x00 }, i32Const(offset), { 0x0B, static_cast<uint8_t>(functions.size()) }, functions });
}

Limits atLeast(uint64_t min)
{
    return Limits { .min = min, .max = std::nullopt, .addressType = AddressType::I32 };
}

Value i32(int32_t value)
{
    return I32 { static_cast<uint32_t>(value) };
}

/// One module instantiated on a fresh store, executed by the reference interpreter.
class Harness
{
  public:
    /// Decodes, validates and instantiates @p builder's module.
    /// @param builder The module.
    /// @param imports What its imports bind to.
    /// @param maxCallDepth The store's call depth limit.
    explicit Harness(ModuleBuilder const& builder,
                     std::vector<ExternValue> const& imports = {},
                     uint32_t maxCallDepth = 10'000):
        Harness { builder, imports, StoreLimits { .maxCallDepth = maxCallDepth } }
    {
    }

    /// Decodes, validates and instantiates @p builder's module in a store of @p limits.
    /// @param observer Observes the store's polls; none when null.
    Harness(ModuleBuilder const& builder,
            std::vector<ExternValue> const& imports,
            StoreLimits limits,
            IPollObserver* observer = nullptr):
        _store { limits,
                 isoline::runtime::StoreServices { .allocator = _allocator, .pollObserver = observer } }
    {
        instantiate(builder, imports);
    }

    /// Creates the harness, in a store of @p limits, with host functions that @p bind adds to the
    /// store before instantiation.
    /// @param observer Observes the store's polls; none when null.
    template <typename Bind>
    Harness(ModuleBuilder const& builder,
            Bind&& bind,
            StoreLimits limits = {},
            IPollObserver* observer = nullptr):
        _store { limits,
                 isoline::runtime::StoreServices { .allocator = _allocator, .pollObserver = observer } }
    {
        instantiate(builder, std::forward<Bind>(bind)(_store));
    }

    /// Invokes the module's function @p index.
    /// @param index The function's index in the module's function index space.
    /// @param args Its arguments.
    /// @return Its results, or how it ended abruptly.
    std::expected<std::vector<Value>, Abrupt> outcome(uint32_t index, std::vector<Value> const& args = {})
    {
        auto const address = function(index);
        auto results = std::vector<Value>(_store.functionType(address).results.size());
        return isoline::runtime::invokeChecked(_store, _interpreter, address, args, results)
            .outcome.transform([&] { return results; });
    }

    /// How invoking the module's function @p index ends abruptly, which it must.
    /// @param index The function's index in the module's function index space.
    /// @param args Its arguments.
    /// @return How it ended.
    Abrupt abrupt(uint32_t index, std::vector<Value> const& args = {})
    {
        auto const ended = outcome(index, args);
        REQUIRE_FALSE(ended.has_value());
        return ended.error();
    }

    /// Invokes the module's function @p index, which must not throw.
    /// @param index The function's index in the module's function index space.
    /// @param args Its arguments.
    /// @return Its results, or the trap.
    std::expected<std::vector<Value>, Trap> invoke(uint32_t index, std::vector<Value> const& args = {})
    {
        return outcome(index, args).transform_error([](Abrupt const& abrupt) {
            REQUIRE(std::holds_alternative<Trap>(abrupt));
            return std::get<Trap>(abrupt);
        });
    }

    /// The single result of invoking function @p index, which must not trap.
    Value result(uint32_t index, std::vector<Value> const& args = {})
    {
        auto const results = invoke(index, args);
        REQUIRE(results.has_value());
        REQUIRE(results->size() == 1);
        return results->front();
    }

    /// The trap invoking function @p index ends with, which it must.
    TrapCode trap(uint32_t index, std::vector<Value> const& args = {})
    {
        auto const results = invoke(index, args);
        REQUIRE_FALSE(results.has_value());
        return results.error().code;
    }

    [[nodiscard]] FuncAddr function(uint32_t index)
    {
        return _store.moduleInstance(_address).functions.at(index);
    }

    [[nodiscard]] Store& store() noexcept { return _store; }

    [[nodiscard]] uint32_t address() const noexcept { return _address; }

    [[nodiscard]] ReferenceInterpreter& interpreter() noexcept { return _interpreter; }

    /// The validated module the harness instantiated.
    [[nodiscard]] isoline::validate::ValidatedModule const& validated() const noexcept { return *_validated; }

    /// The value of the module's global @p index.
    /// @param index The global's index in the module's global index space.
    [[nodiscard]] Value global(uint32_t index)
    {
        return _store.global(_store.moduleInstance(_address).globals.at(index)).value;
    }

  private:
    void instantiate(ModuleBuilder const& builder, std::vector<ExternValue> const& imports)
    {
        // Every feature Isoline knows, so that a test may use any instruction it executes.
        auto const features = FeatureSet::all();
        auto validated = isoline::validate::ValidatedModule::load(
            builder.build(), features, isoline::binary::DecodeLimits {});
        REQUIRE(validated.has_value());
        _validated = *validated;
        auto const address =
            isoline::runtime::instantiate(_store, _interpreter, std::move(*validated), imports);
        REQUIRE(address.has_value());
        _address = *address;
    }

    HeapMemoryAllocator _allocator;
    Store _store;
    ReferenceInterpreter _interpreter;
    std::shared_ptr<isoline::validate::ValidatedModule const> _validated;
    uint32_t _address = 0;
};

FuncType signature(std::vector<ValueType> params, std::vector<ValueType> results)
{
    return FuncType { .params = std::move(params), .results = std::move(results) };
}

/// Adds a host function of @p type, implemented by @p function, to @p store.
/// @return Its address.
template <isoline::runtime::HostCallable F>
FuncAddr addHost(Store& store, FuncType type, F function)
{
    auto const address = store.addHostFunction(std::move(type), makeHostFunction(std::move(function)));
    REQUIRE(address.has_value());
    return *address;
}

} // namespace

TEST_CASE("ReferenceInterpreter.factorial")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI64 }, { TypeI64 }))
        .function(0,
                  {},
                  code({ localGet(0),
                         { 0x50, 0x04, 0x7E }, // i64.eqz; if (result i64)
                         i64Const(1),
                         { 0x05 }, // else
                         localGet(0),
                         localGet(0),
                         i64Const(1),
                         { 0x7D }, // i64.sub
                         call(0),
                         { 0x7E, 0x0B } })); // i64.mul; end
    auto harness = Harness { builder };
    CHECK(harness.result(0, { I64 { 20 } }) == Value { I64 { 2432902008176640000 } });
    CHECK(harness.result(0, { I64 { 0 } }) == Value { I64 { 1 } });
}

TEST_CASE("ReferenceInterpreter.wideArithmetic")
{
    // Each function applies one wide instruction to its parameters and returns both halves.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI64, TypeI64, TypeI64, TypeI64 }, { TypeI64, TypeI64 }))
        .type(signature({ TypeI64, TypeI64 }, { TypeI64, TypeI64 }))
        .function(0, {}, code({ localGet(0), localGet(1), localGet(2), localGet(3), { 0xFC, 0x13 } }))
        .function(0, {}, code({ localGet(0), localGet(1), localGet(2), localGet(3), { 0xFC, 0x14 } }))
        .function(1, {}, code({ localGet(0), localGet(1), { 0xFC, 0x15 } }))
        .function(1, {}, code({ localGet(0), localGet(1), { 0xFC, 0x16 } }));
    auto harness = Harness { builder };
    auto const i64 = [](uint64_t bits) {
        return Value { I64 { bits } };
    };
    auto const both = [&](uint64_t low, uint64_t high) {
        return std::vector<Value> { i64(low), i64(high) };
    };
    // add128: (2^64 - 1) + 1 carries into the high half.
    CHECK(harness.invoke(0, { i64(~0ULL), i64(0), i64(1), i64(0) }) == both(0, 1));
    // sub128: 0 - 1 borrows through both halves.
    CHECK(harness.invoke(1, { i64(0), i64(0), i64(1), i64(0) }) == both(~0ULL, ~0ULL));
    // mul_wide_s: -1 * 2 = -2, sign-extended into the high half.
    CHECK(harness.invoke(2, { i64(~0ULL), i64(2) }) == both(~1ULL, ~0ULL));
    // mul_wide_u: (2^64 - 1) * 2.
    CHECK(harness.invoke(3, { i64(~0ULL), i64(2) }) == both(~1ULL, 1));
}

TEST_CASE("ReferenceInterpreter.brTable")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, { TypeI32 }))
        .function(0,
                  {},
                  code({ { 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40 },
                         localGet(0),
                         { 0x0E, 0x04, 0x00, 0x01, 0x02, 0x03, 0x04 },
                         { 0x0B },
                         i32Const(10),
                         { 0x0F, 0x0B },
                         i32Const(11),
                         { 0x0F, 0x0B },
                         i32Const(12),
                         { 0x0F, 0x0B },
                         i32Const(13),
                         { 0x0F, 0x0B },
                         i32Const(14) }));
    auto harness = Harness { builder };
    for (auto const selector: { 0, 1, 2, 3, 4 })
        CHECK(harness.result(0, { i32(selector) }) == i32(10 + selector));
    CHECK(harness.result(0, { i32(99) }) == i32(14));
    CHECK(harness.result(0, { i32(-1) }) == i32(14));
}

TEST_CASE("ReferenceInterpreter.loopAndBranchValues")
{
    // sum = 0; do { sum += n; n -= 1 } while (n != 0); return sum
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, { TypeI32 }))
        .function(0,
                  { TypeI32 },
                  code({ { 0x03, 0x40 }, // loop
                         localGet(1),
                         localGet(0),
                         { 0x6A, 0x21, 0x01 }, // i32.add; local.set 1
                         localGet(0),
                         i32Const(1),
                         { 0x6B, 0x22, 0x00 }, // i32.sub; local.tee 0
                         { 0x0D, 0x00, 0x0B }, // br_if 0; end
                         localGet(1) }))
        // 100 + block (result i32) { 1; 5; br_if 0 (carries 5, drops 1); drop; 6; br 0 } end
        .function(0,
                  {},
                  code({ i32Const(100),
                         { 0x02, 0x7F },
                         i32Const(1),
                         i32Const(5),
                         localGet(0),
                         { 0x0D, 0x00, 0x1A }, // br_if 0; drop
                         i32Const(6),
                         { 0x0C, 0x00, 0x0B, 0x6A } })); // br 0; end; i32.add
    auto harness = Harness { builder };
    CHECK(harness.result(0, { i32(10) }) == i32(55));
    CHECK(harness.result(1, { i32(1) }) == i32(105));
    CHECK(harness.result(1, { i32(0) }) == i32(106));
}

TEST_CASE("ReferenceInterpreter.memoryLoadStore")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .memory(atLeast(1))
        .function(0,
                  {},
                  code({ i32Const(8),
                         i32Const(0x01020304),
                         memoryAccess(0x36, 2, 0), // i32.store
                         i32Const(8),
                         memoryAccess(0x2D, 0, 0) }))                       // i32.load8_u
        .function(0, {}, code({ i32Const(4), memoryAccess(0x28, 2, 4) }))   // i32.load offset=4 at 4
        .function(0, {}, code({ i32Const(-1), memoryAccess(0x2C, 0, 0) })); // i32.load8_s at 2^32 - 1
    auto harness = Harness { builder };
    CHECK(harness.result(0) == i32(4));
    CHECK(harness.result(1) == i32(0x01020304));
    CHECK(harness.trap(2) == TrapCode::OutOfBoundsMemoryAccess);
}

TEST_CASE("ReferenceInterpreter.outOfBounds")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, { TypeI32 }))
        .memory(atLeast(1))
        .function(0, {}, code({ localGet(0), memoryAccess(0x28, 2, 0) }))
        .function(0, {}, code({ localGet(0), memoryAccess(0x28, 2, 0xFFFF'FFFF) }));
    auto harness = Harness { builder };
    CHECK(harness.trap(0, { i32(65533) }) == TrapCode::OutOfBoundsMemoryAccess);
    CHECK(harness.result(0, { i32(65532) }) == i32(0));
    CHECK(harness.trap(1, { i32(1) }) == TrapCode::OutOfBoundsMemoryAccess);
}

TEST_CASE("ReferenceInterpreter.callIndirectMismatch")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .type(signature({ TypeI32 }, { TypeI32 }))
        .table(TableType { .element = FuncRef, .limits = atLeast(3) })
        .function(0, {}, i32Const(7))                                 // 0: () -> i32
        .function(1, {}, localGet(0))                                 // 1: (i32) -> i32
        .function(1, {}, code({ localGet(0), { 0x11, 0x00, 0x00 } })) // 2: call_indirect type 0
        .element(activeElements(0, { 0x00, 0x01 }));
    auto harness = Harness { builder };
    CHECK(harness.result(2, { i32(0) }) == i32(7));
    CHECK(harness.trap(2, { i32(1) }) == TrapCode::IndirectCallTypeMismatch);
    CHECK(harness.trap(2, { i32(2) }) == TrapCode::UninitializedElement);
    CHECK(harness.trap(2, { i32(3) }) == TrapCode::UndefinedElement);
}

TEST_CASE("ReferenceInterpreter.callIndirectTrapNamesTheElement")
{
    // The reference interpreter appends the element index, and the spec suite asserts it (bulk.wast).
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, {}))
        .table(TableType { .element = FuncRef, .limits = atLeast(3) })
        .function(0, {}, code({ localGet(0), { 0x11, 0x01, 0x00 } })); // call_indirect type 1
    auto harness = Harness { builder };
    auto const messageOf = [&](int32_t element) {
        auto const result = harness.invoke(0, { i32(element) });
        REQUIRE_FALSE(result.has_value());
        return isoline::runtime::specMessage(result.error());
    };
    CHECK(messageOf(2) == "uninitialized element 2");
    CHECK(messageOf(7) == "undefined element 7");
    // Index 0xFFFFFFFF is an index like any other; the 3.0 interpreter widens it to an unsigned
    // address and spells that as Int64.to_string does.
    CHECK(messageOf(-1) == "undefined element 4294967295");
    CHECK(isoline::runtime::specMessage(Trap { TrapCode::Unreachable }) == "unreachable");
}

TEST_CASE("ReferenceInterpreter.infiniteRecursionTraps")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({}, { TypeI32 }))
        .function(0, {}, call(0))
        .function(1, {}, i32Const(3));
    auto harness = Harness { builder };
    CHECK(harness.trap(0) == TrapCode::CallStackExhausted);
    // The trap unwound every frame: the interpreter runs on as fresh.
    CHECK(harness.result(1) == i32(3));
    CHECK(harness.trap(0) == TrapCode::CallStackExhausted);

    SECTION("exactly the store's depth")
    {
        // f(n) = n == 0 ? 0 : f(n - 1): depth n + 1.
        auto counting = ModuleBuilder {};
        counting.type(signature({ TypeI32 }, { TypeI32 }))
            .function(0,
                      {},
                      code({ localGet(0),
                             { 0x45, 0x04, 0x7F }, // i32.eqz; if (result i32)
                             i32Const(0),
                             { 0x05 },
                             localGet(0),
                             i32Const(1),
                             { 0x6B },
                             call(0),
                             { 0x0B } }));
        auto limited = Harness { counting, {}, 100 };
        CHECK(limited.result(0, { i32(99) }) == i32(0));
        CHECK(limited.trap(0, { i32(100) }) == TrapCode::CallStackExhausted);
    }

    SECTION("a store of depth 0 traps on entry")
    {
        auto zero = Harness { builder, {}, 0 };
        CHECK(zero.trap(1) == TrapCode::CallStackExhausted);
    }
}

TEST_CASE("ReferenceInterpreter.interpretDeepNesting")
{
    constexpr auto Depth = 100'000U;
    auto body = Bytes {};
    for ([[maybe_unused]] auto const _: std::views::iota(0U, Depth))
        body.insert(body.end(), { 0x02, 0x40 });
    body.push_back(0x0C);
    auto const label = encodeU32(Depth - 1);
    body.insert(body.end(), label.begin(), label.end());
    body.insert(body.end(), Depth, 0x0B);
    auto const tail = i32Const(1);
    body.insert(body.end(), tail.begin(), tail.end());

    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 })).function(0, {}, std::move(body));
    auto harness = Harness { builder };
    // Its 100'000 labels are within its frame's static cost, which the default stack budget allows.
    CHECK(harness.validated().frameShape(0) == FrameShape { .maxOperands = 1, .maxLabels = Depth });
    CHECK(harness.result(0) == i32(1));
}

TEST_CASE("ReferenceInterpreter.stackBudgetBoundsRecursion")
{
    // A store whose depth limit never stops the recursion: only the stack budget may.
    constexpr auto Unbounded = std::numeric_limits<uint32_t>::max();

    SECTION("labels: a self-call inside many nested blocks")
    {
        // Every frame holds Depth labels; without them in the budget, the label stack alone grows
        // until the host runs out of memory.
        constexpr auto Depth = 1'000U;
        auto body = Bytes {};
        for ([[maybe_unused]] auto const _: std::views::iota(0U, Depth))
            body.insert(body.end(), { 0x02, 0x40 });
        auto const self = call(0);
        body.insert(body.end(), self.begin(), self.end());
        body.insert(body.end(), Depth, 0x0B);

        auto builder = ModuleBuilder {};
        builder.type(signature({}, {})).function(0, {}, std::move(body));
        auto harness = Harness { builder, {}, Unbounded };
        CHECK(harness.trap(0) == TrapCode::CallStackExhausted);
    }

    SECTION("locals: a self-call of a function with many locals")
    {
        auto builder = ModuleBuilder {};
        builder.type(signature({}, {})).function(0, std::vector<ValueType>(50'000, TypeI64), call(0));
        auto harness = Harness { builder, {}, Unbounded };
        CHECK(harness.trap(0) == TrapCode::CallStackExhausted);
    }
}

TEST_CASE("ReferenceInterpreter.signallingNanSurvivesLoadStore")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeF32 }))
        .memory(atLeast(1))
        .function(0,
                  { TypeF32 },
                  code({ i32Const(0),
                         { 0x43, 0x01, 0x00, 0x80, 0x7F }, // f32.const bits 0x7F800001
                         memoryAccess(0x38, 2, 0),         // f32.store
                         i32Const(0),
                         memoryAccess(0x2A, 2, 0), // f32.load
                         { 0x21, 0x00 },           // local.set 0
                         localGet(0),
                         localGet(0),
                         i32Const(1),
                         { 0x1B } })); // select
    auto harness = Harness { builder };
    CHECK(harness.result(0) == Value { F32 { 0x7F80'0001 } });
}

TEST_CASE("ReferenceInterpreter.hostCall")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, { TypeI32 }))
        .importFunction("env", "double", 0)
        .function(0, {}, code({ localGet(0), call(0) }));
    auto calls = 0;
    auto harness =
        Harness { builder, [&](Store& store) {
                     auto const host =
                         addHost(store,
                                 signature({ TypeI32 }, { TypeI32 }),
                                 [&](HostContext&,
                                     std::span<Value const> args,
                                     std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
                                     ++calls;
                                     results[0] = I32 { std::get<I32>(args[0]).bits * 2 };
                                     return {};
                                 });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    CHECK(harness.result(1, { i32(21) }) == i32(42));
    CHECK(calls == 1);
}

TEST_CASE("ReferenceInterpreter.hostTrapPropagates")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).importFunction("env", "fail", 0).function(0, {}, call(0));
    auto harness =
        Harness { builder, [](Store& store) {
                     auto const host = addHost(store,
                                               signature({}, {}),
                                               [](HostContext&,
                                                  std::span<Value const>,
                                                  std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                                   return std::unexpected { Trap { TrapCode::HostTrap } };
                                               });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    CHECK(harness.trap(1) == TrapCode::HostTrap);
}

TEST_CASE("ReferenceInterpreter.multiValue")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .type(signature({}, { TypeI32, TypeI32 }))
        .function(0,
                  {},
                  code({ { 0x02, 0x01 }, // block (type 1)
                         i32Const(1),
                         i32Const(2),
                         { 0x0B, 0x6B } })) // end; i32.sub
        .function(1, {}, code({ i32Const(3), i32Const(4) }))
        .function(0, {}, code({ call(1), { 0x6B } }));
    auto harness = Harness { builder };
    CHECK(harness.result(0) == i32(-1));
    auto const results = harness.invoke(1);
    REQUIRE(results.has_value());
    CHECK(*results == std::vector { i32(3), i32(4) });
    CHECK(harness.result(2) == i32(-1));
}

TEST_CASE("ReferenceInterpreter.bulkMemory")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .memory(atLeast(1))
        .function(0,
                  {},
                  code({ i32Const(16),
                         i32Const(0xAB),
                         i32Const(4),
                         { 0xFC, 0x0B, 0x00 }, // memory.fill
                         i32Const(18),
                         i32Const(16),
                         i32Const(4),
                         { 0xFC, 0x0A, 0x00, 0x00 }, // memory.copy, overlapping
                         i32Const(18),
                         memoryAccess(0x28, 2, 0) }))
        .function(
            0, {}, code({ i32Const(65530), i32Const(0), i32Const(7), { 0xFC, 0x0B, 0x00 }, i32Const(0) }))
        .function(0,
                  {},
                  code({ i32Const(2),
                         { 0x40, 0x00, 0x1A }, // memory.grow; drop
                         { 0x3F, 0x00 } }));   // memory.size
    auto harness = Harness { builder };
    CHECK(harness.result(0) == Value { I32 { 0xABAB'ABAB } });
    CHECK(harness.trap(1) == TrapCode::OutOfBoundsMemoryAccess);
    CHECK(harness.result(2) == i32(3));
    CHECK(harness.store().memory(harness.store().moduleInstance(harness.address()).memories[0]).bytes().size()
          == 3 * PageSize);
}

TEST_CASE("Interpreter.byteGranularMemory")
{
    // A memory of 1-byte pages, three of them: sizes and growth count bytes.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .memory(MemoryType { .limits = Limits { .min = 3, .max = std::nullopt }, .pageSizeLog2 = 0 })
        .function(0, {}, { 0x3F, 0x00 })                                   // memory.size
        .function(0, {}, code({ i32Const(0), memoryAccess(0x28, 2, 0) }))  // i32.load at 0
        .function(0, {}, code({ i32Const(1), { 0x40, 0x00 } }))            // memory.grow 1
        .function(0, {}, code({ i32Const(3), memoryAccess(0x2D, 0, 0) })); // i32.load8_u at 3
    auto harness = Harness { builder };
    CHECK(harness.result(0) == i32(3));
    CHECK(harness.trap(1) == TrapCode::OutOfBoundsMemoryAccess);
    CHECK(harness.trap(3) == TrapCode::OutOfBoundsMemoryAccess);
    CHECK(harness.result(2) == i32(3));
    CHECK(harness.result(0) == i32(4));
    CHECK(harness.result(1) == i32(0));
    CHECK(harness.result(3) == i32(0));
    CHECK(harness.store().memory(harness.store().moduleInstance(harness.address()).memories[0]).bytes().size()
          == 4);
}

TEST_CASE("ReferenceInterpreter.tables")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .function(0,
                  {},
                  code({ { 0xD0, 0x70 }, // ref.null func
                         i32Const(2),
                         { 0xFC, 0x0F, 0x00 },    // table.grow 0
                         { 0x1A },                // drop
                         { 0xFC, 0x10, 0x00 } })) // table.size 0
        .function(0,
                  {},
                  code({ i32Const(0),
                         { 0xD2, 0x00 }, // ref.func 0
                         { 0x26, 0x00 }, // table.set 0
                         i32Const(0),
                         { 0x25, 0x00, 0xD1 } })) // table.get 0; ref.is_null
        .function(0, {}, code({ i32Const(5), { 0x25, 0x00, 0xD1 } }))
        .exportFunction("grow", 0); // declares function 0 for ref.func
    auto harness = Harness { builder };
    CHECK(harness.result(0) == i32(3));
    CHECK(harness.result(1) == i32(0));
    CHECK(harness.trap(2) == TrapCode::OutOfBoundsTableAccess);
}

TEST_CASE("ReferenceInterpreter.instantiateRunsGlobalsAndStart")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({}, { TypeI32 }))
        .memory(atLeast(1))
        .global(GlobalType { .type = TypeI32, .mutability = Mutability::Var }, i32Const(40))
        .function(0, {}, code({ { 0x23, 0x00 }, i32Const(2), { 0x6A, 0x24, 0x00 } })) // global += 2
        .function(1, {}, code({ { 0x23, 0x00 } }))
        .function(1, {}, code({ i32Const(3), memoryAccess(0x2D, 0, 0) }))
        .data(code({ { 0x00 }, i32Const(3), { 0x0B, 0x01, 0x5A } }))
        .start(0);
    auto harness = Harness { builder };
    CHECK(harness.result(1) == i32(42));
    CHECK(harness.result(2) == i32(0x5A));
}

TEST_CASE("ReferenceInterpreter.unreachableTraps")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).function(0, {}, { 0x00 });
    auto harness = Harness { builder };
    CHECK(harness.trap(0) == TrapCode::Unreachable);
}

TEST_CASE("ReferenceInterpreter.ifWithoutElse")
{
    // local 1 = 5; if (local 0) { local 1 = 7 }; return local 1
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, { TypeI32 }))
        .function(0,
                  { TypeI32 },
                  code({ i32Const(5),
                         { 0x21, 0x01 },
                         localGet(0),
                         { 0x04, 0x40 },
                         i32Const(7),
                         { 0x21, 0x01, 0x0B },
                         localGet(1) }));
    auto harness = Harness { builder };
    CHECK(harness.result(0, { i32(1) }) == i32(7));
    CHECK(harness.result(0, { i32(0) }) == i32(5));
}

TEST_CASE("ReferenceInterpreter.hostReentersWasm")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .importFunction("env", "reenter", 0)
        .function(0, {}, code({ call(0), i32Const(1), { 0x6A } })) // reenter() + 1
        .function(0, {}, i32Const(9));
    Harness* self = nullptr;
    auto harness =
        Harness { builder, [&](Store& store) {
                     auto const host = addHost(
                         store,
                         signature({}, { TypeI32 }),
                         [&](HostContext& context,
                             std::span<Value const>,
                             std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
                             auto inner = std::vector<Value>(1);
                             if (auto const done = context.invoke(self->function(2), {}, inner); !done)
                                 return done;
                             results[0] = inner.front();
                             return {};
                         });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    self = &harness;
    CHECK(harness.result(1) == i32(10));
}

TEST_CASE("ReferenceInterpreter.passiveSegments")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .table(TableType { .element = FuncRef, .limits = atLeast(4) })
        .memory(atLeast(1))
        .function(0, {}, i32Const(7))
        .function(0, {}, code({ i32Const(1),
                                i32Const(0),
                                i32Const(2),
                                { 0xFC, 0x0C, 0x00, 0x00 }, // table.init: [null, f0, f0, null]
                                { 0xFC, 0x0D, 0x00 },       // elem.drop
                                i32Const(2),
                                i32Const(1),
                                i32Const(2),
                                { 0xFC, 0x0E, 0x00, 0x00 }, // table.copy 2 <- 1, 2 slots: [null, f0, f0, f0]
                                i32Const(3),
                                { 0xD0, 0x70 },
                                i32Const(1),
                                { 0xFC, 0x11, 0x00 }, // table.fill slot 3 with null: [null, f0, f0, null]
                                i32Const(10),
                                i32Const(1),
                                i32Const(2),
                                { 0xFC, 0x08, 0x00, 0x00 }, // memory.init: "BC" at 10
                                { 0xFC, 0x09, 0x00 },       // data.drop
                                i32Const(10),
                                memoryAccess(0x2F, 1, 0), // i32.load16_u
                                i32Const(2),
                                { 0x11, 0x00, 0x00, 0x6A } })) // call_indirect; i32.add
        .function(
            0, {}, code({ i32Const(0), i32Const(0), i32Const(1), { 0xFC, 0x0C, 0x00, 0x00 }, i32Const(0) }))
        .function(
            0, {}, code({ i32Const(0), i32Const(0), i32Const(1), { 0xFC, 0x08, 0x00, 0x00 }, i32Const(0) }))
        .function(0, {}, code({ i32Const(3), { 0x11, 0x00, 0x00 } }))
        .element({ 0x01, 0x00, 0x02, 0x00, 0x00 })
        .dataCount(1)
        .data({ 0x01, 0x03, 'A', 'B', 'C' });
    auto harness = Harness { builder };
    CHECK(harness.result(1) == i32(0x4342 + 7));
    CHECK(harness.trap(2) == TrapCode::OutOfBoundsTableAccess);
    CHECK(harness.trap(3) == TrapCode::OutOfBoundsMemoryAccess);
    CHECK(harness.trap(4) == TrapCode::UninitializedElement);
}

TEST_CASE("ReferenceInterpreter.hostReentryIsBounded")
{
    // Function 1 calls the host, whose callback invokes function 1 again, forever.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).importFunction("env", "reenter", 0).function(0, {}, call(0));
    Harness* self = nullptr;
    auto reentries = uint32_t { 0 };
    auto harness =
        Harness { builder, [&](Store& store) {
                     auto const host = addHost(store,
                                               signature({}, {}),
                                               [&](HostContext& context,
                                                   std::span<Value const>,
                                                   std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                                   ++reentries;
                                                   return context.invoke(self->function(1), {}, {});
                                               });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    self = &harness;
    CHECK(harness.trap(1) == TrapCode::CallStackExhausted);
    CHECK(reentries == StoreLimits {}.maxNestedInvocations);
    // Every host call scope ended with its abrupt exit.
    CHECK(harness.store().hostCallsInProgress() == 0);
    // Every invocation unwound: the interpreter runs on as fresh, and hits the same bound again.
    reentries = 0;
    CHECK(harness.trap(1) == TrapCode::CallStackExhausted);
    CHECK(reentries == StoreLimits {}.maxNestedInvocations);
}

TEST_CASE("ReferenceInterpreter.nestedInvocationsFollowTheLimit")
{
    // As hostReentryIsBounded, in a store that allows 7 invocations at once, which it counts.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).importFunction("env", "reenter", 0).function(0, {}, call(0));
    Harness* self = nullptr;
    auto reentries = uint32_t { 0 };
    auto deepest = uint64_t { 0 };
    auto refused = isoline::runtime::Entry::Refused;
    auto harness =
        Harness { builder,
                  [&](Store& store) {
                      auto const host =
                          addHost(store,
                                  signature({}, {}),
                                  [&](HostContext& context,
                                      std::span<Value const>,
                                      std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                      ++reentries;
                                      deepest =
                                          std::max(deepest, context.store().used(Limit::NestedInvocations));
                                      auto const nested = isoline::runtime::invokeChecked(
                                          self->store(), self->interpreter(), self->function(1), {}, {});
                                      if (!nested.outcome && deepest == 7)
                                          refused = nested.entry;
                                      return nested.outcome;
                                  });
                      return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                  },
                  StoreLimits { .maxNestedInvocations = 7 } };
    self = &harness;
    CHECK(harness.trap(1) == TrapCode::CallStackExhausted);
    CHECK(reentries == 7);
    // The store counts the invocations in progress, and they all ended.
    CHECK(harness.store().used(Limit::NestedInvocations) == 0);

    CHECK(deepest == 7);
    // The call past the limit entered, and trapped, as a call too deep does.
    CHECK(refused == isoline::runtime::Entry::Entered);
}

TEST_CASE("ReferenceInterpreter.hostTakesI64AndI32")
{
    // import (func (param i64 i32) (result i64)), which returns a + b; function 1 passes its own params.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI64, TypeI32 }, { TypeI64 }))
        .importFunction("env", "add", 0)
        .function(0, {}, code({ localGet(0), localGet(1), call(0) }));
    auto harness =
        Harness { builder, [](Store& store) {
                     auto const host =
                         addHost(store,
                                 signature({ TypeI64, TypeI32 }, { TypeI64 }),
                                 [](HostContext&,
                                    std::span<Value const> args,
                                    std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
                                     results[0] =
                                         I64 { std::get<I64>(args[0]).bits + std::get<I32>(args[1]).bits };
                                     return {};
                                 });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    CHECK(harness.result(1, { I64 { 0x1'0000'0000 }, I32 { 2 } }) == Value { I64 { 0x1'0000'0002 } });
}

namespace
{

/// A module whose function 1 calls its import 0, of type [] -> [i32], implemented by @p function.
template <isoline::runtime::HostCallable F>
Harness callingHost(F function)
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 })).importFunction("env", "host", 0).function(0, {}, call(0));
    return Harness { builder, [&](Store& store) {
                        auto const host = addHost(store, signature({}, { TypeI32 }), std::move(function));
                        return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                    } };
}

} // namespace

TEST_CASE("ReferenceInterpreter.hostResultOfWrongKindIsAHostFailure")
{
    // The host writes an f32 into its i32 result: the call ends with a host failure, not an abort.
    auto harness = callingHost([](HostContext&,
                                  std::span<Value const>,
                                  std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
        results[0] = F32 {};
        return {};
    });
    CHECK(harness.abrupt(1)
          == Abrupt {
              HostFailure { HostFailureKind::ResultMismatch, 0, ValueMismatch { MismatchKind::Kind, 0 } } });
}

TEST_CASE("ReferenceInterpreter.hostFailurePassesThrough")
{
    // A host's own failure ends the invocation exactly as the host reported it.
    auto const failure =
        HostFailure { .kind = HostFailureKind::Embedder, .code = 7, .mismatch = std::nullopt };
    auto harness = callingHost(
        [&](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> std::expected<void, Abrupt> {
            return std::unexpected { Abrupt { failure } };
        });
    CHECK(harness.abrupt(1) == Abrupt { failure });
}

TEST_CASE("ReferenceInterpreter.forgedThrownIsAHostFailure")
{
    // A host throwing an exception the store never allocated is a host failure: no handler sees it.
    auto harness = callingHost(
        [](HostContext&, std::span<Value const>, std::span<Value>) noexcept -> std::expected<void, Abrupt> {
            return std::unexpected { Abrupt { Thrown { .exception = 999 } } };
        });
    CHECK(harness.abrupt(1)
          == Abrupt {
              HostFailure { .kind = HostFailureKind::ResultMismatch, .code = 0, .mismatch = std::nullopt } });
}

TEST_CASE("ReferenceInterpreter.hostReentersThroughItsContext")
{
    // The host finds the caller's export "inc" and invokes it through its context: inc(41) = 42.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .type(signature({ TypeI32 }, { TypeI32 }))
        .importFunction("env", "host", 0)
        .function(0, {}, call(0))
        .function(1, {}, code({ localGet(0), i32Const(1), { 0x6A } })) // i32.add
        .exportFunction("inc", 2);
    auto caller = std::optional<uint32_t> {};
    auto harness =
        Harness { builder, [&](Store& store) {
                     auto const host =
                         addHost(store,
                                 signature({}, { TypeI32 }),
                                 [&](HostContext& context,
                                     std::span<Value const>,
                                     std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
                                     caller = context.callerInstance();
                                     auto const& exports =
                                         context.store().moduleInstance(caller.value_or(0)).exports;
                                     auto const inc =
                                         std::ranges::find(exports,
                                                           std::string_view { "inc" },
                                                           [](auto const& entry) { return entry.first; });
                                     auto const args = std::vector<Value> { I32 { 41 } };
                                     return context.invoke(inc->second.address, args, results);
                                 });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    CHECK(harness.result(1) == i32(42));
    CHECK(caller == harness.address());
}

TEST_CASE("ReferenceInterpreter.tailCalledHostSeesItsCaller")
{
    // A host function reached by return_call is called by the instance whose frame it replaced.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .importFunction("env", "host", 0)
        .function(0, {}, code({ { 0x12, 0x00 } }));
    auto caller = std::optional<uint32_t> {};
    auto harness =
        Harness { builder, [&](Store& store) {
                     auto const host = addHost(store,
                                               signature({}, {}),
                                               [&](HostContext& context,
                                                   std::span<Value const>,
                                                   std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                                   caller = context.callerInstance();
                                                   return {};
                                               });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    REQUIRE(harness.invoke(1).has_value());
    CHECK(caller == harness.address());
}

TEST_CASE("ReferenceInterpreter.hostWritesItsCallersMemory")
{
    // The host writes 42 at byte 3 of its caller's memory 0, which the caller then loads; the caller has
    // no memory 1.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({}, { TypeI32 }))
        .importFunction("env", "host", 0)
        .memory(atLeast(1))
        .function(1, {}, code({ call(0), i32Const(3), memoryAccess(0x2D, 0, 0) })); // i32.load8_u
    auto second = std::optional<std::span<std::byte>> { std::span<std::byte> {} };
    auto harness =
        Harness { builder, [&](Store& store) {
                     auto const host = addHost(store,
                                               signature({}, {}),
                                               [&](HostContext& context,
                                                   std::span<Value const>,
                                                   std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                                   second = context.callerMemory(1);
                                                   auto const memory = context.callerMemory(0);
                                                   if (!memory || memory->size() < 4)
                                                       return std::unexpected { Abrupt { HostFailure {} } };
                                                   (*memory)[3] = std::byte { 42 };
                                                   return {};
                                               });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    CHECK(harness.result(1) == i32(42));
    CHECK(second == std::nullopt);
}

TEST_CASE("ReferenceInterpreter.hostSeesTheInstanceThatCalledIt")
{
    // Three instances: an empty one at 0, B at 1 and A at 2. A calls B's two exports, one calling the
    // host, one tail-calling it; the host sees B both times, not A beneath it nor 0, and writes 42 at
    // byte 3 of B's memory, which B loads, while A's stays 0.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, isoline::runtime::StoreServices { .allocator = allocator } };
    auto interpreter = ReferenceInterpreter {};
    auto const load = [&](ModuleBuilder const& builder, std::vector<ExternValue> const& imports) {
        auto validated = isoline::validate::ValidatedModule::load(
            builder.build(), FeatureSet::all(), isoline::binary::DecodeLimits {});
        REQUIRE(validated.has_value());
        auto const address =
            isoline::runtime::instantiate(store, interpreter, std::move(*validated), imports);
        REQUIRE(address.has_value());
        return *address;
    };
    auto callers = std::vector<std::optional<uint32_t>> {};
    auto const host = addHost(store,
                              signature({}, {}),
                              [&](HostContext& context,
                                  std::span<Value const>,
                                  std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                  callers.push_back(context.callerInstance());
                                  auto const memory = context.callerMemory(0);
                                  if (!memory || memory->size() < 4)
                                      return std::unexpected { Abrupt { HostFailure {} } };
                                  (*memory)[3] = std::byte { 42 };
                                  return {};
                              });
    REQUIRE(load(ModuleBuilder {}, {}) == 0);

    auto b = ModuleBuilder {};
    b.type(signature({}, {}))
        .type(signature({}, { TypeI32 }))
        .importFunction("env", "host", 0)
        .memory(atLeast(1))
        .function(1, {}, code({ call(0), i32Const(3), memoryAccess(0x2D, 0, 0) })) // i32.load8_u
        .function(0, {}, code({ { 0x12, 0x00 } }))                                 // return_call 0
        .exportFunction("viaCall", 1)
        .exportFunction("viaTail", 2);
    auto const bAddress = load(b, { ExternValue { .kind = ExternKind::Func, .address = host } });
    REQUIRE(bAddress == 1);
    auto const& bExports = store.moduleInstance(bAddress).exports;

    auto a = ModuleBuilder {};
    a.type(signature({}, {}))
        .type(signature({}, { TypeI32 }))
        .importFunction("b", "viaTail", 0)
        .importFunction("b", "viaCall", 1)
        .memory(atLeast(1))
        .function(
            1, {}, code({ call(0), call(1), i32Const(3), memoryAccess(0x2D, 0, 0), { 0x6A } })); // + A's byte
    auto const aAddress = load(a, { bExports.at(1).second, bExports.at(0).second });
    REQUIRE(aAddress == 2);

    auto results = std::vector<Value>(1);
    auto const run = isoline::runtime::invokeChecked(
        store, interpreter, store.moduleInstance(aAddress).functions.at(2), {}, results);
    REQUIRE(run.outcome.has_value());
    CHECK(results.front() == i32(42));
    CHECK(callers == std::vector<std::optional<uint32_t>> { bAddress, bAddress });
}

TEST_CASE("ReferenceInterpreter.stackValuesFollowTheLimit")
{
    // Function 0 holds 20 locals and calls function 1: past a store's 10 stack values, the call traps.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .function(0, std::vector<ValueType>(20, isoline::binary::I32), call(1))
        .function(0, {}, {});
    CHECK(Harness { builder, {}, StoreLimits { .maxStackValues = 10 } }.trap(0)
          == TrapCode::CallStackExhausted);
    CHECK(Harness { builder }.invoke(0).has_value());
}

namespace
{

constexpr auto TypeV128 = isoline::binary::V128;

/// A SIMD instruction: the 0xFD prefix, then @p subOpcode as a u32.
Bytes simd(uint32_t subOpcode)
{
    return code({ { 0xFD }, encodeU32(subOpcode) });
}

/// A SIMD load or store of @p subOpcode with alignment @p align and offset @p offset.
Bytes vectorAccess(uint32_t subOpcode, uint32_t align, uint32_t offset)
{
    return code({ simd(subOpcode), encodeU32(align), encodeU32(offset) });
}

/// v128.const of the 16 bytes @p first, @p first + 1, …
Bytes v128Const(uint8_t first)
{
    auto out = simd(0x0C);
    for (auto const index: std::views::iota(0, 16))
        out.push_back(static_cast<uint8_t>(first + index));
    return out;
}

/// v128.const of 16 zero bytes.
Bytes v128Zero()
{
    return code({ simd(0x0C), Bytes(16, 0x00) });
}

/// A vector value of the two halves @p low and @p high.
Value v128(uint64_t low, uint64_t high)
{
    return isoline::runtime::V128 { .lanes = { low, high } };
}

} // namespace

TEST_CASE("Interpreter.v128LoadAtMemoryEnd")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, { TypeV128 }))
        .memory(atLeast(1))
        .function(0, {}, code({ localGet(0), vectorAccess(0x00, 4, 0) })) // v128.load
        .function(
            0, {}, code({ localGet(0), v128Const(0), vectorAccess(0x56, 2, 0), { 0x00 } })); // load32_lane 0
    auto harness = Harness { builder };
    CHECK(harness.result(0, { i32(65520) }) == v128(0, 0));
    CHECK(harness.trap(0, { i32(65521) }) == TrapCode::OutOfBoundsMemoryAccess);
    CHECK(harness.result(1, { i32(65532) }) == v128(0x0706'0504'0000'0000, 0x0F0E'0D0C'0B0A'0908));
    CHECK(harness.trap(1, { i32(65533) }) == TrapCode::OutOfBoundsMemoryAccess);
}

TEST_CASE("Interpreter.vectorLoads")
{
    // Memory 0 .. 15: F0 81 82 03 84 85 86 87 08 09 0A 0B 0C 0D 0E 0F.
    auto const data = code({ { 0x00 },
                             i32Const(0),
                             { 0x0B, 0x10 },
                             { 0xF0, 0x81, 0x82, 0x03, 0x84, 0x85, 0x86, 0x87 },
                             { 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F } });
    struct Case
    {
        Bytes code;
        Value expected;
    };
    auto const cases = std::to_array<Case>({
        // v128.load and the six widening loads, at 0.
        { .code = code({ i32Const(0), vectorAccess(0x00, 0, 0) }),
          .expected = v128(0x8786'8584'0382'81F0, 0x0F0E'0D0C'0B0A'0908) },
        { .code = code({ i32Const(0), vectorAccess(0x01, 0, 0) }),
          .expected = v128(0x0003'FF82'FF81'FFF0, 0xFF87'FF86'FF85'FF84) },
        { .code = code({ i32Const(0), vectorAccess(0x02, 0, 0) }),
          .expected = v128(0x0003'0082'0081'00F0, 0x0087'0086'0085'0084) },
        { .code = code({ i32Const(0), vectorAccess(0x03, 0, 0) }),
          .expected = v128(0x0000'0382'FFFF'81F0, 0xFFFF'8786'FFFF'8584) },
        { .code = code({ i32Const(0), vectorAccess(0x04, 0, 0) }),
          .expected = v128(0x0000'0382'0000'81F0, 0x0000'8786'0000'8584) },
        { .code = code({ i32Const(0), vectorAccess(0x05, 0, 0) }),
          .expected = v128(0x0000'0000'0382'81F0, 0xFFFF'FFFF'8786'8584) },
        { .code = code({ i32Const(0), vectorAccess(0x06, 0, 0) }),
          .expected = v128(0x0000'0000'0382'81F0, 0x0000'0000'8786'8584) },
        // The four splats.
        { .code = code({ i32Const(1), vectorAccess(0x07, 0, 0) }),
          .expected = v128(0x8181'8181'8181'8181, 0x8181'8181'8181'8181) },
        { .code = code({ i32Const(0), vectorAccess(0x08, 0, 0) }),
          .expected = v128(0x81F0'81F0'81F0'81F0, 0x81F0'81F0'81F0'81F0) },
        { .code = code({ i32Const(0), vectorAccess(0x09, 0, 4) }),
          .expected = v128(0x8786'8584'8786'8584, 0x8786'8584'8786'8584) },
        { .code = code({ i32Const(8), vectorAccess(0x0A, 0, 0) }),
          .expected = v128(0x0F0E'0D0C'0B0A'0908, 0x0F0E'0D0C'0B0A'0908) },
        // The two zero-extending loads.
        { .code = code({ i32Const(0), vectorAccess(0x5C, 0, 0) }), .expected = v128(0x0382'81F0, 0) },
        { .code = code({ i32Const(8), vectorAccess(0x5D, 0, 0) }),
          .expected = v128(0x0F0E'0D0C'0B0A'0908, 0) },
        // v128.load16_lane 7 into a zero vector; v128.load8_lane 0 into the bytes 0x30 ….
        { .code = code({ i32Const(0), v128Zero(), vectorAccess(0x55, 0, 0), { 0x07 } }),
          .expected = v128(0, 0x81F0'0000'0000'0000) },
        { .code = code({ i32Const(1), v128Const(0x30), vectorAccess(0x54, 0, 0), { 0x00 } }),
          .expected = v128(0x3736'3534'3332'3181, 0x3F3E'3D3C'3B3A'3938) },
    });
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeV128 })).memory(atLeast(1)).data(data);
    for (auto const& testCase: cases)
        builder.function(0, {}, testCase.code);
    auto harness = Harness { builder };
    auto index = uint32_t { 0 };
    for (auto const& testCase: cases)
    {
        INFO("case " << index);
        CHECK(harness.result(index) == testCase.expected);
        ++index;
    }
}

TEST_CASE("Interpreter.vectorStores")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({ TypeI32 }, { TypeI64 }))
        .memory(atLeast(1))
        // v128.store at 0, v128.store8_lane 5 at 32, v128.store64_lane 1 at 40, all of 00 .. 0F.
        .function(0,
                  {},
                  code({ i32Const(0),
                         v128Const(0),
                         vectorAccess(0x0B, 4, 0),
                         i32Const(32),
                         v128Const(0),
                         vectorAccess(0x58, 0, 0),
                         { 0x05 },
                         i32Const(40),
                         v128Const(0),
                         vectorAccess(0x5B, 3, 0),
                         { 0x01 } }))
        .function(1, {}, code({ localGet(0), memoryAccess(0x29, 3, 0) })) // i64.load
        // v128.store32_lane 0 at the last three bytes.
        .function(0, {}, code({ i32Const(65533), v128Zero(), vectorAccess(0x5A, 0, 0), { 0x00 } }));
    auto harness = Harness { builder };
    REQUIRE(harness.invoke(0).has_value());
    CHECK(harness.result(1, { i32(0) }) == Value { I64 { 0x0706'0504'0302'0100 } });
    CHECK(harness.result(1, { i32(8) }) == Value { I64 { 0x0F0E'0D0C'0B0A'0908 } });
    CHECK(harness.result(1, { i32(32) }) == Value { I64 { 0x05 } });
    CHECK(harness.result(1, { i32(40) }) == Value { I64 { 0x0F0E'0D0C'0B0A'0908 } });
    CHECK(harness.trap(2) == TrapCode::OutOfBoundsMemoryAccess);
}

TEST_CASE("Interpreter.vectorOperators")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeV128 }))
        .type(signature({}, { TypeI32 }))
        .global(GlobalType { .type = TypeV128, .mutability = Mutability::Const }, v128Const(0x40))
        .function(0, {}, code({ v128Const(0x00), simd(0x4D) }))                              // 0 v128.not
        .function(0, {}, code({ v128Const(0xF0), v128Const(0x0F), simd(0x4F) }))             // 1 v128.andnot
        .function(0, {}, code({ v128Const(0x00), v128Zero(), v128Const(0xF0), simd(0x52) })) // 2 bitselect
        .function(1, {}, code({ v128Zero(), simd(0x53) }))                                   // 3 any_true
        .function(0, {}, code({ v128Const(0x00), i32Const(33), simd(0xAB) }))                // 4 i32x4.shl
        .function(0, {}, code({ i32Const(0x1234'5678), simd(0x11) }))                        // 5 i32x4.splat
        .function(0, {}, code({ v128Zero(), i64Const(7), simd(0x1E), { 0x01 } })) // 6 replace_lane 1
        .function(1, {}, code({ v128Const(0x80), simd(0x15), { 0x0F } }))         // 7 extract_lane_s 15
        .function(
            0, {}, code({ v128Const(0x00), v128Const(0x10), simd(0x0D), Bytes(8, 0x1F), Bytes(8, 0x01) }))
        .function(0, {}, code({ { 0x23, 0x00 } })); // 9 global.get of a v128.const global
    auto harness = Harness { builder };
    CHECK(harness.result(0) == v128(0xF8F9'FAFB'FCFD'FEFF, 0xF0F1'F2F3'F4F5'F6F7));
    // andnot: F0 .. FF and not 0F .. 1E; the first operand is the lower on the stack.
    CHECK(harness.result(1) == v128(0xE1E2'E1E4'E1E2'E1F0, 0xE1E2'E1E4'E1E2'E1E8));
    // bitselect of 00 .. 0F and zero by the mask F0 .. FF: each byte's low nibble from the first.
    CHECK(harness.result(2) == v128(0x0706'0504'0302'0100, 0x0F0E'0D0C'0B0A'0908));
    CHECK(harness.result(3) == i32(0));
    CHECK(harness.result(4) == v128(0x0E0C'0A08'0604'0200, 0x1E1C'1A18'1614'1210));
    CHECK(harness.result(5) == v128(0x1234'5678'1234'5678, 0x1234'5678'1234'5678));
    CHECK(harness.result(6) == v128(0, 7));
    CHECK(harness.result(7) == i32(static_cast<int32_t>(0xFFFF'FF8F)));
    CHECK(harness.result(8) == v128(0x1F1F'1F1F'1F1F'1F1F, 0x0101'0101'0101'0101));
    CHECK(harness.result(9) == v128(0x4746'4544'4342'4140, 0x4F4E'4D4C'4B4A'4948));
}

namespace
{

Limits atLeast64(uint64_t min)
{
    return Limits { .min = min, .max = std::nullopt, .addressType = AddressType::I64 };
}

/// A load or store of @p opcode byte with alignment exponent @p align and a u64 offset @p offset.
Bytes memoryAccess64(uint8_t opcode, uint32_t align, uint64_t offset)
{
    return code({ { opcode }, encodeU32(align), isoline::binary::testing::encodeU64(offset) });
}

} // namespace

TEST_CASE("Interpreter.memory64EffectiveAddressOverflowTraps")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI64 }, { TypeI32 }))
        .memory(atLeast64(1))
        .function(0, {}, code({ localGet(0), memoryAccess64(0x28, 2, 0x20) })) // i32.load offset=0x20
        .function(0, {}, code({ localGet(0), memoryAccess64(0x28, 2, ~uint64_t { 0 }) }));
    auto harness = Harness { builder };
    // address + offset wraps past 2^64 to 0x10, in bounds if the sum were taken modulo 2^64.
    CHECK(harness.trap(0, { I64 { 0xFFFF'FFFF'FFFF'FFF0 } }) == TrapCode::OutOfBoundsMemoryAccess);
    CHECK(harness.result(0, { I64 { 8 } }) == i32(0));
    CHECK(harness.trap(0, { I64 { PageSize - 0x20 - 3 } }) == TrapCode::OutOfBoundsMemoryAccess);
    CHECK(harness.result(0, { I64 { PageSize - 0x20 - 4 } }) == i32(0));
    // An offset of 2^64-1 reaches no byte of any memory.
    CHECK(harness.trap(1, { I64 { 0 } }) == TrapCode::OutOfBoundsMemoryAccess);
}

TEST_CASE("Interpreter.memoryGrow64ReturnsI64")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI64 }, { TypeI64 }))
        .memory(Limits { .min = 1, .max = 2, .addressType = AddressType::I64 })
        .function(0, {}, code({ localGet(0), { 0x40, 0x00 } })) // memory.grow
        .function(0, {}, { 0x3F, 0x00 });                       // memory.size
    auto harness = Harness { builder };
    CHECK(harness.result(0, { I64 { 1 } }) == Value { I64 { 1 } });
    CHECK(harness.result(1, { I64 { 0 } }) == Value { I64 { 2 } });
    // Refused: past the maximum, and a delta whose page count passes any address.
    CHECK(harness.result(0, { I64 { 1 } }) == Value { I64 { ~uint64_t { 0 } } });
    CHECK(harness.result(0, { I64 { uint64_t { 1 } << 63 } }) == Value { I64 { ~uint64_t { 0 } } });
    CHECK(harness.result(0, { I64 { 0 } }) == Value { I64 { 2 } });
}

TEST_CASE("Interpreter.table64CallIndirect")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI64 }, { TypeI32 }))
        .type(signature({}, { TypeI32 }))
        .table(TableType { .element = FuncRef, .limits = atLeast64(2) })
        .function(0, {}, code({ localGet(0), { 0x11, 0x01, 0x00 } })) // call_indirect type 1
        .function(1, {}, i32Const(7))
        .element(code({ { 0x00 }, i64Const(0), { 0x0B, 0x02, 0x01, 0x01 } }));
    auto harness = Harness { builder };
    CHECK(harness.result(0, { I64 { 1 } }) == i32(7));
    auto const result = harness.invoke(0, { I64 { uint64_t { 1 } << 32 } });
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == TrapCode::UndefinedElement);
    CHECK(result.error().subject == int64_t { 4'294'967'296 });
    CHECK(isoline::runtime::specMessage(result.error()) == "undefined element 4294967296");
    // The largest index is spelled signed, as Int64.to_string does.
    auto const last = harness.invoke(0, { I64 { ~uint64_t { 0 } } });
    REQUIRE_FALSE(last.has_value());
    CHECK(isoline::runtime::specMessage(last.error()) == "undefined element -1");
}

TEST_CASE("Interpreter.table64Instructions")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI64 }))
        .type(signature({ TypeI64 }, { TypeI32 }))
        .table(TableType { .element = FuncRef, .limits = atLeast64(1) })
        .function(0,
                  {},
                  code({ { 0xD0, 0x70 }, // ref.null func
                         i64Const(2),
                         { 0xFC, 0x0F, 0x00 },                         // table.grow 0
                         { 0x1A },                                     // drop
                         { 0xFC, 0x10, 0x00 } }))                      // table.size 0
        .function(1, {}, code({ localGet(0), { 0x25, 0x00, 0xD1 } })); // table.get 0; ref.is_null
    auto harness = Harness { builder };
    CHECK(harness.result(0) == Value { I64 { 3 } });
    CHECK(harness.result(1, { I64 { 2 } }) == i32(1));
    CHECK(harness.trap(1, { I64 { uint64_t { 1 } << 32 } }) == TrapCode::OutOfBoundsTableAccess);
}

namespace
{

/// A load or store of @p opcode byte from memory @p memory, with alignment exponent @p align and
/// offset @p offset: the memarg's alignment field carries bit 6, a memory index follows.
Bytes memoryAccessIn(uint8_t opcode, uint32_t memory, uint32_t align, uint32_t offset)
{
    return code({ { opcode }, encodeU32(align | 0x40), encodeU32(memory), encodeU32(offset) });
}

/// memory.copy from memory @p source to memory @p destination.
Bytes memoryCopy(uint8_t destination, uint8_t source)
{
    return { 0xFC, 0x0A, destination, source };
}

/// A SIMD load or store of @p subOpcode on memory @p memory, with alignment @p align and offset
/// @p offset: the memarg's alignment field carries bit 6, a memory index follows.
Bytes vectorAccessIn(uint32_t subOpcode, uint32_t memory, uint32_t align, uint32_t offset)
{
    return code({ simd(subOpcode), encodeU32(align | 0x40), encodeU32(memory), encodeU32(offset) });
}

} // namespace

TEST_CASE("Interpreter.laneAccessOnSecondMemory")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({}, { TypeV128 }))
        .memory(atLeast(1))
        .memory(atLeast(1))
        // v128.store8_lane 5 of 30 .. 3F at 4 in memory 1.
        .function(0, {}, code({ i32Const(4), v128Const(0x30), vectorAccessIn(0x58, 1, 0, 0), { 0x05 } }))
        // v128.load8_lane 0 at 4 into a zero vector, from memory 1, then from memory 0.
        .function(1, {}, code({ i32Const(4), v128Zero(), vectorAccessIn(0x54, 1, 0, 0), { 0x00 } }))
        .function(1, {}, code({ i32Const(4), v128Zero(), vectorAccessIn(0x54, 0, 0, 0), { 0x00 } }));
    auto harness = Harness { builder };
    REQUIRE(harness.invoke(0).has_value());
    CHECK(harness.result(1) == v128(0x35, 0));
    CHECK(harness.result(2) == v128(0, 0));
}

TEST_CASE("Interpreter.copyBetweenMemories")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .memory(atLeast(1))
        .memory(atLeast(1))
        .function(0,
                  {},
                  code({ i32Const(0),
                         i32Const(0x0403'0201),
                         memoryAccess(0x36, 2, 0), // i32.store to memory 0
                         i32Const(8),
                         i32Const(0),
                         i32Const(4),
                         memoryCopy(1, 0),
                         i32Const(8),
                         memoryAccessIn(0x28, 1, 2, 0) }))                     // i32.load from memory 1
        .function(0, {}, code({ i32Const(8), memoryAccess(0x28, 2, 0) }))      // memory 0 at 8
        .function(0, {}, code({ i32Const(0), memoryAccessIn(0x28, 1, 2, 0) })) // memory 1 at 0
        .function(0,
                  {},
                  code({ i32Const(static_cast<int32_t>(PageSize) - 2),
                         i32Const(0),
                         i32Const(4),
                         memoryCopy(1, 0),
                         i32Const(0) }));
    auto harness = Harness { builder };
    CHECK(harness.result(0) == i32(0x0403'0201));
    CHECK(harness.result(1) == i32(0));
    CHECK(harness.result(2) == i32(0));
    CHECK(harness.trap(3) == TrapCode::OutOfBoundsMemoryAccess);
}

TEST_CASE("Interpreter.copyBetweenMixedAddressMemories")
{
    // Memory 0 has i64 addresses, memory 1 i32 ones; a copy's count is typed as the narrower side.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .memory(atLeast64(1))
        .memory(atLeast(1))
        .function(0,
                  {},
                  code({ i64Const(0),
                         i32Const(0x0403'0201),
                         memoryAccess(0x36, 2, 0), // i32.store to memory 0
                         i32Const(8),              // destination: i32
                         i64Const(0),              // source: i64
                         i32Const(4),              // count: i32
                         memoryCopy(1, 0),
                         i32Const(8),
                         memoryAccessIn(0x28, 1, 2, 0) }))
        .function(0,
                  {},
                  code({ i64Const(16), // destination: i64
                         i32Const(8),  // source: i32
                         i32Const(4),  // count: i32
                         memoryCopy(0, 1),
                         i64Const(16),
                         memoryAccess(0x28, 2, 0) }))
        // A source past 2^32 is out of bounds, not truncated to 0.
        .function(
            0,
            {},
            code({ i32Const(0), i64Const(int64_t { 1 } << 32), i32Const(4), memoryCopy(1, 0), i32Const(0) }));
    auto harness = Harness { builder };
    CHECK(harness.result(0) == i32(0x0403'0201));
    CHECK(harness.result(1) == i32(0x0403'0201));
    CHECK(harness.trap(2) == TrapCode::OutOfBoundsMemoryAccess);
}

namespace
{

/// A module of typed function references: type 0 is [] -> [i32], type 1 [(ref null 0)] -> [i32].
/// Function 0 returns 7 and is declared for ref.func; functions 1 and 2 are of type 1, and functions
/// from 3 on of type 0, with @p bodies of type 1 and then of type 0.
ModuleBuilder typedReferenceModule(std::initializer_list<Bytes> takingAReference,
                                   std::initializer_list<Bytes> returningI32)
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .type(signature({ isoline::binary::concreteRef(0, isoline::binary::Nullable::Yes) }, { TypeI32 }))
        .function(0, {}, i32Const(7));
    for (auto const& body: takingAReference)
        builder.function(1, {}, body);
    for (auto const& body: returningI32)
        builder.function(0, {}, body);
    builder.element({ 0x03, 0x00, 0x01, 0x00 }); // declares function 0
    return builder;
}

/// ref.null 0: a null (ref null 0).
Bytes refNull0()
{
    return { 0xD0, 0x00 };
}

/// ref.func 0.
Bytes refFunc0()
{
    return { 0xD2, 0x00 };
}

/// call_ref 0.
Bytes callRef0()
{
    return { 0x14, 0x00 };
}

} // namespace

TEST_CASE("Interpreter.callRefNullTraps")
{
    auto harness =
        Harness { typedReferenceModule({ code({ localGet(0), callRef0() }) },
                                       { code({ refFunc0(), call(1) }), code({ refNull0(), call(1) }) }) };
    CHECK(harness.result(2) == i32(7));
    CHECK(harness.trap(3) == TrapCode::NullFunctionReference);
    CHECK(harness.invoke(3).error() == Trap { TrapCode::NullFunctionReference });
}

TEST_CASE("Interpreter.refAsNonNullTraps")
{
    auto harness = Harness { typedReferenceModule(
        {}, { code({ refFunc0(), { 0xD4 }, callRef0() }), code({ refNull0(), { 0xD4 }, callRef0() }) }) };
    CHECK(harness.result(1) == i32(7));
    CHECK(harness.trap(2) == TrapCode::NullReference);
}

TEST_CASE("Interpreter.brOnNullBranches")
{
    // block (result i32) i32.const 42 local.get 0 br_on_null 0 call_ref 0 i32.add end: a null branches
    // with the 42 beneath it; a function falls through with the reference, which call_ref calls.
    auto harness = Harness { typedReferenceModule(
        { code({ { 0x02, 0x7F }, i32Const(42), localGet(0), { 0xD5, 0x00 }, callRef0(), { 0x6A, 0x0B } }) },
        { code({ refNull0(), call(1) }), code({ refFunc0(), call(1) }) }) };
    CHECK(harness.result(2) == i32(42));
    CHECK(harness.result(3) == i32(49));
}

TEST_CASE("Interpreter.brOnNonNullBranches")
{
    // block (result (ref 0)) local.get 0 br_on_non_null 0 i32.const -1 return end call_ref 0: a
    // function branches with the reference; a null falls through without it.
    auto harness =
        Harness { typedReferenceModule({ code({ { 0x02, 0x64, 0x00 },
                                                localGet(0),
                                                { 0xD6, 0x00 },
                                                i32Const(-1),
                                                { 0x0F, 0x0B },
                                                callRef0() }) },
                                       { code({ refNull0(), call(1) }), code({ refFunc0(), call(1) }) }) };
    CHECK(harness.result(2) == i32(-1));
    CHECK(harness.result(3) == i32(7));
}

TEST_CASE("Interpreter.callRefOfAHostReference")
{
    // A host function hands the guest a (ref 0) to another host function, which call_ref calls. The
    // reference's function must have type 0: one of another type is a checked abort (Require), which
    // P3's typed host boundary turns into an error before it reaches the guest.
    using isoline::runtime::Ref;
    using isoline::runtime::RefKind;
    auto const nonNullTo0 = isoline::binary::concreteRef(0, isoline::binary::Nullable::No);
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .type(signature({}, { nonNullTo0 }))
        .importFunction("env", "callee", 1)
        .function(0, {}, code({ call(0), callRef0() }));
    auto harness =
        Harness { builder, [](Store& store) {
                     auto const callee =
                         addHost(store,
                                 signature({}, { TypeI32 }),
                                 [](HostContext&,
                                    std::span<Value const>,
                                    std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
                                     results[0] = I32 { 7 };
                                     return {};
                                 });
                     auto const typeId = store.function(callee).typeId;
                     auto const reference = Ref { .kind = RefKind::Func,
                                                  .heap = { .kind = isoline::binary::AbstractHeapType::Func },
                                                  .address = callee };
                     auto const provider = addHost(
                         store,
                         signature({},
                                   { isoline::binary::concreteRef(typeId, isoline::binary::Nullable::No) }),
                         [reference](HostContext&,
                                     std::span<Value const>,
                                     std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
                             results[0] = reference;
                             return {};
                         });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = provider } };
                 } };
    CHECK(harness.result(1) == i32(7));
}

namespace
{

/// return_call @p index.
Bytes returnCall(uint32_t index)
{
    return code({ { 0x12 }, encodeU32(index) });
}

/// A function [i64] -> [i64] counting its parameter down to 0, calling itself with @p self per step:
/// local.get 0 i64.eqz if (result i64) i64.const 0 else <step> end, where the step, local.get 0
/// i64.const 1 i64.sub <self>, sits inside @p blocks nested blocks of result i64.
Bytes countDown(Bytes const& self, uint32_t blocks = 0)
{
    auto step = code({ localGet(0), i64Const(1), { 0x7D }, self });
    for ([[maybe_unused]] auto const block: std::views::iota(0U, blocks))
        step = code({ { 0x02, 0x7E }, step, { 0x0B } });
    return code({ localGet(0), { 0x50, 0x04, 0x7E }, i64Const(0), { 0x05 }, step, { 0x0B } });
}

} // namespace

TEST_CASE("Interpreter.tailCallDoesNotGrowDepth")
{
    // A store allowing 100 frames: a million tail calls run in one frame, where plain calls trap.
    // The tail call sits in 5 blocks: were their labels left behind, a million calls would leave 6
    // million, past the stacks' budget of 4 Mi entries, and trap.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI64 }, { TypeI64 }))
        .function(0, {}, countDown(returnCall(0), 5))
        .function(0, {}, countDown(call(1)));
    auto harness = Harness { builder, {}, 100 };
    CHECK(harness.result(0, { I64 { 1'000'000 } }) == Value { I64 { 0 } });
    CHECK(harness.result(1, { I64 { 98 } }) == Value { I64 { 0 } });
    CHECK(harness.trap(1, { I64 { 100 } }) == TrapCode::CallStackExhausted);
    // The frames given back: the depth is what it was, so the deepest plain recursion still runs.
    CHECK(harness.result(1, { I64 { 98 } }) == Value { I64 { 0 } });
    CHECK(harness.result(0, { I64 { 0 } }) == Value { I64 { 0 } });
}

TEST_CASE("Interpreter.tailCallKeepsLocalsBounded")
{
    // 50'000 locals per frame exhaust the stacks' budget after about 80 plain calls; a tail call
    // frees the frame's locals before the callee's are pushed, so a thousand of them complete.
    auto const locals = std::vector<ValueType>(50'000, TypeI64);
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI64 }, { TypeI64 }))
        .function(0, locals, countDown(returnCall(0)))
        .function(0, locals, countDown(call(1)));
    auto harness = Harness { builder };
    CHECK(harness.result(0, { I64 { 1'000 } }) == Value { I64 { 0 } });
    CHECK(harness.trap(1, { I64 { 1'000 } }) == TrapCode::CallStackExhausted);
}

TEST_CASE("Interpreter.returnCallToHost")
{
    // Function 1 tail-calls the host import 0 over an operand of its own, and what follows the
    // return_call never runs; function 2 calls 1 with a value beneath, which the tail call leaves in
    // place: 1's frame returns with the host's results only.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .importFunction("env", "seven", 0)
        .function(0, {}, code({ i32Const(99), returnCall(0), i32Const(13) }))
        .function(0, {}, code({ i32Const(35), call(1), { 0x6A } })); // i32.add
    auto harness =
        Harness { builder, [](Store& store) {
                     auto const seven =
                         addHost(store,
                                 signature({}, { TypeI32 }),
                                 [](HostContext&,
                                    std::span<Value const>,
                                    std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
                                     results[0] = I32 { 7 };
                                     return {};
                                 });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = seven } };
                 } };
    CHECK(harness.result(1) == i32(7));
    CHECK(harness.result(2) == i32(42));
}

TEST_CASE("Interpreter.returnCallMovesArgumentsOverOperands")
{
    // Function 1 leaves 99 beneath its argument, local 0, and tail-calls the identity function 0:
    // the callee's parameter is the local, not the 99 the frame's operands begin with.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, { TypeI32 }))
        .function(0, {}, localGet(0))
        .function(0, {}, code({ i32Const(99), localGet(0), returnCall(0) }));
    auto harness = Harness { builder };
    CHECK(harness.result(1, { i32(5) }) == i32(5));
}

TEST_CASE("Interpreter.returnCallIndirectTrapsAsCallIndirect")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, { TypeI32 }))
        .type(signature({ TypeI32 }, { TypeI32 }))
        .table(TableType { .element = FuncRef, .limits = atLeast(3) })
        .function(0, {}, i32Const(7))                                 // 0: () -> i32
        .function(1, {}, localGet(0))                                 // 1: (i32) -> i32
        .function(1, {}, code({ localGet(0), { 0x13, 0x00, 0x00 } })) // 2: return_call_indirect type 0
        .element(activeElements(0, { 0x00, 0x01 }));
    auto harness = Harness { builder };
    CHECK(harness.result(2, { i32(0) }) == i32(7));
    CHECK(harness.trap(2, { i32(1) }) == TrapCode::IndirectCallTypeMismatch);
    CHECK(harness.trap(2, { i32(2) }) == TrapCode::UninitializedElement);
    CHECK(harness.trap(2, { i32(3) }) == TrapCode::UndefinedElement);
    auto const undefined = harness.invoke(2, { i32(3) });
    REQUIRE_FALSE(undefined.has_value());
    CHECK(isoline::runtime::specMessage(undefined.error()) == "undefined element 3");
}

TEST_CASE("Interpreter.returnCallRefTrapsOnNull")
{
    // Function 1 tail-calls its (ref null 0) argument; 2 passes it function 0, 3 a null.
    auto harness =
        Harness { typedReferenceModule({ code({ localGet(0), { 0x15, 0x00 } }) },
                                       { code({ refFunc0(), call(1) }), code({ refNull0(), call(1) }) }) };
    CHECK(harness.result(2) == i32(7));
    CHECK(harness.trap(3) == TrapCode::NullFunctionReference);
}

namespace
{

/// throw @p tag.
Bytes throwTag(uint32_t tag)
{
    return code({ { 0x08 }, encodeU32(tag) });
}

/// The opening of a try_table of block type @p blockType with one clause: kind byte @p kind, its tag
/// @p tag when the kind names one (catch, catch_ref), and its label @p label.
Bytes tryTable(uint8_t blockType, uint8_t kind, uint32_t tag, uint32_t label)
{
    auto const byTag = kind == 0x00 || kind == 0x01;
    return code({ { 0x1F, blockType, 0x01, kind }, byTag ? encodeU32(tag) : Bytes {}, encodeU32(label) });
}

/// Clause kinds' bytes.
constexpr auto ClauseCatch = uint8_t { 0x00 };
constexpr auto ClauseCatchAll = uint8_t { 0x02 };
constexpr auto ClauseCatchAllRef = uint8_t { 0x03 };

/// Block types' bytes: none, i32, exnref.
constexpr auto NoResult = uint8_t { 0x40 };
constexpr auto I32Result = uint8_t { 0x7F };
constexpr auto ExnRefResult = uint8_t { 0x69 };

/// The exception @p outcome ended with, which must be an uncaught one.
Thrown thrownOf(std::expected<std::vector<Value>, Abrupt> const& outcome)
{
    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(std::holds_alternative<Thrown>(outcome.error()));
    return std::get<Thrown>(outcome.error());
}

} // namespace

TEST_CASE("Interpreter.catchPayload")
{
    // 5 + block (result i32) try_table (catch $e 0) i32.const 99 i32.const 42 throw $e end i32.const 0
    // end: the catch resets the operands to the try_table's, dropping the 99, and branches to the block
    // with the payload; the 5 beneath the block stays.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, { TypeI32 }))
        .tag(0)
        .function(1,
                  {},
                  code({ i32Const(5),
                         { 0x02, I32Result },
                         tryTable(NoResult, ClauseCatch, 0, 0),
                         i32Const(99),
                         i32Const(42),
                         throwTag(0),
                         { 0x0B },
                         i32Const(0),
                         { 0x0B, 0x6A } }));
    auto harness = Harness { builder };
    CHECK(harness.result(0) == i32(47));
}

TEST_CASE("Interpreter.catchClauseOrder")
{
    // Tags 0 and 1, both [i32]; each function throws one of them with payload 7:
    //   block $outer (result i32) block $one (result i32) block $all
    //     try_table (catch 0 $outer) (catch_all $all)
    //       try_table (catch 1 $one) i32.const 7 throw end
    //     end
    //   end i32.const -1 return
    //   end i32.const 1000 i32.add return
    //   end
    // The inner try_table catches tag 1 only, so tag 0 passes it to the outer, whose first matching
    // clause, catch 0 before catch_all, takes it.
    auto const body = [](uint32_t thrown) {
        return code({ { 0x02, I32Result, 0x02, I32Result, 0x02, NoResult },
                      { 0x1F, NoResult, 0x02, 0x00, 0x00, 0x02, 0x02, 0x00 },
                      tryTable(NoResult, ClauseCatch, 1, 2),
                      i32Const(7),
                      throwTag(thrown),
                      { 0x0B, 0x0B, 0x0B },
                      i32Const(-1),
                      { 0x0F, 0x0B },
                      i32Const(1000),
                      { 0x6A, 0x0F, 0x0B } });
    };
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, { TypeI32 }))
        .tag(0)
        .tag(0)
        .function(1, {}, body(0))
        .function(1, {}, body(1));
    auto harness = Harness { builder };
    CHECK(harness.result(0) == i32(7));    // the outer catch 0, to $outer
    CHECK(harness.result(1) == i32(1007)); // the inner catch 1, to $one
}

TEST_CASE("Interpreter.throwRefRethrows")
{
    // block (result i32) try_table (catch $e 0)
    //   block (result exnref) try_table (catch_all_ref 0) i32.const 7 throw $e end unreachable end
    //   throw_ref
    // end unreachable end: the exnref rethrows the same exception, which the outer catch unpacks.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, { TypeI32 }))
        .tag(0)
        .function(1,
                  {},
                  code({ { 0x02, I32Result },
                         tryTable(NoResult, ClauseCatch, 0, 0),
                         { 0x02, ExnRefResult },
                         tryTable(NoResult, ClauseCatchAllRef, 0, 0),
                         i32Const(7),
                         throwTag(0),
                         { 0x0B, 0x00, 0x0B },
                         { 0x0A },
                         { 0x0B, 0x00, 0x0B } }));
    // A store of one exception: throw_ref allocates none, so one invocation fits; the next one's throw
    // finds the arena full, and its sweep reclaims the first invocation's exception, which nothing
    // refers to any more.
    auto harness = Harness { builder, {}, StoreLimits { .maxExceptions = 1 } };
    CHECK(harness.result(0) == i32(7));
    CHECK(harness.result(0) == i32(7));
    CHECK(harness.store().liveExceptions() == 1);
}

TEST_CASE("Interpreter.throwRefOfNullTraps")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).function(0, {}, { 0xD0, 0x69, 0x0A });
    auto harness = Harness { builder };
    CHECK(harness.trap(0) == TrapCode::NullExceptionReference);
    CHECK(isoline::runtime::specMessage(harness.invoke(0).error()) == "null exception reference");
}

TEST_CASE("Interpreter.unwindDeepFrames")
{
    // Function 0 counts its i32 down in nested calls and throws at 0; function 1 calls it with 10'000
    // inside try_table (catch_all 1) and returns 1 once it catches: 10'001 frames unwound in a loop,
    // with no host recursion (ASan would see a deep host stack).
    constexpr auto Depth = 10'000;
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({ TypeI32 }, {}))
        .type(signature({}, { TypeI32 }))
        .tag(0)
        .function(1,
                  {},
                  code({ localGet(0),
                         { 0x45, 0x04, NoResult },
                         throwTag(0),
                         { 0x0B },
                         localGet(0),
                         i32Const(1),
                         { 0x6B },
                         call(0) }))
        .function(2,
                  {},
                  code({ { 0x02, NoResult },
                         tryTable(NoResult, ClauseCatchAll, 0, 0),
                         i32Const(Depth),
                         call(0),
                         { 0x0B },
                         i32Const(0),
                         { 0x0F, 0x0B },
                         i32Const(1) }));
    auto harness = Harness { builder, {}, 2 * Depth };
    CHECK(harness.result(1) == i32(1));
    // Every frame unwound gave its depth back: the deepest call fits again.
    CHECK(harness.result(1) == i32(1));
}

TEST_CASE("Interpreter.exceptionCapTraps")
{
    // Each call throws and catches with catch_all_ref, keeping the exnref in table slot i: a store of
    // 16 exceptions holds 16, and the 17th throw traps.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({ TypeI32 }, {}))
        .table(TableType { .element = isoline::binary::ExnRef, .limits = atLeast(32) })
        .tag(0)
        .function(1,
                  {},
                  code({ localGet(0),
                         { 0x02, ExnRefResult },
                         tryTable(NoResult, ClauseCatchAllRef, 0, 0),
                         throwTag(0),
                         { 0x0B, 0x00, 0x0B },
                         { 0x26, 0x00 } })); // table.set 0
    auto harness = Harness { builder, {}, StoreLimits { .maxExceptions = 16 } };
    for (auto const index: std::views::iota(0, 16))
        REQUIRE(harness.invoke(0, { i32(index) }).has_value());
    CHECK(harness.trap(0, { i32(16) }) == TrapCode::ExceptionLimitExceeded);
    // The slots hold the 16 exceptions, all distinct.
    auto const& table = harness.store().table(harness.store().moduleInstance(harness.address()).tables.at(0));
    CHECK(table.elements[0].kind == isoline::runtime::RefKind::Exn);
    CHECK(table.elements[0].address != table.elements[15].address);
    CHECK(table.elements[16].kind == isoline::runtime::RefKind::Null);
}

TEST_CASE("Interpreter.exceptionCrossesHost")
{
    // Function 2 calls the host import 0 inside try_table (catch $e 0); the host invokes function 1,
    // which throws $e 5 uncaught there, and returns the Thrown: the exception goes on in function 2.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({ TypeI32 }, {}))
        .type(signature({}, { TypeI32 }))
        .importFunction("env", "host", 0)
        .tag(1)
        .function(0, {}, code({ i32Const(5), throwTag(0) }))
        .function(2,
                  {},
                  code({ { 0x02, I32Result },
                         tryTable(NoResult, ClauseCatch, 0, 0),
                         call(0),
                         { 0x0B },
                         i32Const(0),
                         { 0x0B } }));
    Harness* self = nullptr;
    auto ownException = false;
    auto harness =
        Harness { builder, [&](Store& store) {
                     auto const host = addHost(
                         store,
                         signature({}, {}),
                         [&](HostContext& context,
                             std::span<Value const>,
                             std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                             if (!ownException)
                                 return context.invoke(self->function(1), {}, {});
                             // An exception of the host's own making, of the guest's tag. The embedder
                             // calls it directly too, without a caller instance.
                             auto const tag = context.store().moduleInstance(self->address()).tags.at(0);
                             return context.addException(tag, { i32(9) }).and_then([](auto exception) {
                                 return std::expected<void, Abrupt> { std::unexpected {
                                     Abrupt { Thrown { .exception = exception } } } };
                             });
                         });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    self = &harness;
    CHECK(harness.result(2) == i32(5));
    ownException = true;
    CHECK(harness.result(2) == i32(9));
    // Uncaught by the guest too, it ends the invocation, through the host, with the same exception.
    auto const thrown = thrownOf(harness.outcome(0));
    CHECK(harness.store().exception(thrown.exception).payload == std::vector { i32(9) });
}

TEST_CASE("Interpreter.uncaughtIsThrown")
{
    // throw $e 3, with no handler: the invocation ends with Thrown, whose exception carries the tag
    // and payload. A try_table left by its end, or by a branch, no longer catches.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, {}))
        .tag(0)
        .function(1, {}, code({ i32Const(3), throwTag(0) }))
        .function(
            1, {}, code({ tryTable(NoResult, ClauseCatchAll, 0, 0), { 0x0B }, i32Const(4), throwTag(0) }))
        .function(1,
                  {},
                  code({ tryTable(NoResult, ClauseCatchAll, 0, 0),
                         { 0x0C, 0x00, 0x0B },
                         i32Const(5),
                         throwTag(0) }));
    auto harness = Harness { builder };
    auto const tag = harness.store().moduleInstance(harness.address()).tags.at(0);
    for (auto const& [function, payload]:
         std::to_array<std::pair<uint32_t, int32_t>>({ { 0, 3 }, { 1, 4 }, { 2, 5 } }))
    {
        CAPTURE(function);
        auto const thrown = thrownOf(harness.outcome(function));
        CHECK(harness.store().exception(thrown.exception).tag == tag);
        CHECK(harness.store().exception(thrown.exception).payload == std::vector { i32(payload) });
    }
}

TEST_CASE("Interpreter.importedTagIsTheSameTag")
{
    // Tag identity is the store's address: function 0 throws the imported tag and catches it by that
    // import; function 1 throws it too, but catches only tag 1, a tag of its own of the same type.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).type(signature({}, { TypeI32 })).importTag("m", "e", 0).tag(0);
    for (auto const caught: { 0U, 1U })
        builder.function(1,
                         {},
                         code({ { 0x02, NoResult },
                                tryTable(NoResult, ClauseCatch, caught, 0),
                                throwTag(0),
                                { 0x0B },
                                i32Const(0),
                                { 0x0F, 0x0B },
                                i32Const(1) }));
    auto shared = isoline::runtime::TagAddr {};
    auto harness =
        Harness { builder, [&](Store& store) {
                     // A decoy first, so that no TagAddr equals the tag index naming it.
                     static_cast<void>(store.addTag(store.internType(signature({}, {}))).value());
                     shared = store.addTag(store.internType(signature({}, {}))).value();
                     return std::vector { ExternValue { .kind = ExternKind::Tag, .address = shared } };
                 } };
    CHECK(harness.result(0) == i32(1));
    auto const thrown = thrownOf(harness.outcome(1));
    CHECK(harness.store().exception(thrown.exception).tag == shared);
}

TEST_CASE("Interpreter.catchToALoop")
{
    // loop try_table (catch_all 0) local 0 += 1; if local 0 < 3 throw end end end local.get 0: the
    // clause's label is the loop, so a catch repeats it, until a pass throws no more.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({}, { TypeI32 }))
        .tag(0)
        .function(1,
                  { TypeI32 },
                  code({ { 0x03, NoResult },
                         tryTable(NoResult, ClauseCatchAll, 0, 0),
                         localGet(0),
                         i32Const(1),
                         { 0x6A, 0x22, 0x00 }, // i32.add local.tee 0
                         i32Const(3),
                         { 0x49, 0x04, NoResult }, // i32.lt_u if
                         throwTag(0),
                         { 0x0B, 0x0B, 0x0B },
                         localGet(0) }));
    auto harness = Harness { builder };
    CHECK(harness.result(0) == i32(3));
}

TEST_CASE("Interpreter.catchToTheFunctionLabel")
{
    // try_table (catch $e 0) i32.const 11 throw $e end i32.const 0: label 0 outside the try_table is
    // the function's, so the catch returns the payload; function 1 adds it to an operand of its own.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, { TypeI32 }))
        .tag(0)
        .function(
            1,
            {},
            code({ tryTable(NoResult, ClauseCatch, 0, 0), i32Const(11), throwTag(0), { 0x0B }, i32Const(0) }))
        .function(1, {}, code({ i32Const(100), call(0), { 0x6A } }));
    auto harness = Harness { builder };
    CHECK(harness.result(0) == i32(11));
    CHECK(harness.result(1) == i32(111));
}

TEST_CASE("Interpreter.hostThrowsAPayloadOfItsTagsKinds")
{
    // The host throws the guest's tag [i64 f32] with a payload of those kinds (Store::addException
    // checks them); the guest's catch hands both to a block of [i64 f32] (type 2) and keeps the i64.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({ TypeI64, TypeF32 }, {}))
        .type(signature({}, { TypeI64, TypeF32 }))
        .type(signature({}, { TypeI64 }))
        .importFunction("env", "host", 0)
        .tag(1)
        .function(3,
                  {},
                  code({ { 0x02, 0x02 },
                         tryTable(NoResult, ClauseCatch, 0, 0),
                         call(0),
                         { 0x0B, 0x00, 0x0B, 0x1A } }));
    auto harness =
        Harness { builder, [](Store& store) {
                     auto const host =
                         addHost(store,
                                 signature({}, {}),
                                 [](HostContext& context,
                                    std::span<Value const>,
                                    std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                     auto const tag =
                                         context.store().moduleInstance(*context.callerInstance()).tags.at(0);
                                     return context.addException(tag, { I64 { 42 }, F32 { 0x3F80'0000 } })
                                         .and_then([](auto exception) {
                                             return std::expected<void, Abrupt> { std::unexpected {
                                                 Abrupt { Thrown { .exception = exception } } } };
                                         });
                                 });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    CHECK(harness.result(1) == Value { I64 { 42 } });
}

namespace
{

/// A host import of type [] -> [] that runs @p function, added to @p store.
/// @return The imports binding it.
template <isoline::runtime::HostCallable F>
std::vector<ExternValue> hostImport(Store& store, F function)
{
    return { ExternValue { .kind = ExternKind::Func,
                           .address = addHost(store, signature({}, {}), std::move(function)) } };
}

/// The rounds a guest that a test interrupts runs at most: should its poll go missing, it ends
/// normally, which fails the test, rather than hanging it.
constexpr auto MaxRounds = int32_t { 1 } << 24;

/// A loop of MaxRounds rounds, counted down in local 0, an i32 the function declares: spins until
/// interrupted.
Bytes spin()
{
    return code({ i32Const(MaxRounds),
                  { 0x21, 0x00 }, // local.set 0
                  isoline::binary::testing::loop(code({ localGet(0),
                                                        i32Const(1),
                                                        { 0x6B, 0x22, 0x00 }, // i32.sub; local.tee 0
                                                        isoline::binary::testing::brIf(0) })) });
}

/// A try_table of @p clauses clauses that catch tag 1, around a throw of tag 0, which none catches.
Bytes uncaughtPast(uint32_t clauses)
{
    auto body = code({ { 0x1F, 0x40 }, encodeU32(clauses) });
    for ([[maybe_unused]] auto const clause: std::views::iota(uint32_t { 0 }, clauses))
        body.insert(body.end(), { 0x00, 0x01, 0x00 }); // catch 1 0
    body.insert(body.end(), { 0x08, 0x00, 0x0B });     // throw 0; end
    return body;
}

} // namespace

TEST_CASE("ReferenceInterpreter.interruptIsStickyBeforeEntry")
{
    // export "work" first calls host "entered", then returns 1.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({}, { TypeI32 }))
        .importFunction("env", "entered", 0)
        .function(1, {}, code({ call(0), i32Const(1) }));
    auto entered = 0;
    auto harness =
        Harness { builder, [&](Store& store) {
                     return hostImport(store,
                                       [&](HostContext&,
                                           std::span<Value const>,
                                           std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                           ++entered;
                                           return {};
                                       });
                 } };
    harness.store().interrupter()->interrupt();
    CHECK(harness.trap(1) == TrapCode::Interrupted);
    CHECK(harness.trap(1) == TrapCode::Interrupted);
    // The host import invoked directly does not run either.
    CHECK(harness.trap(0) == TrapCode::Interrupted);
    CHECK(entered == 0);
}

TEST_CASE("ReferenceInterpreter.interruptStopsBulkFillWithinOneGranule")
{
    // A 1024-page memory, filled whole: 64 MiB, 1024 granules.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .memory(atLeast(1024))
        .function(0, {}, code({ i32Const(0), i32Const(0xAB), i32Const(64 << 20), memoryFill() }));
    auto counter = PollCounter { PollTrigger { .site = PollSite::BulkGranule, .ordinal = 3 } };
    auto harness = Harness { builder, {}, StoreLimits {}, &counter };
    counter.interruptAtTrigger(harness.store().interrupter());
    CHECK(harness.trap(0) == TrapCode::Interrupted);
    CHECK(counter.pollsAfterTrigger() == 0);
    CHECK(counter.count(PollSite::BulkGranule) == 3);
    // The two granules before the third poll were filled, and nothing after them.
    auto const bytes =
        harness.store().memory(harness.store().moduleInstance(harness.address()).memories.at(0)).bytes();
    CHECK(bytes[(2 * isoline::runtime::BulkPollBytes) - 1] == std::byte { 0xAB });
    CHECK(bytes[2 * isoline::runtime::BulkPollBytes] == std::byte { 0 });
}

TEST_CASE("ReferenceInterpreter.copyGranulesRunBackwardsAboveTheSource")
{
    // memory.copy of three granules one granule up, within one memory: the last granule is copied
    // first, so an interrupt after it leaves exactly that granule copied. Only the source's last
    // granule holds 0xAB, which tells the two orders apart.
    constexpr auto Granule = static_cast<int32_t>(isoline::runtime::BulkPollBytes);
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .memory(atLeast(4))
        .function(0,
                  {},
                  code({ i32Const(2 * Granule),
                         i32Const(0xAB),
                         i32Const(Granule),
                         memoryFill(),
                         i32Const(Granule),
                         i32Const(0),
                         i32Const(3 * Granule),
                         { 0xFC, 0x0A, 0x00, 0x00 } })); // memory.copy
    // The fill polls once, the copy before each of its granules: interrupted before its second.
    auto counter = PollCounter { PollTrigger { .site = PollSite::BulkGranule, .ordinal = 3 } };
    auto harness = Harness { builder, {}, StoreLimits {}, &counter };
    counter.interruptAtTrigger(harness.store().interrupter());
    CHECK(harness.trap(0) == TrapCode::Interrupted);
    auto const bytes =
        harness.store().memory(harness.store().moduleInstance(harness.address()).memories.at(0)).bytes();
    constexpr auto GranuleBytes = isoline::runtime::BulkPollBytes;
    CHECK(bytes[3 * GranuleBytes] == std::byte { 0xAB });
    CHECK(bytes[(4 * GranuleBytes) - 1] == std::byte { 0xAB });
    CHECK(bytes[GranuleBytes] == std::byte { 0 });
}

TEST_CASE("ReferenceInterpreter.copyGranulesRunBackwardsWheneverTheDestinationIsAbove")
{
    // The order is the addresses' alone (runtime::copyOrder()), not whether the ranges overlap: a copy
    // of two granules to four granules up runs backwards too, within one memory and from memory 1 into
    // memory 0. Only the source's second granule holds 0xAB, so an interrupt before the copy's second
    // granule leaves 0xAB at the destination's second granule backwards, and nothing forwards.
    constexpr auto Granule = static_cast<int32_t>(isoline::runtime::BulkPollBytes);
    constexpr auto GranuleBytes = isoline::runtime::BulkPollBytes;
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .memory(atLeast(8))
        .memory(atLeast(8))
        .function(0,
                  {},
                  code({ i32Const(Granule),
                         i32Const(0xAB),
                         i32Const(Granule),
                         memoryFill(),
                         i32Const(4 * Granule),
                         i32Const(0),
                         i32Const(2 * Granule),
                         memoryCopy(0, 0) }))
        .function(0,
                  {},
                  code({ i32Const(Granule),
                         i32Const(0xAB),
                         i32Const(Granule),
                         { 0xFC, 0x0B, 0x01 }, // memory.fill of memory 1
                         i32Const(4 * Granule),
                         i32Const(0),
                         i32Const(2 * Granule),
                         memoryCopy(0, 1) }));
    for (auto const function: { 0U, 1U })
    {
        CAPTURE(function);
        // The fill polls once, the copy before each of its granules: interrupted before its second.
        auto counter = PollCounter { PollTrigger { .site = PollSite::BulkGranule, .ordinal = 3 } };
        auto harness = Harness { builder, {}, StoreLimits {}, &counter };
        counter.interruptAtTrigger(harness.store().interrupter());
        CHECK(harness.trap(function) == TrapCode::Interrupted);
        auto const bytes =
            harness.store().memory(harness.store().moduleInstance(harness.address()).memories.at(0)).bytes();
        CHECK(bytes[5 * GranuleBytes] == std::byte { 0xAB });
        CHECK(bytes[(6 * GranuleBytes) - 1] == std::byte { 0xAB });
        CHECK(bytes[4 * GranuleBytes] == std::byte { 0 });
    }
}

TEST_CASE("ReferenceInterpreter.pollsAtEveryBackEdgeAndEntry")
{
    // Each row runs 10 rounds, each calling $empty (function 0), directly or through table 0 or a
    // function reference.
    auto const init = code({ i32Const(10), { 0x21, 0x00 } });                        // local.set 0
    auto const countdown = code({ localGet(0), i32Const(1), { 0x6B, 0x22, 0x00 } }); // i32.sub; local.tee 0
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({ FuncRef }, {}))
        .type(signature({ TypeI32 }, {}))
        .tag(0)
        .table(TableType { .element = FuncRef, .limits = atLeast(2) })
        .element(activeElements(0, { 0x00, 0x09 })) // table 0: $empty, $selfIndirect
        .element({ 0x03, 0x00, 0x02, 0x00, 0x0A })  // declares $empty and $selfRef for ref.func
        .function(0, {}, {})
        // br_if back to the loop while the count is not 0.
        .function(
            0,
            { TypeI32 },
            code({ init, { 0x03, 0x40 }, call(0), countdown, isoline::binary::testing::brIf(0), { 0x0B } }))
        // br_table: the loop while the count is not 0, else out of the block around it.
        .function(0,
                  { TypeI32 },
                  code({ init,
                         { 0x02, 0x40, 0x03, 0x40 },
                         call(0),
                         countdown,
                         { 0x45 },                   // i32.eqz
                         { 0x0E, 0x01, 0x00, 0x01 }, // br_table 0 1
                         { 0x0B, 0x0B } }))
        // br_on_non_null: a loop of type [funcref] -> [], re-entered with ref.func 0 while the count
        // is not 0.
        .function(0,
                  { TypeI32 },
                  code({ init,
                         { 0xD0, 0x70 }, // ref.null func
                         { 0x03, 0x01 }, // loop (type 1)
                         { 0x1A },       // drop
                         call(0),
                         countdown,
                         { 0x04, 0x70, 0xD2, 0x00, 0x05, 0xD0, 0x70, 0x0B }, // if: ref.func 0 else ref.null
                         { 0xD6, 0x00 },                                     // br_on_non_null 0
                         { 0x0B } }))
        // try_table (catch_all $loop): a throw while the count is not 0.
        .function(0,
                  { TypeI32 },
                  code({ init,
                         { 0x03, 0x40 },
                         { 0x1F, 0x40, 0x01, 0x02, 0x00 }, // try_table (catch_all 0)
                         call(0),
                         countdown,
                         { 0x04, 0x40, 0x08, 0x00, 0x0B }, // if: throw 0
                         { 0x0B, 0x0B } }))
        // $self(n): call $empty; return_call $self(n - 1) while n > 1.
        .function(2,
                  {},
                  code({ call(0),
                         localGet(0),
                         i32Const(1),
                         { 0x4B, 0x04, 0x40 }, // i32.gt_u; if
                         localGet(0),
                         i32Const(1),
                         { 0x6B, 0x12, 0x05 }, // i32.sub; return_call 5
                         { 0x0B } }))
        // br_on_null back to the loop with null while the count is not 0.
        .function(0,
                  { TypeI32 },
                  code({ init,
                         { 0x03, 0x40 },
                         call(0),
                         countdown,
                         { 0x04, 0x70, 0xD0, 0x70, 0x05, 0xD2, 0x00, 0x0B }, // if: ref.null else ref.func 0
                         { 0xD5, 0x00, 0x1A },                               // br_on_null 0; drop
                         { 0x0B } }))
        // As br_if's row, calling $empty through call_indirect.
        .function(0,
                  { TypeI32 },
                  code({ init,
                         { 0x03, 0x40 },
                         i32Const(0),
                         { 0x11, 0x00, 0x00 }, // call_indirect (type 0) 0
                         countdown,
                         isoline::binary::testing::brIf(0),
                         { 0x0B } }))
        // As br_if's row, calling $empty through call_ref.
        .function(0,
                  { TypeI32 },
                  code({ init,
                         { 0x03, 0x40 },
                         { 0xD2, 0x00, 0x14, 0x00 }, // ref.func 0; call_ref (type 0)
                         countdown,
                         isoline::binary::testing::brIf(0),
                         { 0x0B } }))
        // $selfIndirect(n): as $self, through return_call_indirect of table element 1.
        .function(2,
                  {},
                  code({ call(0),
                         localGet(0),
                         i32Const(1),
                         { 0x4B, 0x04, 0x40 },
                         localGet(0),
                         i32Const(1),
                         { 0x6B },
                         i32Const(1),
                         { 0x13, 0x02, 0x00 }, // return_call_indirect (type 2) 0
                         { 0x0B } }))
        // $selfRef(n): as $self, through return_call_ref.
        .function(2,
                  {},
                  code({ call(0),
                         localGet(0),
                         i32Const(1),
                         { 0x4B, 0x04, 0x40 },
                         localGet(0),
                         i32Const(1),
                         { 0x6B, 0xD2, 0x0A, 0x15, 0x02 }, // i32.sub; ref.func 10; return_call_ref (type 2)
                         { 0x0B } }));

    struct Row
    {
        uint32_t function;
        std::vector<Value> args;
        uint64_t backEdges;
        uint64_t entries;
    };
    auto const rows = std::vector<Row> {
        { .function = 1, .args = {}, .backEdges = 9, .entries = 1 + 10 },
        { .function = 2, .args = {}, .backEdges = 9, .entries = 1 + 10 },
        { .function = 3, .args = {}, .backEdges = 9, .entries = 1 + 10 },
        { .function = 4, .args = {}, .backEdges = 9, .entries = 1 + 10 },
        { .function = 5, .args = { i32(10) }, .backEdges = 0, .entries = 1 + 10 + 9 },
        { .function = 6, .args = {}, .backEdges = 9, .entries = 1 + 10 },
        { .function = 7, .args = {}, .backEdges = 9, .entries = 1 + 10 },
        { .function = 8, .args = {}, .backEdges = 9, .entries = 1 + 10 },
        { .function = 9, .args = { i32(10) }, .backEdges = 0, .entries = 1 + 10 + 9 },
        { .function = 10, .args = { i32(10) }, .backEdges = 0, .entries = 1 + 10 + 9 },
    };
    for (auto const& row: rows)
    {
        INFO("function " << row.function);
        auto counter = PollCounter {};
        auto harness = Harness { builder, {}, StoreLimits {}, &counter };
        CHECK(harness.outcome(row.function, row.args).has_value());
        CHECK(counter.count(PollSite::LoopBackEdge) == row.backEdges);
        CHECK(counter.count(PollSite::FunctionEntry) == row.entries);
        // A step per poll, so a budget of exactly that many steps runs the call, and one fewer traps.
        auto const steps = row.backEdges + row.entries;
        CHECK(harness.store().used(Limit::Steps) == steps);
        CHECK(Harness { builder, {}, StoreLimits { .maxSteps = steps } }.outcome(row.function, row.args));
        CHECK(Harness { builder, {}, StoreLimits { .maxSteps = steps - 1 } }.trap(row.function, row.args)
              == TrapCode::StepLimitExceeded);
    }
}

TEST_CASE("ReferenceInterpreter.interruptStopsTailRecursion", "[threading]")
{
    // $f(n): return_call $f(n - 1) while n is not 0: no back-edge at all, interrupted from another
    // thread long before n runs out.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .function(0,
                  {},
                  code({ localGet(0),
                         { 0x04, 0x40 }, // if
                         localGet(0),
                         i32Const(1),
                         { 0x6B, 0x12, 0x00 }, // i32.sub; return_call 0
                         { 0x0B } }));
    auto counter = PollCounter { PollTrigger { .site = PollSite::FunctionEntry, .ordinal = 2 } };
    auto harness = Harness { builder, {}, StoreLimits {}, &counter };
    auto watchdog = InterruptWatchdog { counter, harness.store().interrupter() };
    CHECK(harness.trap(0, { i32(MaxRounds) }) == TrapCode::Interrupted);
    CHECK(watchdog.finish() == TriggerWait::Triggered);
}

TEST_CASE("ReferenceInterpreter.interruptFromAnotherThread", "[threading]")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).function(0, { TypeI32 }, spin());
    auto counter = PollCounter { PollTrigger { .site = PollSite::LoopBackEdge, .ordinal = 1 } };
    auto harness = Harness { builder, {}, StoreLimits {}, &counter };
    auto watchdog = InterruptWatchdog { counter, harness.store().interrupter() };
    CHECK(harness.trap(0) == TrapCode::Interrupted);
    CHECK(watchdog.finish() == TriggerWait::Triggered);
    // Sticky: the next call runs nothing.
    CHECK(harness.trap(0) == TrapCode::Interrupted);
}

TEST_CASE("ReferenceInterpreter.interruptReplacesTheHostOutcome")
{
    // The host raises the interrupt and returns: whatever it returns, the call ends Interrupted.
    enum class Ending : uint8_t
    {
        Success,
        Failure,
        Throw,
    };
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).tag(0).importFunction("env", "interrupts", 0).function(0, {}, call(0));
    for (auto const ending: { Ending::Success, Ending::Failure, Ending::Throw })
    {
        INFO("ending " << static_cast<int>(ending));
        Harness* self = nullptr;
        auto harness = Harness { builder, [&](Store& store) {
                                    return hostImport(
                                        store,
                                        [&](HostContext& context,
                                            std::span<Value const>,
                                            std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                            self->store().interrupter()->interrupt();
                                            switch (ending)
                                            {
                                                case Ending::Success: return {};
                                                case Ending::Failure:
                                                    return std::unexpected { Abrupt {
                                                        HostFailure { .kind = HostFailureKind::Embedder,
                                                                      .code = 1,
                                                                      .mismatch = std::nullopt } } };
                                                case Ending::Throw: break;
                                            }
                                            auto const tag =
                                                context.store().moduleInstance(self->address()).tags.at(0);
                                            return context.addException(tag, {}).and_then([](auto exception) {
                                                return std::expected<void, Abrupt> { std::unexpected {
                                                    Abrupt { Thrown { .exception = exception } } } };
                                            });
                                        });
                                } };
        self = &harness;
        CHECK(harness.trap(1) == TrapCode::Interrupted);
    }
}

TEST_CASE("ReferenceInterpreter.absorbedNestedInterruptStillEndsTheCall")
{
    // The host invokes a spinning function, which is interrupted at its first back-edge; the host
    // swallows the Interrupted and returns success, and the outer call still ends Interrupted.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .importFunction("env", "absorbs", 0)
        .function(0, {}, call(0))
        .function(0, { TypeI32 }, spin());
    auto counter = PollCounter { PollTrigger { .site = PollSite::LoopBackEdge, .ordinal = 1 } };
    auto nested = std::expected<void, Abrupt> {};
    Harness* self = nullptr;
    auto harness = Harness {
        builder,
        [&](Store& store) {
            return hostImport(store,
                              [&](HostContext& context,
                                  std::span<Value const>,
                                  std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                  nested = context.invoke(self->function(2), {}, {});
                                  return {};
                              });
        },
        StoreLimits {},
        &counter,
    };
    self = &harness;
    counter.interruptAtTrigger(harness.store().interrupter());
    CHECK(harness.trap(1) == TrapCode::Interrupted);
    CHECK(nested == std::unexpected { Abrupt { Trap { TrapCode::Interrupted } } });
    CHECK(counter.count(PollSite::HostReturn) == 1);
}

TEST_CASE("ReferenceInterpreter.pollsWhileUnwinding")
{
    // A throw past one try_table of 3 x 4096 clauses that do not catch it: 12289 labels and clauses
    // scanned, so 3 polls at Unwind.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .tag(0)
        .tag(0)
        .function(0, {}, uncaughtPast(3 * isoline::runtime::UnwindPollItems));
    auto counter = PollCounter {};
    auto harness = Harness { builder, {}, StoreLimits {}, &counter };
    CHECK(std::holds_alternative<Thrown>(harness.abrupt(0)));
    CHECK(counter.count(PollSite::Unwind) == 3);
    // Unwinding charges no step: the call's one step is its entry.
    CHECK(counter.count(PollSite::FunctionEntry) == 1);
    CHECK(harness.store().used(Limit::Steps) == 1);
}

TEST_CASE("ReferenceInterpreter.interruptStopsALongUnwind")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .tag(0)
        .tag(0)
        .function(0, {}, uncaughtPast(3 * isoline::runtime::UnwindPollItems));
    auto counter = PollCounter { PollTrigger { .site = PollSite::Unwind, .ordinal = 2 } };
    auto harness = Harness { builder, {}, StoreLimits {}, &counter };
    counter.interruptAtTrigger(harness.store().interrupter());
    CHECK(harness.trap(0) == TrapCode::Interrupted);
    CHECK(counter.pollsAfterTrigger() == 0);
}

TEST_CASE("ReferenceInterpreter.interruptStopsUnwindingPastLabels")
{
    // A throw from inside 4096 nested blocks: the 4096th label unwound past polls, and is interrupted.
    constexpr auto Depth = static_cast<int>(isoline::runtime::UnwindPollItems);
    auto body = Bytes {};
    for ([[maybe_unused]] auto const level: std::views::iota(0, Depth))
        body.insert(body.end(), { 0x02, 0x40 }); // block
    body.insert(body.end(), { 0x08, 0x00 });     // throw 0
    for ([[maybe_unused]] auto const level: std::views::iota(0, Depth))
        body.push_back(0x0B); // end
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).tag(0).function(0, {}, body);
    auto counter = PollCounter { PollTrigger { .site = PollSite::Unwind, .ordinal = 1 } };
    auto harness = Harness { builder, {}, StoreLimits {}, &counter };
    counter.interruptAtTrigger(harness.store().interrupter());
    CHECK(harness.trap(0) == TrapCode::Interrupted);
    CHECK(counter.count(PollSite::Unwind) == 1);
}

TEST_CASE("ReferenceInterpreter.tableOperationsPollPerElementGranule")
{
    // A table of 4 granules of BulkPollElements: $empty at element 0 and at the last element of the
    // first granule, null elsewhere.
    constexpr auto Granule = static_cast<int32_t>(isoline::runtime::BulkPollElements);
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .table(TableType { .element = FuncRef, .limits = atLeast(4 * isoline::runtime::BulkPollElements) })
        .element(activeElements(0, { 0x00 }))
        .element(code({ { 0x00 }, i32Const(Granule - 1), { 0x0B, 0x01, 0x00 } }))
        .function(0, {}, {})
        // table.copy one element up, of two granules: overlapping within each granule and across them.
        .function(
            0, {}, code({ i32Const(1), i32Const(0), i32Const(2 * Granule), { 0xFC, 0x0E, 0x00, 0x00 } }))
        // table.fill of two granules and one element.
        .function(
            0,
            {},
            code({ i32Const(Granule), { 0xD0, 0x70 }, i32Const((2 * Granule) + 1), { 0xFC, 0x11, 0x00 } }));
    auto counter = PollCounter {};
    auto harness = Harness { builder, {}, StoreLimits {}, &counter };
    auto const& elements =
        harness.store().table(harness.store().moduleInstance(harness.address()).tables.at(0)).elements;
    auto const isFunction = [&](int32_t index) {
        return elements.at(static_cast<std::size_t>(index)).kind == isoline::runtime::RefKind::Func;
    };

    auto const beforeCopy = counter.count(PollSite::BulkGranule);
    REQUIRE(harness.outcome(1).has_value());
    CHECK(counter.count(PollSite::BulkGranule) - beforeCopy == 2);
    // As if copied at once: each element holds what the one below it held. Within a granule the copy
    // runs backwards (element 2 is not element 0's), and the upper granule is copied first (element
    // Granule + 1 is not element Granule - 1's).
    CHECK(isFunction(1));
    CHECK_FALSE(isFunction(2));
    CHECK(isFunction(Granule));
    CHECK_FALSE(isFunction(Granule + 1));

    auto const beforeFill = counter.count(PollSite::BulkGranule);
    REQUIRE(harness.outcome(2).has_value());
    CHECK(counter.count(PollSite::BulkGranule) - beforeFill == 3);
    CHECK_FALSE(isFunction(Granule));
}

TEST_CASE("ReferenceInterpreter.tableCopyOntoItself")
{
    // A table.copy whose source and destination are the same range changes nothing, and still polls.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .table(TableType { .element = FuncRef, .limits = atLeast(4) })
        .element(activeElements(0, { 0x00, 0x00 }))
        .function(0, {}, code({ i32Const(0), i32Const(0), i32Const(4), { 0xFC, 0x0E, 0x00, 0x00 } }));
    auto counter = PollCounter {};
    auto harness = Harness { builder, {}, StoreLimits {}, &counter };
    auto const before = counter.count(PollSite::BulkGranule);
    REQUIRE(harness.outcome(0).has_value());
    CHECK(counter.count(PollSite::BulkGranule) - before == 1);
    auto const& elements =
        harness.store().table(harness.store().moduleInstance(harness.address()).tables.at(0)).elements;
    CHECK(elements[1].kind == isoline::runtime::RefKind::Func);
    CHECK(elements[2].kind == isoline::runtime::RefKind::Null);
}

TEST_CASE("ReferenceInterpreter.interruptStopsUnwindingPastFrames")
{
    // A throw from inside 4095 nested blocks of a callee: its frame, unwound past after them, is the
    // 4096th item scanned, polls, and is interrupted.
    constexpr auto Depth = static_cast<int>(isoline::runtime::UnwindPollItems) - 1;
    auto body = Bytes {};
    for ([[maybe_unused]] auto const level: std::views::iota(0, Depth))
        body.insert(body.end(), { 0x02, 0x40 }); // block
    body.insert(body.end(), { 0x08, 0x00 });     // throw 0
    for ([[maybe_unused]] auto const level: std::views::iota(0, Depth))
        body.push_back(0x0B); // end
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).tag(0).function(0, {}, call(1)).function(0, {}, body);
    auto counter = PollCounter { PollTrigger { .site = PollSite::Unwind, .ordinal = 1 } };
    auto harness = Harness { builder, {}, StoreLimits {}, &counter };
    counter.interruptAtTrigger(harness.store().interrupter());
    CHECK(harness.trap(0) == TrapCode::Interrupted);
    CHECK(counter.count(PollSite::Unwind) == 1);
}

namespace
{

/// global.get 0; i32.const 1; i32.add; global.set 0: counts in global 0.
Bytes countInGlobal()
{
    return code({ { 0x23, 0x00 }, i32Const(1), { 0x6A }, { 0x24, 0x00 } });
}

/// A module whose global 0, a mutable i32, counts the entries of function 0, of type 0 [] -> [] and
/// @p locals i32 locals, which counts and then calls itself.
ModuleBuilder countingRecursion(uint32_t locals)
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .global(GlobalType { .type = TypeI32, .mutability = Mutability::Var }, i32Const(0))
        .function(0, std::vector<ValueType>(locals, TypeI32), code({ countInGlobal(), call(0) }));
    return builder;
}

/// A countdown loop of 10 rounds in local 0, an i32 the function declares: 1 entry and 9 back-edges,
/// so 10 steps.
Bytes loop10()
{
    return code({ i32Const(10),
                  { 0x21, 0x00 }, // local.set 0
                  isoline::binary::testing::loop(code({ localGet(0),
                                                        i32Const(1),
                                                        { 0x6B, 0x22, 0x00 }, // i32.sub; local.tee 0
                                                        isoline::binary::testing::brIf(0) })) });
}

} // namespace

TEST_CASE("ReferenceInterpreter.stackLimitIsTheStaticFrameCost")
{
    // f has no params and 8 locals, and its body holds at most 2 operands and no labels: its frame
    // costs 0 + 8 + 2 + 0 + 1 = 11. 100 frames charge 1'100; the 101st would exceed that.
    auto const builder = countingRecursion(8);
    auto harness = Harness { builder, {}, StoreLimits { .maxStackValues = 1'100 } };
    CHECK(harness.validated().frameShape(0) == FrameShape { .maxOperands = 2, .maxLabels = 0 });
    CHECK(harness.validated().frameCost(0) == 11);
    CHECK(harness.trap(0) == TrapCode::CallStackExhausted);
    CHECK(harness.global(0) == i32(100));
    // Every frame's charge was given back.
    CHECK(harness.store().chargedStackValues() == 0);
}

TEST_CASE("ReferenceInterpreter.hostMediatedRecursionHitsTheSameCap")
{
    // f (cost 11, as above, and function 1 here) calls host h (cost 1), which invokes f through its
    // context and passes on whatever it ends with. Each round charges 12; with 1'000 invocations
    // allowed, only the stack cap binds: f's 101st entry would charge 99 x 12 + 11 + 1 + 11 = 1'211.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .importFunction("env", "h", 0)
        .global(GlobalType { .type = TypeI32, .mutability = Mutability::Var }, i32Const(0))
        .function(0, std::vector<ValueType>(8, TypeI32), code({ countInGlobal(), call(0) }));
    Harness* self = nullptr;
    auto harness = Harness {
        builder,
        [&](Store& store) {
            return hostImport(store,
                              [&](HostContext& context,
                                  std::span<Value const>,
                                  std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                  return context.invoke(self->function(1), {}, {});
                              });
        },
        StoreLimits { .maxStackValues = 1'200, .maxNestedInvocations = 1'000 },
    };
    self = &harness;
    CHECK(harness.trap(1) == TrapCode::CallStackExhausted);
    CHECK(harness.global(0) == i32(100));
    CHECK(harness.store().chargedStackValues() == 0);
    CHECK(harness.store().used(Limit::NestedInvocations) == 0);
}

TEST_CASE("ReferenceInterpreter.zeroCostFramesAreCharged")
{
    // (func $r (call $r)) holds nothing, and still costs 1: a budget of 50 traps entering the 51st.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).function(0, {}, call(0));
    auto counter = PollCounter {};
    auto harness = Harness { builder, {}, StoreLimits { .maxStackValues = 50 }, &counter };
    CHECK(harness.trap(0) == TrapCode::CallStackExhausted);
    CHECK(counter.count(PollSite::FunctionEntry) == 51);
    CHECK(harness.store().chargedStackValues() == 0);
}

TEST_CASE("ReferenceInterpreter.stepLimitIsExact")
{
    // 1 entry + 9 back-edges = 10 steps.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).function(0, { TypeI32 }, loop10());
    CHECK(Harness { builder, {}, StoreLimits { .maxSteps = 10 } }.invoke(0).has_value());
    CHECK(Harness { builder, {}, StoreLimits { .maxSteps = 9 } }.trap(0) == TrapCode::StepLimitExceeded);
}

TEST_CASE("ReferenceInterpreter.stepBudgetIsPerOutermostCall")
{
    // Function 1 is loop10, 10 steps; function 2 calls host h (function 0), which invokes function 1.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .importFunction("env", "h", 0)
        .function(0, { TypeI32 }, loop10())
        .function(0, {}, call(0));
    Harness* self = nullptr;
    auto const bind = [&](Store& store) {
        return hostImport(store,
                          [&](HostContext& context,
                              std::span<Value const>,
                              std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                              return context.invoke(self->function(1), {}, {});
                          });
    };

    // Two consecutive calls of 10 steps each both fit a budget of 10.
    auto harness = Harness { builder, bind, StoreLimits { .maxSteps = 10 } };
    self = &harness;
    CHECK(harness.invoke(1).has_value());
    CHECK(harness.invoke(1).has_value());

    // A host-nested call shares its caller's budget: function 2's entry, h's entry, the nested 10 and
    // h's return take 13 steps.
    CHECK(harness.trap(2) == TrapCode::StepLimitExceeded);
    auto fits = Harness { builder, bind, StoreLimits { .maxSteps = 13 } };
    self = &fits;
    CHECK(fits.invoke(2).has_value());
    CHECK(fits.store().used(Limit::Steps) == 13);
    auto tooFew = Harness { builder, bind, StoreLimits { .maxSteps = 12 } };
    self = &tooFew;
    CHECK(tooFew.trap(2) == TrapCode::StepLimitExceeded);
}

TEST_CASE("ReferenceInterpreter.callDepthCountsAcrossInvocations")
{
    // maxCallDepth counts the Wasm frames of every invocation in progress, not of one: function 1 calls
    // host h (function 0), which invokes function 2. Function 2 alone fits a depth of 1; nested in
    // function 1's frame it is the second, so a depth of 1 refuses it and a depth of 2 lets it run.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .importFunction("env", "h", 0)
        .function(0, {}, call(0))
        .function(0, {}, {});
    Harness* self = nullptr;
    auto const bind = [&](Store& store) {
        return hostImport(store,
                          [&](HostContext& context,
                              std::span<Value const>,
                              std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                              return context.invoke(self->function(2), {}, {});
                          });
    };
    auto shallow = Harness { builder, bind, StoreLimits { .maxCallDepth = 1 } };
    self = &shallow;
    CHECK(shallow.invoke(2).has_value());
    CHECK(shallow.trap(1) == TrapCode::CallStackExhausted);
    auto deep = Harness { builder, bind, StoreLimits { .maxCallDepth = 2 } };
    self = &deep;
    CHECK(deep.invoke(1).has_value());
}

TEST_CASE("ReferenceInterpreter.frameChargeReturnsToZero")
{
    // However an invocation ends, every frame's charge is given back, each in a store of its own.
    SECTION("call stack exhausted")
    {
        auto builder = ModuleBuilder {};
        builder.type(signature({}, {})).function(0, { TypeI64, TypeI64 }, call(0));
        auto harness = Harness { builder };
        CHECK(harness.trap(0) == TrapCode::CallStackExhausted);
        CHECK(harness.store().chargedStackValues() == 0);
    }
    SECTION("an uncaught throw from depth 50")
    {
        // f(n) = n == 0 ? throw : f(n - 1), entered with 49.
        auto builder = ModuleBuilder {};
        builder.type(signature({ TypeI32 }, {}))
            .tag(0)
            .function(0,
                      {},
                      code({ localGet(0),
                             { 0x45, 0x04, 0x40 }, // i32.eqz; if
                             i32Const(0),
                             { 0x08, 0x00 }, // throw 0
                             { 0x05 },       // else
                             localGet(0),
                             i32Const(1),
                             { 0x6B },
                             call(0),
                             { 0x0B } }));
        auto counter = PollCounter {};
        auto harness = Harness { builder, {}, StoreLimits {}, &counter };
        CHECK(std::holds_alternative<Thrown>(harness.abrupt(0, { i32(49) })));
        CHECK(counter.count(PollSite::FunctionEntry) == 50);
        CHECK(harness.store().chargedStackValues() == 0);
    }
    SECTION("interrupted")
    {
        auto builder = ModuleBuilder {};
        builder.type(signature({}, {})).function(0, {}, call(1)).function(0, { TypeI32 }, spin());
        auto counter = PollCounter { PollTrigger { .site = PollSite::LoopBackEdge, .ordinal = 5 } };
        auto harness = Harness { builder, {}, StoreLimits {}, &counter };
        counter.interruptAtTrigger(harness.store().interrupter());
        CHECK(harness.trap(0) == TrapCode::Interrupted);
        CHECK(harness.store().chargedStackValues() == 0);
    }
    SECTION("a host callee's trap")
    {
        auto builder = ModuleBuilder {};
        builder.type(signature({}, {})).importFunction("env", "h", 0).function(0, {}, call(0));
        auto harness = Harness { builder, [](Store& store) {
                                    return hostImport(
                                        store,
                                        [](HostContext&,
                                           std::span<Value const>,
                                           std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                            return std::unexpected { Abrupt { Trap { TrapCode::HostTrap } } };
                                        });
                                } };
        CHECK(harness.trap(1) == TrapCode::HostTrap);
        CHECK(harness.store().chargedStackValues() == 0);
    }
}

TEST_CASE("ReferenceInterpreter.hostFrameIsCharged")
{
    // h takes an i32 and an i64 and returns an i32: its frame costs 2 + 1 + 1 = 4, charged before it
    // runs, whether the embedder or Wasm (f, of cost 4: 1 param, 2 operands and itself) calls it.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32, TypeI64 }, { TypeI32 }))
        .type(signature({ TypeI32 }, { TypeI32 }))
        .importFunction("env", "h", 0)
        .function(1, {}, code({ localGet(0), i64Const(2), call(0) }));
    auto calls = 0;
    auto const bind = [&](Store& store) {
        return std::vector { ExternValue {
            .kind = ExternKind::Func,
            .address = addHost(store,
                               signature({ TypeI32, TypeI64 }, { TypeI32 }),
                               [&](HostContext&,
                                   std::span<Value const> args,
                                   std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
                                   ++calls;
                                   results[0] = args[0];
                                   return {};
                               }) } };
    };
    CHECK(isoline::runtime::hostFrameCost(signature({ TypeI32, TypeI64 }, { TypeI32 })) == 4);
    auto const args = std::vector<Value> { i32(7), I64 { 2 } };

    auto tight = Harness { builder, bind, StoreLimits { .maxStackValues = 3 } };
    CHECK(tight.trap(0, args) == TrapCode::CallStackExhausted);
    CHECK(calls == 0);
    auto exact = Harness { builder, bind, StoreLimits { .maxStackValues = 4 } };
    CHECK(exact.result(0, args) == i32(7));
    CHECK(calls == 1);

    // Called from f, the host frame comes on top of f's 4: 7 is too few, 8 enough.
    CHECK(Harness { builder, bind, StoreLimits { .maxStackValues = 7 } }.trap(1, { i32(5) })
          == TrapCode::CallStackExhausted);
    CHECK(calls == 1);
    auto enough = Harness { builder, bind, StoreLimits { .maxStackValues = 8 } };
    CHECK(enough.result(1, { i32(5) }) == i32(5));
    CHECK(enough.store().chargedStackValues() == 0);
}

TEST_CASE("ReferenceInterpreter.callPollsBeforeDepthAndStack")
{
    // f (function 0, cost 1) calls g (function 1, 8 locals: cost 9). At a callee entry the poll comes
    // first, then the depth, then the stack charge: a call refused for its stack or depth has cost its
    // step, and a spent budget traps before either.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .function(0, {}, call(1))
        .function(0, std::vector<ValueType>(8, TypeI32), {});

    SECTION("stack")
    {
        auto refused = Harness { builder, {}, StoreLimits { .maxStackValues = 9, .maxSteps = 2 } };
        CHECK(refused.trap(0) == TrapCode::CallStackExhausted);
        CHECK(refused.store().used(Limit::Steps) == 2);
        CHECK(Harness { builder, {}, StoreLimits { .maxStackValues = 9, .maxSteps = 1 } }.trap(0)
              == TrapCode::StepLimitExceeded);
        CHECK(Harness { builder, {}, StoreLimits { .maxStackValues = 10, .maxSteps = 2 } }.invoke(0));
    }
    SECTION("depth")
    {
        auto refused = Harness { builder, {}, StoreLimits { .maxCallDepth = 1, .maxSteps = 2 } };
        CHECK(refused.trap(0) == TrapCode::CallStackExhausted);
        CHECK(refused.store().used(Limit::Steps) == 2);
        CHECK(Harness { builder, {}, StoreLimits { .maxCallDepth = 1, .maxSteps = 1 } }.trap(0)
              == TrapCode::StepLimitExceeded);
    }
}

TEST_CASE("ReferenceInterpreter.spentBudgetReplacesTheHostOutcome")
{
    // A host function that traps, invoked directly: its entry and its return take 2 steps. With both
    // left, its trap is the outcome; with 1, the return's poll finds the budget spent, which replaces it.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).importFunction("env", "h", 0);
    auto calls = 0;
    auto const bind = [&](Store& store) {
        return hostImport(store,
                          [&](HostContext&,
                              std::span<Value const>,
                              std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                              ++calls;
                              return std::unexpected { Abrupt { Trap { TrapCode::HostTrap } } };
                          });
    };
    CHECK(Harness { builder, bind, StoreLimits { .maxSteps = 2 } }.trap(0) == TrapCode::HostTrap);
    CHECK(calls == 1);
    CHECK(Harness { builder, bind, StoreLimits { .maxSteps = 1 } }.trap(0) == TrapCode::StepLimitExceeded);
    CHECK(calls == 2);
}

TEST_CASE("ReferenceInterpreter.tailCallReleasesTheCallerFirst")
{
    // f and g each hold 4 locals and cost 5; f return_calls g. The caller's charge is given back before
    // the callee's is taken, so a cap of 5 holds them one after the other.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .function(0, std::vector<ValueType>(4, TypeI32), { 0x12, 0x01 }) // return_call 1
        .function(0, std::vector<ValueType>(4, TypeI32), {});
    auto harness = Harness { builder, {}, StoreLimits { .maxStackValues = 5 } };
    CHECK(harness.validated().frameCost(0) == 5);
    CHECK(harness.validated().frameCost(1) == 5);
    CHECK(harness.invoke(0).has_value());
    CHECK(harness.store().chargedStackValues() == 0);
}

TEST_CASE("ReferenceInterpreter.hostChangeOfTheFloatEnvironmentDoesNotLeak")
{
    // Host h (function 0) rounds upward and flushes subnormals, then re-enters f32.add (function 1)
    // with its own operands; function 2 calls h and then adds its arguments.
    constexpr auto One = uint32_t { 0x3F80'0000 };
    constexpr auto Tiny = uint32_t { 0x3080'0000 }; // 2^-30: 1 + Tiny rounds to 1, upward to the next float.
    constexpr auto Subnormal = uint32_t { 0x0000'0001 };
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({ TypeF32, TypeF32 }, { TypeF32 }))
        .importFunction("env", "h", 0)
        .function(1, {}, code({ localGet(0), localGet(1), { 0x92 } }))           // f32.add
        .function(1, {}, code({ call(0), localGet(0), localGet(1), { 0x92 } })); // h; f32.add
    auto const host = HostFloatEnvironment {};
    auto const embedderControl = isoline::runtime::readFloatControl();
    auto nested = std::vector<uint32_t> {};
    Harness* self = nullptr;
    auto harness = Harness {
        builder,
        [&](Store& store) {
            return hostImport(store,
                              [&](HostContext& context,
                                  std::span<Value const>,
                                  std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                  departFromIeeeDefaults();
                                  for (auto const& [lhs, rhs]:
                                       { std::pair { One, Tiny }, std::pair { Subnormal, Subnormal } })
                                  {
                                      auto const args = std::array<Value, 2> { F32 { lhs }, F32 { rhs } };
                                      auto results = std::array<Value, 1> {};
                                      if (auto const added = context.invoke(self->function(1), args, results);
                                          !added)
                                          return added;
                                      nested.push_back(std::get<F32>(results[0]).bits);
                                  }
                                  return {};
                              });
        },
    };
    self = &harness;

    // The re-entered guest runs under the defaults, and so does its caller once h has returned.
    CHECK(harness.result(2, { F32 { One }, F32 { Tiny } }) == Value { F32 { One } });
    CHECK(harness.result(2, { F32 { Subnormal }, F32 { Subnormal } }) == Value { F32 { 2 * Subnormal } });
    CHECK(nested == std::vector { One, 2 * Subnormal, One, 2 * Subnormal });

    // What h did to the environment ends with the embedder's call.
    CHECK(std::fegetround() == FE_TONEAREST);
    CHECK(governedBits(hostFloatControlLayout(), isoline::runtime::readFloatControl())
          == governedBits(hostFloatControlLayout(), embedderControl));
}

namespace
{

/// The catch_ref clause's byte.
constexpr auto ClauseCatchRef = uint8_t { 0x01 };

/// exnref, as a value type.
constexpr auto TypeExnRef = isoline::binary::ExnRef;

/// block (result exnref) try_table (@p clause 0 0) @p throwing end unreachable end: what @p throwing
/// throws, caught as an exnref; by tag 0 when @p clause is catch_ref.
Bytes caught(Bytes const& throwing, uint8_t clause = ClauseCatchAllRef)
{
    return code({ { 0x02, ExnRefResult }, tryTable(NoResult, clause, 0, 0), throwing, { 0x0B, 0x00, 0x0B } });
}

/// loop: caught(@p throwing, @p clause), dropped, as many rounds as the i32 local @p counter holds.
Bytes dropRounds(uint32_t counter, Bytes const& throwing, uint8_t clause = ClauseCatchAllRef)
{
    return code({ { 0x03, NoResult },
                  caught(throwing, clause),
                  { 0x1A }, // drop
                  localGet(counter),
                  i32Const(1),
                  { 0x6B, 0x22 }, // i32.sub; local.tee
                  encodeU32(counter),
                  { 0x0D, 0x00, 0x0B } }); // br_if 0; end
}

/// throw $e @p payload, $e a tag of one i32 param.
Bytes throwI32(int32_t payload)
{
    return code({ i32Const(payload), throwTag(0) });
}

} // namespace

TEST_CASE("ReferenceInterpreter.reclaimsDroppedExceptions")
{
    // A million rounds of throw, catch_ref and drop in a store of 16 exceptions: every sweep reclaims
    // the dropped ones, so the cap bounds the exceptions held at once, not the throws.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({ TypeI32 }, {}))
        .tag(0)
        .function(1, {}, dropRounds(0, throwTag(0), ClauseCatchRef));
    auto harness = Harness { builder, {}, StoreLimits { .maxExceptions = 16 } };
    CHECK(harness.invoke(0, { i32(1'000'000) }).has_value());
    CHECK(harness.store().liveExceptions() <= 16);
}

TEST_CASE("ReferenceInterpreter.keepsExceptionsInLocals")
{
    // block (result i32) try_table (catch $e 0)
    //   caught(throw $e 42) local.set 0; 20 rounds of caught(throw $e 7) drop; local.get 0 throw_ref
    // end unreachable end: the sweeps the rounds take keep the exception local 0 holds.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, { TypeI32 }))
        .tag(0)
        .function(1,
                  { TypeExnRef, TypeI32 },
                  code({ { 0x02, I32Result },
                         tryTable(NoResult, ClauseCatch, 0, 0),
                         caught(throwI32(42)),
                         { 0x21, 0x00 }, // local.set 0
                         i32Const(20),
                         { 0x21, 0x01 }, // local.set 1
                         dropRounds(1, throwI32(7)),
                         localGet(0),
                         { 0x0A }, // throw_ref
                         { 0x0B, 0x00, 0x0B } }));
    auto harness = Harness { builder, {}, StoreLimits { .maxExceptions = 16 } };
    CHECK(harness.result(0) == i32(42));
}

TEST_CASE("ReferenceInterpreter.keepsExceptionsInPayloads")
{
    // B = caught(throw $b 42); A = caught(throw $a B), kept in global 0 only; 20 rounds of throw and
    // drop; then throw_ref A, caught by $a, hands B, whose throw_ref the outer catch $b unpacks: B
    // survived the sweeps through A's payload.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({ TypeExnRef }, {}))
        .type(signature({}, { TypeI32 }))
        .tag(0)
        .tag(1)
        .global(GlobalType { .type = TypeExnRef, .mutability = Mutability::Var }, { 0xD0, 0x69 })
        .function(2,
                  { TypeExnRef, TypeI32 },
                  code({ caught(throwI32(42)),
                         { 0x21, 0x00 }, // local.set 0: B
                         caught(code({ localGet(0), throwTag(1) })),
                         { 0x24, 0x00 },             // global.set 0: A
                         { 0xD0, 0x69, 0x21, 0x00 }, // local 0 = ref.null exn: only A holds B
                         i32Const(20),
                         { 0x21, 0x01 },
                         dropRounds(1, throwI32(7)),
                         { 0x02, I32Result },
                         tryTable(NoResult, ClauseCatch, 0, 0),
                         { 0x02, ExnRefResult },
                         tryTable(NoResult, ClauseCatch, 1, 0),
                         { 0x23, 0x00, 0x0A }, // global.get 0; throw_ref A
                         { 0x0B, 0x00, 0x0B },
                         { 0x0A }, // throw_ref B
                         { 0x0B, 0x00, 0x0B } }));
    auto harness = Harness { builder, {}, StoreLimits { .maxExceptions = 16 } };
    CHECK(harness.result(0) == i32(42));
    CHECK(harness.store().liveExceptions() <= 16);
}

TEST_CASE("ReferenceInterpreter.noSweepUnderAHostFrame")
{
    // Function 2 calls the host, which invokes function 1: 17 rounds of throw and drop. A host's
    // locals are no roots, so no sweep runs under it, and the 17th throw traps in a store of 16. The
    // same rounds invoked by the embedder sweep, and succeed.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({ TypeI32 }, {}))
        .importFunction("env", "host", 0)
        .tag(0)
        .function(1, {}, dropRounds(0, throwTag(0)))
        .function(0, {}, call(0));
    Harness* self = nullptr;
    auto harness = Harness {
        builder,
        [&](Store& store) {
            return hostImport(store,
                              [&](HostContext& context, std::span<Value const>, std::span<Value>) noexcept {
                                  auto const args = std::array { i32(17) };
                                  return context.invoke(self->function(1), args, {});
                              });
        },
        StoreLimits { .maxExceptions = 16 },
    };
    self = &harness;
    CHECK(harness.trap(2) == TrapCode::ExceptionLimitExceeded);
    CHECK(harness.invoke(1, { i32(17) }).has_value());
}

TEST_CASE("ReferenceInterpreter.keepsOuterFramesExceptionsAcrossHostReentry")
{
    // Function 2 catches $e 42 into local 0, calls the host, which re-enters for 10 rounds of throw
    // and drop (no sweep under it), then calls function 1 for 20 more, which sweep with function 2's
    // frame beneath: local 0 survives both, and its throw_ref reaches the outer catch.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, {}))
        .type(signature({}, { TypeI32 }))
        .importFunction("env", "host", 1)
        .tag(0)
        .function(0, {}, dropRounds(0, throwI32(7)))
        .function(2,
                  { TypeExnRef },
                  code({ { 0x02, I32Result },
                         tryTable(NoResult, ClauseCatch, 0, 0),
                         caught(throwI32(42)),
                         { 0x21, 0x00 },
                         call(0),
                         i32Const(20),
                         call(1),
                         localGet(0),
                         { 0x0A },
                         { 0x0B, 0x00, 0x0B } }));
    Harness* self = nullptr;
    auto harness = Harness {
        builder,
        [&](Store& store) {
            return hostImport(store,
                              [&](HostContext& context, std::span<Value const>, std::span<Value>) noexcept {
                                  auto const args = std::array { i32(10) };
                                  return context.invoke(self->function(1), args, {});
                              });
        },
        StoreLimits { .maxExceptions = 16 },
    };
    self = &harness;
    CHECK(harness.result(2) == i32(42));
    CHECK(harness.store().liveExceptions() <= 16);
}

TEST_CASE("ReferenceInterpreter.hostStashedExceptionSurvives")
{
    // Function 1 hands caught(throw $e 42) to the host, which keeps its address past its frame; 20
    // rounds of throw and drop later, in a store of 16, the address still names that exception: an
    // exnref argument escapes, and no sweep reclaims it or hands its slot to another.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({ TypeExnRef }, {}))
        .type(signature({}, {}))
        .importFunction("env", "stash", 1)
        .tag(0)
        .function(2, {}, code({ caught(throwI32(42)), call(0) }))
        .function(0, {}, dropRounds(0, throwI32(7)));
    auto stashed = std::optional<ExnAddr> {};
    auto harness = Harness {
        builder,
        [&](Store& store) {
            return std::vector { ExternValue {
                .kind = ExternKind::Func,
                .address = addHost(store,
                                   signature({ TypeExnRef }, {}),
                                   [&](HostContext&, std::span<Value const> args, std::span<Value>) noexcept
                                       -> std::expected<void, Abrupt> {
                                       stashed = std::get<Ref>(args[0]).address;
                                       return {};
                                   }) } };
        },
        StoreLimits { .maxExceptions = 16 },
    };
    REQUIRE(harness.invoke(1).has_value());
    REQUIRE(stashed.has_value());
    REQUIRE(harness.invoke(2, { i32(20) }).has_value());
    CHECK(harness.store().isLiveException(*stashed));
    CHECK(harness.store().exception(*stashed).payload == std::vector { i32(42) });
}

TEST_CASE("ReferenceInterpreter.interruptStopsThrowCatchLoop", "[threading]")
{
    // loop $l (try_table (catch_all $l) (throw $e)): every throw's exception is dropped, so the sweeps
    // never let the arena of 16 stop the loop; the catch's branch to the loop polls, and an interrupt
    // from another thread ends it.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .tag(0)
        .function(0,
                  {},
                  code({ { 0x03, NoResult },
                         tryTable(NoResult, ClauseCatchAll, 0, 0),
                         throwTag(0),
                         { 0x0B, 0x0B } }));
    auto counter = PollCounter { PollTrigger { .site = PollSite::LoopBackEdge, .ordinal = 1'000 } };
    auto harness = Harness { builder, {}, StoreLimits { .maxExceptions = 16 }, &counter };
    auto watchdog = InterruptWatchdog { counter, harness.store().interrupter() };
    CHECK(harness.trap(0) == TrapCode::Interrupted);
    CHECK(watchdog.finish() == TriggerWait::Triggered);
    CHECK(harness.store().liveExceptions() <= 16);
}

TEST_CASE("ReferenceInterpreter.hostEscapesWhatItKeeps")
{
    // Function 1 puts caught(throw $e 42) in global 0 and calls the host, which reads it through its
    // read-only store, borrowed for its frame, and escapes it to keep it; the guest then clears the
    // global, and 20 rounds of throw and drop in a store of 16 leave the kept exception alone.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, {}))
        .importFunction("env", "keep", 1)
        .tag(0)
        .global(GlobalType { .type = TypeExnRef, .mutability = Mutability::Var }, { 0xD0, 0x69 })
        .function(1,
                  {},
                  code({ caught(throwI32(42)),
                         { 0x24, 0x00 }, // global.set 0
                         call(0),
                         { 0xD0, 0x69, 0x24, 0x00 } })) // global 0 = ref.null exn
        .function(0, {}, dropRounds(0, throwI32(7)));
    auto kept = std::optional<ExnAddr> {};
    auto harness = Harness {
        builder,
        [&](Store& store) {
            return hostImport(store,
                              [&](HostContext& context,
                                  std::span<Value const>,
                                  std::span<Value>) noexcept -> std::expected<void, Abrupt> {
                                  auto const& store = context.store();
                                  auto const global =
                                      store.moduleInstance(context.callerInstance().value()).globals.at(0);
                                  auto const& value = store.global(global).value;
                                  kept = std::get<Ref>(value).address;
                                  context.markEscaped(value);
                                  return {};
                              });
        },
        StoreLimits { .maxExceptions = 16 },
    };
    REQUIRE(harness.invoke(1).has_value());
    REQUIRE(kept.has_value());
    REQUIRE(harness.invoke(2, { i32(20) }).has_value());
    CHECK(harness.store().isLiveException(*kept));
    CHECK(harness.store().exception(*kept).payload == std::vector { i32(42) });
}

TEST_CASE("ReferenceInterpreter.keepsExceptionsOnTheOperandStack")
{
    // block (result i32) try_table (catch $e 0)
    //   caught(throw $e 42); i32.const 20; call $rounds; throw_ref
    // end unreachable end: the exnref waits on the operand stack, beneath the call, while the 20
    // rounds sweep a store of 16; it is the only reference to its exception.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, { TypeI32 }))
        .tag(0)
        .function(0, {}, dropRounds(0, throwI32(7)))
        .function(1,
                  {},
                  code({ { 0x02, I32Result },
                         tryTable(NoResult, ClauseCatch, 0, 0),
                         caught(throwI32(42)),
                         i32Const(20),
                         call(0),
                         { 0x0A },
                         { 0x0B, 0x00, 0x0B } }));
    auto harness = Harness { builder, {}, StoreLimits { .maxExceptions = 16 } };
    CHECK(harness.result(1) == i32(42));
}

TEST_CASE("ReferenceInterpreter.keepsTheExceptionsOfAPayloadBeingThrown")
{
    // 15 rounds of throw and drop, then caught(throw $e 42), the 16th, fills the arena; throw $a takes
    // that exnref straight off the operand stack, so while its allocation sweeps, the payload being
    // thrown is the exnref's only reference. catch $a hands it back, and its throw_ref reaches catch $e.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({ TypeExnRef }, {}))
        .type(signature({}, { TypeI32 }))
        .tag(0)
        .tag(1)
        .function(2,
                  { TypeI32 },
                  code({ i32Const(15),
                         { 0x21, 0x00 },
                         dropRounds(0, throwI32(7)),
                         { 0x02, I32Result },
                         tryTable(NoResult, ClauseCatch, 0, 0),
                         { 0x02, ExnRefResult },
                         tryTable(NoResult, ClauseCatch, 1, 0),
                         caught(throwI32(42)),
                         throwTag(1),
                         { 0x0B, 0x00, 0x0B },
                         { 0x0A },
                         { 0x0B, 0x00, 0x0B } }));
    auto harness = Harness { builder, {}, StoreLimits { .maxExceptions = 16 } };
    CHECK(harness.result(0) == i32(42));
}

TEST_CASE("ReferenceInterpreter.keepsAnExceptionAHostReturned")
{
    // The host allocates $e 42 and returns its exnref, unescaped: once pushed, the operand stack keeps
    // it through the 20 rounds' sweeps, and its throw_ref reaches catch $e.
    auto builder = ModuleBuilder {};
    builder.type(signature({ TypeI32 }, {}))
        .type(signature({}, { TypeExnRef }))
        .type(signature({}, { TypeI32 }))
        .importFunction("env", "make", 1)
        .tag(0)
        .function(0, {}, dropRounds(0, throwI32(7)))
        .function(2,
                  {},
                  code({ { 0x02, I32Result },
                         tryTable(NoResult, ClauseCatch, 0, 0),
                         call(0),
                         i32Const(20),
                         call(1),
                         { 0x0A },
                         { 0x0B, 0x00, 0x0B } }));
    auto harness = Harness {
        builder,
        [&](Store& store) {
            return std::vector { ExternValue {
                .kind = ExternKind::Func,
                .address = addHost(
                    store,
                    signature({}, { TypeExnRef }),
                    [](HostContext& context, std::span<Value const>, std::span<Value> results) noexcept
                        -> std::expected<void, Abrupt> {
                        auto const tag =
                            context.store().moduleInstance(context.callerInstance().value()).tags.at(0);
                        return context.addException(tag, { i32(42) }).transform([&](ExnAddr exception) {
                            results[0] = Ref { .kind = isoline::runtime::RefKind::Exn,
                                               .heap = TypeExnRef.heap,
                                               .address = exception };
                        });
                    }) } };
        },
        StoreLimits { .maxExceptions = 16 },
    };
    CHECK(harness.result(2) == i32(42));
    CHECK(harness.store().liveExceptions() <= 16);
}

TEST_CASE("Interpreter.uninitializedTypedLocalNamesItsTypeId")
{
    // Types 0 and 1 are equal, so they share TypeId 0 and type 2 gets TypeId 1: a (ref null 2) local
    // starts as the null of TypeId 1, as ref.null 2 does, not of type index 2.
    using isoline::runtime::Ref;
    auto const nullableTo2 = isoline::binary::concreteRef(2, isoline::binary::Nullable::Yes);
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .type(signature({}, {}))
        .type(signature({ TypeI32 }, {}))
        .type(signature({}, { nullableTo2 }))
        .function(3, { nullableTo2 }, code({ localGet(0) }))
        .function(3, {}, code({ { 0xD0, 0x02 } })); // ref.null 2
    auto harness = Harness { builder };
    auto const local = harness.result(0);
    REQUIRE(std::holds_alternative<Ref>(local));
    CHECK(std::get<Ref>(local).heap.typeIndex == 1);
    CHECK(local == harness.result(1));
}
