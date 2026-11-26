// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
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
#include <isoline/binary/testing/ModuleBuilder.hpp>
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
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Module;
using isoline::binary::Mutability;
using isoline::binary::Opcode;
using isoline::binary::TableType;
using isoline::binary::testing::ModuleBuilder;
using isoline::runtime::AllocationFailure;
using isoline::runtime::ExternValue;
using isoline::runtime::FuncAddr;
using isoline::runtime::I32;
using isoline::runtime::IMemoryAllocator;
using isoline::runtime::InstantiationError;
using isoline::runtime::LinkError;
using isoline::runtime::LinkErrorCode;
using isoline::runtime::PageSize;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Store;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::UnlimitedAllocator;
using isoline::runtime::Value;

namespace
{

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

/// Grants memories and tables up to a fixed number of bytes.
class CappedAllocator final: public IMemoryAllocator
{
  public:
    explicit CappedAllocator(uint64_t capacity): _capacity { capacity } {}

    [[nodiscard]] bool mayHold(uint64_t bytes) const noexcept override { return bytes <= _capacity; }

  private:
    uint64_t _capacity;
};

/// Evaluates i32.const only, and records which functions it is asked to invoke.
class StubExecutor final: public isoline::runtime::ICodeExecutor
{
  public:
    std::vector<FuncAddr> invoked; ///< Every function invoked, in order.

    [[nodiscard]] std::expected<void, Trap> invoke(Store& /*store*/,
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

    [[nodiscard]] std::expected<void, Trap> invoke(Store& /*store*/,
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
std::shared_ptr<Module const> moduleOf(ModuleBuilder const& builder,
                                       FeatureSet const& features = FeatureSet::wasm2())
{
    auto const bytes = builder.build();
    auto module = isoline::binary::decodeModule(bytes, features);
    REQUIRE(module.has_value());
    REQUIRE(isoline::validate::validateModule(*module, features).has_value());
    return std::make_shared<Module const>(std::move(*module));
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

/// The bytes of i32.const @p value; a one-byte signed LEB128, so @p value must be below 64.
std::vector<uint8_t> i32Const(uint8_t value)
{
    return { 0x41, value };
}

/// The error of @p result, which must have failed.
template <typename T>
InstantiationError errorOf(std::expected<T, InstantiationError> const& result)
{
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

} // namespace

TEST_CASE("Instantiate.unknownImport")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("m", "f", 0);
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto executor = StubExecutor {};
    auto const result = instantiate(store, executor, moduleOf(builder), {});
    CHECK(errorOf(result) == InstantiationError { LinkError { LinkErrorCode::UnknownImport, 0 } });
}

TEST_CASE("Instantiate.incompatibleImportType")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("m", "f", 0);
    auto const module = moduleOf(builder);
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
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
    SECTION("a tag, which P1 has no instances of to link")
    {
        // The decoder reads no tag import, but a module built by hand may hold one.
        auto tagged = std::make_shared<Module>();
        tagged->types.push_back(FuncType {});
        tagged->imports.push_back(isoline::binary::Import { .module = "m",
                                                            .name = "t",
                                                            .kind = ExternKind::Tag,
                                                            .typeIndex = 0,
                                                            .table = {},
                                                            .memory = {},
                                                            .global = {} });
        auto const imports = std::vector { ExternValue { .kind = ExternKind::Tag, .address = 0 } };
        CHECK(errorOf(instantiate(store, executor, tagged, imports)) == incompatible);
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
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
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
        REQUIRE(store.growMemory(imports.front().address, 1) == 1);
        CHECK(instantiate(store, executor, importing(atLeast(2)), imports).has_value());
    }
}

TEST_CASE("Instantiate.tableAndGlobalImports")
{
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
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
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
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
    REQUIRE(module->data[1].offset.front().c == 65'535);
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto executor = StubExecutor {};
    auto const result = instantiate(store, executor, module, {});
    CHECK(errorOf(result) == InstantiationError { Trap { TrapCode::OutOfBoundsMemoryAccess } });
    // The instance stays in the store, and the segment before the faulty one is written.
    auto const& memory = store.memory(0);
    REQUIRE(memory.bytes.size() == PageSize);
    CHECK(memory.bytes[0] == std::byte { 0xAA });
    CHECK(memory.bytes[1] == std::byte { 0xBB });
    CHECK(memory.bytes[65'535] == std::byte { 0x00 });
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
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
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
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
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
    CHECK(store.memory(instance.memories[0]).bytes[3] == std::byte { 0x11 });
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
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
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
    auto const allocator = CappedAllocator { 2 * PageSize };
    auto store = Store { allocator };
    auto executor = StubExecutor {};
    CHECK(errorOf(instantiate(store, executor, moduleOf(builder), {}))
          == InstantiationError { AllocationFailure { .kind = ExternKind::Memory } });
}

TEST_CASE("Instantiate.tableAllocationRefused")
{
    auto builder = ModuleBuilder {};
    builder.table(TableType { .element = FuncRef, .limits = atLeast(10) });
    auto const allocator = CappedAllocator { 5 * sizeof(Ref) };
    auto store = Store { allocator };
    auto executor = StubExecutor {};
    CHECK(errorOf(instantiate(store, executor, moduleOf(builder), {}))
          == InstantiationError { AllocationFailure { .kind = ExternKind::Table } });

    // The failed instantiation registered no instance: the next one takes the first address.
    auto const next = instantiate(store, executor, moduleOf(ModuleBuilder {}), {});
    REQUIRE(next.has_value());
    CHECK(*next == 0);
}

TEST_CASE("Instantiate.memoryGrowToFourGiB")
{
    /// Grants a single page at most, and records each size it is asked about.
    class RecordingAllocator final: public IMemoryAllocator
    {
      public:
        [[nodiscard]] bool mayHold(uint64_t bytes) const noexcept override
        {
            asked.push_back(bytes);
            return bytes <= PageSize;
        }

        mutable std::vector<uint64_t> asked; ///< Every size asked about, in order.
    };
    auto const allocator = RecordingAllocator {};
    auto store = Store { allocator };
    auto const memory = store.addMemory(MemoryType { .limits = atLeast(1) });
    REQUIRE(memory.has_value());
    allocator.asked.clear();

    // Growing to exactly 65536 pages, the reach of a 32-bit address, asks the allocator for 4 GiB.
    CHECK(store.growMemory(*memory, 65'535) == -1);
    CHECK(allocator.asked == std::vector { 65'536 * PageSize });

    // One page further is beyond the reach, and refused without asking.
    CHECK(store.growMemory(*memory, 65'536) == -1);
    CHECK(allocator.asked.size() == 1);

    // The unlimited allocator would grant exactly those 4 GiB, and not a byte more.
    auto const unlimited = UnlimitedAllocator {};
    CHECK(unlimited.mayHold(65'536 * PageSize));
    CHECK_FALSE(unlimited.mayHold((65'536 * PageSize) + 1));
}

TEST_CASE("Instantiate.memoryGrowFailureReturnsMinusOne")
{
    auto const allocator = CappedAllocator { 2 * PageSize };
    auto store = Store { allocator };
    auto const memory = store.addMemory(MemoryType { .limits = atLeast(1) });
    REQUIRE(memory.has_value());

    CHECK(store.growMemory(*memory, 5) == -1);
    CHECK(store.memory(*memory).bytes.size() == PageSize);
    CHECK(store.growMemory(*memory, 0) == 1);
    CHECK(store.growMemory(*memory, 1) == 1);
    CHECK(store.memory(*memory).bytes.size() == 2 * PageSize);
    CHECK(store.growMemory(*memory, 1) == -1);

    SECTION("above the maximum")
    {
        auto const unlimited = UnlimitedAllocator {};
        auto bounded = Store { unlimited };
        auto const address = bounded.addMemory(MemoryType { .limits = between(1, 2) });
        REQUIRE(address.has_value());
        CHECK(bounded.growMemory(*address, 2) == -1);
        CHECK(bounded.growMemory(*address, 1) == 1);
        CHECK(bounded.growMemory(*address, std::numeric_limits<uint64_t>::max()) == -1);
    }
    SECTION("beyond what a vector can hold")
    {
        class Unbounded final: public IMemoryAllocator
        {
          public:
            [[nodiscard]] bool mayHold(uint64_t /*bytes*/) const noexcept override { return true; }
        };
        auto const unbounded = Unbounded {};
        auto huge = Store { unbounded };
        auto const address = huge.addMemory(MemoryType {
            .limits = Limits { .min = 0, .max = std::nullopt, .addressType = AddressType::I64 } });
        REQUIRE(address.has_value());
        CHECK(huge.growMemory(*address, (uint64_t { 1 } << 47) + 1) == -1);
        CHECK(huge.memory(*address).bytes.empty());
    }
}

TEST_CASE("Instantiate.tableGrow")
{
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const table = store.addTable(TableType { .element = FuncRef, .limits = between(1, 3) }, Ref {});
    REQUIRE(table.has_value());
    auto const init = Ref { .kind = RefKind::Func, .heap = FuncRef.heap, .address = 7 };
    CHECK(store.growTable(*table, 3, init) == -1);
    CHECK(store.growTable(*table, 2, init) == 1);
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
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
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
    auto const allocator = UnlimitedAllocator {};
    for (auto const trapAt: { 0U, 1U, 2U, 3U })
    {
        auto store = Store { allocator };
        auto executor = TrappingExecutor { trapAt };
        CAPTURE(trapAt);
        CHECK(errorOf(instantiate(store, executor, module, {}))
              == InstantiationError { Trap { TrapCode::Unreachable } });
    }
    auto store = Store { allocator };
    auto executor = TrappingExecutor { 4 };
    CHECK(instantiate(store, executor, module, {}).has_value());
}

TEST_CASE("Instantiate.tagImportIsIncompatible")
{
    // Wasm 2.0 has no tags, so the import is built as data; no tag an embedder supplies matches it.
    auto module = Module { *moduleOf(ModuleBuilder {}) };
    module.imports.push_back(isoline::binary::Import { .module = "m", .name = "t", .kind = ExternKind::Tag });
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto executor = StubExecutor {};
    auto const imports = std::vector { ExternValue { .kind = ExternKind::Tag, .address = 0 } };
    auto const result =
        instantiate(store, executor, std::make_shared<Module const>(std::move(module)), imports);
    CHECK(errorOf(result) == InstantiationError { LinkError { LinkErrorCode::IncompatibleImportType, 0 } });
}

TEST_CASE("Instantiate.i64DataOffset")
{
    // A memory of i64 addresses (memory64), which no Wasm 2.0 decoder yields: its data segment's
    // offset is an i64.
    auto builder = ModuleBuilder {};
    builder.memory(atLeast(1)).data(activeData(3, { 0x2A }));
    auto module = Module { *moduleOf(builder) };
    module.memories[0].limits.addressType = AddressType::I64;
    module.data[0].offset.front() = isoline::binary::Instruction { .opcode = Opcode::I64Const, .c = 5 };
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto executor = TrappingExecutor { std::numeric_limits<std::size_t>::max() };
    auto const result = instantiate(store, executor, std::make_shared<Module const>(std::move(module)), {});
    REQUIRE(result.has_value());
    auto const& memory = store.memory(store.moduleInstance(*result).memories[0]);
    CHECK(memory.bytes[5] == std::byte { 0x2A });
    CHECK(memory.bytes[3] == std::byte { 0 });
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
}

TEST_CASE("Instantiate.specMessages")
{
    using isoline::runtime::LinkErrorCode;
    using isoline::runtime::specMessage;
    CHECK(specMessage(LinkErrorCode::UnknownImport) == "unknown import");
    CHECK(specMessage(LinkErrorCode::IncompatibleImportType) == "incompatible import type");
    CHECK(specMessage(AllocationFailure { .kind = ExternKind::Memory }) == "out of memory");
}

TEST_CASE("Instantiate.memory64")
{
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
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
        CHECK(store.memory(store.moduleInstance(*result).memories[0]).bytes[5] == std::byte { 0x2A });
    }
    SECTION("an i64 data offset past 2^32")
    {
        auto builder = ModuleBuilder {};
        builder.memory(at64(1)).data({ 0x00, 0x42, 0x80, 0x80, 0x80, 0x80, 0x10, 0x0B, 0x01, 0x2A });
        CHECK(errorOf(instantiate(store, executor, moduleOf(builder, FeatureSet::wasm3()), {}))
              == InstantiationError { Trap { TrapCode::OutOfBoundsMemoryAccess } });
    }
    SECTION("a guest asks for more than the allocator grants")
    {
        // 2^40 pages is a valid memory64 minimum, and 64 PiB; the store refuses before allocating.
        auto builder = ModuleBuilder {};
        builder.memory(at64(uint64_t { 1 } << 40));
        CHECK(errorOf(instantiate(store, executor, moduleOf(builder, FeatureSet::wasm3()), {}))
              == InstantiationError { AllocationFailure { .kind = ExternKind::Memory } });
        auto table = ModuleBuilder {};
        table.table(TableType { .element = FuncRef, .limits = at64(uint64_t { 1 } << 62) });
        CHECK(errorOf(instantiate(store, executor, moduleOf(table, FeatureSet::wasm3()), {}))
              == InstantiationError { AllocationFailure { .kind = ExternKind::Table } });
    }
}
