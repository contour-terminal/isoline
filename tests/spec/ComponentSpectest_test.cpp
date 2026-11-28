// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ComponentSpectest.hpp"
#include <isoline/api/Engine.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/api/Component.hpp>
#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/text/Forms.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::component::api::Component;
using isoline::component::api::ComponentConfig;
using isoline::component::api::ComponentStore;
using isoline::component::api::EngineCoreModuleLoader;
using isoline::component::api::EngineCoreModuleValidator;
using isoline::spectest::ComponentSpectestRows;
using isoline::spectest::makeComponentSpectest;
using isoline::spectest::SpectestRowKind;

namespace
{

/// A guest of the spectest's resource1: run(rep) makes a resource1 of @p rep through the host's
/// constructor, checks it through [static]resource1.assert and [method]resource1.simple, drops it, and
/// returns the host's drop count times 1000 plus the representation it last dropped.
constexpr auto DropGuest = std::string_view { R"((component
  (import "host" (instance $host
    (export "resource1" (type $r (sub resource)))
    (export "[constructor]resource1" (func (param "r" u32) (result (own $r))))
    (export "[static]resource1.assert" (func (param "r" (own $r)) (param "rep" u32)))
    (export "[method]resource1.simple" (func (param "self" (borrow $r)) (param "rep" u32)))
    (export "[static]resource1.drops" (func (result u32)))
    (export "[static]resource1.last-drop" (func (result u32)))
  ))
  (alias export $host "resource1" (type $r))
  (alias export $host "[constructor]resource1" (func $ctor))
  (alias export $host "[static]resource1.assert" (func $assert))
  (alias export $host "[method]resource1.simple" (func $simple))
  (alias export $host "[static]resource1.drops" (func $drops))
  (alias export $host "[static]resource1.last-drop" (func $last-drop))
  (core func $ctor (canon lower (func $ctor)))
  (core func $assert (canon lower (func $assert)))
  (core func $simple (canon lower (func $simple)))
  (core func $drops (canon lower (func $drops)))
  (core func $last-drop (canon lower (func $last-drop)))
  (core func $drop (canon resource.drop $r))
  (core module $m
    (import "" "ctor" (func $ctor (param i32) (result i32)))
    (import "" "assert" (func $assert (param i32 i32)))
    (import "" "simple" (func $simple (param i32 i32)))
    (import "" "drop" (func $drop (param i32)))
    (import "" "drops" (func $drops (result i32)))
    (import "" "last-drop" (func $last (result i32)))
    (func (export "run") (param $rep i32) (result i32)
      (local $handle i32)
      (local.set $handle (call $ctor (local.get $rep)))
      (call $simple (local.get $handle) (local.get $rep))
      (call $drop (local.get $handle))
      (call $assert (call $ctor (i32.const 9)) (i32.const 9))
      (i32.add (i32.mul (call $drops) (i32.const 1000)) (call $last))))
  (core instance $i (instantiate $m (with "" (instance
    (export "ctor" (func $ctor)) (export "assert" (func $assert)) (export "simple" (func $simple))
    (export "drop" (func $drop)) (export "drops" (func $drops)) (export "last-drop" (func $last-drop))))))
  (func (export "run") (param "rep" u32) (result u32) (canon lift (core func $i "run")))
))" };

} // namespace

TEST_CASE("ComponentSpectest.rowsMatchWasmtime")
{
    // link_component_spectest's definitions in its order (wasmtime v51, crates/wast/src/spectest.rs),
    // `func_wrap_concurrent` the async ones, and the instance each lies in.
    struct Expected
    {
        std::string_view path;
        SpectestRowKind kind;
    };
    auto const expected = std::vector<Expected> {
        { .path = "host-echo-u32", .kind = SpectestRowKind::Async },
        { .path = "host-return-two", .kind = SpectestRowKind::Function },
        { .path = "host#return-three", .kind = SpectestRowKind::Function },
        { .path = "host#nested#return-four", .kind = SpectestRowKind::Function },
        { .path = "host#simple-module", .kind = SpectestRowKind::Module },
        { .path = "host#resource1", .kind = SpectestRowKind::Resource },
        { .path = "host#resource2", .kind = SpectestRowKind::Resource },
        { .path = "host#resource1-again", .kind = SpectestRowKind::Resource },
        { .path = "host#[constructor]resource1", .kind = SpectestRowKind::Function },
        { .path = "host#[static]resource1.assert", .kind = SpectestRowKind::Function },
        { .path = "host#[static]resource1.last-drop", .kind = SpectestRowKind::Function },
        { .path = "host#[static]resource1.drops", .kind = SpectestRowKind::Function },
        { .path = "host#[method]resource1.simple", .kind = SpectestRowKind::Function },
        { .path = "host#[method]resource1.take-borrow", .kind = SpectestRowKind::Function },
        { .path = "host#[method]resource1.take-own", .kind = SpectestRowKind::Function },
        { .path = "host#never-return", .kind = SpectestRowKind::Async },
        { .path = "host#return-two-slowly", .kind = SpectestRowKind::Async },
        { .path = "host#echo-slowly", .kind = SpectestRowKind::Async },
        { .path = "host#[method]resource1.never-return", .kind = SpectestRowKind::Async },
        { .path = "host#return-hi", .kind = SpectestRowKind::Function },
        { .path = "host#unit-future", .kind = SpectestRowKind::Async },
    };
    REQUIRE(ComponentSpectestRows.size() == expected.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, expected.size()))
    {
        INFO(expected[index].path);
        CHECK(ComponentSpectestRows.at(index).path == expected[index].path);
        CHECK(ComponentSpectestRows.at(index).kind == expected[index].kind);
    }

    auto const engine = isoline::api::Engine { isoline::api::EngineConfig {} };
    auto const config = ComponentConfig {};
    auto linker = makeComponentSpectest(engine, config);
    if (!linker)
        FAIL(linker.error());

    // Every row but the async ones is defined: a definition at its path is refused as a redefinition.
    for (auto const& row: ComponentSpectestRows)
    {
        INFO(row.path);
        auto const again = linker->defineResource(
            std::string { row.path }, isoline::component::api::HostResourceType { "probe", nullptr });
        if (row.kind == SpectestRowKind::Async)
            CHECK(again.has_value());
        else
        {
            REQUIRE_FALSE(again.has_value());
            CHECK(again.error().code == isoline::component::api::LinkerErrorCode::Redefinition);
        }
    }

    // resource1's drops count through a guest: the guest drops the resource of 42 and hands its own of
    // 9 to the host, which keeps it, as Wasmtime's host does.
    auto fresh = makeComponentSpectest(engine, config);
    REQUIRE(fresh.has_value());
    auto const bytes =
        isoline::text::compileText(DropGuest, isoline::text::TextLimits {}, isoline::component::text::Forms);
    if (!bytes)
        FAIL(isoline::text::specMessage(bytes.error()));
    auto loader = EngineCoreModuleLoader { engine };
    auto validator = EngineCoreModuleValidator { engine };
    auto const component = Component::load(engine, *bytes, config, loader, validator);
    REQUIRE(component.has_value());
    auto allocator = isoline::api::HeapMemoryAllocator {};
    auto store = ComponentStore { engine, allocator, config };
    auto instance = store.instantiate(*component, *fresh);
    REQUIRE(instance.has_value());
    auto const run = instance->function("run");
    REQUIRE(run.has_value());
    REQUIRE(run->has_value());
    auto const callWith = [&](uint32_t rep) {
        auto builder = instance->argumentsFor(**run);
        REQUIRE(builder.has_value());
        builder->scalar(rep);
        auto const arguments = builder->finish();
        REQUIRE(arguments.has_value());
        auto const result = instance->call(**run, *arguments);
        if (!result)
            FAIL(result.error().message());
        return result->root(0).scalar<uint32_t>();
    };
    CHECK(callWith(42) == 1042);
    CHECK(callWith(7) == 2007);
}
