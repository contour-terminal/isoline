// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/ComponentFixtures.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/Calls.hpp>
#include <isoline/component/runtime/ComponentStore.hpp>
#include <isoline/component/runtime/testing/StackThread.hpp>
#include <isoline/component/runtime/testing/StoreFixtures.hpp>

using isoline::component::binary::BorrowType;
using isoline::component::binary::CanonBuiltin;
using isoline::component::binary::CanonDefinition;
using isoline::component::binary::CanonOptionKind;
using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreSort;
using isoline::component::binary::DefinedValueType;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::ListType;
using isoline::component::binary::PrimitiveValueType;
using isoline::component::binary::Sort;
using isoline::component::binary::SortIndex;
using isoline::component::binary::TypeBound;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::TypeIndex;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::liftedIdentity;
using isoline::component::canonical::Ownership;
using isoline::component::canonical::ResourceValue;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::ValueBuilder;
using isoline::component::canonical::Values;
using isoline::component::runtime::CallError;
using isoline::component::runtime::ComponentStore;
using isoline::component::runtime::HostResourceIdentity;
using isoline::component::runtime::HostResourceImport;
using isoline::component::runtime::IHostCallee;
using isoline::component::runtime::LiftedFunction;
using isoline::component::runtime::ResolvedImports;
using isoline::component::runtime::StoreState;
using isoline::component::runtime::testing::ArgumentOwnership;
using isoline::component::runtime::testing::call;
using isoline::component::runtime::testing::callWith;
using isoline::component::runtime::testing::coreModule;
using isoline::component::runtime::testing::embedderFailure;
using isoline::component::runtime::testing::Engines;
using isoline::component::runtime::testing::functionType;
using isoline::component::runtime::testing::Harness;
using isoline::component::runtime::testing::HarnessOptions;
using isoline::component::runtime::testing::hostFunction;
using isoline::component::runtime::testing::importsOf;
using isoline::component::runtime::testing::liftedExport;
using isoline::component::runtime::testing::option;
using isoline::component::runtime::testing::trapCodeOf;
using isoline::component::runtime::testing::u32Result;

namespace
{

/// A module of a memory and a bump `realloc` that counts its calls at address 0 and keeps the old
/// block's bytes, as definitions.py's string stores rely on.
constexpr auto BumpMemoryModule = std::string_view { R"((module
  (memory (export "mem") 1)
  (global $heap (mut i32) (i32.const 1024))
  (func (export "realloc") (param $old i32) (param $oldSize i32) (param $align i32) (param $new i32) (result i32)
    (local $ptr i32)
    (i32.store (i32.const 0) (i32.add (i32.load (i32.const 0)) (i32.const 1)))
    (local.set $ptr (i32.and (i32.add (global.get $heap) (i32.sub (local.get $align) (i32.const 1)))
                             (i32.sub (i32.const 0) (local.get $align))))
    (global.set $heap (i32.add (local.get $ptr) (local.get $new)))
    (memory.copy (local.get $ptr) (local.get $old)
                 (select (local.get $oldSize) (local.get $new) (i32.lt_u (local.get $oldSize) (local.get $new))))
    (local.get $ptr)))
)" };

/// The 32-bit little-endian word at @p address of the memory @p function's options name.
[[nodiscard]] uint32_t wordAt(ComponentStore& store, LiftedFunction const& function, uint32_t address)
{
    REQUIRE(function.options.memory.has_value());
    auto const range = store.coreStore().memoryRange(*function.options.memory, address, 4);
    REQUIRE(range.has_value());
    auto word = uint32_t { 0 };
    std::memcpy(&word, range->data(), sizeof word);
    return word;
}

/// Writes the 32-bit little-endian @p value at @p address of the memory @p function's options name.
void setWordAt(ComponentStore& store, LiftedFunction const& function, uint32_t address, uint32_t value)
{
    REQUIRE(function.options.memory.has_value());
    auto const range = store.coreStore().memoryRange(*function.options.memory, address, 4);
    REQUIRE(range.has_value());
    std::memcpy(range->data(), &value, sizeof value);
}

/// Which memory a lifted export's canonical options name.
enum class CanonicalMemory : uint8_t
{
    None,             ///< None.
    MemoryAndRealloc, ///< The module's `mem` and `realloc`.
};

/// An export of a single-module component: a core function lifted.
struct LiftedExport
{
    std::string name;                                             ///< The core export, and the component's.
    std::vector<isoline::component::binary::ValueTypeRef> params; ///< Its parameters.
    std::optional<isoline::component::binary::ValueTypeRef> result {}; ///< Its result.
    CanonicalMemory memory = CanonicalMemory::None;                    ///< What its options name of memory.
    std::optional<std::string> postReturn {};   ///< Its `post-return`, by core export name.
    std::optional<CanonOptionKind> encoding {}; ///< Its `string-encoding`, if given.
};

/// A host function import of such a component.
struct HostImport
{
    std::string name;                                                  ///< Its name.
    std::vector<isoline::component::binary::ValueTypeRef> params;      ///< Its parameters.
    std::optional<isoline::component::binary::ValueTypeRef> result {}; ///< Its result.
};

/// A component of one core module @p moduleText that imports the host functions @p hostImports
/// (lowered without options, as `host` `name`), and lifts each of @p exports with the options it names.
/// @param moduleText The core module.
/// @param hostImports The host functions, imported first.
/// @param exports The lifted exports.
/// @param valueTypes Value types defined before anything, so type index i names the i-th.
/// @return The component's bytes.
[[nodiscard]] std::vector<std::byte> singleModuleComponent(
    std::string_view moduleText,
    std::vector<HostImport> const& hostImports,
    std::vector<LiftedExport> const& exports,
    std::vector<DefinedValueType> const& valueTypes = {})
{
    auto builder = ComponentBuilder {};
    for (auto const& valueType: valueTypes)
        std::ignore = builder.type(TypeDefinition { .type = valueType });
    auto inlineExports = std::vector<CoreInlineExport> {};
    for (auto const& import: hostImports)
    {
        auto const type = functionType(builder, import.params, import.result);
        auto const function = builder.import(import.name, ExternDesc { .sort = Sort::Func, .index = type });
        auto const lowered = builder.canonLower(function, {});
        inlineExports.push_back(
            CoreInlineExport { .name = import.name, .sort = CoreSort::Func, .index = lowered });
    }
    auto arguments = std::vector<std::pair<std::string, uint32_t>> {};
    if (!inlineExports.empty())
        arguments.emplace_back("host", builder.coreInlineExports(std::move(inlineExports)));
    auto const module = builder.coreModule(coreModule(moduleText));
    auto const instance = builder.coreInstantiate(module, std::move(arguments));
    for (auto const& entry: exports)
    {
        auto const type = functionType(builder, entry.params, entry.result);
        auto const core = builder.aliasCoreExport(instance, entry.name, CoreSort::Func);
        auto options = std::vector<isoline::component::binary::CanonOption> {};
        if (entry.encoding)
            options.push_back(option(*entry.encoding));
        if (entry.memory == CanonicalMemory::MemoryAndRealloc)
        {
            options.push_back(
                option(CanonOptionKind::Memory, builder.aliasCoreExport(instance, "mem", CoreSort::Memory)));
            options.push_back(option(CanonOptionKind::Realloc,
                                     builder.aliasCoreExport(instance, "realloc", CoreSort::Func)));
        }
        if (entry.postReturn)
            options.push_back(option(CanonOptionKind::PostReturn,
                                     builder.aliasCoreExport(instance, *entry.postReturn, CoreSort::Func)));
        auto const lifted = builder.canonLift(core, std::move(options), type);
        std::ignore = builder.exportItem(entry.name, Sort::Func, lifted);
    }
    return builder.build();
}

/// The module of the re-entrance tests: `call` counts its entries at address 0, calls the host's `f`,
/// and marks its return from `f` at address 4; its post-return counts its runs at 8; `g` returns 41;
/// `bad` traps.
constexpr auto ReentrantModule = std::string_view { R"((module
  (import "host" "f" (func $f (result i32)))
  (memory (export "mem") 1)
  (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 64))
  (func (export "call") (result i32)
    (local $r i32)
    (i32.store (i32.const 0) (i32.add (i32.load (i32.const 0)) (i32.const 1)))
    (local.set $r (call $f))
    (i32.store (i32.const 4) (i32.const 1))
    (local.get $r))
  (func (export "post") (param i32) (i32.store (i32.const 8) (i32.add (i32.load (i32.const 8)) (i32.const 1))))
  (func (export "g") (result i32) (i32.const 41))
  (func (export "bad") (result i32) unreachable))
)" };

/// The re-entrance tests' component.
[[nodiscard]] std::vector<std::byte> reentrantComponent()
{
    return singleModuleComponent(
        ReentrantModule,
        { HostImport { .name = "f", .params = {}, .result = PrimitiveValueType::U32 } },
        { LiftedExport { .name = "call",
                         .params = {},
                         .result = PrimitiveValueType::U32,
                         .memory = CanonicalMemory::MemoryAndRealloc,
                         .postReturn = "post" },
          LiftedExport { .name = "g", .params = {}, .result = PrimitiveValueType::U32 },
          LiftedExport { .name = "bad", .params = {}, .result = PrimitiveValueType::U32 } });
}

/// The trap @p outcome failed with (REQUIREd to have failed).
template <typename T>
[[nodiscard]] isoline::component::canonical::Trap trapIn(
    std::expected<T, isoline::component::canonical::Trap> const& outcome)
{
    REQUIRE_FALSE(outcome.has_value());
    return outcome.error();
}

/// A u32 result of @p value into @p results.
[[nodiscard]] std::expected<void, isoline::api::HostFailure> returnU32(ValueBuilder& results, uint32_t value)
{
    results.scalar(value);
    return {};
}

} // namespace

TEST_CASE("Calls.identityRoundTrip")
{
    auto const engine = GENERATE(from_range(Engines));
    auto harness = Harness { liftedIdentity().build(), HarnessOptions { .engine = engine } };
    auto const instance = harness.instance();
    auto outcome = call(*harness.store, instance, "id", [](ValueBuilder& args) { args.scalar(7); });
    CHECK(u32Result(outcome) == 7);
    CHECK(harness.store->state() == StoreState::Live);
    // The arguments ended once lowered; only the results the embedder holds are live.
    CHECK(harness.store->liveBytes()->live() > 0);
    outcome.values.reset();
    CHECK(harness.store->liveBytes()->live() == 0);
    // A second call of the same instance works the same, and so does one whose arguments are lent.
    CHECK(u32Result(call(*harness.store, instance, "id", [](ValueBuilder& args) { args.scalar(9); })) == 9);
    auto lent = callWith(
        *harness.store,
        liftedExport(*harness.store, instance, "id"),
        [](ValueBuilder& args) { args.scalar(11); },
        ArgumentOwnership::Lent,
        [](Values const&) {});
    CHECK(u32Result(lent) == 11);
}

TEST_CASE("Calls.stringThroughTwoComponents")
{
    // B echoes a UTF-8 string; A (UTF-16) imports B's echo and exports run, which calls it. Both keep
    // their realloc calls' count at address 0 of their memory.
    auto const engine = GENERATE(from_range(Engines));
    auto b = ComponentBuilder {};
    {
        auto const type = functionType(b, { PrimitiveValueType::String }, PrimitiveValueType::String);
        auto const module = b.coreModule(coreModule(R"((module
          (memory (export "mem") 1)
          (global $heap (mut i32) (i32.const 1024))
          (func (export "realloc") (param $old i32) (param $oldSize i32) (param $align i32) (param $new i32) (result i32)
            (local $ptr i32)
            (i32.store (i32.const 0) (i32.add (i32.load (i32.const 0)) (i32.const 1)))
            (local.set $ptr (i32.and (i32.add (global.get $heap) (i32.sub (local.get $align) (i32.const 1)))
                                     (i32.sub (i32.const 0) (local.get $align))))
            (global.set $heap (i32.add (local.get $ptr) (local.get $new)))
            (memory.copy (local.get $ptr) (local.get $old)
                         (select (local.get $oldSize) (local.get $new) (i32.lt_u (local.get $oldSize) (local.get $new))))
            (local.get $ptr))
          (func (export "echo") (param i32 i32) (result i32)
            (i32.store (i32.const 8) (local.get 0))
            (i32.store (i32.const 12) (local.get 1))
            (i32.const 8))))"));
        auto const instance = b.coreInstantiate(module, {});
        auto const memory = b.aliasCoreExport(instance, "mem", CoreSort::Memory);
        auto const realloc = b.aliasCoreExport(instance, "realloc", CoreSort::Func);
        auto const echo = b.aliasCoreExport(instance, "echo", CoreSort::Func);
        auto const lifted = b.canonLift(
            echo,
            { option(CanonOptionKind::Memory, memory), option(CanonOptionKind::Realloc, realloc) },
            type);
        std::ignore = b.exportItem("echo", Sort::Func, lifted);
    }
    auto a = ComponentBuilder {};
    {
        auto const type = functionType(a, { PrimitiveValueType::String }, PrimitiveValueType::String);
        auto const echo = a.import("echo", ExternDesc { .sort = Sort::Func, .index = type });
        auto const memoryModule = a.coreModule(coreModule(BumpMemoryModule));
        auto const memoryInstance = a.coreInstantiate(memoryModule, {});
        auto const memory = a.aliasCoreExport(memoryInstance, "mem", CoreSort::Memory);
        auto const realloc = a.aliasCoreExport(memoryInstance, "realloc", CoreSort::Func);
        auto const options = std::vector {
            option(CanonOptionKind::Utf16),
            option(CanonOptionKind::Memory, memory),
            option(CanonOptionKind::Realloc, realloc),
        };
        auto const lowered = a.canonLower(echo, options);
        auto const host = a.coreInlineExports(
            { CoreInlineExport { .name = "echo", .sort = CoreSort::Func, .index = lowered } });
        auto const module = a.coreModule(coreModule(R"((module
          (import "host" "echo" (func $echo (param i32 i32 i32)))
          (func (export "run") (param i32 i32) (result i32)
            (call $echo (local.get 0) (local.get 1) (i32.const 8))
            (i32.const 8))))"));
        auto const instance = a.coreInstantiate(module, { { "host", host } });
        auto const run = a.aliasCoreExport(instance, "run", CoreSort::Func);
        std::ignore = a.exportItem("run", Sort::Func, a.canonLift(run, options, type));
    }
    auto root = ComponentBuilder {};
    auto const bComponent = root.nested(std::move(b));
    auto const aComponent = root.nested(std::move(a));
    auto const bInstance = root.instantiate(bComponent, {});
    auto const echo = root.aliasExport(bInstance, "echo", Sort::Func);
    auto const aInstance =
        root.instantiate(aComponent, { { "echo", SortIndex { .sort = Sort::Func, .index = echo } } });
    std::ignore = root.exportItem("run", Sort::Func, root.aliasExport(aInstance, "run", Sort::Func));
    std::ignore = root.exportItem("echo", Sort::Func, echo);

    auto harness = Harness { root.build(), HarnessOptions { .engine = engine } };
    auto const instance = harness.instance();
    auto const text = std::string_view { "a\xC3\xA9\xF0\x9F\x98\x80" }; // "aé😀"
    auto outcome = call(*harness.store, instance, "run", [&](ValueBuilder& args) { args.string(text); });
    REQUIRE(outcome.result.has_value());
    REQUIRE(outcome.values.has_value());
    auto const result = outcome.values->root(0).string().utf8();
    REQUIRE(result.has_value());
    CHECK(*result == text);
    // definitions.py: host UTF-8 → A UTF-16 reallocs twice (worst case 14 bytes, then shrinks to 8); A →
    // B UTF-8 three times (4 bytes, grows to the worst case 12 at the first non-ASCII code point, shrinks
    // to 7); B's result → A UTF-16 twice more. Lifting from memory never reallocs.
    CHECK(wordAt(*harness.store, liftedExport(*harness.store, instance, "run"), 0) == 4);
    CHECK(wordAt(*harness.store, liftedExport(*harness.store, instance, "echo"), 0) == 3);
    outcome.values.reset();
    CHECK(harness.store->liveBytes()->live() == 0);
}

TEST_CASE("Calls.postReturnRunsAfterResults")
{
    // f returns "hi" through a return area; post-return clobbers the string and counts its runs at 8.
    auto const engine = GENERATE(from_range(Engines));
    auto const bytes = singleModuleComponent(R"((module
      (memory (export "mem") 1)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 256))
      (func (export "f") (result i32)
        (i32.store (i32.const 0) (i32.const 16))
        (i32.store (i32.const 4) (i32.const 2))
        (i32.store16 (i32.const 16) (i32.const 0x6968))
        (i32.const 0))
      (func (export "post") (param i32)
        (i32.store16 (i32.const 16) (i32.const 0x5858))
        (i32.store (i32.const 8) (i32.add (i32.load (i32.const 8)) (i32.const 1)))))
    )",
                                             {},
                                             { LiftedExport { .name = "f",
                                                              .params = {},
                                                              .result = PrimitiveValueType::String,
                                                              .memory = CanonicalMemory::MemoryAndRealloc,
                                                              .postReturn = "post" } });
    auto harness = Harness { bytes, HarnessOptions { .engine = engine } };
    auto& store = *harness.store;
    auto const instance = harness.instance();
    auto const& f = liftedExport(store, instance, "f");
    auto postReturnsSeen = std::vector<uint32_t> {};
    auto const outcome = callWith(
        store,
        f,
        [](ValueBuilder&) {},
        ArgumentOwnership::Owned,
        [&](Values const&) { postReturnsSeen.push_back(wordAt(store, f, 8)); });
    REQUIRE(outcome.result.has_value());
    REQUIRE(outcome.values.has_value());
    auto const text = outcome.values->root(0).string().utf8();
    REQUIRE(text.has_value());
    CHECK(*text == "hi");
    CHECK(postReturnsSeen == std::vector<uint32_t> { 0 });
    CHECK(wordAt(store, f, 8) == 1);
    // Exactly once per call.
    REQUIRE(call(store, instance, "f").result.has_value());
    CHECK(wordAt(store, f, 8) == 2);
}

TEST_CASE("Calls.postReturnSkippedWhenResolveFails")
{
    // A failing on_resolve ends the call as its failure: post-return does not run, the store locks down.
    auto const bytes = singleModuleComponent(R"((module
      (memory (export "mem") 1)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 256))
      (func (export "f") (result i32) (i32.const 3))
      (func (export "post") (param i32) (i32.store (i32.const 8) (i32.const 1))))
    )",
                                             {},
                                             { LiftedExport { .name = "f",
                                                              .params = {},
                                                              .result = PrimitiveValueType::U32,
                                                              .memory = CanonicalMemory::MemoryAndRealloc,
                                                              .postReturn = "post" } });
    auto harness = Harness { bytes };
    auto& store = *harness.store;
    auto const& f = liftedExport(store, harness.instance(), "f");
    auto start = []() -> std::expected<isoline::component::runtime::CallArguments, CallError> {
        return std::unexpected { CallError { embedderFailure(7) } };
    };
    auto nothing = [](Values) -> std::expected<void, CallError> {
        return {};
    };
    SECTION("on_start fails")
    {
        auto const called = isoline::component::runtime::canonLift(store, f, start, nothing);
        REQUIRE_FALSE(called.has_value());
        CHECK(std::get<isoline::api::HostFailure>(called.error()) == embedderFailure(7));
    }
    SECTION("on_resolve fails")
    {
        auto args = [&]() -> std::expected<isoline::component::runtime::CallArguments, CallError> {
            auto none = ValueBuilder { *f.side.arena, {}, store.liveBytes() };
            auto built = none.finish();
            if (!built)
                return std::unexpected { CallError { embedderFailure(9) } };
            return isoline::component::runtime::CallArguments { std::move(*built) };
        };
        auto failing = [](Values) -> std::expected<void, CallError> {
            return std::unexpected { CallError { embedderFailure(8) } };
        };
        auto const called = isoline::component::runtime::canonLift(store, f, args, failing);
        REQUIRE_FALSE(called.has_value());
        CHECK(std::get<isoline::api::HostFailure>(called.error()) == embedderFailure(8));
        CHECK(wordAt(store, f, 8) == 0);
    }
    CHECK(store.state() == StoreState::LockedDown);
}

TEST_CASE("Calls.mayLeaveDuringReallocAndPostReturn")
{
    // realloc and post-return both call the host's h: each is refused before the host runs.
    auto const engine = GENERATE(from_range(Engines));
    auto const bytes = singleModuleComponent(
        R"((module
      (import "host" "h" (func $h))
      (memory (export "mem") 1)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32) (call $h) (i32.const 64))
      (func (export "take") (param i32 i32))
      (func (export "give") (result i32) (i32.const 3))
      (func (export "post") (param i32) (call $h)))
    )",
        { HostImport { .name = "h", .params = {} } },
        { LiftedExport { .name = "take",
                         .params = { PrimitiveValueType::String },
                         .memory = CanonicalMemory::MemoryAndRealloc },
          LiftedExport {
              .name = "give", .params = {}, .result = PrimitiveValueType::U32, .postReturn = "post" } });
    auto harness = Harness { bytes, HarnessOptions { .engine = engine } };
    auto host = hostFunction(
        [](ComponentStore&, Values const&, ValueBuilder&) -> std::expected<void, isoline::api::HostFailure> {
            return {};
        });
    auto const instance = harness.instance(importsOf({ host }));
    auto& store = *harness.store;
    SECTION("realloc")
    {
        auto const outcome = call(store, instance, "take", [](ValueBuilder& args) { args.string("x"); });
        CHECK(trapCodeOf(outcome) == TrapCode::CannotLeave);
        CHECK_FALSE(outcome.values.has_value());
    }
    SECTION("post-return")
    {
        auto const outcome = call(store, instance, "give");
        CHECK(trapCodeOf(outcome) == TrapCode::CannotLeave);
        CHECK(outcome.values.has_value()); // the results were delivered before post-return ran
    }
    CHECK(host->calls == 0);
    CHECK(store.state() == StoreState::LockedDown);
    CHECK(store.instanceState(instance).mayLeave == isoline::component::runtime::MayLeave::Yes);
    CHECK(trapCodeOf(call(store, instance, "give")) == TrapCode::CannotEnter);
}

TEST_CASE("Calls.reentrantSyncExportSucceeds")
{
    auto const engine = GENERATE(from_range(Engines));
    auto harness = Harness { reentrantComponent(), HarnessOptions { .engine = engine } };
    LiftedFunction const* g = nullptr;
    auto host = hostFunction([&](ComponentStore& store,
                                 Values const&,
                                 ValueBuilder& results) -> std::expected<void, isoline::api::HostFailure> {
        auto const nested =
            callWith(store, *g, [](ValueBuilder&) {}, ArgumentOwnership::Owned, [](Values const&) {});
        if (!nested.result || !nested.values)
            return std::unexpected { embedderFailure() };
        return returnU32(results, nested.values->root(0).scalar<uint32_t>() + 1);
    });
    auto const instance = harness.instance(importsOf({ host }));
    g = &liftedExport(*harness.store, instance, "g");
    CHECK(u32Result(call(*harness.store, instance, "call")) == 42);
    CHECK(host->calls == 1);
    CHECK(harness.store->state() == StoreState::Live);
    CHECK(harness.store->activeCalls() == 0);
}

TEST_CASE("Calls.nestedTrapLocksDown")
{
    auto const engine = GENERATE(from_range(Engines));
    auto harness = Harness { reentrantComponent(), HarnessOptions { .engine = engine } };
    LiftedFunction const* bad = nullptr;
    auto nestedTrap = std::optional<TrapCode> {};
    auto host = hostFunction([&](ComponentStore& store,
                                 Values const&,
                                 ValueBuilder&) -> std::expected<void, isoline::api::HostFailure> {
        auto const nested =
            callWith(store, *bad, [](ValueBuilder&) {}, ArgumentOwnership::Owned, [](Values const&) {});
        if (!nested.result)
            if (auto const* trap = std::get_if<isoline::component::canonical::Trap>(&nested.result.error()))
                nestedTrap = trap->code;
        return std::unexpected { embedderFailure(5) };
    });
    auto const instance = harness.instance(importsOf({ host }));
    bad = &liftedExport(*harness.store, instance, "bad");
    auto const outcome = call(*harness.store, instance, "call");
    REQUIRE_FALSE(outcome.result.has_value());
    CHECK(std::get<isoline::api::HostFailure>(outcome.result.error()) == embedderFailure(5));
    CHECK(nestedTrap == TrapCode::Core);
    CHECK(harness.store->state() == StoreState::LockedDown);
    // The next call enters no guest: `call` would count its entry, and reach the host.
    auto const& entry = liftedExport(*harness.store, instance, "call");
    auto starts = uint32_t { 0 };
    auto const refused = callWith(
        *harness.store,
        entry,
        [&](ValueBuilder&) { ++starts; },
        ArgumentOwnership::Owned,
        [](Values const&) {});
    CHECK(trapCodeOf(refused) == TrapCode::CannotEnter);
    CHECK(starts == 0); // refused before on_start
    CHECK(wordAt(*harness.store, entry, 0) == 1);
    CHECK(host->calls == 1);
}

TEST_CASE("Calls.swallowedNestedTrapStillFails")
{
    auto const engine = GENERATE(from_range(Engines));
    auto harness = Harness { reentrantComponent(), HarnessOptions { .engine = engine } };
    LiftedFunction const* bad = nullptr;
    auto host = hostFunction([&](ComponentStore& store,
                                 Values const&,
                                 ValueBuilder& results) -> std::expected<void, isoline::api::HostFailure> {
        std::ignore =
            callWith(store, *bad, [](ValueBuilder&) {}, ArgumentOwnership::Owned, [](Values const&) {});
        return returnU32(results, 0); // swallowed
    });
    auto const instance = harness.instance(importsOf({ host }));
    bad = &liftedExport(*harness.store, instance, "bad");
    auto const outcome = call(*harness.store, instance, "call");
    CHECK(trapCodeOf(outcome) == TrapCode::CannotEnter);
    CHECK_FALSE(outcome.values.has_value());
    // Refused as the host returned: the guest never resumed after its call of f.
    CHECK(wordAt(*harness.store, liftedExport(*harness.store, instance, "call"), 4) == 0);
    CHECK(harness.store->state() == StoreState::LockedDown);
}

TEST_CASE("Calls.trapSwallowedByTheEmbedderStillFails")
{
    // The embedder's on_start or on_resolve calls `bad` and ignores its failure: the outer call fails
    // CannotEnter all the same, before the callee runs or before post-return.
    auto const engine = GENERATE(from_range(Engines));
    auto harness = Harness { reentrantComponent(), HarnessOptions { .engine = engine } };
    auto host =
        hostFunction([](ComponentStore&, Values const&, ValueBuilder& results)
                         -> std::expected<void, isoline::api::HostFailure> { return returnU32(results, 1); });
    auto const instance = harness.instance(importsOf({ host }));
    auto& store = *harness.store;
    auto const& bad = liftedExport(store, instance, "bad");
    auto const& entry = liftedExport(store, instance, "call");
    auto const swallow = [&] {
        std::ignore =
            callWith(store, bad, [](ValueBuilder&) {}, ArgumentOwnership::Owned, [](Values const&) {});
    };
    auto none = [&]() -> std::expected<isoline::component::runtime::CallArguments, CallError> {
        auto builder = ValueBuilder { *entry.side.arena, {}, store.liveBytes() };
        auto built = builder.finish();
        if (!built)
            return std::unexpected { CallError { embedderFailure(9) } };
        return isoline::component::runtime::CallArguments { std::move(*built) };
    };
    auto resolved = uint32_t { 0 };
    SECTION("in on_start")
    {
        auto start = [&]() {
            swallow();
            return none();
        };
        auto resolve = [&](Values) -> std::expected<void, CallError> {
            ++resolved;
            return {};
        };
        auto const called = isoline::component::runtime::canonLift(store, entry, start, resolve);
        REQUIRE_FALSE(called.has_value());
        CHECK(trapCodeOf(called.error()) == TrapCode::CannotEnter);
        CHECK(wordAt(store, entry, 0) == 0); // the callee never ran
        CHECK(resolved == 0);
    }
    SECTION("in on_resolve")
    {
        auto resolve = [&](Values) -> std::expected<void, CallError> {
            ++resolved;
            swallow();
            return {};
        };
        auto const called = isoline::component::runtime::canonLift(store, entry, none, resolve);
        REQUIRE_FALSE(called.has_value());
        CHECK(trapCodeOf(called.error()) == TrapCode::CannotEnter);
        CHECK(wordAt(store, entry, 0) == 1);
        CHECK(resolved == 1);
        CHECK(wordAt(store, entry, 8) == 0); // no post-return after a swallowed trap
    }
    CHECK(store.state() == StoreState::LockedDown);
}

namespace
{

/// A component exporting rec(n: u32, data: list<u8>) -> u32: rec calls itself n deep through a lowered
/// import of its own lifted export (a funcref table a second core module fills), each hop passing data,
/// which the overlapping realloc puts where the caller's copy lies; at depth 0 it calls the host's probe
/// and returns data's length. Its memory holds 130 pages: data of up to 8 MiB.
[[nodiscard]] std::vector<std::byte> recursionComponent()
{
    auto builder = ComponentBuilder {};
    auto const list = builder.type(
        TypeDefinition { .type = DefinedValueType { ListType { .element = PrimitiveValueType::U8 } } });
    auto const type = functionType(
        builder, { PrimitiveValueType::U32, TypeIndex { .index = list } }, PrimitiveValueType::U32);
    auto const probeType = functionType(builder, {}, std::nullopt);
    auto const probe = builder.import("probe", ExternDesc { .sort = Sort::Func, .index = probeType });
    auto const host = builder.coreInlineExports({ CoreInlineExport {
        .name = "probe", .sort = CoreSort::Func, .index = builder.canonLower(probe, {}) } });
    auto const module = builder.coreModule(coreModule(R"((module
      (import "host" "probe" (func $probe))
      (type $rec (func (param i32 i32 i32) (result i32)))
      (memory (export "mem") 130)
      (table (export "t") 1 funcref)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 1024))
      (func (export "rec") (param $n i32) (param $p i32) (param $l i32) (result i32)
        (if (result i32) (i32.eqz (local.get $n))
          (then (call $probe) (local.get $l))
          (else (call_indirect (type $rec)
                  (i32.sub (local.get $n) (i32.const 1)) (local.get $p) (local.get $l) (i32.const 0))))))
    )"));
    auto const instance = builder.coreInstantiate(module, { { "host", host } });
    auto const memory = builder.aliasCoreExport(instance, "mem", CoreSort::Memory);
    auto const realloc = builder.aliasCoreExport(instance, "realloc", CoreSort::Func);
    auto const rec = builder.aliasCoreExport(instance, "rec", CoreSort::Func);
    auto const table = builder.aliasCoreExport(instance, "t", CoreSort::Table);
    auto const options =
        std::vector { option(CanonOptionKind::Memory, memory), option(CanonOptionKind::Realloc, realloc) };
    auto const lifted = builder.canonLift(rec, options, type);
    auto const lowered = builder.canonLower(lifted, options);
    auto const functions = builder.coreInlineExports(
        { CoreInlineExport { .name = "rec", .sort = CoreSort::Func, .index = lowered } });
    auto const tables = builder.coreInlineExports(
        { CoreInlineExport { .name = "t", .sort = CoreSort::Table, .index = table } });
    auto const wiring = builder.coreModule(coreModule(R"((module
      (import "m" "t" (table 1 funcref))
      (import "l" "rec" (func $rec (param i32 i32 i32) (result i32)))
      (elem (i32.const 0) func $rec)))"));
    std::ignore = builder.coreInstantiate(wiring, { { "m", tables }, { "l", functions } });
    std::ignore = builder.exportItem("rec", Sort::Func, lifted);
    return builder.build();
}

/// What one component hop costs of the host's stack, measured.
struct HopCost
{
    uint64_t bytesPerHop = 0; ///< Between two depths of rec, per hop.
    uint64_t base = 0;        ///< From the calling thread's frame to rec's probe at depth 0.
};

/// The stack a thread needs to nest @p hops component calls: hops × bytes per hop + reserve, the
/// formula docs/embedding.md gives, with twice the measured cost as the margin and twice the base as the
/// reserve.
[[nodiscard]] std::size_t stackFor(uint32_t hops, HopCost const& cost)
{
    return static_cast<std::size_t>((uint64_t { hops } * cost.bytesPerHop * 2) + (cost.base * 2)
                                    + (uint64_t { 64 } << 10));
}

/// A store of recursionComponent(), its probe recording the stack address it ran at.
struct RecursionHarness
{
    explicit RecursionHarness(isoline::api::ExecutionEngine engine,
                              uint32_t maxNestedCalls = 250,
                              uint64_t maxLiveValueBytes = uint64_t { 256 } << 20,
                              uint32_t engineNesting = 250):
        harness { recursionComponent(),
                  HarnessOptions { .engine = engine,
                                   .storeLimits = storeLimitsOf(engineNesting),
                                   .limits = limitsOf(maxNestedCalls, maxLiveValueBytes) } },
        root { harness.instance(importsOf({ probe })) },
        rec { &liftedExport(*harness.store, root, "rec") }
    {
    }

    /// rec(hops, a list of @p length bytes), owned; no Catch2 assertion, so any thread may run it.
    [[nodiscard]] isoline::component::runtime::testing::CallOutcome call(uint32_t hops, uint32_t length) const
    {
        return callWith(
            *harness.store,
            *rec,
            [&](ValueBuilder& args) {
                args.scalar(hops).begin(length);
                std::ignore = args.packedElements();
                args.end();
            },
            ArgumentOwnership::Owned,
            [](Values const&) {});
    }

    [[nodiscard]] static isoline::api::StoreLimits storeLimitsOf(uint32_t engineNesting)
    {
        auto limits = isoline::api::StoreLimits {};
        limits.maxNestedInvocations = engineNesting;
        return limits;
    }

    [[nodiscard]] static isoline::component::ComponentLimits limitsOf(uint32_t maxNestedCalls,
                                                                      uint64_t maxLiveValueBytes)
    {
        auto limits = isoline::component::ComponentLimits {};
        limits.maxNestedCalls = maxNestedCalls;
        limits.maxLiveValueBytes = maxLiveValueBytes;
        return limits;
    }

    std::uintptr_t probed = 0; ///< The stack address the probe last ran at.
    std::shared_ptr<IHostCallee> probe = hostFunction([this](ComponentStore&, Values const&, ValueBuilder&)
                                                          -> std::expected<void, isoline::api::HostFailure> {
        probed = isoline::component::runtime::testing::stackAddress();
        return {};
    });
    Harness harness;
    uint32_t root = 0;
    LiftedFunction const* rec = nullptr;
};

/// The stack one component hop takes on @p engine, measured on a thread of ample stack.
[[nodiscard]] HopCost measureHop(isoline::api::ExecutionEngine engine)
{
    constexpr auto Shallow = uint32_t { 10 };
    constexpr auto Deep = uint32_t { 40 };
    auto recursion = RecursionHarness { engine };
    auto shallow = std::uintptr_t { 0 };
    auto deep = std::uintptr_t { 0 };
    auto top = std::uintptr_t { 0 };
    auto atZero = std::uintptr_t { 0 };
    auto succeeded = true;
    isoline::component::runtime::testing::runOnStack(std::size_t { 64 } << 20, [&] {
        top = isoline::component::runtime::testing::stackAddress();
        succeeded = recursion.call(0, 16).result.has_value();
        atZero = recursion.probed;
        succeeded = succeeded && recursion.call(Shallow, 16).result.has_value();
        shallow = recursion.probed;
        succeeded = succeeded && recursion.call(Deep, 16).result.has_value();
        deep = recursion.probed;
    });
    REQUIRE(succeeded);
    REQUIRE(shallow > deep); // the stack grows down on every platform the build targets
    REQUIRE(top > atZero);
    return HopCost { .bytesPerHop = (shallow - deep) / (Deep - Shallow), .base = top - atZero };
}

} // namespace

TEST_CASE("Calls.recursiveSelfCallIsBounded")
{
    // 100 hops, each passing a list: each hop's arguments are destroyed once lowered, so one copy is live
    // at a time, and the budget holds one and a half. (Scaled from the plan's 200 MiB of a 256 MiB budget,
    // which takes the unoptimised sanitizer build past the suite's timeout; the bound is the same: keeping
    // a hop's arguments to its return traps at the second hop.) It runs on a thread sized by the stack
    // formula.
    constexpr auto Hops = uint32_t { 100 };
    constexpr auto Length = uint32_t { 8 } << 20;
    auto const engine = GENERATE(from_range(Engines));
    auto const cost = measureHop(engine);
    auto recursion = RecursionHarness { engine, 250, Length + (Length / 2) };
    auto outcome = isoline::component::runtime::testing::CallOutcome { .result = {} };
    isoline::component::runtime::testing::runOnStack(stackFor(Hops, cost),
                                                     [&] { outcome = recursion.call(Hops, Length); });
    CHECK(u32Result(outcome) == Length);
    CHECK(recursion.harness.store->state() == StoreState::Live);
    outcome.values.reset();
    CHECK(recursion.harness.store->liveBytes()->live() == 0);
}

TEST_CASE("Calls.nestingPastTheLimitTraps")
{
    // One hop past the store's nesting limit traps CallStackExhausted before the host's stack runs out:
    // the store locks down and every value lifted on the way is released. The limit is the lower of the
    // engine's maxNestedInvocations and the component store's maxNestedCalls: the component default (80)
    // lowers the engine's default (100), and never raises an engine's lower one.
    struct Row
    {
        std::string_view name;
        uint32_t engineNesting = 0;
        uint32_t componentNesting = 0;
        uint32_t limit = 0;
    };
    auto const defaultEngine = isoline::api::StoreLimits {}.maxNestedInvocations;
    auto const defaultComponent = isoline::component::ComponentLimits {}.maxNestedCalls;
    REQUIRE(defaultComponent < defaultEngine);
    auto const row = GENERATE_COPY(
        Row { .name = "component limit", .engineNesting = 250, .componentNesting = 20, .limit = 20 },
        Row { .name = "the defaults: 100 lowered to 80",
              .engineNesting = defaultEngine,
              .componentNesting = defaultComponent,
              .limit = defaultComponent },
        Row { .name = "an engine's lower limit kept",
              .engineNesting = 30,
              .componentNesting = defaultComponent,
              .limit = 30 });
    auto const engine = GENERATE(from_range(Engines));
    CAPTURE(row.name);
    auto const cost = measureHop(engine);
    // rec(n) runs in n + 1 invocations: the outermost and n hops.
    auto recursion =
        RecursionHarness { engine, row.componentNesting, uint64_t { 256 } << 20, row.engineNesting };
    auto outcome = isoline::component::runtime::testing::CallOutcome { .result = {} };
    isoline::component::runtime::testing::runOnStack(stackFor(row.limit + 1, cost),
                                                     [&] { outcome = recursion.call(row.limit, 4096); });
    REQUIRE_FALSE(outcome.result.has_value());
    auto const* trap = std::get_if<isoline::component::canonical::Trap>(&outcome.result.error());
    REQUIRE(trap != nullptr);
    CHECK(trap->code == TrapCode::Core);
    CHECK(trap->core == isoline::api::TrapCode::CallStackExhausted);
    CHECK(recursion.harness.store->state() == StoreState::LockedDown);
    CHECK(recursion.harness.store->liveBytes()->live() == 0);

    // At the limit, it fits.
    auto fits = RecursionHarness { engine, row.componentNesting, uint64_t { 256 } << 20, row.engineNesting };
    isoline::component::runtime::testing::runOnStack(stackFor(row.limit, cost),
                                                     [&] { outcome = fits.call(row.limit - 1, 4096); });
    CHECK(u32Result(outcome) == 4096);
}

TEST_CASE("Calls.stackPerHop", "[.][measure]")
{
    // Prints what one component hop costs of the stack in this build, for docs/embedding.md's table.
    auto const engine = GENERATE(from_range(Engines));
    auto const cost = measureHop(engine);
    std::cout << std::format("component hop: {} bytes per hop, {} bytes base\n", cost.bytesPerHop, cost.base);
    CHECK(cost.bytesPerHop > 0);
}

TEST_CASE("Calls.oneInvocationScopePerCall")
{
    // take(s) runs the argument's realloc, the callee and post-return: one step budget covers all three,
    // so a budget of their sum succeeds and one less traps, though each piece alone fits in it.
    auto const engine = GENERATE(from_range(Engines));
    auto const bytes = singleModuleComponent(R"((module
      (memory (export "mem") 1)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32)
        (local $i i32)
        (loop (local.set $i (i32.add (local.get $i) (i32.const 1))) (br_if 0 (i32.lt_u (local.get $i) (i32.const 20))))
        (i32.const 64))
      (func (export "take") (param i32 i32) (result i32)
        (local $i i32)
        (loop (local.set $i (i32.add (local.get $i) (i32.const 1))) (br_if 0 (i32.lt_u (local.get $i) (i32.const 20))))
        (local.get 1))
      (func (export "post") (param i32)
        (local $i i32)
        (loop (local.set $i (i32.add (local.get $i) (i32.const 1))) (br_if 0 (i32.lt_u (local.get $i) (i32.const 20))))))
    )",
                                             {},
                                             { LiftedExport { .name = "take",
                                                              .params = { PrimitiveValueType::String },
                                                              .result = PrimitiveValueType::U32,
                                                              .memory = CanonicalMemory::MemoryAndRealloc,
                                                              .postReturn = "post" } });
    auto const callTake = [](Harness& harness) {
        return call(
            *harness.store, harness.instance(), "take", [](ValueBuilder& args) { args.string("abc"); });
    };
    auto measured = Harness { bytes, HarnessOptions { .engine = engine } };
    REQUIRE(u32Result(callTake(measured)) == 3);
    auto const steps = measured.store->coreStore().used(isoline::api::Limit::Steps);

    // Each piece alone, as a core call outside any scope, costs a part of what the call did together.
    auto& core = measured.store->coreStore();
    auto const& take = liftedExport(*measured.store, measured.instance(), "take");
    auto const stepsOf = [&](isoline::api::Function const& function,
                             std::vector<isoline::api::Value> args,
                             std::size_t results) {
        auto out = std::vector<isoline::api::Value>(results);
        REQUIRE(core.call(function, args, out).has_value());
        return core.used(isoline::api::Limit::Steps);
    };
    using isoline::api::I32;
    REQUIRE(take.options.realloc.has_value());
    REQUIRE(take.options.postReturn.has_value());
    auto const realloc = stepsOf(*take.options.realloc, { I32 { 0 }, I32 { 0 }, I32 { 1 }, I32 { 3 } }, 1);
    auto const callee = stepsOf(take.core, { I32 { 64 }, I32 { 3 } }, 1);
    auto const postReturn = stepsOf(*take.options.postReturn, { I32 { 3 } }, 0);
    CHECK(realloc > 0);
    CHECK(callee > 0);
    CHECK(postReturn > 0);
    CHECK(steps >= realloc + callee + postReturn);

    auto limits = isoline::api::StoreLimits {};
    limits.maxSteps = steps;
    auto exact = Harness { bytes, HarnessOptions { .engine = engine, .storeLimits = limits } };
    CHECK(u32Result(callTake(exact)) == 3);
    CHECK(exact.store->state() == StoreState::Live);

    limits.maxSteps = steps - 1;
    auto shortOfOne = Harness { bytes, HarnessOptions { .engine = engine, .storeLimits = limits } };
    auto const outcome = callTake(shortOfOne);
    REQUIRE_FALSE(outcome.result.has_value());
    auto const* trap = std::get_if<isoline::component::canonical::Trap>(&outcome.result.error());
    REQUIRE(trap != nullptr);
    CHECK(trap->code == TrapCode::Core);
    CHECK(shortOfOne.store->state() == StoreState::LockedDown);
}

TEST_CASE("Calls.uncaughtCoreExceptionIsTrap")
{
    auto const engine = GENERATE(from_range(Engines));
    auto const bytes = singleModuleComponent(R"((module
      (tag $t)
      (func (export "f") (throw $t)))
    )",
                                             {},
                                             { LiftedExport { .name = "f", .params = {} } });
    auto harness = Harness { bytes, HarnessOptions { .engine = engine } };
    auto const instance = harness.instance();
    CHECK(trapCodeOf(call(*harness.store, instance, "f")) == TrapCode::UncaughtException);
    CHECK(harness.store->state() == StoreState::LockedDown);
}

TEST_CASE("Calls.interruptIsTrapAndLocksDown")
{
    auto const engine = GENERATE(from_range(Engines));
    auto const bytes = singleModuleComponent(
        R"((module
      (func (export "f") (result i32)
        (local $i i32)
        (loop (local.set $i (i32.add (local.get $i) (i32.const 1))) (br_if 0 (i32.lt_u (local.get $i) (i32.const 1000))))
        (local.get $i)))
    )",
        {},
        { LiftedExport { .name = "f", .params = {}, .result = PrimitiveValueType::U32 } });
    auto harness = Harness { bytes, HarnessOptions { .engine = engine } };
    auto const instance = harness.instance();
    harness.store->coreStore().interrupter()->interrupt();
    CHECK(trapCodeOf(call(*harness.store, instance, "f")) == TrapCode::Interrupted);
    CHECK(harness.store->state() == StoreState::LockedDown);
}

TEST_CASE("Calls.lowerDuringStartFunction")
{
    // The core start function calls a lowered host function while no task is in progress.
    auto const engine = GENERATE(from_range(Engines));
    auto const bytes = singleModuleComponent(
        R"((module
      (import "host" "h" (func $h (result i32)))
      (global $g (mut i32) (i32.const 0))
      (func $start (global.set $g (call $h)))
      (start $start)
      (func (export "get") (result i32) (global.get $g)))
    )",
        { HostImport { .name = "h", .params = {}, .result = PrimitiveValueType::U32 } },
        { LiftedExport { .name = "get", .params = {}, .result = PrimitiveValueType::U32 } });
    auto harness = Harness { bytes, HarnessOptions { .engine = engine } };
    auto host =
        hostFunction([](ComponentStore&, Values const&, ValueBuilder& results)
                         -> std::expected<void, isoline::api::HostFailure> { return returnU32(results, 5); });
    auto const instance = harness.instance(importsOf({ host }));
    CHECK(host->calls == 1);
    CHECK(u32Result(call(*harness.store, instance, "get")) == 5);
}

TEST_CASE("Calls.reallocResultIsChecked")
{
    // take(list<u32>) lowers 8 bytes through a realloc that returns the word at address 0, and traps
    // while the word at 4 is set: a misaligned, out-of-bounds, wrapping or trapping one is refused.
    struct Row
    {
        std::string_view name;
        uint32_t address = 0;
        uint32_t traps = 0;
        TrapCode code = TrapCode::Core;
    };
    auto const row =
        GENERATE(Row { .name = "misaligned", .address = 66, .code = TrapCode::UnalignedPointer },
                 Row { .name = "past the end", .address = 65532, .code = TrapCode::OutOfBounds },
                 Row { .name = "wrapping", .address = 0xFFFF'FFFC, .code = TrapCode::OutOfBounds },
                 Row { .name = "trapping", .address = 64, .traps = 1, .code = TrapCode::Core });
    auto const engine = GENERATE(from_range(Engines));
    CAPTURE(row.name);
    auto const bytes = singleModuleComponent(R"((module
      (memory (export "mem") 1)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32)
        (if (i32.load (i32.const 4)) (then unreachable))
        (i32.load (i32.const 0)))
      (func (export "take") (param i32 i32)))
    )",
                                             {},
                                             { LiftedExport { .name = "take",
                                                              .params = { TypeIndex { .index = 0 } },
                                                              .memory = CanonicalMemory::MemoryAndRealloc } },
                                             { ListType { .element = PrimitiveValueType::U32 } });
    auto harness = Harness { bytes, HarnessOptions { .engine = engine } };
    auto& store = *harness.store;
    auto const instance = harness.instance();
    auto const& take = liftedExport(store, instance, "take");
    setWordAt(store, take, 0, row.address);
    setWordAt(store, take, 4, row.traps);
    auto const outcome =
        call(store, instance, "take", [](ValueBuilder& args) { args.begin(2).scalar(1).scalar(2).end(); });
    CHECK(trapCodeOf(outcome) == row.code);
    CHECK(store.state() == StoreState::LockedDown);
    CHECK(store.liveBytes()->live() == 0);
}

TEST_CASE("Calls.liftFailuresAreNamedTraps")
{
    // f's results are lifted from what its core function returns: each malformed one is its own trap.
    struct Row
    {
        std::string_view name;
        std::string_view body; ///< f's core body, returning i32.
        isoline::component::binary::ValueTypeRef result;
        TrapCode code = TrapCode::Core;
    };
    auto const row =
        GENERATE(Row { .name = "surrogate char",
                       .body = "(i32.const 0xD800)",
                       .result = PrimitiveValueType::Char,
                       .code = TrapCode::InvalidChar },
                 Row { .name = "char past the last",
                       .body = "(i32.const 0x110000)",
                       .result = PrimitiveValueType::Char,
                       .code = TrapCode::InvalidChar },
                 Row { .name = "misaligned return area",
                       .body = "(i32.const 1)",
                       .result = PrimitiveValueType::String,
                       .code = TrapCode::UnalignedPointer },
                 Row { .name = "return area past the end",
                       .body = "(i32.const 65536)",
                       .result = PrimitiveValueType::String,
                       .code = TrapCode::OutOfBounds },
                 Row { .name = "string past the end",
                       .body = "(i32.store (i32.const 0) (i32.const 65535)) (i32.store (i32.const 4) "
                               "(i32.const 2)) (i32.const 0)",
                       .result = PrimitiveValueType::String,
                       .code = TrapCode::OutOfBounds },
                 Row { .name = "invalid UTF-8",
                       .body = "(i32.store (i32.const 0) (i32.const 16)) (i32.store (i32.const 4) (i32.const "
                               "1)) (i32.store8 (i32.const 16) (i32.const 0xFF)) (i32.const 0)",
                       .result = PrimitiveValueType::String,
                       .code = TrapCode::InvalidEncoding },
                 Row { .name = "option case past the last",
                       .body = "(i32.store8 (i32.const 0) (i32.const 2)) (i32.const 0)",
                       .result = TypeIndex { .index = 0 },
                       .code = TrapCode::InvalidDiscriminant });
    auto const engine = GENERATE(from_range(Engines));
    CAPTURE(row.name);
    auto const module = std::format(R"((module
      (memory (export "mem") 1)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 256))
      (func (export "f") (result i32) {}))
    )",
                                    row.body);
    auto const bytes = singleModuleComponent(
        module,
        {},
        { LiftedExport {
            .name = "f", .params = {}, .result = row.result, .memory = CanonicalMemory::MemoryAndRealloc } },
        { isoline::component::binary::OptionType { .type = PrimitiveValueType::U32 } });
    auto harness = Harness { bytes, HarnessOptions { .engine = engine } };
    auto const outcome = call(*harness.store, harness.instance(), "f");
    CHECK(trapCodeOf(outcome) == row.code);
    CHECK_FALSE(outcome.values.has_value());
    CHECK(harness.store->state() == StoreState::LockedDown);
    CHECK(harness.store->liveBytes()->live() == 0);
}

TEST_CASE("Calls.liftChargesLiveBytesFirst")
{
    // f returns a list<u8> of 4096 bytes, past a 1024-byte budget: refused before it is copied.
    auto const engine = GENERATE(from_range(Engines));
    auto const bytes = singleModuleComponent(R"((module
      (memory (export "mem") 1)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 256))
      (func (export "f") (result i32)
        (i32.store (i32.const 0) (i32.const 16)) (i32.store (i32.const 4) (i32.const 4096)) (i32.const 0)))
    )",
                                             {},
                                             { LiftedExport { .name = "f",
                                                              .params = {},
                                                              .result = TypeIndex { .index = 0 },
                                                              .memory = CanonicalMemory::MemoryAndRealloc } },
                                             { ListType { .element = PrimitiveValueType::U8 } });
    auto limits = isoline::component::ComponentLimits {};
    limits.maxLiveValueBytes = 1024;
    auto harness = Harness { bytes, HarnessOptions { .engine = engine, .limits = limits } };
    auto const outcome = call(*harness.store, harness.instance(), "f");
    CHECK(trapCodeOf(outcome) == TrapCode::LiveValueBytes);
    CHECK(harness.store->state() == StoreState::LockedDown);
    CHECK(harness.store->liveBytes()->live() == 0);
}

namespace
{

/// Which way a host string test's string crosses.
enum class StringDirection : uint8_t
{
    FromHost, ///< h: func() -> string, its result lowered into the guest's return area at 8.
    ToHost,   ///< h: func(s: string), the guest passing 100 bytes at 65530, past its memory's end.
};

/// A component whose `run: func()` calls the host's h, lowered with a bump memory's options.
[[nodiscard]] std::vector<std::byte> hostStringComponent(StringDirection direction)
{
    auto builder = ComponentBuilder {};
    auto const type = direction == StringDirection::FromHost
                          ? functionType(builder, {}, PrimitiveValueType::String)
                          : functionType(builder, { PrimitiveValueType::String }, std::nullopt);
    auto const h = builder.import("h", ExternDesc { .sort = Sort::Func, .index = type });
    auto const memoryInstance = builder.coreInstantiate(builder.coreModule(coreModule(BumpMemoryModule)), {});
    auto const options = std::vector {
        option(CanonOptionKind::Memory, builder.aliasCoreExport(memoryInstance, "mem", CoreSort::Memory)),
        option(CanonOptionKind::Realloc, builder.aliasCoreExport(memoryInstance, "realloc", CoreSort::Func)),
    };
    auto const host = builder.coreInlineExports({ CoreInlineExport {
        .name = "h", .sort = CoreSort::Func, .index = builder.canonLower(h, options) } });
    auto const module = builder.coreModule(coreModule(direction == StringDirection::FromHost ? R"((module
      (import "host" "h" (func $h (param i32)))
      (func (export "run") (call $h (i32.const 8))))
    )"
                                                                                             : R"((module
      (import "host" "h" (func $h (param i32 i32)))
      (func (export "run") (call $h (i32.const 65530) (i32.const 100))))
    )"));
    auto const instance = builder.coreInstantiate(module, { { "host", host } });
    std::ignore = builder.exportItem(
        "run",
        Sort::Func,
        builder.canonLift(
            builder.aliasCoreExport(instance, "run", CoreSort::Func), {}, functionType(builder, {}, {})));
    return builder.build();
}

} // namespace

TEST_CASE("Calls.hostStringsAreChecked")
{
    // What the host hands over is checked as a guest's would be: a string that is no UTF-8 traps, one
    // past the budget traps, missing results are the host's failure; and a guest string past its memory
    // traps before the host runs.
    enum class HostDoes : uint8_t
    {
        InvalidUtf8,
        PastTheBudget,
        NoResult,
        Nothing,
    };
    struct Row
    {
        std::string_view name;
        StringDirection direction = StringDirection::FromHost;
        HostDoes host = HostDoes::Nothing;
        std::optional<TrapCode> code {}; ///< The trap; none for the host's ResultMismatch failure.
        uint32_t hostCalls = 1;
    };
    auto const row = GENERATE(
        Row { .name = "invalid UTF-8", .host = HostDoes::InvalidUtf8, .code = TrapCode::InvalidEncoding },
        Row { .name = "past the budget", .host = HostDoes::PastTheBudget, .code = TrapCode::LiveValueBytes },
        Row { .name = "no result", .host = HostDoes::NoResult },
        Row { .name = "argument past memory",
              .direction = StringDirection::ToHost,
              .code = TrapCode::OutOfBounds,
              .hostCalls = 0 });
    auto const engine = GENERATE(from_range(Engines));
    CAPTURE(row.name);
    auto limits = isoline::component::ComponentLimits {};
    limits.maxLiveValueBytes = 4096;
    auto harness =
        Harness { hostStringComponent(row.direction), HarnessOptions { .engine = engine, .limits = limits } };
    auto function = hostFunction([&](ComponentStore&, Values const&, ValueBuilder& results)
                                     -> std::expected<void, isoline::api::HostFailure> {
        switch (row.host)
        {
            case HostDoes::InvalidUtf8: {
                auto const encoded =
                    results.encodedString(isoline::component::canonical::StringEncoding::Utf8, 2);
                if (encoded.size() == 2)
                {
                    encoded[0] = std::byte { 0xFF };
                    encoded[1] = std::byte { 0xFE };
                }
                break;
            }
            case HostDoes::PastTheBudget: results.string(std::string(8192, 'x')); break;
            case HostDoes::NoResult:
            case HostDoes::Nothing: break;
        }
        return {};
    });
    auto const root = harness.instance(importsOf({ function }));
    auto const outcome = call(*harness.store, root, "run");
    REQUIRE_FALSE(outcome.result.has_value());
    if (row.code)
        CHECK(trapCodeOf(outcome.result.error()) == *row.code);
    else
        CHECK(std::get<isoline::api::HostFailure>(outcome.result.error()).kind
              == isoline::api::HostFailureKind::ResultMismatch);
    CHECK(function->calls == row.hostCalls);
    CHECK(harness.store->state() == StoreState::LockedDown);
    CHECK(harness.store->liveBytes()->live() == 0);
}

TEST_CASE("Calls.encodingsRoundTrip")
{
    // echo in latin1+utf16 and in an explicit utf8: Latin-1 text stays one byte a character, other text
    // switches to UTF-16; both come back as they went.
    auto const engine = GENERATE(from_range(Engines));
    auto const echo = [](std::string name, CanonOptionKind encoding) {
        return LiftedExport { .name = std::move(name),
                              .params = { PrimitiveValueType::String },
                              .result = PrimitiveValueType::String,
                              .memory = CanonicalMemory::MemoryAndRealloc,
                              .encoding = encoding };
    };
    auto const bytes = singleModuleComponent(
        R"((module
      (memory (export "mem") 1)
      (global $heap (mut i32) (i32.const 1024))
      (func (export "realloc") (param $old i32) (param $oldSize i32) (param $align i32) (param $new i32) (result i32)
        (local $ptr i32)
        (local.set $ptr (i32.and (i32.add (global.get $heap) (i32.sub (local.get $align) (i32.const 1)))
                                 (i32.sub (i32.const 0) (local.get $align))))
        (global.set $heap (i32.add (local.get $ptr) (local.get $new)))
        (memory.copy (local.get $ptr) (local.get $old)
                     (select (local.get $oldSize) (local.get $new) (i32.lt_u (local.get $oldSize) (local.get $new))))
        (local.get $ptr))
      (func $echo (param i32 i32) (result i32)
        (i32.store (i32.const 8) (local.get 0))
        (i32.store (i32.const 12) (local.get 1))
        (i32.const 8))
      (export "latin1" (func $echo))
      (export "utf8" (func $echo)))
    )",
        {},
        { echo("latin1", CanonOptionKind::Latin1Utf16), echo("utf8", CanonOptionKind::Utf8) });
    auto harness = Harness { bytes, HarnessOptions { .engine = engine } };
    auto const instance = harness.instance();
    for (auto const* function: { "latin1", "utf8" })
        for (auto const text: { std::string_view { "a\xC3\xA9" }, std::string_view { "a\xF0\x9F\x98\x80" } })
        {
            CAPTURE(function, text);
            auto const outcome =
                call(*harness.store, instance, function, [&](ValueBuilder& args) { args.string(text); });
            REQUIRE(outcome.result.has_value());
            REQUIRE(outcome.values.has_value());
            CHECK(outcome.values->root(0).string().utf8() == text);
        }
}

TEST_CASE("Calls.guestMemoryChecksEveryAccess")
{
    // GuestMemory bounds every access by the memory's size at that moment, polls the store, and runs no
    // realloc in a locked-down store.
    auto const bytes = singleModuleComponent(
        R"((module
      (memory (export "mem") 1)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 64))
      (func (export "f")))
    )",
        {},
        { LiftedExport { .name = "f", .params = {}, .memory = CanonicalMemory::MemoryAndRealloc } });
    auto harness = Harness { bytes };
    auto& store = *harness.store;
    auto const instance = harness.instance();
    auto const& f = liftedExport(store, instance, "f");
    auto memory =
        isoline::component::runtime::GuestMemory { store, *f.options.memory, f.options.realloc, instance };
    CHECK(memory.size() == 65536);
    auto two = std::array<std::byte, 2> {};
    CHECK(memory.read(65534, two).has_value());
    CHECK(trapIn(memory.read(65535, two)).code == TrapCode::OutOfBounds);
    CHECK(trapIn(memory.write(65535, two)).code == TrapCode::OutOfBounds);
    CHECK(memory.reallocate(0, 0, 1, 8)
          == std::expected<uint32_t, isoline::component::canonical::Trap> { 64 });
    {
        auto const scope = store.coreStore().openInvocation();
        CHECK(memory.poll().has_value());
        store.coreStore().interrupter()->interrupt();
        CHECK(trapIn(memory.poll()).code == TrapCode::Interrupted);
    }
    store.lockDown();
    CHECK(trapIn(memory.reallocate(0, 0, 1, 8)).code == TrapCode::CannotEnter);
}

TEST_CASE("Calls.hostCalleeRefusedInALockedDownStore")
{
    // callComponentFunction() refuses a host callee in a locked-down store before it asks for arguments.
    auto harness = Harness { reentrantComponent() };
    auto host =
        hostFunction([](ComponentStore&, Values const&, ValueBuilder& results)
                         -> std::expected<void, isoline::api::HostFailure> { return returnU32(results, 1); });
    auto& store = *harness.store;
    auto const& entry = liftedExport(store, harness.instance(importsOf({ host })), "call");
    store.lockDown();
    auto starts = uint32_t { 0 };
    auto start = [&]() -> std::expected<isoline::component::runtime::CallArguments, CallError> {
        ++starts;
        return std::unexpected { CallError { embedderFailure() } };
    };
    auto resolve = [](Values) -> std::expected<void, CallError> {
        return {};
    };
    auto const called = isoline::component::runtime::callComponentFunction(
        store,
        isoline::component::runtime::ComponentFunction { host },
        entry.side,
        entry.type,
        start,
        resolve);
    REQUIRE_FALSE(called.has_value());
    CHECK(trapCodeOf(called.error()) == TrapCode::CannotEnter);
    CHECK(starts == 0);
    CHECK(host->calls == 0);
}

TEST_CASE("Calls.borrowHeldAtReturnTraps")
{
    // take(b: borrow<r>) of a host resource returns without dropping the borrow it was lent: task.return
    // traps, and the store locks down.
    auto const engine = GENERATE(from_range(Engines));
    auto builder = ComponentBuilder {};
    auto const resource =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const borrow =
        builder.type(TypeDefinition { .type = DefinedValueType { BorrowType { .resource = resource } } });
    auto const module = builder.coreModule(coreModule(R"((module (func (export "take") (param i32))))"));
    auto const instance = builder.coreInstantiate(module, {});
    std::ignore =
        builder.exportItem("take",
                           Sort::Func,
                           builder.canonLift(builder.aliasCoreExport(instance, "take", CoreSort::Func),
                                             {},
                                             functionType(builder, { TypeIndex { .index = borrow } }, {})));
    auto harness = Harness { builder.build(), HarnessOptions { .engine = engine } };
    auto const identity = std::make_shared<HostResourceIdentity const>(HostResourceIdentity { .name = "r" });
    auto imports = ResolvedImports {};
    imports.values.emplace_back(HostResourceImport { .identity = identity });
    imports.imports = { 0 };
    auto& store = *harness.store;
    auto const root = harness.instance(imports);
    auto const type = store.hostResourceTypeOf(*identity);
    REQUIRE(type.has_value());
    auto const outcome = call(store, root, "take", [&](ValueBuilder& args) {
        args.resource(ResourceValue { .type = *type, .rep = 5, .ownership = Ownership::Borrow });
    });
    CHECK(trapCodeOf(outcome) == TrapCode::BorrowOutstanding);
    CHECK(store.state() == StoreState::LockedDown);
}

TEST_CASE("Calls.guestDestructorRunsOnDrop")
{
    // run(rep) makes a resource of rep and drops it: r's destructor records rep, and traps (through the
    // host's failing h) for rep 13; r2 has no destructor, and its drop runs none.
    auto const engine = GENERATE(from_range(Engines));
    auto builder = ComponentBuilder {};
    auto const hType = functionType(builder, {}, std::nullopt);
    auto const h = builder.import("h", ExternDesc { .sort = Sort::Func, .index = hType });
    auto const host = builder.coreInlineExports(
        { CoreInlineExport { .name = "h", .sort = CoreSort::Func, .index = builder.canonLower(h, {}) } });
    auto const first = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "host" "h" (func $h))
      (memory (export "mem") 1)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 64))
      (func (export "dtor") (param $rep i32)
        (i32.store (i32.const 0) (i32.add (i32.load (i32.const 0)) (i32.const 1)))
        (i32.store (i32.const 4) (local.get $rep))
        (if (i32.eq (local.get $rep) (i32.const 13)) (then (call $h))))
      (func (export "get") (result i32) (i32.load (i32.const 4))))
    )")),
                                               { { "host", host } });
    auto const r = builder.resource(builder.aliasCoreExport(first, "dtor", CoreSort::Func));
    auto const r2 = builder.resource();
    auto const builtin = [&](CanonBuiltin which, uint32_t type) {
        return builder.canon(CanonDefinition {
            .builtin = which, .immediates = { type }, .options = {}, .result = std::nullopt });
    };
    auto const builtins = builder.coreInlineExports({
        CoreInlineExport {
            .name = "new", .sort = CoreSort::Func, .index = builtin(CanonBuiltin::ResourceNew, r) },
        CoreInlineExport {
            .name = "drop", .sort = CoreSort::Func, .index = builtin(CanonBuiltin::ResourceDrop, r) },
        CoreInlineExport {
            .name = "new2", .sort = CoreSort::Func, .index = builtin(CanonBuiltin::ResourceNew, r2) },
        CoreInlineExport {
            .name = "drop2", .sort = CoreSort::Func, .index = builtin(CanonBuiltin::ResourceDrop, r2) },
    });
    auto const second = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "r" "new" (func $new (param i32) (result i32)))
      (import "r" "drop" (func $drop (param i32)))
      (import "r" "new2" (func $new2 (param i32) (result i32)))
      (import "r" "drop2" (func $drop2 (param i32)))
      (func (export "run") (param $rep i32) (call $drop (call $new (local.get $rep))))
      (func (export "run2") (param $rep i32) (call $drop2 (call $new2 (local.get $rep)))))
    )")),
                                                { { "r", builtins } });
    auto const runType = functionType(builder, { PrimitiveValueType::U32 }, std::nullopt);
    for (auto const* name: { "run", "run2" })
        std::ignore = builder.exportItem(
            name,
            Sort::Func,
            builder.canonLift(builder.aliasCoreExport(second, name, CoreSort::Func), {}, runType));
    auto const getOptions = std::vector {
        option(CanonOptionKind::Memory, builder.aliasCoreExport(first, "mem", CoreSort::Memory)),
        option(CanonOptionKind::Realloc, builder.aliasCoreExport(first, "realloc", CoreSort::Func)),
    };
    std::ignore = builder.exportItem("get",
                                     Sort::Func,
                                     builder.canonLift(builder.aliasCoreExport(first, "get", CoreSort::Func),
                                                       getOptions,
                                                       functionType(builder, {}, PrimitiveValueType::U32)));

    auto harness = Harness { builder.build(), HarnessOptions { .engine = engine } };
    auto failing = hostFunction(
        [](ComponentStore&, Values const&, ValueBuilder&) -> std::expected<void, isoline::api::HostFailure> {
            return std::unexpected { embedderFailure(3) };
        });
    auto& store = *harness.store;
    auto const root = harness.instance(importsOf({ failing }));
    auto const& get = liftedExport(store, root, "get");
    auto const runWith = [&](std::string_view name, uint32_t rep) {
        return call(store, root, name, [&](ValueBuilder& args) { args.scalar(rep); });
    };
    REQUIRE(runWith("run", 7).result.has_value());
    CHECK(wordAt(store, get, 0) == 1);
    CHECK(wordAt(store, get, 4) == 7);
    REQUIRE(runWith("run2", 8).result.has_value());
    CHECK(wordAt(store, get, 0) == 1);
    // The destructor's host failure ends the drop as a trap, and the call.
    CHECK(trapCodeOf(runWith("run", 13)) == TrapCode::Core);
    CHECK(failing->calls == 1);
    CHECK(store.state() == StoreState::LockedDown);
}
