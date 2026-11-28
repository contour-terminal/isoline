// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>
#include <optional>
#include <vector>

#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::component::text
{

/// Reads a component of the text format (Explainer.md, as wast 263 reads it) into the binary::ComponentPool
/// it denotes, which binary::encodeComponent() encodes as wasm-tools 1.263.0 encodes the same text.
///
/// Every form the AST has a shape for is read: core modules (their fields through P10's
/// isoline::text::ModuleReader), core instances and types (module types included), nested components,
/// instances, every alias form, every defvaltype, function types (`(func async …)` too), component and
/// instance types, resources, every row of binary::CanonBuiltinTable by its text name, imports and
/// exports with name attributes, and `@custom`. Its sugar is expanded as wast expands it: inline value
/// and function types become type definitions before their use, inline type uses type definitions,
/// inline imports imports, inline exports exports after every field, `(with "n" (instance …))` bundles
/// instances before their use, `(func …)`/`(core func …)` the canon or alias they abbreviate, and an
/// export name after an index (`(func $i "f")`) the export aliases it reaches. A `$id` defined in an
/// enclosing scope, a module, type or component, gets an outer alias before its use, as wast inserts it.
///
/// Identifiers are resolved per index space and scope, forward references included; numeric indices are
/// kept as written, for validation to check. Whenever a component's definitions or the component itself
/// carry an identifier or an `@name`, its scope ends with a `component-name` custom section, laid out as
/// wasm-tools lays it out. `(component quote "…"*)` reads the quoted text; `(component binary "…"*)`
/// decodes its bytes (binary::decodeComponent under the reader's features and limits), a binary that
/// does not decode refused as an unexpected token at `binary`.
///
/// Gated forms are read whatever the features, as the decoder decodes them, and validation refuses
/// them; the features are those a binary form decodes under. Forms the AST has no shape for (`start`,
/// values, `core rec` and core GC types) are refused as unexpected tokens.
///
/// No recursion: scopes (components, component and instance types, module types) are read from a work
/// list, one after its enclosing scope is resolved, and inline value types on an explicit stack. Scopes
/// open at once are capped by ComponentLimits::maxNesting (module types, held in place, uncounted, as the
/// decoder counts them), and the embedded core modules' bytes together by
/// ComponentLimits::maxComponentBytes, each by TextLimits::maxModuleBytes too. Memory is linear in the
/// source; every lookup is in an ordered map, never a hash table a source could collide.
class ComponentReader
{
  public:
    /// A reader of the given caps and features.
    /// @param text The caps on reading text, the embedded core modules' included.
    /// @param limits The component caps: maxNesting, maxComponentBytes, and a binary form's decode caps.
    /// @param features The gated features a binary form decodes under.
    ComponentReader(isoline::text::TextLimits text,
                    ComponentLimits limits,
                    ComponentFeatureSet features) noexcept:
        _text { text }, _limits { limits }, _features { features }
    {
    }

    /// Reads one `(component …)` node of a parsed tree: `(component $id? field*)`, the script form
    /// `(component definition $id? field*)`, or a binary or quote form.
    /// @param tree The tree; its source must outlive the call.
    /// @param component The `(component …)` list.
    /// @return The pool, or the first error.
    [[nodiscard]] std::expected<binary::ComponentPool, isoline::text::TextError> read(
        isoline::text::SExpressionTree const& tree, isoline::text::NodeId component) const;

  private:
    isoline::text::TextLimits _text; ///< The caps on reading text.
    ComponentLimits _limits;         ///< The component caps.
    ComponentFeatureSet _features;   ///< What a binary form decodes under.
};

/// The `binary` keyword of a binary form, `(component definition? $id? (@name …)? binary "…"*)`: only in
/// that place, after the head, so that a `binary` anywhere else is the field it is refused as.
/// @param tree The tree.
/// @param component A `(component …)` list of @p tree, whose first child is the `component` keyword.
/// @return The keyword's node, or nothing for a form that is no binary form.
[[nodiscard]] std::optional<isoline::text::NodeId> binaryKeywordOf(isoline::text::SExpressionTree const& tree,
                                                                   isoline::text::NodeId component);

/// The bytes of a binary form: its strings after the `binary` keyword, concatenated; annotations among
/// them are ignored.
/// @param tree The tree.
/// @param component The `(component …)` list, where too many bytes are reported.
/// @param keyword Its `binary` keyword, as binaryKeywordOf() found it.
/// @param maxBytes The most bytes the form may hold.
/// @return The bytes; or UnexpectedToken (or UnknownOperator) at what is no string, or OutputTooLarge.
[[nodiscard]] std::expected<std::vector<std::byte>, isoline::text::TextError> binaryFormBytes(
    isoline::text::SExpressionTree const& tree,
    isoline::text::NodeId component,
    isoline::text::NodeId keyword,
    std::size_t maxBytes);

} // namespace isoline::component::text
