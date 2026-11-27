// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <string_view>

#include <isoline/binary/Module.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

/// Reads a module of the text format (spec §6.6) into the binary::Module it denotes.
///
/// Identifiers are resolved per index space (types, functions, tables, memories, globals, tags,
/// element and data segments, and each function's locals) and recorded in a name section, appended
/// last as binary::SynthesizedSections places it. A type use without an explicit type reuses the first
/// identical function type, or appends one after every explicit type, in order of first use. Numeric
/// indices are kept as written, out of range or not: the validator refuses them, so that invalid
/// modules still read. Reading takes memory linear in the source, and time linear in it but for a
/// logarithmic factor: every lookup, of an identifier, a label or an implicit type, is in an ordered
/// map, never a hash table, which a source could collide; and a deferred error is located only when
/// reported.
///
/// The annotations Isoline understands are read once every field has (Annotations.hpp): `@custom`
/// into custom sections where their placements put them, `@name` into the name section over the name
/// an identifier gives, and `@metadata.code.branch_hint` into binary::Module::branchHints. Every other
/// annotation is ignored wherever it stands.
class ModuleReader
{
  public:
    /// Builds a reader.
    /// @param limits The caps on reading text, held for every read.
    explicit ModuleReader(TextLimits limits) noexcept: _limits { limits } {}

    /// Reads one `(module …)` node of a parsed tree: `(module $id? field*)`, or the script form
    /// `(module definition $id? field*)`. Its identifier names the module in the name section.
    /// @param tree The tree; its source must outlive the call.
    /// @param module The `(module …)` list.
    /// @return The module, or the first error.
    [[nodiscard]] std::expected<binary::Module, TextError> read(SExpressionTree const& tree,
                                                                NodeId module) const;

    /// Reads the top level of a parsed tree as a bare sequence of module fields.
    /// @param tree The tree; its source must outlive the call.
    /// @return The module, or the first error.
    [[nodiscard]] std::expected<binary::Module, TextError> readInline(SExpressionTree const& tree) const;

  private:
    TextLimits _limits; ///< The caps on reading text.
};

/// The keyword a list starts with.
/// @param tree The tree.
/// @param list A node of @p tree, or NoNode.
/// @return The text of its first child that is no annotation, when that is a Keyword atom; empty for
///         NoNode, an atom, an empty list, or a list that starts with anything else.
[[nodiscard]] std::string_view headKeyword(SExpressionTree const& tree, NodeId list) noexcept;

/// The first node at a tree's top level that is no annotation: the form a whole text holds, when it
/// holds one.
/// @param tree The tree.
/// @return The node, or NoNode for a text of annotations only, or of nothing.
[[nodiscard]] NodeId firstTopLevelNode(SExpressionTree const& tree) noexcept;

/// Requires a form to be the whole text: no node but annotations at the top level after it.
/// @param tree The tree.
/// @param form A top-level node of @p tree.
/// @return Nothing, or the error the first node after @p form is refused with (unexpectedKeyword()).
[[nodiscard]] std::expected<void, TextError> requireSoleTopLevelNode(SExpressionTree const& tree,
                                                                     NodeId form);

/// Refuses a known annotation outside a text's form: the reference reads the annotations within each
/// module only, and refuses any it leaves (parse.ml), once the module has read. Unknown ones are
/// ignored, wherever they stand.
/// @param tree The tree.
/// @param form The top-level node that is the text's form.
/// @return Nothing, or MisplacedAnnotation at the first known annotation at the top level.
[[nodiscard]] std::expected<void, TextError> refuseAnnotationsOutside(SExpressionTree const& tree,
                                                                      NodeId form);

} // namespace isoline::text
