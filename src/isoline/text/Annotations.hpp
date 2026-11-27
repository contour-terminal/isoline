// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>

// The annotations Isoline understands (SExpression.hpp's KnownAnnotations), as the reference
// interpreter's custom handlers read them (`interpreter/custom/handler_custom.ml`, `handler_name.ml` and
// `handler_branch_hint.ml` at wg-3.0): `@custom` into a custom section, `@name` into the name section,
// and `@metadata.code.branch_hint` into binary::Module::branchHints. The module reader records where
// it finds each one it accepts in place; readAnnotations() then reads them all once the module has
// parsed, so that every syntax error of the module comes before any annotation's, as in the reference.
namespace isoline::text
{

/// Which side of a section an `@custom` annotation places its custom section on.
enum class PlacementSide : uint8_t
{
    Before, ///< `(before …)`: right before the section.
    After,  ///< `(after …)`: right after it; the default.
};

/// One word of an `@custom` placement that names a section.
struct SectionWord
{
    std::string_view word;         ///< The word.
    binary::SectionAnchor section; ///< The section it names.
};

/// The words of a placement's section (handler_custom.ml's parse_section): one per section, and
/// `first` and `last` for the first and last of them. The writer spells an anchor by its first row.
inline constexpr auto SectionWords = std::to_array<SectionWord>({
    { .word = "type", .section = binary::SectionAnchor::Type },
    { .word = "import", .section = binary::SectionAnchor::Import },
    { .word = "func", .section = binary::SectionAnchor::Function },
    { .word = "table", .section = binary::SectionAnchor::Table },
    { .word = "memory", .section = binary::SectionAnchor::Memory },
    { .word = "tag", .section = binary::SectionAnchor::Tag },
    { .word = "global", .section = binary::SectionAnchor::Global },
    { .word = "export", .section = binary::SectionAnchor::Export },
    { .word = "start", .section = binary::SectionAnchor::Start },
    { .word = "elem", .section = binary::SectionAnchor::Element },
    { .word = "code", .section = binary::SectionAnchor::Code },
    { .word = "data", .section = binary::SectionAnchor::Data },
    { .word = "datacount", .section = binary::SectionAnchor::DataCount },
    { .word = "first", .section = binary::SectionAnchor::Type },
    { .word = "last", .section = binary::SectionAnchor::Data },
});

/// Where an `@custom` annotation asks for its custom section: before or after a section, whether the
/// module has that section or not. Placements order as the reference's `compare_place`: by section,
/// and before a section ahead of after it.
struct CustomPlacement
{
    /// The section, Type to Data, never BeforeFirst: `first` names the type section, `last` the data
    /// section.
    binary::SectionAnchor section = binary::SectionAnchor::Data;
    PlacementSide side = PlacementSide::After; ///< Which side of it.

    /// Placements compare by section, then by side.
    [[nodiscard]] constexpr auto operator<=>(CustomPlacement const&) const noexcept = default;
};

/// An `@custom` annotation, read.
struct CustomAnnotation
{
    binary::CustomSection section; ///< Its name and contents; its anchor is placeCustomSections()'s.
    CustomPlacement placement;     ///< The placement it asks for; `(after last)` without one.
};

/// Reads an `@custom` annotation: `(@custom "name" placement? "content"*)`, its contents concatenated.
/// Where it stands is not checked: only a module field may be one (readAnnotations() checks that).
/// @param tree The tree.
/// @param annotation An `@custom` Annotation node of @p tree.
/// @return The annotation, or the first error: CustomMissingName without a string first,
///         CustomMalformedUtf8 when that is no UTF-8, CustomMalformedPlacement for a list that does not
///         start with `before` or `after`, CustomMalformedSectionKind when no section follows that,
///         and CustomUnexpectedToken at anything left after the contents or the section.
[[nodiscard]] std::expected<CustomAnnotation, TextError> readCustomAnnotation(SExpressionTree const& tree,
                                                                              NodeId annotation);

/// Appends custom sections to a module where their placements put them: ordered by placement, equal
/// placements in the order given, each anchored after the last section at or before its place that
/// @p module has (binary::writtenAnchor()), so that decoding the encoding finds the same anchors.
///
/// Precondition: every non-custom section of @p module is final, the data count included. The
/// sections go after the custom sections @p module holds already in each slot.
/// @param customs The annotations, in source order.
/// @param module The module they belong to.
void placeCustomSections(std::vector<CustomAnnotation> customs, binary::Module& module);

/// What an `@name` annotation names: an entry of one name subsection.
struct NameTarget
{
    binary::NameKind kind = binary::NameKind::Module; ///< The subsection.
    uint32_t index = 0; ///< A direct subsection's entity, or an indirect one's outer entity (a function).
    uint32_t inner = 0; ///< An indirect subsection's inner entity (a local or label); otherwise 0.
};

/// What a branch hint annotation in a defined function names.
struct HintTarget
{
    uint32_t function = 0; ///< The function, in the function index space.
    /// The index in the function's body of the first instruction (no else or end) that starts after
    /// the annotation in source order; nothing when none does.
    std::optional<uint32_t> instruction;
};

/// Where a module's reader found the known annotations it accepts in place. Every other known
/// annotation of the module is misplaced.
struct AnnotationSites
{
    std::set<NodeId> moduleFields;      ///< The annotations that are module fields: `@custom`'s place.
    std::map<NodeId, NameTarget> names; ///< The `@name` annotations where a name may stand.
    std::map<NodeId, HintTarget> hints; ///< The branch hint annotations in a defined function.
};

/// The known annotations among some nodes, without looking into any annotation: what one holds is its
/// own contents, which its handler reads. One loop over the nodes, never recursion, whatever the
/// annotations nest to.
/// @param tree The tree.
/// @param first The first node.
/// @param end The node after the last one, at most nodes().size().
/// @return The known Annotation nodes among them, in source order.
[[nodiscard]] std::vector<NodeId> knownAnnotationsIn(SExpressionTree const& tree, NodeId first, NodeId end);

/// Reads a module's known annotations into it once the module has parsed, kind by kind in
/// KnownAnnotations order, as the reference reads them; within a kind, every annotation's own errors come
/// before any error between two of them. The reference records annotations by prepending them, so its
/// custom and name handlers meet them, and report them, last first; its branch hint handler reverses
/// them into source order.
/// - `@custom`: its contents, then MisplacedCustom unless it is a module field; then the custom
///   sections are placed, after every custom section @p module holds.
/// - `@metadata.code.branch_hint`: its contents concatenated (BranchHintUnexpectedToken at a token
///   that is no string), BranchHintNotInFunction outside a defined function, BranchHintInvalidValue
///   unless they are "\00" or "\01", BranchHintInvalidPlacement when no instruction follows; then
///   BranchHintDuplicate for a hint whose instruction is not after the one hinted before it in its
///   function. The hints become binary::Module::branchHints.
/// - `@name`: one string (NameStringExpected, NameMalformedUtf8, NameUnexpectedToken), then
///   MisplacedName unless it stands where a name may; then NameMultipleModule, NameMultipleFunctionNames
///   or NameMultipleLocalNames for a second name of one entity, by its subsection's shape. Each name
///   replaces the one an identifier gave, in @p names.
/// @param tree The tree the module was read from.
/// @param annotations The module's known annotations, in source order (knownAnnotationsIn()).
/// @param sites Where the reader accepted them.
/// @param module The module read; receives the custom sections and the branch hints.
/// @param names The names the module's identifiers give; receives the names the annotations give.
/// @return Nothing, or the first error.
[[nodiscard]] std::expected<void, TextError> readAnnotations(SExpressionTree const& tree,
                                                             std::span<NodeId const> annotations,
                                                             AnnotationSites const& sites,
                                                             binary::Module& module,
                                                             binary::NameSection& names);

} // namespace isoline::text
