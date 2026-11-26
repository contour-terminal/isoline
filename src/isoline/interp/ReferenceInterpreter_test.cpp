// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
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
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interp/ReferenceInterpreter.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/Validator.hpp>

using isoline::FeatureSet;
using isoline::binary::AddressType;
using isoline::binary::ConstExpr;
using isoline::binary::ExternKind;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::Instruction;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Module;
using isoline::binary::Mutability;
using isoline::binary::Opcode;
using isoline::binary::TableType;
using isoline::binary::ValueType;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::ModuleBuilder;
using isoline::interp::ReferenceInterpreter;
using isoline::runtime::ExternValue;
using isoline::runtime::F32;
using isoline::runtime::FuncAddr;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Store;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::UnlimitedAllocator;
using isoline::runtime::Value;

namespace
{

/// The default page size, 64 KiB.
constexpr auto PageSize = isoline::binary::pageSize(MemoryType {});

constexpr auto TypeI32 = isoline::binary::I32;
constexpr auto TypeI64 = isoline::binary::I64;
constexpr auto TypeF32 = isoline::binary::F32;

/// Bytes of a function body, assembled from pieces.
using Bytes = std::vector<uint8_t>;

/// The concatenation of @p pieces.
Bytes code(std::initializer_list<Bytes> pieces)
{
    auto out = Bytes {};
    for (auto const& piece: pieces)
        out.insert(out.end(), piece.begin(), piece.end());
    return out;
}

/// The signed LEB128 encoding of @p value.
Bytes encodeS64(int64_t value)
{
    auto out = Bytes {};
    while (true)
    {
        auto const byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        auto const done = (value == 0 && (byte & 0x40) == 0) || (value == -1 && (byte & 0x40) != 0);
        out.push_back(done ? byte : static_cast<uint8_t>(byte | 0x80));
        if (done)
            return out;
    }
}

Bytes i32Const(int32_t value)
{
    return code({ { 0x41 }, encodeS64(value) });
}

Bytes i64Const(int64_t value)
{
    return code({ { 0x42 }, encodeS64(value) });
}

Bytes localGet(uint32_t index)
{
    return code({ { 0x20 }, encodeU32(index) });
}

Bytes call(uint32_t index)
{
    return code({ { 0x10 }, encodeU32(index) });
}

/// A load or store of @p opcode byte with alignment exponent @p align and offset @p offset.
Bytes memoryAccess(uint8_t opcode, uint32_t align, uint32_t offset)
{
    return code({ { opcode }, encodeU32(align), encodeU32(offset) });
}

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
        _store { _allocator, maxCallDepth }
    {
        instantiate(builder, imports);
    }

    /// Creates the harness with host functions that @p bind adds to the store before instantiation.
    template <typename Bind>
    Harness(ModuleBuilder const& builder, Bind&& bind): _store { _allocator, 10'000 }
    {
        instantiate(builder, std::forward<Bind>(bind)(_store));
    }

    /// Invokes the module's function @p index.
    /// @param index The function's index in the module's function index space.
    /// @param args Its arguments.
    /// @return Its results, or the trap.
    std::expected<std::vector<Value>, Trap> invoke(uint32_t index, std::vector<Value> const& args = {})
    {
        auto const address = function(index);
        auto results = std::vector<Value>(_store.function(address).type.results.size());
        return _interpreter.invoke(_store, address, args, results).transform([&] { return results; });
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

  private:
    void instantiate(ModuleBuilder const& builder, std::vector<ExternValue> const& imports)
    {
        // Every feature Isoline knows, so that a test may use any instruction it executes.
        auto const features = FeatureSet::all();
        auto const bytes = builder.build();
        auto module = isoline::binary::decodeModule(bytes, features);
        REQUIRE(module.has_value());
        REQUIRE(isoline::validate::validateModule(*module, features).has_value());
        auto const address = isoline::runtime::instantiate(
            _store, _interpreter, std::make_shared<Module const>(std::move(*module)), imports);
        REQUIRE(address.has_value());
        _address = *address;
    }

    UnlimitedAllocator _allocator;
    Store _store;
    ReferenceInterpreter _interpreter;
    uint32_t _address = 0;
};

FuncType signature(std::vector<ValueType> params, std::vector<ValueType> results)
{
    return FuncType { .params = std::move(params), .results = std::move(results) };
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
                         store.addHostFunction(signature({ TypeI32 }, { TypeI32 }),
                                               [&](std::span<Value const> args,
                                                   std::span<Value> results) -> std::expected<void, Trap> {
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
                     auto const host = store.addHostFunction(
                         signature({}, {}),
                         [](std::span<Value const>, std::span<Value>) -> std::expected<void, Trap> {
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
    CHECK(harness.store().memory(harness.store().moduleInstance(harness.address()).memories[0]).bytes.size()
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
    CHECK(harness.store().memory(harness.store().moduleInstance(harness.address()).memories[0]).bytes.size()
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

TEST_CASE("ReferenceInterpreter.evaluateConst")
{
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {}))
        .function(0, {}, {})
        .global(GlobalType { .type = TypeI32, .mutability = Mutability::Const }, i32Const(9));
    auto harness = Harness { builder };
    auto const instruction = [](Opcode opcode, uint64_t immediate = 0, uint32_t index = 0) {
        return Instruction { .opcode = opcode, .a = index, .c = immediate };
    };
    auto const evaluate = [&](ConstExpr const& expression) {
        return harness.interpreter().evaluateConst(harness.store(), harness.address(), expression);
    };

    // Extended-const: (9 + 3) * 2 - 1.
    auto const arithmetic = evaluate(ConstExpr { instruction(Opcode::GlobalGet),
                                                 instruction(Opcode::I32Const, 3),
                                                 instruction(Opcode::I32Add),
                                                 instruction(Opcode::I32Const, 2),
                                                 instruction(Opcode::I32Mul),
                                                 instruction(Opcode::I32Const, 1),
                                                 instruction(Opcode::I32Sub),
                                                 instruction(Opcode::End) });
    REQUIRE(arithmetic.has_value());
    CHECK(*arithmetic == i32(23));

    auto const wide = evaluate(ConstExpr { instruction(Opcode::I64Const, ~uint64_t { 0 }),
                                           instruction(Opcode::I64Const, 2),
                                           instruction(Opcode::I64Mul),
                                           instruction(Opcode::End) });
    REQUIRE(wide.has_value());
    CHECK(*wide == Value { I64 { ~uint64_t { 1 } } });

    auto const reference = evaluate(ConstExpr { instruction(Opcode::RefFunc), instruction(Opcode::End) });
    REQUIRE(reference.has_value());
    REQUIRE(std::holds_alternative<isoline::runtime::Ref>(*reference));
    CHECK(std::get<isoline::runtime::Ref>(*reference).address == harness.function(0));
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
                     auto const host = store.addHostFunction(
                         signature({}, { TypeI32 }),
                         [&](std::span<Value const>, std::span<Value> results) -> std::expected<void, Trap> {
                             auto const inner = self->invoke(2);
                             if (!inner)
                                 return std::unexpected { inner.error() };
                             results[0] = inner->front();
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
                     auto const host = store.addHostFunction(
                         signature({}, {}),
                         [&](std::span<Value const>, std::span<Value>) -> std::expected<void, Trap> {
                             ++reentries;
                             return self->invoke(1).transform([](auto const&) {});
                         });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    self = &harness;
    CHECK(harness.trap(1) == TrapCode::CallStackExhausted);
    CHECK(reentries == ReferenceInterpreter::MaxNestedInvocations);
    // Every invocation unwound: the interpreter runs on as fresh, and hits the same bound again.
    reentries = 0;
    CHECK(harness.trap(1) == TrapCode::CallStackExhausted);
    CHECK(reentries == ReferenceInterpreter::MaxNestedInvocations);
}

TEST_CASE("ReferenceInterpreter.hostExceptionUnwindsInvocationCount")
{
    // Function 1 calls the host; the callback throws on its first call, then re-enters forever.
    auto builder = ModuleBuilder {};
    builder.type(signature({}, {})).importFunction("env", "reenter", 0).function(0, {}, call(0));
    Harness* self = nullptr;
    auto reentries = uint32_t { 0 };
    auto shouldThrow = true;
    auto harness =
        Harness { builder, [&](Store& store) {
                     auto const host = store.addHostFunction(
                         signature({}, {}),
                         [&](std::span<Value const>, std::span<Value>) -> std::expected<void, Trap> {
                             if (shouldThrow)
                                 throw std::runtime_error { "host failure" };
                             ++reentries;
                             return self->invoke(1).transform([](auto const&) {});
                         });
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
                 } };
    self = &harness;
    CHECK_THROWS_AS(std::ignore = harness.invoke(1), std::runtime_error);
    // The throwing invocation left no count behind: the full nesting bound is still available.
    shouldThrow = false;
    CHECK(harness.trap(1) == TrapCode::CallStackExhausted);
    CHECK(reentries == ReferenceInterpreter::MaxNestedInvocations);
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
