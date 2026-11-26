// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/ProvidedMemory.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::FeatureSet;
using isoline::binary::AddressType;
using isoline::binary::ConstExpr;
using isoline::binary::ExternKind;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Mutability;
using isoline::binary::Opcode;
using isoline::binary::TableType;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::ModuleBuilder;
using isoline::runtime::Abrupt;
using isoline::runtime::AllocationFailure;
using isoline::runtime::ExternValue;
using isoline::runtime::FuncAddr;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::I32;
using isoline::runtime::InstantiationError;
using isoline::runtime::Limit;
using isoline::runtime::LimitExceeded;
using isoline::runtime::LinkError;
using isoline::runtime::LinkErrorCode;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Refusal;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;
using isoline::runtime::Thrown;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;
using isoline::runtime::testing::GrowDecision;
using isoline::runtime::testing::ProvidedMemoryAllocator;
using isoline::validate::ValidatedModule;

namespace
{

/// The default page size, 64 KiB.
constexpr auto PageSize = isoline::binary::pageSize(MemoryType {});

/// Limits of at least @p min, unbounded.
Limits atLeast(uint64_t min)
{
    return Limits { .min = min, .max = std::nullopt, .addressType = AddressType::I32 };
}

/// Limits from @p min to @p max.
Limits between(uint64_t min, uint64_t max)
{
    return Limits { .min = min, .max = max, .addressType = AddressType::I32 };
}

/// The embedder's decision that grants every grow.
GrowDecision grantAll(uint64_t /*newBytes*/)
{
    return GrowDecision::Grant;
}

/// Evaluates i32.const only, and records which functions it is asked to invoke.
class StubExecutor final: public isoline::runtime::ICodeExecutor
{
  public:
    std::vector<FuncAddr> invoked; ///< Every function invoked, in order.

    [[nodiscard]] std::expected<void, Abrupt> invoke(Store& /*store*/,
                                                     FuncAddr function,
                                                     std::span<Value const> /*args*/,
                                                     std::span<Value> /*results*/) override
    {
        invoked.push_back(function);
        return {};
    }

    [[nodiscard]] std::expected<Value, Trap> evaluateConst(Store& /*store*/,
                                                           uint32_t /*moduleInstance*/,
                                                           ConstExpr const& expression) override
    {
        REQUIRE(expression.size() == 2);
        REQUIRE(expression.front().opcode == Opcode::I32Const);
        return I32 { .bits = static_cast<uint32_t>(expression.front().c) };
    }
};

/// Evaluates i32.const, i64.const and ref.null like the interpreter, but traps on one evaluation.
class TrappingExecutor final: public isoline::runtime::ICodeExecutor
{
  public:
    /// @param trapAt The number of evaluations to perform before the one that traps.
    explicit TrappingExecutor(std::size_t trapAt): _trapAt { trapAt } {}

    [[nodiscard]] std::expected<void, Abrupt> invoke(Store& /*store*/,
                                                     FuncAddr /*function*/,
                                                     std::span<Value const> /*args*/,
                                                     std::span<Value> /*results*/) override
    {
        return {};
    }

    [[nodiscard]] std::expected<Value, Trap> evaluateConst(Store& /*store*/,
                                                           uint32_t /*moduleInstance*/,
                                                           ConstExpr const& expression) override
    {
        if (_evaluations++ == _trapAt)
            return std::unexpected { Trap { TrapCode::Unreachable } };
        auto const& instruction = expression.front();
        if (instruction.opcode == Opcode::I64Const)
            return isoline::runtime::I64 { .bits = instruction.c };
        if (instruction.opcode == Opcode::RefNull)
            return Ref { .kind = RefKind::Null, .heap = FuncRef.heap, .address = 0 };
        REQUIRE(instruction.opcode == Opcode::I32Const);
        return I32 { .bits = static_cast<uint32_t>(instruction.c) };
    }

  private:
    std::size_t _trapAt;
    std::size_t _evaluations = 0;
};

/// Decodes and validates @p builder's module; fails the test unless it is valid.
std::shared_ptr<ValidatedModule const> moduleOf(ModuleBuilder const& builder,
                                                FeatureSet const& features = FeatureSet::wasm2())
{
    auto validated = ValidatedModule::load(builder.build(), features, isoline::binary::DecodeLimits {});
    REQUIRE(validated.has_value());
    return std::move(*validated);
}

/// The bytes of an active data segment for memory 0 at @p offset.
std::vector<uint8_t> activeData(uint8_t offset, std::vector<uint8_t> const& contents)
{
    auto entry = std::vector<uint8_t> { 0x00, 0x41 };
    auto const offsetBytes = isoline::binary::testing::encodeU32(offset);
    entry.insert(entry.end(), offsetBytes.begin(), offsetBytes.end());
    entry.push_back(0x0B);
    entry.push_back(static_cast<uint8_t>(contents.size()));
    entry.insert(entry.end(), contents.begin(), contents.end());
    return entry;
}

/// The error of @p result, which must have failed.
template <typename T>
InstantiationError errorOf(std::expected<T, InstantiationError> const& result)
{
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

} // namespace

TEST_CASE("Instantiate.keepsTheValidatedModule")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, {});
    auto const validated = moduleOf(builder);
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const address = instantiate(store, executor, validated, {});
    REQUIRE(address.has_value());
    auto const& instance = store.moduleInstance(*address);
    CHECK(instance.validated == validated);
    CHECK(instance.module.get() == &validated->module());
}

TEST_CASE("Instantiate.unknownImport")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("m", "f", 0);
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const result = instantiate(store, executor, moduleOf(builder), {});
    CHECK(errorOf(result) == InstantiationError { LinkError { LinkErrorCode::UnknownImport, 0 } });
}

TEST_CASE("Instantiate.incompatibleImportType")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("m", "f", 0);
    auto const module = moduleOf(builder);
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const incompatible = InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } };

    SECTION("another kind")
    {
        auto const memory = store.addMemory(MemoryType { .limits = atLeast(1) });
        REQUIRE(memory.has_value());
        auto const imports = std::vector { ExternValue { .kind = ExternKind::Memory, .address = *memory } };
        CHECK(errorOf(instantiate(store, executor, module, imports)) == incompatible);
    }
    SECTION("another function type")
    {
        auto const host =
            store.addHostFunction(FuncType { .params = { isoline::binary::I32 }, .results = {} },
                                  [](auto, auto) -> std::expected<void, Trap> { return {}; });
        auto const imports = std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
        CHECK(errorOf(instantiate(store, executor, module, imports)) == incompatible);
    }
    SECTION("a tag")
    {
        auto const tag = store.addTag(store.internType(FuncType {}));
        auto const imports = std::vector { ExternValue { .kind = ExternKind::Tag, .address = tag } };
        CHECK(errorOf(instantiate(store, executor, module, imports)) == incompatible);
    }
    SECTION("the right function type")
    {
        auto const host =
            store.addHostFunction(FuncType {}, [](auto, auto) -> std::expected<void, Trap> { return {}; });
        auto const imports = std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
        auto const instance = instantiate(store, executor, module, imports);
        REQUIRE(instance.has_value());
        CHECK(store.moduleInstance(*instance).functions == std::vector<FuncAddr> { host });
    }
}

TEST_CASE("Instantiate.memoryLimitsSubtyping")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const incompatible = InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } };
    auto const importing = [](Limits const& limits) {
        auto builder = ModuleBuilder {};
        builder.importMemory("m", "mem", limits);
        return moduleOf(builder);
    };
    auto const provided = [&](Limits const& limits) {
        auto const memory = store.addMemory(MemoryType { .limits = limits });
        REQUIRE(memory.has_value());
        return std::vector { ExternValue { .kind = ExternKind::Memory, .address = *memory } };
    };

    CHECK(errorOf(instantiate(store, executor, importing(atLeast(2)), provided(atLeast(1)))) == incompatible);
    CHECK(instantiate(store, executor, importing(atLeast(2)), provided(atLeast(3))).has_value());
    // A required maximum needs a maximum no larger.
    CHECK(errorOf(instantiate(store, executor, importing(between(1, 3)), provided(atLeast(1))))
          == incompatible);
    CHECK(errorOf(instantiate(store, executor, importing(between(1, 3)), provided(between(1, 4))))
          == incompatible);
    CHECK(instantiate(store, executor, importing(between(1, 3)), provided(between(2, 3))).has_value());

    SECTION("the current size counts, not the allocated minimum")
    {
        auto const imports = provided(atLeast(1));
        REQUIRE(store.growMemory(imports.front().address, 1) == 1U);
        CHECK(instantiate(store, executor, importing(atLeast(2)), imports).has_value());
    }
}

TEST_CASE("Instantiate.tableAndGlobalImports")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto builder = ModuleBuilder {};
    builder.importTable("m", "t", TableType { .element = FuncRef, .limits = atLeast(2) })
        .importGlobal("m", "g", GlobalType { .type = isoline::binary::I32, .mutability = Mutability::Var });
    auto const module = moduleOf(builder);

    auto const table = store.addTable(TableType { .element = FuncRef, .limits = atLeast(2) }, Ref {});
    REQUIRE(table.has_value());
    auto const tableValue = ExternValue { .kind = ExternKind::Table, .address = *table };
    auto const constGlobal = store.addGlobal(GlobalType { .type = isoline::binary::I32 }, I32 {});
    auto const varGlobal =
        store.addGlobal(GlobalType { .type = isoline::binary::I32, .mutability = Mutability::Var }, I32 {});

    auto const withGlobal = [&](uint32_t global) {
        return std::vector { tableValue, ExternValue { .kind = ExternKind::Global, .address = global } };
    };
    CHECK(errorOf(instantiate(store, executor, module, withGlobal(constGlobal)))
          == InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 1 } });
    auto const instance = instantiate(store, executor, module, withGlobal(varGlobal));
    REQUIRE(instance.has_value());
    CHECK(store.moduleInstance(*instance).tables == std::vector { *table });
    CHECK(store.moduleInstance(*instance).globals == std::vector { varGlobal });
}

TEST_CASE("Instantiate.importSubtyping")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const incompatible = InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } };
    auto const importingTable = [](Limits const& limits) {
        auto builder = ModuleBuilder {};
        builder.importTable("m", "t", TableType { .element = FuncRef, .limits = limits });
        return moduleOf(builder);
    };
    auto const providedTable = [&](TableType const& type) {
        auto const table =
            store.addTable(type, Ref { .kind = RefKind::Null, .heap = type.element.heap, .address = 0 });
        REQUIRE(table.has_value());
        return std::vector { ExternValue { .kind = ExternKind::Table, .address = *table } };
    };

    SECTION("a table's element type")
    {
        auto const imports =
            providedTable(TableType { .element = isoline::binary::ExternRef, .limits = atLeast(1) });
        CHECK(errorOf(instantiate(store, executor, importingTable(atLeast(1)), imports)) == incompatible);
    }
    SECTION("a table's current size below the required minimum")
    {
        auto const imports = providedTable(TableType { .element = FuncRef, .limits = atLeast(1) });
        CHECK(errorOf(instantiate(store, executor, importingTable(atLeast(2)), imports)) == incompatible);
    }
    SECTION("a table without the required maximum")
    {
        auto const imports = providedTable(TableType { .element = FuncRef, .limits = atLeast(1) });
        CHECK(errorOf(instantiate(store, executor, importingTable(between(1, 3)), imports)) == incompatible);
    }
    SECTION("a table whose maximum exceeds the required one")
    {
        auto const imports = providedTable(TableType { .element = FuncRef, .limits = between(1, 4) });
        CHECK(errorOf(instantiate(store, executor, importingTable(between(1, 3)), imports)) == incompatible);
    }
    SECTION("a matching table")
    {
        auto const imports = providedTable(TableType { .element = FuncRef, .limits = between(2, 3) });
        CHECK(instantiate(store, executor, importingTable(between(1, 3)), imports).has_value());
    }
    SECTION("a global's value type")
    {
        auto builder = ModuleBuilder {};
        builder.importGlobal("m", "g", GlobalType { .type = isoline::binary::I32 });
        auto const global =
            store.addGlobal(GlobalType { .type = isoline::binary::I64 }, isoline::runtime::I64 {});
        auto const imports = std::vector { ExternValue { .kind = ExternKind::Global, .address = global } };
        CHECK(errorOf(instantiate(store, executor, moduleOf(builder), imports)) == incompatible);
    }
    SECTION("a memory's address type")
    {
        // P1 decodes no memory64 import, but a host can hand in a 64-bit memory.
        auto builder = ModuleBuilder {};
        builder.importMemory("m", "mem", atLeast(1));
        auto const memory = store.addMemory(MemoryType {
            .limits = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I64 } });
        REQUIRE(memory.has_value());
        auto const imports = std::vector { ExternValue { .kind = ExternKind::Memory, .address = *memory } };
        CHECK(errorOf(instantiate(store, executor, moduleOf(builder), imports)) == incompatible);
    }
}

TEST_CASE("Instantiate.dataSegmentOutOfBounds")
{
    // The second segment writes two bytes at 65535 of a one-page memory; its offset is encoded by
    // hand, as the signed LEB128 0xFF 0xFF 0x03.
    auto builder = ModuleBuilder {};
    builder.memory(atLeast(1))
        .data(activeData(0, { 0xAA, 0xBB }))
        .data({ 0x00, 0x41, 0xFF, 0xFF, 0x03, 0x0B, 0x02, 0x01, 0x02 });
    auto const module = moduleOf(builder);
    REQUIRE(module->module().data[1].offset.front().c == 65'535);
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const result = instantiate(store, executor, module, {});
    CHECK(errorOf(result) == InstantiationError { Trap { TrapCode::OutOfBoundsMemoryAccess } });
    // The instance stays in the store, and the segment before the faulty one is written.
    auto const& memory = store.memory(0);
    REQUIRE(memory.bytes().size() == PageSize);
    CHECK(memory.bytes()[0] == std::byte { 0xAA });
    CHECK(memory.bytes()[1] == std::byte { 0xBB });
    CHECK(memory.bytes()[65'535] == std::byte { 0x00 });
    CHECK(store.moduleInstance(0).data[0] == std::nullopt);
}

TEST_CASE("Instantiate.tableSegmentOutOfBounds")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .function(0, {}, {})
        .table(TableType { .element = FuncRef, .limits = atLeast(2) })
        .element({ 0x00, 0x41, 0x01, 0x0B, 0x01, 0x00 })        // table[1] = f0
        .element({ 0x00, 0x41, 0x01, 0x0B, 0x02, 0x00, 0x00 }); // table[1..3): one slot too far
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const result = instantiate(store, executor, moduleOf(builder), {});
    CHECK(errorOf(result) == InstantiationError { Trap { TrapCode::OutOfBoundsTableAccess } });
    auto const& elements = store.table(0).elements;
    REQUIRE(elements.size() == 2);
    CHECK(elements[0].kind == RefKind::Null);
    CHECK(elements[1] == Ref { .kind = RefKind::Func, .heap = FuncRef.heap, .address = 0 });
}

TEST_CASE("Instantiate.segmentsDropped")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .function(0, {}, {})
        .table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .memory(atLeast(1))
        .element({ 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00 }) // active
        .element({ 0x01, 0x00, 0x01, 0x00 })             // passive
        .element({ 0x03, 0x00, 0x01, 0x00 })             // declarative
        .data(activeData(3, { 0x11 }))
        .data({ 0x01, 0x02, 0x22, 0x33 }); // passive
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const result = instantiate(store, executor, moduleOf(builder), {});
    REQUIRE(result.has_value());
    auto const& instance = store.moduleInstance(*result);
    REQUIRE(instance.elements.size() == 3);
    CHECK(instance.elements[0] == std::nullopt);
    REQUIRE(instance.elements[1].has_value());
    CHECK(*instance.elements[1]
          == std::vector { Ref { .kind = RefKind::Func, .heap = FuncRef.heap, .address = 0 } });
    CHECK(instance.elements[2] == std::nullopt);
    REQUIRE(instance.data.size() == 2);
    CHECK(instance.data[0] == std::nullopt);
    REQUIRE(instance.data[1].has_value());
    CHECK(instance.data[1]->size() == 2);
    CHECK(store.memory(instance.memories[0]).bytes()[3] == std::byte { 0x11 });
    CHECK(store.table(instance.tables[0]).elements[0].kind == RefKind::Func);
}

TEST_CASE("Instantiate.globalsAndStart")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .function(0, {}, {})
        .function(0, {}, {})
        .global(GlobalType { .type = isoline::binary::I32, .mutability = Mutability::Var }, i32Const(42))
        .start(1);
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const result = instantiate(store, executor, moduleOf(builder), {});
    REQUIRE(result.has_value());
    auto const& instance = store.moduleInstance(*result);
    REQUIRE(instance.globals.size() == 1);
    CHECK(store.global(instance.globals[0]).value == Value { I32 { .bits = 42 } });
    CHECK(executor.invoked == std::vector { instance.functions[1] });
    auto const& started = store.function(instance.functions[1]);
    CHECK_FALSE(started.isHost());
    CHECK(started.moduleInstance == *result);
    CHECK(started.codeIndex == 1);
}

TEST_CASE("Instantiate.memoryAllocationRefused")
{
    auto builder = ModuleBuilder {};
    builder.memory(atLeast(3));
    auto allocator = ProvidedMemoryAllocator { 2 * PageSize, 0, grantAll };
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    CHECK(errorOf(instantiate(store, executor, moduleOf(builder), {}))
          == InstantiationError {
              AllocationFailure { .kind = ExternKind::Memory, .reason = Refusal::Embedder } });
    CHECK(store.memoryCount() == 0);
}

TEST_CASE("Instantiate.tableAllocationRefused")
{
    // A table of 64-bit addresses at the most slots a byte count can express, more than a vector can
    // hold (at most 2^63 bytes), in a store whose budget does not stop it first: the host has no
    // memory for it.
    auto builder = ModuleBuilder {};
    builder.table(TableType { .element = FuncRef,
                              .limits = Limits { .min = std::numeric_limits<uint64_t>::max() / sizeof(Ref),
                                                 .max = std::nullopt,
                                                 .addressType = AddressType::I64 } });
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxTableElements = std::numeric_limits<uint64_t>::max() },
                         StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    CHECK(errorOf(instantiate(store, executor, moduleOf(builder, FeatureSet::wasm3()), {}))
          == InstantiationError {
              AllocationFailure { .kind = ExternKind::Table, .reason = Refusal::HostMemory } });

    // The failed instantiation registered no instance: the next one takes the first address.
    auto const next = instantiate(store, executor, moduleOf(ModuleBuilder {}), {});
    REQUIRE(next.has_value());
    CHECK(*next == 0);
}

TEST_CASE("Instantiate.memoryGrowToFourGiB")
{
    // The embedder holds a single page, and records each size it is asked about.
    auto asked = std::vector<uint64_t> {};
    auto allocator = ProvidedMemoryAllocator { PageSize, 0, [&](uint64_t bytes) {
                                                  asked.push_back(bytes);
                                                  return GrowDecision::Grant;
                                              } };
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const memory = store.addMemory(MemoryType { .limits = atLeast(1) });
    REQUIRE(memory.has_value());

    // Growing to exactly 65536 pages, the reach of a 32-bit address and the store's default limit, asks
    // the embedder for 4 GiB.
    CHECK(store.growMemory(*memory, 65'535) == std::unexpected { Refusal::Embedder });
    CHECK(asked == std::vector { 65'536 * PageSize });

    // One page further is beyond the reach, and refused without asking.
    CHECK(store.growMemory(*memory, 65'536) == std::unexpected { Refusal::TypeMaximum });
    CHECK(asked.size() == 1);
}

TEST_CASE("Instantiate.memoryGrowFailureReturnsMinusOne")
{
    auto allocator = ProvidedMemoryAllocator { 2 * PageSize, 0, grantAll };
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const memory = store.addMemory(MemoryType { .limits = atLeast(1) });
    REQUIRE(memory.has_value());

    CHECK(store.growMemory(*memory, 5) == std::unexpected { Refusal::Embedder });
    CHECK(store.memory(*memory).bytes().size() == PageSize);
    CHECK(store.growMemory(*memory, 0) == 1U);
    CHECK(store.growMemory(*memory, 1) == 1U);
    CHECK(store.memory(*memory).bytes().size() == 2 * PageSize);
    CHECK(store.growMemory(*memory, 1) == std::unexpected { Refusal::Embedder });

    SECTION("above the maximum")
    {
        auto heap = HeapMemoryAllocator {};
        auto bounded = Store { StoreLimits {}, StoreServices { .allocator = heap } };
        auto const address = bounded.addMemory(MemoryType { .limits = between(1, 2) });
        REQUIRE(address.has_value());
        CHECK(bounded.growMemory(*address, 2) == std::unexpected { Refusal::TypeMaximum });
        CHECK(bounded.growMemory(*address, 1) == 1U);
        CHECK(bounded.growMemory(*address, std::numeric_limits<uint64_t>::max())
              == std::unexpected { Refusal::TypeMaximum });
    }
    SECTION("beyond what a vector can hold")
    {
        auto heap = HeapMemoryAllocator {};
        auto huge = Store { StoreLimits { .maxMemoryBytes = std::numeric_limits<uint64_t>::max() },
                            StoreServices { .allocator = heap } };
        auto const address = huge.addMemory(MemoryType {
            .limits = Limits { .min = 0, .max = std::nullopt, .addressType = AddressType::I64 } });
        REQUIRE(address.has_value());
        CHECK(huge.growMemory(*address, (uint64_t { 1 } << 47) + 1)
              == std::unexpected { Refusal::HostMemory });
        CHECK(huge.memory(*address).bytes().empty());
    }
}

TEST_CASE("Instantiate.tableGrow")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const table = store.addTable(TableType { .element = FuncRef, .limits = between(1, 3) }, Ref {});
    REQUIRE(table.has_value());
    auto const init = Ref { .kind = RefKind::Func, .heap = FuncRef.heap, .address = 7 };
    CHECK(store.growTable(*table, 3, init) == std::unexpected { Refusal::TypeMaximum });
    CHECK(store.growTable(*table, 2, init) == 1U);
    auto const& elements = store.table(*table).elements;
    REQUIRE(elements.size() == 3);
    CHECK(elements[0].kind == RefKind::Null);
    CHECK(elements[2] == init);
}

TEST_CASE("Instantiate.exportsResolve")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .importFunction("m", "f", 0)
        .function(0, {}, {})
        .memory(atLeast(1))
        .exportFunction("imported", 0)
        .exportFunction("defined", 1)
        .exportEntity("mem", ExternKind::Memory, 0);
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const host =
        store.addHostFunction(FuncType {}, [](auto, auto) -> std::expected<void, Trap> { return {}; });
    auto const imports = std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
    auto const result = instantiate(store, executor, moduleOf(builder), imports);
    REQUIRE(result.has_value());
    auto const& instance = store.moduleInstance(*result);
    REQUIRE(instance.exports.size() == 3);
    CHECK(instance.exports[0].first == "imported");
    CHECK(instance.exports[0].second == ExternValue { .kind = ExternKind::Func, .address = host });
    CHECK(instance.exports[1].first == "defined");
    CHECK(instance.exports[1].second
          == ExternValue { .kind = ExternKind::Func, .address = instance.functions[1] });
    CHECK(instance.exports[2].second
          == ExternValue { .kind = ExternKind::Memory, .address = instance.memories[0] });
}

TEST_CASE("Instantiate.constantExpressionTraps")
{
    // One of each constant expression instantiation evaluates, in its order: the global's
    // initializer, the element segment's item (ref.null func), the element segment's offset, then
    // the data segment's offset. A trap in any of them ends instantiation with that trap.
    auto builder = ModuleBuilder {};
    builder.table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .memory(atLeast(1))
        .global(GlobalType { .type = isoline::binary::I32, .mutability = Mutability::Const }, i32Const(1))
        .element({ 0x04, 0x41, 0x00, 0x0B, 0x01, 0xD0, 0x70, 0x0B })
        .data(activeData(0, { 0x2A }));
    auto const module = moduleOf(builder);
    auto allocator = HeapMemoryAllocator {};
    for (auto const trapAt: { 0U, 1U, 2U, 3U })
    {
        auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
        auto executor = TrappingExecutor { trapAt };
        CAPTURE(trapAt);
        CHECK(errorOf(instantiate(store, executor, module, {}))
              == InstantiationError { Trap { TrapCode::Unreachable } });
    }
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = TrappingExecutor { 4 };
    CHECK(instantiate(store, executor, module, {}).has_value());
}

TEST_CASE("Instantiate.tagImports")
{
    // A tag import matches a tag of its type exactly, and binds to that very tag.
    auto const features = FeatureSet::wasm3();
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { isoline::binary::I32 }, .results = {} })
        .importTag("m", "t", 0)
        .tag(0)
        .exportEntity("imported", ExternKind::Tag, 0)
        .exportEntity("defined", ExternKind::Tag, 1);
    auto const module = moduleOf(builder, features);
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};

    auto const other =
        store.addTag(store.internType(FuncType { .params = { isoline::binary::F32 }, .results = {} }));
    auto const otherImports = std::vector { ExternValue { .kind = ExternKind::Tag, .address = other } };
    CHECK(errorOf(instantiate(store, executor, module, otherImports))
          == InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });
    auto const function =
        store.addHostFunction(FuncType { .params = { isoline::binary::I32 }, .results = {} },
                              [](auto, auto) -> std::expected<void, Abrupt> { return {}; });
    auto const functionImports =
        std::vector { ExternValue { .kind = ExternKind::Func, .address = function } };
    CHECK(errorOf(instantiate(store, executor, module, functionImports))
          == InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });

    auto const tag =
        store.addTag(store.internType(FuncType { .params = { isoline::binary::I32 }, .results = {} }));
    auto const imports = std::vector { ExternValue { .kind = ExternKind::Tag, .address = tag } };
    auto const first = instantiate(store, executor, module, imports);
    REQUIRE(first.has_value());
    auto const& instance = store.moduleInstance(*first);
    REQUIRE(instance.tags.size() == 2);
    CHECK(instance.tags[0] == tag);
    CHECK(instance.tags[1] != tag);
    CHECK(store.tag(instance.tags[1]).typeId == store.tag(tag).typeId);
    CHECK(instance.exports.at(0).second == ExternValue { .kind = ExternKind::Tag, .address = tag });
    CHECK(instance.exports.at(1).second
          == ExternValue { .kind = ExternKind::Tag, .address = instance.tags[1] });
    // A second instance defines a tag of its own: tags are not shared by type.
    auto const second = instantiate(store, executor, module, imports);
    REQUIRE(second.has_value());
    CHECK(store.moduleInstance(*second).tags[0] == tag);
    CHECK(store.moduleInstance(*second).tags[1] != store.moduleInstance(*first).tags[1]);
}

namespace
{

/// Evaluates nothing, and ends every invocation with an uncaught exception.
class ThrowingExecutor final: public isoline::runtime::ICodeExecutor
{
  public:
    [[nodiscard]] std::expected<void, Abrupt> invoke(Store& /*store*/,
                                                     FuncAddr /*function*/,
                                                     std::span<Value const> /*args*/,
                                                     std::span<Value> /*results*/) override
    {
        return std::unexpected { Abrupt { Thrown { .exception = 3 } } };
    }

    [[nodiscard]] std::expected<Value, Trap> evaluateConst(Store& /*store*/,
                                                           uint32_t /*moduleInstance*/,
                                                           ConstExpr const& /*expression*/) override
    {
        return std::unexpected { Trap { TrapCode::Unreachable } };
    }
};

} // namespace

TEST_CASE("Instantiate.startThrows")
{
    // A start function's uncaught exception fails instantiation; the instance stays, as after a trap.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, {}).start(0);
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = ThrowingExecutor {};
    CHECK(errorOf(instantiate(store, executor, moduleOf(builder), {}))
          == InstantiationError { Thrown { .exception = 3 } });
}

TEST_CASE("Instantiate.i64DataOffset")
{
    // A memory of i64 addresses (memory64): its data segment's offset is an i64.
    auto builder = ModuleBuilder {};
    builder.memory(Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I64 })
        .data({ 0x00, 0x42, 0x05, 0x0B, 0x01, 0x2A }); // (data (i64.const 5) "\2A")
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = TrappingExecutor { std::numeric_limits<std::size_t>::max() };
    auto const result = instantiate(store, executor, moduleOf(builder, FeatureSet::wasm3()), {});
    REQUIRE(result.has_value());
    auto const& memory = store.memory(store.moduleInstance(*result).memories[0]);
    CHECK(memory.bytes()[5] == std::byte { 0x2A });
    CHECK(memory.bytes()[3] == std::byte { 0 });
}

TEST_CASE("Value.defaults")
{
    using isoline::runtime::defaultValue;
    CHECK(defaultValue(isoline::binary::I32) == Value { I32 {} });
    CHECK(defaultValue(isoline::binary::F64) == Value { isoline::runtime::F64 {} });
    CHECK(defaultValue(isoline::binary::V128) == Value { isoline::runtime::V128 {} });
    CHECK(isoline::runtime::V128 { .lanes = { 1, 2 } } != isoline::runtime::V128 { .lanes = { 1, 3 } });
    CHECK(defaultValue(isoline::binary::ExternRef)
          == Value { Ref { .kind = RefKind::Null, .heap = isoline::binary::ExternRef.heap, .address = 0 } });
}

TEST_CASE("Trap.specMessages")
{
    using isoline::runtime::specMessage;
    CHECK(specMessage(TrapCode::Unreachable) == std::string_view { "unreachable" });
    CHECK(specMessage(TrapCode::OutOfBoundsMemoryAccess)
          == std::string_view { "out of bounds memory access" });
    CHECK(specMessage(TrapCode::HostTrap) == std::string_view { "host trap" });
    CHECK(specMessage(TrapCode::NullFunctionReference) == std::string_view { "null function reference" });
    CHECK(specMessage(TrapCode::NullReference) == std::string_view { "null reference" });
}

TEST_CASE("Instantiate.specMessages")
{
    using isoline::runtime::LinkErrorCode;
    using isoline::runtime::specMessage;
    CHECK(specMessage(LinkErrorCode::UnknownImport) == "unknown import");
    CHECK(specMessage(LinkErrorCode::IncompatibleImportType) == "incompatible import type");
    CHECK(specMessage(AllocationFailure { .kind = ExternKind::Memory, .reason = Refusal::HostMemory })
          == "out of memory");
}

TEST_CASE("Instantiate.pageSizeMustMatch")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const bytePages = MemoryType { .limits = atLeast(0), .pageSizeLog2 = 0 };
    auto const importing = [](MemoryType const& type) {
        auto builder = ModuleBuilder {};
        builder.importMemory("m", "mem", type);
        return moduleOf(builder, FeatureSet::all());
    };
    auto const provided = [&](MemoryType const& type) {
        auto const memory = store.addMemory(type);
        REQUIRE(memory.has_value());
        return std::vector { ExternValue { .kind = ExternKind::Memory, .address = *memory } };
    };
    auto const incompatible = InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } };
    // A 64 KiB-page memory where 1-byte pages are declared, and the other way round.
    CHECK(errorOf(instantiate(
              store, executor, importing(bytePages), provided(MemoryType { .limits = atLeast(0) })))
          == incompatible);
    CHECK(errorOf(instantiate(
              store, executor, importing(MemoryType { .limits = atLeast(0) }), provided(bytePages)))
          == incompatible);
    CHECK(instantiate(store, executor, importing(bytePages), provided(bytePages)).has_value());
    // Sizes compare in the memory's own pages: 70000 bytes are 70000 pages, more than 2^16.
    auto const large = MemoryType { .limits = atLeast(70'000), .pageSizeLog2 = 0 };
    CHECK(instantiate(store, executor, importing(large), provided(large)).has_value());
}

TEST_CASE("Instantiate.memory64")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    // Evaluates i64.const offsets, which StubExecutor does not.
    auto executor = TrappingExecutor { std::numeric_limits<std::size_t>::max() };
    auto const at64 = [](uint64_t min) {
        return Limits { .min = min, .max = std::nullopt, .addressType = AddressType::I64 };
    };
    SECTION("an import matches by address type")
    {
        auto builder = ModuleBuilder {};
        builder.importMemory("m", "mem", at64(1));
        auto const module = moduleOf(builder, FeatureSet::wasm3());
        auto const memory64 = store.addMemory(MemoryType { .limits = at64(1) });
        auto const memory32 = store.addMemory(MemoryType { .limits = atLeast(1) });
        REQUIRE(memory64.has_value());
        REQUIRE(memory32.has_value());
        auto const of = [](auto address) {
            return std::vector { ExternValue { .kind = ExternKind::Memory, .address = address } };
        };
        CHECK(instantiate(store, executor, module, of(*memory64)).has_value());
        CHECK(errorOf(instantiate(store, executor, module, of(*memory32)))
              == InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });
    }
    SECTION("an i64 data offset")
    {
        auto builder = ModuleBuilder {};
        builder.memory(at64(1)).data({ 0x00, 0x42, 0x05, 0x0B, 0x01, 0x2A });
        auto const result = instantiate(store, executor, moduleOf(builder, FeatureSet::wasm3()), {});
        REQUIRE(result.has_value());
        CHECK(store.memory(store.moduleInstance(*result).memories[0]).bytes()[5] == std::byte { 0x2A });
    }
    SECTION("an i64 data offset past 2^32")
    {
        auto builder = ModuleBuilder {};
        builder.memory(at64(1)).data({ 0x00, 0x42, 0x80, 0x80, 0x80, 0x80, 0x10, 0x0B, 0x01, 0x2A });
        CHECK(errorOf(instantiate(store, executor, moduleOf(builder, FeatureSet::wasm3()), {}))
              == InstantiationError { Trap { TrapCode::OutOfBoundsMemoryAccess } });
    }
    SECTION("a guest asks for more than the store holds")
    {
        // 2^40 pages is a valid memory64 minimum, and 64 PiB; the store refuses before allocating.
        auto builder = ModuleBuilder {};
        builder.memory(at64(uint64_t { 1 } << 40));
        CHECK(errorOf(instantiate(store, executor, moduleOf(builder, FeatureSet::wasm3()), {}))
              == InstantiationError { LimitExceeded { Limit::MemoryBytes } });
        auto table = ModuleBuilder {};
        table.table(TableType { .element = FuncRef, .limits = at64(uint64_t { 1 } << 62) });
        CHECK(errorOf(instantiate(store, executor, moduleOf(table, FeatureSet::wasm3()), {}))
              == InstantiationError { LimitExceeded { Limit::TableElements } });
    }
    SECTION("a demand past 2^64 saturates")
    {
        // Two memories of 2^48 pages, 2^64 bytes each, and two tables of 2^63 elements: each sum
        // overflows a u64, and is refused rather than wrapped below the budget.
        auto memories = ModuleBuilder {};
        memories.memory(at64(uint64_t { 1 } << 48)).memory(at64(uint64_t { 1 } << 48));
        CHECK(errorOf(instantiate(store, executor, moduleOf(memories, FeatureSet::wasm3()), {}))
              == InstantiationError { LimitExceeded { Limit::MemoryBytes } });
        auto tables = ModuleBuilder {};
        tables.table(TableType { .element = FuncRef, .limits = at64(uint64_t { 1 } << 63) })
            .table(TableType { .element = FuncRef, .limits = at64(uint64_t { 1 } << 63) });
        CHECK(errorOf(instantiate(store, executor, moduleOf(tables, FeatureSet::wasm3()), {}))
              == InstantiationError { LimitExceeded { Limit::TableElements } });
        CHECK(store.memoryCount() == 0);
        CHECK(store.tableCount() == 0);
    }
}

namespace
{

/// The export named @p name of the instance at @p address.
ExternValue exportOf(Store const& store, uint32_t address, std::string_view name)
{
    for (auto const& [exported, value]: store.moduleInstance(address).exports)
        if (exported == name)
            return value;
    FAIL("no export " << name);
    return {};
}

/// (ref null $index) of the module's type @p index.
isoline::binary::ValueType refNullTo(uint32_t index)
{
    return isoline::binary::concreteRef(index, isoline::binary::Nullable::Yes);
}

} // namespace

TEST_CASE("Instantiate.typeIdentityAcrossModules")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};
    auto const identity =
        FuncType { .params = { isoline::binary::I32 }, .results = { isoline::binary::I32 } };

    // A: (func (param i32) (result i32)) at type index 0, exported.
    auto a = ModuleBuilder {};
    a.type(identity).function(0, {}, { 0x20, 0x00 }).exportFunction("f", 0);
    auto const aAddress = instantiate(store, interpreter, moduleOf(a), {});
    REQUIRE(aAddress.has_value());
    auto const f = exportOf(store, *aAddress, "f");

    // B: the same type at index 1, imported, put in a table and called through it.
    auto b = ModuleBuilder {};
    b.type(FuncType {})
        .type(identity)
        .type(FuncType { .params = {}, .results = { isoline::binary::I32 } })
        .importFunction("a", "f", 1)
        .table(TableType { .element = FuncRef, .limits = atLeast(1) })
        .element({ 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00 })
        .function(2, {}, { 0x41, 0x07, 0x41, 0x00, 0x11, 0x01, 0x00 }) // call_indirect (type 1) 7 [0]
        .exportFunction("call", 1);
    auto const bAddress = instantiate(store, interpreter, moduleOf(b), std::vector { f });
    REQUIRE(bAddress.has_value());
    auto const& bInstance = store.moduleInstance(*bAddress);
    // Equal types are one TypeId, whatever their index in each module.
    REQUIRE(bInstance.types.size() == 3);
    CHECK(bInstance.types[1] == store.function(f.address).typeId);
    CHECK(bInstance.types[0] != bInstance.types[1]);
    CHECK(store.type(bInstance.types[1]) == identity);

    auto results = std::vector<Value>(1);
    REQUIRE(interpreter.invoke(store, exportOf(store, *bAddress, "call").address, {}, results).has_value());
    CHECK(results[0] == Value { I32 { .bits = 7 } });

    // A type that differs is incompatible, at import as at call_indirect.
    auto c = ModuleBuilder {};
    c.type(FuncType { .params = { isoline::binary::I64 }, .results = { isoline::binary::I32 } })
        .importFunction("a", "f", 0);
    CHECK(errorOf(instantiate(store, interpreter, moduleOf(c), std::vector { f }))
          == InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });
}

TEST_CASE("Instantiate.typesInStoreForm")
{
    // A type naming another names it by its TypeId once interned, so the type (func (param (ref
    // null $t))) of two modules is one, though $t has another index in each.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const wasm3 = FeatureSet::wasm3();
    auto a = ModuleBuilder {};
    a.type(FuncType { .params = { isoline::binary::I32 }, .results = {} })
        .type(FuncType { .params = { refNullTo(0) }, .results = {} });
    auto b = ModuleBuilder {};
    b.type(FuncType {})
        .type(FuncType { .params = { isoline::binary::I32 }, .results = {} })
        .type(FuncType { .params = { refNullTo(1) }, .results = {} });
    auto const aAddress = instantiate(store, executor, moduleOf(a, wasm3), {});
    auto const bAddress = instantiate(store, executor, moduleOf(b, wasm3), {});
    REQUIRE(aAddress.has_value());
    REQUIRE(bAddress.has_value());
    auto const& aTypes = store.moduleInstance(*aAddress).types;
    auto const& bTypes = store.moduleInstance(*bAddress).types;
    CHECK(aTypes[1] == bTypes[2]);
    CHECK(store.type(aTypes[1]).params.at(0) == refNullTo(aTypes[0]));
}

TEST_CASE("Instantiate.tableInitializer")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};
    auto const nonNullFunc =
        isoline::binary::ValueType { .kind = isoline::binary::ValueKind::Ref,
                                     .nullable = isoline::binary::Nullable::No,
                                     .heap = { .kind = isoline::binary::AbstractHeapType::Func } };
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .function(0, {}, {})
        .function(0, {}, {})
        .table(TableType { .element = nonNullFunc, .limits = atLeast(3) }, { 0xD2, 0x01 })
        .exportFunction("g", 1);
    auto const address = instantiate(store, interpreter, moduleOf(builder, FeatureSet::wasm3()), {});
    REQUIRE(address.has_value());
    auto const& instance = store.moduleInstance(*address);
    auto const& elements = store.table(instance.tables.at(0)).elements;
    REQUIRE(elements.size() == 3);
    for (auto const& element: elements)
    {
        CHECK(element.kind == RefKind::Func);
        CHECK(element.address == instance.functions[1]);
    }

    // An initializer is evaluated like any constant expression, and a trap there ends instantiation.
    auto trapping = ModuleBuilder {};
    trapping.table(TableType { .element = FuncRef, .limits = atLeast(1) }, { 0xD0, 0x70 });
    auto executor = TrappingExecutor { 0 };
    CHECK(errorOf(instantiate(store, executor, moduleOf(trapping, FeatureSet::wasm3()), {}))
          == InstantiationError { Trap { TrapCode::Unreachable } });
}

TEST_CASE("Instantiate.importsMatchByStoreTypes")
{
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};
    auto const wasm3 = FeatureSet::wasm3();
    // The exporter's type 1 is (func); the importers declare it at index 0.
    auto exporter = ModuleBuilder {};
    exporter.type(FuncType { .params = { isoline::binary::I32 }, .results = {} })
        .type(FuncType {})
        .global(GlobalType { .type = refNullTo(1), .mutability = Mutability::Const }, { 0xD0, 0x01 })
        .global(GlobalType { .type = refNullTo(1), .mutability = Mutability::Var }, { 0xD0, 0x01 })
        .table(TableType { .element = refNullTo(1), .limits = atLeast(1) })
        .exportEntity("const", ExternKind::Global, 0)
        .exportEntity("var", ExternKind::Global, 1)
        .exportEntity("table", ExternKind::Table, 0);
    auto const exporterAddress = instantiate(store, interpreter, moduleOf(exporter, wasm3), {});
    REQUIRE(exporterAddress.has_value());

    auto const importing = [&](std::string_view name, auto declare) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType {});
        declare(builder);
        auto const result = instantiate(store,
                                        interpreter,
                                        moduleOf(builder, wasm3),
                                        std::vector { exportOf(store, *exporterAddress, name) });
        return result.has_value();
    };
    auto const global = [](isoline::binary::ValueType type, Mutability mutability) {
        return [=](ModuleBuilder& builder) {
            builder.importGlobal("m", "g", GlobalType { .type = type, .mutability = mutability });
        };
    };
    auto const table = [](isoline::binary::ValueType element) {
        return [=](ModuleBuilder& builder) {
            builder.importTable("m", "t", TableType { .element = element, .limits = atLeast(1) });
        };
    };
    // An immutable global by subtyping; a mutable one and a table's elements by equality.
    CHECK(importing("const", global(refNullTo(0), Mutability::Const)));
    CHECK(importing("const", global(FuncRef, Mutability::Const)));
    CHECK_FALSE(importing("const", global(isoline::binary::ExternRef, Mutability::Const)));
    CHECK(importing("var", global(refNullTo(0), Mutability::Var)));
    CHECK_FALSE(importing("var", global(FuncRef, Mutability::Var)));
    CHECK(importing("table", table(refNullTo(0))));
    CHECK_FALSE(importing("table", table(FuncRef)));
}

TEST_CASE("Instantiate.failedInstantiationInternsNoType")
{
    // A module of many distinct types that cannot link, or cannot allocate: each failed attempt
    // leaves the store's types as they were, so retrying it grows nothing.
    auto const distinctTypes = [](ModuleBuilder& builder) {
        for (auto const count: std::views::iota(std::size_t { 0 }, std::size_t { 50 }))
            builder.type(FuncType { .params = std::vector(count, isoline::binary::I32), .results = {} });
    };
    auto unlinkable = ModuleBuilder {};
    distinctTypes(unlinkable);
    unlinkable.importFunction("m", "f", 3);
    auto unallocatable = ModuleBuilder {};
    distinctTypes(unallocatable);
    unallocatable.memory(atLeast(1));

    // The embedder has no room for a page.
    auto allocator = ProvidedMemoryAllocator { 0, 0, grantAll };
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto executor = StubExecutor {};
    auto const host = store.addHostFunction(
        FuncType { .params = { isoline::binary::I64 }, .results = {} },
        [](std::span<Value const>, std::span<Value>) -> std::expected<void, Trap> { return {}; });
    auto const before = store.typeCount();
    auto const imports = std::vector { ExternValue { .kind = ExternKind::Func, .address = host } };
    auto const unlinkableModule = moduleOf(unlinkable, FeatureSet::wasm3());
    auto const unallocatableModule = moduleOf(unallocatable, FeatureSet::wasm3());
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 3))
    {
        CHECK(errorOf(instantiate(store, executor, unlinkableModule, imports))
              == InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });
        CHECK(errorOf(instantiate(store, executor, unallocatableModule, {}))
              == InstantiationError {
                  AllocationFailure { .kind = ExternKind::Memory, .reason = Refusal::Embedder } });
        CHECK(store.typeCount() == before);
    }

    // One that succeeds interns its types, each at the TypeId it was resolved to; a type the store
    // had already, (func (param i64)), keeps its id.
    auto linkable = ModuleBuilder {};
    distinctTypes(linkable);
    linkable.type(FuncType { .params = { isoline::binary::I64 }, .results = {} })
        .type(FuncType { .params = { refNullTo(2) }, .results = {} })
        .type(FuncType { .params = { isoline::binary::I32 }, .results = {} })
        .importFunction("m", "f", 50);
    auto const address = instantiate(store, executor, moduleOf(linkable, FeatureSet::wasm3()), imports);
    REQUIRE(address.has_value());
    auto const& types = store.moduleInstance(*address).types;
    CHECK(store.typeCount() == before + 51);
    CHECK(types[50] == store.function(host).typeId);
    for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 50 }))
        CHECK(store.type(types[index]).params.size() == index);
    CHECK(types[52] == types[1]); // equal types of one module share their TypeId
    CHECK(store.type(types[51]).params.at(0) == refNullTo(types[2]));
}

TEST_CASE("Instantiate.importOfAnUnheldTypeMatchesNothing")
{
    // An importer's type the store does not hold yet resolves to a TypeId no table or global in the
    // store carries, so a table, a mutable global, or an immutable one of a non-bottom type, whose
    // element or value type names it, matches nothing — and interns nothing.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};
    auto const wasm3 = FeatureSet::wasm3();
    auto exporter = ModuleBuilder {};
    exporter.type(FuncType {})
        .global(GlobalType { .type = refNullTo(0), .mutability = Mutability::Const }, { 0xD0, 0x00 })
        .global(GlobalType { .type = refNullTo(0), .mutability = Mutability::Var }, { 0xD0, 0x00 })
        .table(TableType { .element = refNullTo(0), .limits = atLeast(1) })
        .exportEntity("const", ExternKind::Global, 0)
        .exportEntity("var", ExternKind::Global, 1)
        .exportEntity("table", ExternKind::Table, 0);
    auto const exporterAddress = instantiate(store, interpreter, moduleOf(exporter, wasm3), {});
    REQUIRE(exporterAddress.has_value());
    auto const before = store.typeCount();

    // The importer's type 0 is one the store has never seen.
    auto const importing = [&](std::string_view name, auto declare) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = { isoline::binary::F64, isoline::binary::F32 }, .results = {} });
        declare(builder);
        return errorOf(instantiate(store,
                                   interpreter,
                                   moduleOf(builder, wasm3),
                                   std::vector { exportOf(store, *exporterAddress, name) }));
    };
    auto const incompatible = InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } };
    CHECK(importing("table",
                    [](ModuleBuilder& builder) {
                        builder.importTable(
                            "m", "t", TableType { .element = refNullTo(0), .limits = atLeast(1) });
                    })
          == incompatible);
    CHECK(importing("var",
                    [](ModuleBuilder& builder) {
                        builder.importGlobal(
                            "m", "g", GlobalType { .type = refNullTo(0), .mutability = Mutability::Var });
                    })
          == incompatible);
    CHECK(importing("const",
                    [](ModuleBuilder& builder) {
                        builder.importGlobal(
                            "m", "g", GlobalType { .type = refNullTo(0), .mutability = Mutability::Const });
                    })
          == incompatible);
    CHECK(store.typeCount() == before);
}
