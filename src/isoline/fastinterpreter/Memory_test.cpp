// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/FastInterpreter.hpp>
#include <isoline/fastinterpreter/Operations.hpp>
#include <isoline/fastinterpreter/testing/Engines.hpp>
#include <isoline/fastinterpreter/testing/Lowering.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::binary::AddressType;
using isoline::binary::ExternKind;
using isoline::binary::FuncType;
using isoline::binary::Limits;
using isoline::binary::MemoryAccess;
using isoline::binary::MemoryAccessKind;
using isoline::binary::MemoryAccessTable;
using isoline::binary::MemoryType;
using isoline::binary::Opcode;
using isoline::binary::ValueKind;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::i64Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::ModuleBuilder;
using isoline::fastinterpreter::availableDispatches;
using isoline::fastinterpreter::FastInterpreter;
using isoline::fastinterpreter::LoweringConfig;
using isoline::fastinterpreter::testing::EngineUnderTest;
using isoline::fastinterpreter::testing::fast;
using isoline::fastinterpreter::testing::fastEngineOfEachDispatch;
using isoline::fastinterpreter::testing::Harness;
using isoline::fastinterpreter::testing::lowered;
using isoline::fastinterpreter::testing::memoryInstruction;
using isoline::fastinterpreter::testing::op;
using isoline::fastinterpreter::testing::Reference;
using isoline::fastinterpreter::testing::referenceAndFastEngineOfEachDispatch;
using isoline::fastinterpreter::testing::validated;
using isoline::runtime::Abrupt;
using isoline::runtime::ExternValue;
using isoline::runtime::FuncAddr;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::HostContext;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::ILinearMemory;
using isoline::runtime::IMemoryAllocator;
using isoline::runtime::makeHostFunction;
using isoline::runtime::MemoryRequest;
using isoline::runtime::Refusal;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;

namespace binary = isoline::binary;

namespace
{

/// What a host function returns.
using Outcome = std::expected<void, Abrupt>;

/// What invoking a function gave: its results, or how it ended abruptly.
using Results = std::expected<std::vector<Value>, Abrupt>;

/// The bytes of a 64 KiB page.
constexpr auto PageBytes = uint64_t { 65'536 };

/// memory.grow of @p memory, dropping its result.
[[nodiscard]] Bytes growAndDrop(uint32_t memory)
{
    return code({ { 0x40 }, encodeU32(memory), op(0x1A) });
}

/// A constant of @p kind: i32 and i64 of @p value, f32 and f64 of its low bits.
[[nodiscard]] Bytes constantOf(ValueKind kind, uint64_t value)
{
    switch (kind)
    {
        case ValueKind::I32: return i32Const(static_cast<int32_t>(static_cast<uint32_t>(value)));
        case ValueKind::I64: return i64Const(static_cast<int64_t>(value));
        case ValueKind::F32: {
            auto bytes = Bytes { 0x43 };
            for (auto const index: std::views::iota(0U, 4U))
                bytes.push_back(static_cast<uint8_t>(value >> (8U * index)));
            return bytes;
        }
        default: {
            REQUIRE(kind == ValueKind::F64);
            auto bytes = Bytes { 0x44 };
            for (auto const index: std::views::iota(0U, 8U))
                bytes.push_back(static_cast<uint8_t>(value >> (8U * index)));
            return bytes;
        }
    }
}

/// The address constant of a memory of @p type.
[[nodiscard]] Bytes addressOf(AddressType type, uint64_t address)
{
    return type == AddressType::I64 ? i64Const(static_cast<int64_t>(address))
                                    : i32Const(static_cast<int32_t>(static_cast<uint32_t>(address)));
}

/// An active data segment writing @p bytes to memory @p memory, of address type @p type, at @p offset.
[[nodiscard]] Bytes dataSegment(uint32_t memory, AddressType type, uint64_t offset, Bytes const& bytes)
{
    auto const prefix = memory == 0 ? Bytes { 0x00 } : code({ { 0x02 }, encodeU32(memory) });
    return code(
        { prefix, addressOf(type, offset), { 0x0B }, encodeU32(static_cast<uint32_t>(bytes.size())), bytes });
}

/// A memory's storage that moves on every grow: the bytes are copied into a new buffer, and the old one is
/// kept, filled with RetiredByte, so that code still reading it reads that, and ASan sees no use after free.
class MovingMemory final: public ILinearMemory
{
  public:
    /// What a retired buffer holds.
    static constexpr auto RetiredByte = std::byte { 0xA5 };

    explicit MovingMemory(uint64_t size): _buffers(1, std::vector<std::byte>(static_cast<std::size_t>(size)))
    {
    }

    [[nodiscard]] std::span<std::byte> bytes() noexcept override { return _buffers.back(); }

    [[nodiscard]] std::span<std::byte const> bytes() const noexcept override { return _buffers.back(); }

    [[nodiscard]] uint64_t guardBytes() const noexcept override { return 0; }

    [[nodiscard]] std::expected<void, Refusal> grow(uint64_t newBytes) noexcept override
    {
        auto moved = std::vector<std::byte>(static_cast<std::size_t>(newBytes));
        std::ranges::copy(_buffers.back(), moved.begin());
        std::ranges::fill(_buffers.back(), RetiredByte);
        _buffers.push_back(std::move(moved));
        return {};
    }

    /// How many times it moved.
    [[nodiscard]] std::size_t moves() const noexcept { return _buffers.size() - 1; }

  private:
    std::vector<std::vector<std::byte>> _buffers; ///< Every buffer it had; the last is the current one.
};

/// Hands out MovingMemory: an embedder whose memories move whenever they grow.
class MovingMemoryAllocator final: public IMemoryAllocator
{
  public:
    [[nodiscard]] std::expected<std::unique_ptr<ILinearMemory>, Refusal> allocate(
        MemoryRequest const& request) noexcept override
    {
        auto memory = std::make_unique<MovingMemory>(request.initialBytes);
        _memories.push_back(memory.get());
        return memory;
    }

    /// The moves of every memory it handed out.
    [[nodiscard]] std::size_t moves() const noexcept
    {
        auto total = std::size_t { 0 };
        for (auto const* memory: _memories)
            total += memory->moves();
        return total;
    }

  private:
    std::vector<MovingMemory const*> _memories;
};

/// Where memory 0 grows while the guest's function runs.
enum class GrowPath : uint8_t
{
    InFunction, ///< memory.grow in the function itself.
    InCallee,   ///< memory.grow in a Wasm function it calls.
    InHost,     ///< A host function it calls grows the memory through the store.
    InReentry,  ///< A host function it calls invokes a Wasm function that runs memory.grow.
};

/// Every GrowPath, with its name.
[[nodiscard]] std::vector<std::pair<GrowPath, std::string>> growPaths()
{
    return { { GrowPath::InFunction, "in the function" },
             { GrowPath::InCallee, "in a callee" },
             { GrowPath::InHost, "in a host function" },
             { GrowPath::InReentry, "in a host function's re-entry" } };
}

/// What stores the memories come from.
enum class Storage : uint8_t
{
    Heap,   ///< The store's default heap memory, which may or may not move.
    Moving, ///< An embedder's memory that moves on every grow (MovingMemory).
};

/// A module whose memory 0 has one page of up to four, and whose function 1, () -> i32, stores
/// 0x11223344 at 8, grows memory 0 by a page along @p path, stores 0x55667788 at the old size, and returns
/// what it loads from 8. Function 0 is the imported host function `env.grow`, () -> (); function 2,
/// () -> (), grows memory 0 by a page.
[[nodiscard]] ModuleBuilder growingModule(GrowPath path)
{
    auto module = ModuleBuilder {};
    module.type(FuncType { .params = {}, .results = { binary::I32 } });
    module.type(FuncType {});
    module.importFunction("env", "grow", 1);
    module.memory(Limits { .min = 1, .max = 4, .addressType = AddressType::I32 });
    auto grow = Bytes {};
    switch (path)
    {
        case GrowPath::InFunction: grow = code({ i32Const(1), growAndDrop(0) }); break;
        case GrowPath::InCallee: grow = op(0x10, 2); break;
        case GrowPath::InHost:
        case GrowPath::InReentry: grow = op(0x10, 0); break;
    }
    module.function(0,
                    {},
                    code({ i32Const(8),
                           i32Const(0x1122'3344),
                           memoryInstruction(Opcode::I32Store, 0),
                           grow,
                           i32Const(static_cast<int32_t>(PageBytes)),
                           i32Const(0x5566'7788),
                           memoryInstruction(Opcode::I32Store, 0),
                           i32Const(8),
                           memoryInstruction(Opcode::I32Load, 0) }));
    module.function(1, {}, code({ i32Const(1), growAndDrop(0) }));
    return module;
}

/// The little-endian 32-bit value at @p address of @p bytes.
[[nodiscard]] uint32_t wordAt(std::span<std::byte const> bytes, uint64_t address)
{
    REQUIRE(address + 4 <= bytes.size());
    auto value = uint32_t { 0 };
    for (auto const index: std::views::iota(0U, 4U))
        value |= uint32_t { std::to_integer<uint8_t>(bytes[address + index]) } << (8U * index);
    return value;
}

/// The function @p index of the module instance at @p instance.
[[nodiscard]] FuncAddr exportedFunction(Store const& store, uint32_t instance, uint32_t index)
{
    return store.moduleInstance(instance).functions.at(index);
}

/// Several modules instantiated on one store, by one engine, the later ones importing from the earlier.
class Linked
{
  public:
    explicit Linked(EngineUnderTest engine):
        _store { StoreLimits {}, StoreServices { .allocator = _allocator } }, _engine { engine }
    {
        if (engine.dispatch)
            _executor = std::make_unique<FastInterpreter>(*engine.dispatch);
        else
            _executor = std::make_unique<isoline::interpreter::ReferenceInterpreter>();
    }

    /// Instantiates @p builder's module with @p imports.
    /// @return Its instance's address.
    uint32_t instantiate(ModuleBuilder const& builder, std::vector<ExternValue> const& imports = {})
    {
        auto const module = validated(builder);
        auto const code =
            _engine.dispatch ? lowered(module, LoweringConfig { .dispatch = *_engine.dispatch }) : nullptr;
        auto const address = isoline::runtime::instantiate(_store, *_executor, module, code, imports);
        REQUIRE(address.has_value());
        return *address;
    }

    /// Invokes function @p index of instance @p instance with @p args.
    [[nodiscard]] Results outcome(uint32_t instance, uint32_t index, std::vector<Value> const& args = {})
    {
        auto const address = exportedFunction(_store, instance, index);
        auto results = std::vector<Value>(_store.functionType(address).results.size());
        return isoline::runtime::invokeChecked(_store, *_executor, address, args, results)
            .outcome.transform([&] { return results; });
    }

    /// Memory @p index of instance @p instance.
    [[nodiscard]] std::span<std::byte const> memory(uint32_t instance, uint32_t index) const
    {
        return std::as_const(_store).memory(_store.moduleInstance(instance).memories.at(index)).bytes();
    }

    [[nodiscard]] Store& store() noexcept { return _store; }

  private:
    HeapMemoryAllocator _allocator;
    Store _store;
    EngineUnderTest _engine;
    std::unique_ptr<isoline::runtime::ICodeExecutor> _executor;
};

/// How a cross-instance call reaches the other instance's export.
enum class CrossCall : uint8_t
{
    Call,       ///< call of the import.
    ReturnCall, ///< call of a function of its own that return_calls the import.
};

/// Every scalar row of MemoryAccessTable: the accesses the fast interpreter lowers to its own operations.
[[nodiscard]] std::vector<MemoryAccess> scalarAccesses()
{
    auto rows = std::vector<MemoryAccess> {};
    for (auto const& row: MemoryAccessTable)
        if (row.value != ValueKind::V128)
            rows.push_back(row);
    return rows;
}

/// Where an access falls.
enum class Bounds : uint8_t
{
    Outside, ///< Past the memory's end: it traps.
    Inside,  ///< Within the memory.
};

/// One bounds check at the edge of a memory.
struct EdgeCase
{
    std::string name;                ///< What it checks.
    MemoryType memory;               ///< The memory accessed.
    uint32_t memoryIndex = 0;        ///< Its index: 0, or 1 after a memory 0 of one page.
    Opcode opcode {};                ///< The access.
    uint64_t address = 0;            ///< The dynamic address.
    uint64_t offset = 0;             ///< The static offset.
    Bounds bounds = Bounds::Outside; ///< Whether it is in bounds.
};

/// The memory of @p pages pages of 2^@p pageSizeLog2 bytes, of @p type addresses.
[[nodiscard]] MemoryType memoryOf(AddressType type, uint64_t pages, uint8_t pageSizeLog2 = 16)
{
    return MemoryType { .limits = Limits { .min = pages, .max = std::nullopt, .addressType = type },
                        .pageSizeLog2 = pageSizeLog2 };
}

/// The edge cases: every scalar access at the last address it fits and the first it does not, by address
/// and by offset, of both address types, in memory 0 and in memory 1; then the overflowing sums, and memories
/// of custom page sizes and of no pages.
[[nodiscard]] std::vector<EdgeCase> edgeCases()
{
    auto cases = std::vector<EdgeCase> {};
    auto const max = std::numeric_limits<uint64_t>::max();
    for (auto const& row: scalarAccesses())
    {
        for (auto const type: { AddressType::I32, AddressType::I64 })
        {
            for (auto const memoryIndex: { 0U, 1U })
            {
                auto const memory = memoryOf(type, 1);
                auto const text = std::format("{} in memory {} of {} addresses",
                                              binary::opcodeInfo(row.opcode).text,
                                              memoryIndex,
                                              type == AddressType::I64 ? "i64" : "i32");
                auto const last = PageBytes - row.width;
                auto add = [&](std::string what, uint64_t address, uint64_t offset, Bounds bounds) {
                    cases.push_back(EdgeCase { .name = std::format("{}: {}", text, what),
                                               .memory = memory,
                                               .memoryIndex = memoryIndex,
                                               .opcode = row.opcode,
                                               .address = address,
                                               .offset = offset,
                                               .bounds = bounds });
                };
                add("the last address", last, 0, Bounds::Inside);
                add("one past the last address", last + 1, 0, Bounds::Outside);
                add("the last offset", 0, last, Bounds::Inside);
                add("one past the last offset", 0, last + 1, Bounds::Outside);
                add("address and offset at the end", last - 3, 3, Bounds::Inside);
                add("address and offset past the end", last - 3, 4, Bounds::Outside);
            }
        }
    }
    auto const add = [&](std::string name,
                         MemoryType memory,
                         Opcode opcode,
                         uint64_t address,
                         uint64_t offset,
                         Bounds bounds) {
        cases.push_back(EdgeCase { .name = std::move(name),
                                   .memory = memory,
                                   .memoryIndex = 0,
                                   .opcode = opcode,
                                   .address = address,
                                   .offset = offset,
                                   .bounds = bounds });
    };
    auto const memory32 = memoryOf(AddressType::I32, 1);
    auto const memory64 = memoryOf(AddressType::I64, 1);
    add("i32 address and offset at their largest",
        memory32,
        Opcode::I32Load,
        0xFFFF'FFFF,
        0xFFFF'FFFF,
        Bounds::Outside);
    add("i32.load at size - 4", memory32, Opcode::I32Load, PageBytes - 4, 0, Bounds::Inside);
    add("i32.load at size - 3", memory32, Opcode::I32Load, PageBytes - 3, 0, Bounds::Outside);
    add("memory64: offset 2^64-1 and address 1", memory64, Opcode::I32Load, 1, max, Bounds::Outside);
    add("memory64: offset 2^64-1 and address 0", memory64, Opcode::I32Load8U, 0, max, Bounds::Outside);
    add("memory64: address 2^64-1", memory64, Opcode::I32Load8U, max, 0, Bounds::Outside);
    add("memory64: address and offset 2^63, summing to 2^64",
        memory64,
        Opcode::I64Load,
        uint64_t { 1 } << 63U,
        uint64_t { 1 } << 63U,
        Bounds::Outside);
    add("memory64: address 2^64-4 and offset 4, summing to 0",
        memory64,
        Opcode::I32Store,
        max - 3,
        4,
        Bounds::Outside);
    add("memory64: offset 2^64-4 and a width of 8", memory64, Opcode::I64Store, 0, max - 3, Bounds::Outside);
    // Pages of one byte: ten of them.
    auto const bytePages = memoryOf(AddressType::I32, 10, 0);
    add("page size 1: i32.load at 6", bytePages, Opcode::I32Load, 6, 0, Bounds::Inside);
    add("page size 1: i32.load at 7", bytePages, Opcode::I32Load, 7, 0, Bounds::Outside);
    add("page size 1: i64.load8_u at 9", bytePages, Opcode::I64Load8U, 9, 0, Bounds::Inside);
    add("page size 1: i64.load8_u at 10", bytePages, Opcode::I64Load8U, 10, 0, Bounds::Outside);
    add("page size 1: i64.store at 2", bytePages, Opcode::I64Store, 0, 2, Bounds::Inside);
    add("page size 1: i64.store at 3", bytePages, Opcode::I64Store, 0, 3, Bounds::Outside);
    auto const bytePages64 = memoryOf(AddressType::I64, 10, 0);
    add("page size 1, memory64: i32.load at 6", bytePages64, Opcode::I32Load, 6, 0, Bounds::Inside);
    add("page size 1, memory64: i32.load at 7", bytePages64, Opcode::I32Load, 3, 4, Bounds::Outside);
    add("page size 1, memory64: offset 2^64-1", bytePages64, Opcode::I32Load8S, 1, max, Bounds::Outside);
    // Pages of 64 KiB, the page size declared explicitly.
    auto const wholePages = memoryOf(AddressType::I32, 2, 16);
    add("page size 65536: i64.load at the end",
        wholePages,
        Opcode::I64Load,
        (2 * PageBytes) - 8,
        0,
        Bounds::Inside);
    add("page size 65536: i64.load past the end",
        wholePages,
        Opcode::I64Load,
        (2 * PageBytes) - 7,
        0,
        Bounds::Outside);
    // A memory of no pages: nothing is in bounds.
    add("no pages: i32.load8_u at 0",
        memoryOf(AddressType::I32, 0),
        Opcode::I32Load8U,
        0,
        0,
        Bounds::Outside);
    add("no pages, memory64: i32.store8 at 0",
        memoryOf(AddressType::I64, 0),
        Opcode::I32Store8,
        0,
        0,
        Bounds::Outside);
    return cases;
}

/// The bytes the edge cases' memories start with: 0x80 to 0x8F, so that a narrow load's sign shows.
[[nodiscard]] Bytes patternOf(std::size_t size)
{
    auto bytes = Bytes {};
    for (auto const index: std::views::iota(std::size_t { 0 }, size))
        bytes.push_back(static_cast<uint8_t>(0x80U + (index % 16U)));
    return bytes;
}

/// A module of @p edge's memory, at its index after a memory 0 of one page, its bytes patternOf() at its
/// start and its end, and of one function, (address) -> (value) for a load or (address) -> () for a store
/// of an all-ones-but-the-top value, doing @p edge's access.
[[nodiscard]] ModuleBuilder edgeModule(EdgeCase const& edge)
{
    auto const* row = binary::memoryAccessOf(edge.opcode);
    REQUIRE(row != nullptr);
    auto const addressType = edge.memory.limits.addressType;
    auto const addressKind = addressType == AddressType::I64 ? binary::I64 : binary::I32;
    auto module = ModuleBuilder {};
    auto const load = row->access == MemoryAccessKind::Load;
    module.type(FuncType { .params = { addressKind },
                           .results = load ? std::vector { binary::ValueType { .kind = row->value } }
                                           : std::vector<binary::ValueType> {} });
    if (edge.memoryIndex != 0)
        module.memory(memoryOf(AddressType::I32, 1));
    module.memory(edge.memory);
    auto const size = edge.memory.limits.min << edge.memory.pageSizeLog2;
    auto const pattern = std::min<uint64_t>(size, 16);
    if (pattern > 0)
    {
        module.data(dataSegment(edge.memoryIndex, addressType, 0, patternOf(pattern)));
        module.data(dataSegment(edge.memoryIndex, addressType, size - pattern, patternOf(pattern)));
    }
    auto const value = load ? Bytes {} : constantOf(row->value, 0x7EDC'BA98'7654'3210);
    module.function(
        0, {}, code({ localGet(0), value, memoryInstruction(edge.opcode, edge.offset, edge.memoryIndex) }));
    return module;
}

/// The address argument of @p edge.
[[nodiscard]] Value addressArgument(EdgeCase const& edge)
{
    if (edge.memory.limits.addressType == AddressType::I64)
        return I64 { edge.address };
    return I32 { static_cast<uint32_t>(edge.address) };
}

/// What running an edge case gave: its results, or how it ended, and the memory's bytes after it.
struct EdgeOutcome
{
    Results results;              ///< Its results, or how it ended abruptly.
    std::vector<std::byte> bytes; ///< The accessed memory's bytes after it.

    [[nodiscard]] bool operator==(EdgeOutcome const&) const = default;
};

/// Runs @p edge on @p engine.
[[nodiscard]] EdgeOutcome runEdge(EdgeCase const& edge, EngineUnderTest engine)
{
    auto harness = Harness { edgeModule(edge), engine };
    auto results = harness.outcome(0, { addressArgument(edge) });
    auto const& instance = harness.instance();
    auto const bytes = std::as_const(harness.store()).memory(instance.memories.at(edge.memoryIndex)).bytes();
    return EdgeOutcome { .results = std::move(results), .bytes = { bytes.begin(), bytes.end() } };
}

/// The fast interpreter that runs @p harness's module.
[[nodiscard]] FastInterpreter const& fastInterpreterOf(Harness& harness)
{
    auto const* interpreter = dynamic_cast<FastInterpreter const*>(&harness.executor());
    REQUIRE(interpreter != nullptr);
    return *interpreter;
}

} // namespace

TEST_CASE("Memory.cacheRefreshedAfterEveryGrowPath")
{
    // Review Focus 3: whichever way memory 0 grows while a function runs, the function's next access sees
    // the grown memory, at its new size and, where the storage moved, at its new place: a store at the old
    // size lands, and a load of what was stored before the grow reads it back, not a moved-away buffer.
    auto const engine = referenceAndFastEngineOfEachDispatch();
    auto const [path, pathName] = GENERATE(from_range(growPaths()));
    auto const storage = GENERATE(Storage::Heap, Storage::Moving);
    INFO(pathName << (storage == Storage::Moving ? ", on a memory that moves" : ", on the heap"));
    auto heap = HeapMemoryAllocator {};
    auto moving = MovingMemoryAllocator {};
    auto& allocator = storage == Storage::Moving ? static_cast<IMemoryAllocator&>(moving) : heap;
    Harness* self = nullptr;
    auto harness =
        Harness { growingModule(path),
                  engine,
                  [&self, path](Store& store) {
                      auto const host = store.addHostFunction(
                          FuncType {},
                          makeHostFunction([&self, &store, path](HostContext& context,
                                                                 std::span<Value const>,
                                                                 std::span<Value>) noexcept -> Outcome {
                              if (path == GrowPath::InReentry)
                                  return context.invoke(self->function(2), {}, {});
                              if (!store.growMemory(self->instance().memories.at(0), 1))
                                  return std::unexpected { Abrupt { Trap { TrapCode::HostTrap } } };
                              return {};
                          }));
                      REQUIRE(host.has_value());
                      return std::vector { ExternValue { .kind = ExternKind::Func, .address = *host } };
                  },
                  allocator };
    self = &harness;
    auto const results = harness.outcome(1);
    REQUIRE(results.has_value());
    REQUIRE(results->size() == 1);
    CHECK(std::get<I32>(results->front()).bits == 0x1122'3344U);
    auto const bytes = std::as_const(harness.store()).memory(harness.instance().memories.at(0)).bytes();
    CHECK(bytes.size() == 2 * PageBytes);
    CHECK(wordAt(bytes, PageBytes) == 0x5566'7788U);
    if (storage == Storage::Moving)
        CHECK(moving.moves() == 1);
}

TEST_CASE("Memory.crossInstanceCallSwitchesMemory0")
{
    // Instance A calls B's export, which stores to B's memory 0 and loads it back; A then loads its own: each
    // sees its own bytes, A's at an address B's smaller memory does not have.
    auto const engine = referenceAndFastEngineOfEachDispatch();
    auto const crossCall = GENERATE(CrossCall::Call, CrossCall::ReturnCall);
    INFO((crossCall == CrossCall::Call ? "call" : "return_call"));
    auto linked = Linked { engine };
    auto other = ModuleBuilder {};
    other.type(FuncType { .params = { binary::I32, binary::I32 }, .results = { binary::I32 } });
    other.memory(Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 });
    other.function(0,
                   {},
                   code({ localGet(0),
                          localGet(1),
                          memoryInstruction(Opcode::I32Store, 0),
                          localGet(0),
                          memoryInstruction(Opcode::I32Load, 0) }));
    auto const b = linked.instantiate(other);

    auto caller = ModuleBuilder {};
    caller.type(FuncType { .params = { binary::I32, binary::I32 }, .results = { binary::I32 } });
    caller.type(FuncType { .params = {}, .results = { binary::I32 } });
    caller.importFunction("b", "put", 0);
    caller.memory(Limits { .min = 2, .max = std::nullopt, .addressType = AddressType::I32 });
    constexpr auto PastOther = int32_t { 70'000 };
    auto const callee = crossCall == CrossCall::Call ? op(0x10, 0) : op(0x10, 2);
    caller.function(1,
                    {},
                    code({ i32Const(0),
                           i32Const(111),
                           memoryInstruction(Opcode::I32Store, 0),
                           i32Const(PastOther),
                           i32Const(7),
                           memoryInstruction(Opcode::I32Store, 0),
                           i32Const(0),
                           i32Const(222),
                           callee,
                           i32Const(0),
                           memoryInstruction(Opcode::I32Load, 0),
                           op(0x6A),
                           i32Const(PastOther),
                           memoryInstruction(Opcode::I32Load, 0),
                           op(0x6A) }));
    caller.function(0, {}, code({ localGet(0), localGet(1), op(0x12, 0) }));
    auto const a = linked.instantiate(
        caller,
        { ExternValue { .kind = ExternKind::Func, .address = exportedFunction(linked.store(), b, 0) } });

    auto const results = linked.outcome(a, 1);
    REQUIRE(results.has_value());
    REQUIRE(results->size() == 1);
    CHECK(std::get<I32>(results->front()).bits == 222U + 111U + 7U);
    CHECK(wordAt(linked.memory(a, 0), 0) == 111U);
    CHECK(wordAt(linked.memory(a, 0), PastOther) == 7U);
    CHECK(wordAt(linked.memory(b, 0), 0) == 222U);
}

TEST_CASE("Memory.boundsMatchReferenceAtTheEdges")
{
    // Every scalar access at the edges of its memory: in bounds it gives the reference's value and leaves the
    // reference's bytes; out of bounds it traps with the reference's Trap, and writes nothing.
    auto const engine = fastEngineOfEachDispatch();
    for (auto const& edge: edgeCases())
    {
        INFO(edge.name);
        auto const expected = runEdge(edge, Reference);
        auto const observed = runEdge(edge, engine);
        CHECK(observed == expected);
        CHECK(expected.results.has_value() == (edge.bounds == Bounds::Inside));
        if (edge.bounds == Bounds::Outside)
            CHECK(observed.results
                  == std::unexpected { Abrupt { Trap { TrapCode::OutOfBoundsMemoryAccess } } });
    }
}

TEST_CASE("Memory.generationSkipsUnchangedRefresh")
{
    // Memory 0 is read again only when some memory grew since it was last read (Store::memoryGeneration()):
    // a call that grows nothing refreshes nothing; a callee's memory.grow refreshes once, at the grow, so its
    // return to the caller finds the generation unchanged; a host's grow refreshes once, where it returns.
    auto const engine = fastEngineOfEachDispatch();
    auto module = ModuleBuilder {};
    module.type(FuncType { .params = {}, .results = { binary::I32 } });
    module.type(FuncType {});
    module.importFunction("env", "host", 1);
    module.memory(Limits { .min = 1, .max = 4, .addressType = AddressType::I32 });
    auto const load = code({ i32Const(0), memoryInstruction(Opcode::I32Load, 0) });
    // 1: calls 4 and the host, which grow nothing; 2: calls 5, which grows; 3: grows through the host.
    module.function(0, {}, code({ op(0x10, 4), op(0x10, 0), load }));
    module.function(0, {}, code({ op(0x10, 5), load }));
    module.function(0, {}, code({ op(0x10, 0), load }));
    module.function(1, {}, code({ i32Const(0), memoryInstruction(Opcode::I32Load, 0), op(0x1A) }));
    module.function(1, {}, code({ i32Const(1), growAndDrop(0) }));
    auto grows = false;
    Harness* self = nullptr;
    auto harness =
        Harness { module, engine, [&self, &grows](Store& store) {
                     auto const host = store.addHostFunction(
                         FuncType {},
                         makeHostFunction([&self, &store, &grows](HostContext&,
                                                                  std::span<Value const>,
                                                                  std::span<Value>) noexcept -> Outcome {
                             if (grows && !store.growMemory(self->instance().memories.at(0), 1))
                                 return std::unexpected { Abrupt { Trap { TrapCode::HostTrap } } };
                             return {};
                         }));
                     REQUIRE(host.has_value());
                     return std::vector { ExternValue { .kind = ExternKind::Func, .address = *host } };
                 } };
    self = &harness;
    auto const& interpreter = fastInterpreterOf(harness);
    REQUIRE(harness.outcome(1).has_value());
    CHECK(interpreter.memoryRefreshes() == 0);
    REQUIRE(harness.outcome(2).has_value());
    CHECK(interpreter.memoryRefreshes() == 1);
    grows = true;
    REQUIRE(harness.outcome(3).has_value());
    CHECK(interpreter.memoryRefreshes() == 2);
    CHECK(harness.store().memoryGeneration() == 2);
}

TEST_CASE("Memory.sizeAndGrowMatchReference")
{
    // memory.size and memory.grow of every address type, page size and memory index: the reference's pages,
    // its old size from a grow, and its -1 for a grow past the maximum, of the memory's address type.
    auto const engine = fastEngineOfEachDispatch();
    for (auto const type: { AddressType::I32, AddressType::I64 })
    {
        for (auto const pageSizeLog2: { uint8_t { 0 }, uint8_t { 16 } })
        {
            for (auto const memoryIndex: { 0U, 1U })
            {
                INFO((type == AddressType::I64 ? "i64" : "i32")
                     << " addresses, pages of 2^" << uint32_t { pageSizeLog2 } << ", memory " << memoryIndex);
                auto const kind = type == AddressType::I64 ? binary::I64 : binary::I32;
                auto module = ModuleBuilder {};
                module.type(FuncType { .params = { kind }, .results = { kind, kind, kind } });
                if (memoryIndex != 0)
                    module.memory(memoryOf(AddressType::I32, 1));
                module.memory(MemoryType { .limits = Limits { .min = 2, .max = 5, .addressType = type },
                                           .pageSizeLog2 = pageSizeLog2 });
                auto const size = code({ { 0x3F }, encodeU32(memoryIndex) });
                auto const grow = code({ { 0x40 }, encodeU32(memoryIndex) });
                module.function(0, {}, code({ size, localGet(0), grow, size }));
                // A refused grow's -1, of the address type, taken for an address: past the end, whatever the
                // width of its slot.
                module.type(FuncType { .params = { kind }, .results = { binary::I32 } });
                module.function(
                    1, {}, code({ localGet(0), grow, memoryInstruction(Opcode::I32Load, 4, memoryIndex) }));
                auto const argument = [&](uint64_t delta) -> Value {
                    if (type == AddressType::I64)
                        return I64 { delta };
                    return I32 { static_cast<uint32_t>(delta) };
                };
                for (auto const delta:
                     { uint64_t { 0 }, uint64_t { 2 }, uint64_t { 4 }, std::numeric_limits<uint64_t>::max() })
                {
                    INFO("delta " << delta);
                    auto reference = Harness { module, Reference };
                    auto observed = Harness { module, engine };
                    CHECK(observed.outcome(0, { argument(delta) })
                          == reference.outcome(0, { argument(delta) }));
                    CHECK(observed.outcome(1, { argument(delta) })
                          == reference.outcome(1, { argument(delta) }));
                }
            }
        }
    }
}

TEST_CASE("Memory.importedMemoryGrownByItsExporter")
{
    // An imported memory of i64 addresses is memory 0 of the importer too: what the exporter's function
    // stores past the size it grew it from, the importer loads once the call returns, at the new size.
    auto const engine = referenceAndFastEngineOfEachDispatch();
    auto linked = Linked { engine };
    auto exporter = ModuleBuilder {};
    exporter.type(FuncType { .params = {}, .results = {} });
    exporter.memory(Limits { .min = 1, .max = 2, .addressType = AddressType::I64 });
    exporter.function(0,
                      {},
                      code({ i64Const(1),
                             { 0x40, 0x00 },
                             op(0x1A),
                             i64Const(static_cast<int64_t>(PageBytes)),
                             i32Const(0x5566'7788),
                             memoryInstruction(Opcode::I32Store, 0) }));
    exporter.exportEntity("memory", ExternKind::Memory, 0);
    auto const b = linked.instantiate(exporter);

    auto importer = ModuleBuilder {};
    importer.type(FuncType { .params = {}, .results = {} });
    importer.type(FuncType { .params = {}, .results = { binary::I32 } });
    importer.importFunction("b", "grow", 0);
    importer.importMemory(
        "b",
        "memory",
        MemoryType { .limits = Limits { .min = 1, .max = 2, .addressType = AddressType::I64 } });
    importer.function(1,
                      {},
                      code({ op(0x10, 0),
                             i64Const(static_cast<int64_t>(PageBytes)),
                             memoryInstruction(Opcode::I32Load, 0) }));
    auto const& exported = linked.store().moduleInstance(b);
    auto const a = linked.instantiate(
        importer,
        { ExternValue { .kind = ExternKind::Func, .address = exported.functions.at(0) },
          ExternValue { .kind = ExternKind::Memory, .address = exported.memories.at(0) } });
    auto const results = linked.outcome(a, 1);
    REQUIRE(results.has_value());
    REQUIRE(results->size() == 1);
    CHECK(std::get<I32>(results->front()).bits == 0x5566'7788U);
}

TEST_CASE("Memory.accessMaskAtTheEdges")
{
    // The Spectre mask: all ones exactly where the access fits, zero elsewhere, the offsets near 2^63 and
    // 2^64 included, where the bytes left after the access would wrap to a positive count.
    struct Row
    {
        uint64_t effective; ///< The access's offset.
        uint64_t size;      ///< The memory's size.
        uint64_t width;     ///< The access's width.
        uint64_t mask;      ///< The mask expected.
    };
    constexpr auto All = std::numeric_limits<uint64_t>::max();
    constexpr auto Half = uint64_t { 1 } << 63U;
    constexpr auto Size = uint64_t { 2 } * PageBytes;
    constexpr auto Rows = std::to_array<Row>({
        { .effective = 0, .size = Size, .width = 4, .mask = All },
        { .effective = Size - 4, .size = Size, .width = 4, .mask = All },
        { .effective = Size - 3, .size = Size, .width = 4, .mask = 0 },
        { .effective = Size, .size = Size, .width = 1, .mask = 0 },
        { .effective = Half - 1, .size = Size, .width = 1, .mask = 0 },
        { .effective = Half, .size = Size, .width = 1, .mask = 0 },
        { .effective = Half + 1, .size = Size, .width = 8, .mask = 0 },
        // Offsets the bytes left would wrap to a positive count for, without the offset's own sign.
        { .effective = Half + Size, .size = Size, .width = 4, .mask = 0 },
        { .effective = All - 1, .size = Size, .width = 4, .mask = 0 },
        { .effective = All, .size = Size, .width = 1, .mask = 0 },
        { .effective = 0, .size = 0, .width = 1, .mask = 0 },
        // A memory smaller than the access fits nothing, whatever the offset: its size less the width wraps.
        { .effective = Half - 1, .size = 0, .width = 8, .mask = 0 },
        { .effective = Half - 1, .size = 7, .width = 8, .mask = 0 },
        { .effective = 0, .size = 7, .width = 8, .mask = 0 },
        { .effective = 0, .size = 8, .width = 8, .mask = All },
        { .effective = 1, .size = 8, .width = 8, .mask = 0 },
    });
    for (auto const& row: Rows)
    {
        INFO("offset " << row.effective << ", size " << row.size << ", width " << row.width);
        CHECK(isoline::fastinterpreter::accessMask(row.effective, row.size, row.width) == row.mask);
    }
}

TEST_CASE("Memory.i32AddressIgnoresTheSlotsHighBits")
{
    // Code written by hand loads i32 memory 0 from a slot holding an i64, high bits set: an access of i32
    // addresses reads its low 32 bits, so 0xFFFF'FFFF'FFFF'FFFE is the address 0xFFFF'FFFE, which traps,
    // and 0xFFFF'FFFF'0000'0004 is 4, which reads what is there. Without the truncation the first would sum
    // past 2^64 into the memory.
    auto const dispatch = GENERATE(from_range(availableDispatches()));
    auto module = ModuleBuilder {};
    module.type(FuncType { .params = { binary::I64 }, .results = { binary::I32 } });
    module.memory(Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 });
    module.data(dataSegment(0, AddressType::I32, 4, { 0x44, 0x33, 0x22, 0x11 }));
    // Lowered: i32.wrap_i64 s1, s0; i32.load s1, s1, #0; return s1. By hand, the load reads s0 itself.
    module.function(0, {}, code({ localGet(0), op(0xA7), memoryInstruction(Opcode::I32Load, 0) }));
    auto const byHand = [dispatch](isoline::fastinterpreter::LoweredModule const& lowered) {
        REQUIRE(lowered.functions().front().frameSlots >= 2);
        using isoline::fastinterpreter::headerWord;
        using isoline::fastinterpreter::OperationId;
        return isoline::fastinterpreter::testing::handWritten(lowered,
                                                              { headerWord(dispatch, OperationId::I32Load),
                                                                1,
                                                                0,
                                                                headerWord(dispatch, OperationId::Return),
                                                                1 });
    };
    auto harness = Harness { module, fast(dispatch), byHand };
    CHECK(harness.trap(0, { I64 { 0xFFFF'FFFF'FFFF'FFFE } }) == TrapCode::OutOfBoundsMemoryAccess);
    CHECK(harness.result(0, { I64 { 0xFFFF'FFFF'0000'0004 } }) == Value { I32 { 0x1122'3344 } });
}

TEST_CASE("Memory.growOfAnAliasRefreshesMemory0")
{
    // A module that imports one memory twice has it as memory 0 and memory 1: memory.grow 1 grows memory 0
    // too, which the next access of memory 0 sees at its new size.
    auto const engine = referenceAndFastEngineOfEachDispatch();
    auto linked = Linked { engine };
    auto exporter = ModuleBuilder {};
    exporter.memory(Limits { .min = 1, .max = 2, .addressType = AddressType::I32 });
    exporter.exportEntity("memory", ExternKind::Memory, 0);
    auto const b = linked.instantiate(exporter);
    auto importer = ModuleBuilder {};
    importer.type(FuncType { .params = {}, .results = { binary::I32 } });
    auto const limits = Limits { .min = 1, .max = 2, .addressType = AddressType::I32 };
    importer.importMemory("b", "memory", limits);
    importer.importMemory("b", "alias", limits);
    importer.function(0,
                      {},
                      code({ i32Const(1),
                             growAndDrop(1),
                             i32Const(static_cast<int32_t>(PageBytes)),
                             i32Const(0x5566'7788),
                             memoryInstruction(Opcode::I32Store, 0),
                             i32Const(static_cast<int32_t>(PageBytes)),
                             memoryInstruction(Opcode::I32Load, 0, 1) }));
    auto const memory = ExternValue { .kind = ExternKind::Memory,
                                      .address = linked.store().moduleInstance(b).memories.at(0) };
    auto const a = linked.instantiate(importer, { memory, memory });
    auto const results = linked.outcome(a, 0);
    REQUIRE(results.has_value());
    REQUIRE(results->size() == 1);
    CHECK(std::get<I32>(results->front()).bits == 0x5566'7788U);
    CHECK(linked.memory(b, 0).size() == 2 * PageBytes);
}
