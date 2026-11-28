// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <isoline/api/Engine.hpp>
#include <isoline/component/api/Component.hpp>
#include <isoline/component/api/Linker.hpp>

// The host definitions every component script may import, as Wasmtime v51's link_component_spectest
// defines them (crates/wast/src/spectest.rs) in full (D12).
namespace isoline::spectest
{

/// What a ComponentSpectestRows row defines.
enum class SpectestRowKind : uint8_t
{
    Function, ///< A host function.
    Module,   ///< A core module.
    Resource, ///< A host resource type.
    Async,    ///< An async host function: not defined before P12, so an import of it does not link.
};

/// One definition of the component spectest: its linker path and what it is.
struct ComponentSpectestRow
{
    std::string_view path; ///< Its path, segments joined by component::api::PathSeparator.
    SpectestRowKind kind;  ///< What it defines.
};

/// The reason a command importing an async row is skipped for: a SkipReasons row's.
inline constexpr std::string_view AsyncSkipReason = "async (P12)";

/// Every definition of link_component_spectest, in its order. `resource1-again` is `resource1`'s
/// type under a second name, so the two are one resource type; `resource2` is a type of its own.
inline constexpr auto ComponentSpectestRows = std::to_array<ComponentSpectestRow>({
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
});

/// A linker holding every row of ComponentSpectestRows but the async ones: host functions written with
/// typed::makeTypedHostFunction, `simple-module` through Linker::defineModule(). The resource1 functions
/// share one count of the resource1 drops and the representation of the last dropped.
/// @param engine The engine `simple-module` is loaded on, which the components it links must share.
/// @param config The features and caps of the linker.
/// @return The linker; or why a definition failed.
/// @throws std::bad_alloc When a definition cannot be stored.
[[nodiscard]] std::expected<component::api::Linker, std::string> makeComponentSpectest(
    isoline::api::Engine const& engine, component::api::ComponentConfig const& config);

} // namespace isoline::spectest
