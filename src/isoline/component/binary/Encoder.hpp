// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>

namespace isoline::component::binary
{

/// Encodes a pool as a component binary, the decoder's inverse.
///
/// The encoding is canonical: every LEB128 in its shortest form; adjacent definitions of one section
/// kind in one section (a custom section, a core module and a nested component are a section each),
/// and no section for a vector of none, which defines nothing the pool could keep;
/// a name without attributes in the plain form (0x00); core types in their shortest forms (a
/// nullable reference to an abstract heap as its shorthand, a memory of 64 KiB pages without the
/// custom-page-sizes field). Core module bytes and custom section payloads are copied verbatim.
/// Nested components and type scopes are written from an explicit stack, never by recursion, in
/// time and memory linear in the output: the output once while it is laid out and once written,
/// plus a constant per pool entry (its layout, its piece list and one piece per nested scope: about
/// 100 bytes per scope on libstdc++).
///
/// Precondition: @p pool is well formed, as decodeComponent() and testing::ComponentBuilder make
/// pools: entry 0 is a component; each NestedComponent, ComponentTypeDefinition and
/// InstanceTypeDefinition names a later entry of its kind; every byte range lies in
/// ComponentPool::bytes; every sort, option and immediate is one the grammar has a byte for in its
/// position; and a component's scope holds no ExportDeclaration, a type scope only declarations.
/// @param pool The pool to encode.
/// @return The component's bytes, from its preamble on.
[[nodiscard]] std::vector<std::byte> encodeComponent(ComponentPool const& pool);

/// Canonicalizes the core modules embedded in a component; P10's core canonicalize is wired in Task 18.
class ICoreModuleCanonicalizer
{
  public:
    virtual ~ICoreModuleCanonicalizer() = default;

    /// The canonical form of one embedded core module.
    /// @param module The module's bytes, from its preamble on.
    /// @return Its canonical bytes, or why it has none.
    [[nodiscard]] virtual std::expected<std::vector<std::byte>, std::string> canonicalize(
        std::span<std::byte const> module) = 0;
};

/// The canonicalizer that keeps every embedded core module as it is.
class VerbatimCoreModules final: public ICoreModuleCanonicalizer
{
  public:
    /// The bytes of @p module, unchanged.
    /// @param module The module's bytes.
    /// @return A copy of them.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> canonicalize(
        std::span<std::byte const> module) override;
};

/// The canonical form of a component: decoded, its embedded modules canonicalized, encoded again.
///
/// Minimal LEBs, merged adjacent same-kind sections and the other forms encodeComponent() writes;
/// the embedded modules through @p modules, each called once, in ordinal order (coreModulePositions()).
/// Canonicalizing a canonical component returns it unchanged when @p modules is idempotent.
/// @param bytes The component's bytes, from its preamble on.
/// @param features The gated features whose forms are admitted, as decodeComponent() takes them.
/// @param limits The caps to enforce, as decodeComponent() takes them.
/// @param modules Canonicalizes each embedded core module.
/// @return The canonical bytes; or the decoder's error; or DecodeErrorCode::CoreModuleRefused at the
///         refused module's offset when @p modules fails (its message is not kept).
[[nodiscard]] std::expected<std::vector<std::byte>, DecodeError> canonicalizeComponent(
    std::span<std::byte const> bytes,
    ComponentFeatureSet features,
    ComponentLimits const& limits,
    ICoreModuleCanonicalizer& modules);

} // namespace isoline::component::binary
