// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

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
using isoline::runtime::PageSize;
using isoline::runtime::Store;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::UnlimitedAllocator;
using isoline::runtime::Value;

namespace
{

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
        auto const features = FeatureSet::wasm2();
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
    // Index 0xFFFFFFFF is an index like any other; the interpreter spells it as Int32.to_string does.
    CHECK(messageOf(-1) == "undefined element -1");
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
