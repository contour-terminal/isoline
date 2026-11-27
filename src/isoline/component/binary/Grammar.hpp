// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include <isoline/binary/Types.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>

/// The bytes of Binary.md's grammar that both the decoder and the encoder speak, so that each byte
/// has one row: what the decoder reads, the encoder writes. Private to isoline_component.
namespace isoline::component::binary
{

/// The size of a preamble: the magic number, then the version and layer word.
inline constexpr auto PreambleSize = std::size_t { 8 };

/// The magic number every WebAssembly binary starts with, "\0asm".
inline constexpr auto Magic = std::to_array<std::byte>(
    { std::byte { 0x00 }, std::byte { 0x61 }, std::byte { 0x73 }, std::byte { 0x6D } });

/// One version and layer word, and the kind of binary it introduces.
struct PreambleRow
{
    std::array<std::byte, 4> word; ///< The four bytes after the magic number.
    LayerKind layer;               ///< What they announce.
};

/// The preambles there are: a core module's, and the pre-standard component's (version 0x0d).
inline constexpr auto PreambleTable = std::to_array<PreambleRow>({
    { .word = { std::byte { 0x01 }, std::byte { 0x00 }, std::byte { 0x00 }, std::byte { 0x00 } },
      .layer = LayerKind::CoreModule },
    { .word = { std::byte { 0x0D }, std::byte { 0x00 }, std::byte { 0x01 }, std::byte { 0x00 } },
      .layer = LayerKind::Component },
});

/// One kind byte, or a pair of them led by 0x00 for a core sort, and the sort it names.
struct KindRow
{
    uint8_t byte;                         ///< The first byte.
    std::optional<uint8_t> coreByte;      ///< The core sort byte after a first byte of 0x00.
    Sort sort;                            ///< The sort named.
    std::optional<ComponentFeature> gate; ///< The feature that introduced it.
};

/// What a component may name by sort, in a sortidx, an export alias and an extern type:
/// wasm-tools' ComponentExternalKind, of the core sorts the module alone.
inline constexpr auto ExternalKinds = std::to_array<KindRow>({
    { .byte = 0x00, .coreByte = 0x11, .sort = Sort::CoreModule, .gate = std::nullopt },
    { .byte = 0x01, .coreByte = std::nullopt, .sort = Sort::Func, .gate = std::nullopt },
    { .byte = 0x02, .coreByte = std::nullopt, .sort = Sort::Value, .gate = ComponentFeature::Values },
    { .byte = 0x03, .coreByte = std::nullopt, .sort = Sort::Type, .gate = std::nullopt },
    { .byte = 0x04, .coreByte = std::nullopt, .sort = Sort::Component, .gate = std::nullopt },
    { .byte = 0x05, .coreByte = std::nullopt, .sort = Sort::Instance, .gate = std::nullopt },
});

/// What an outer alias may name (`outeraliassort`).
inline constexpr auto OuterAliasKinds = std::to_array<KindRow>({
    { .byte = 0x00, .coreByte = 0x10, .sort = Sort::CoreType, .gate = std::nullopt },
    { .byte = 0x00, .coreByte = 0x11, .sort = Sort::CoreModule, .gate = std::nullopt },
    { .byte = 0x03, .coreByte = std::nullopt, .sort = Sort::Type, .gate = std::nullopt },
    { .byte = 0x04, .coreByte = std::nullopt, .sort = Sort::Component, .gate = std::nullopt },
});

/// The byte of `sort` that a core sort's byte follows.
inline constexpr auto CoreSortPrefix = uint8_t { 0x00 };

/// A core extern kind's byte, as core inline exports and core export aliases name a definition.
struct CoreExternKindRow
{
    uint8_t byte;  ///< The byte.
    CoreSort sort; ///< The core sort it names.
};

/// The core extern kinds: func, table, memory, global and tag, in CoreSort's order, which is also
/// isoline::binary::ExternKind's.
inline constexpr auto CoreExternKinds = std::to_array<CoreExternKindRow>({
    { .byte = 0x00, .sort = CoreSort::Func },
    { .byte = 0x01, .sort = CoreSort::Table },
    { .byte = 0x02, .sort = CoreSort::Memory },
    { .byte = 0x03, .sort = CoreSort::Global },
    { .byte = 0x04, .sort = CoreSort::Tag },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: CoreExternKinds)
        {
            if (static_cast<std::size_t>(row.sort) != index)
                return false;
            ++index;
        }
        return index == isoline::binary::ExternKindCount;
    }(),
    "CoreExternKinds lists the core extern kinds in CoreSort's order, which is ExternKind's");

/// An alias's target byte and the target it names.
struct AliasTargetRow
{
    uint8_t byte;       ///< The byte after the sort.
    AliasTarget target; ///< The target it names.
};

/// Every alias target.
inline constexpr auto AliasTargets = std::to_array<AliasTargetRow>({
    { .byte = 0x00, .target = AliasTarget::Export },
    { .byte = 0x01, .target = AliasTarget::CoreExport },
    { .byte = 0x02, .target = AliasTarget::Outer },
});

/// Whether a name's form carries attributes.
enum class NameForm : uint8_t
{
    Plain,      ///< The name alone.
    Attributed, ///< The name, then a vector of attributes (🏷️/🔗).
};

/// A `nameattributes` form byte and the form it introduces.
struct NameFormRow
{
    uint8_t byte;  ///< The byte.
    NameForm form; ///< The form.
};

/// Every name form; 0x01 is the legacy interface-name byte wasm-tools still reads as 0x00. The
/// encoder writes the first row of a form, so a canonical name never uses 0x01.
inline constexpr auto NameForms = std::to_array<NameFormRow>({
    { .byte = 0x00, .form = NameForm::Plain },
    { .byte = 0x01, .form = NameForm::Plain },
    { .byte = 0x02, .form = NameForm::Attributed },
});

/// A type bound's byte and the bound it introduces; Equal reads a type index after it.
struct TypeBoundRow
{
    uint8_t byte;    ///< The byte.
    TypeBound bound; ///< The bound.
};

/// Every type bound.
inline constexpr auto TypeBounds = std::to_array<TypeBoundRow>({
    { .byte = 0x00, .bound = TypeBound::Equal },
    { .byte = 0x01, .bound = TypeBound::SubResource },
});

/// The bytes of `deftype` that open a scope of declarations, and the scope each opens.
struct TypeScopeFormRow
{
    uint8_t byte;    ///< The byte.
    ScopeKind scope; ///< The scope it opens.
};

/// Every form of `deftype` that holds declarations: component and instance types.
inline constexpr auto TypeScopeForms = std::to_array<TypeScopeFormRow>({
    { .byte = 0x41, .scope = ScopeKind::ComponentType },
    { .byte = 0x42, .scope = ScopeKind::InstanceType },
});

/// The byte of `deftype` that introduces a resource type.
inline constexpr auto ResourceTypeForm = uint8_t { 0x3F };

/// The byte of `core:deftype` that introduces a module type; after a 0x00, a non-final subtype.
inline constexpr auto ModuleTypeForm = uint8_t { 0x50 };

/// The byte that introduces a core function type, as the core binary format has it.
inline constexpr auto CoreFuncTypeForm = uint8_t { 0x60 };

/// The byte that prefixes a non-final core subtype among a component's core types.
inline constexpr auto NonFinalSubTypePrefix = uint8_t { 0x00 };

/// The two bytes of `core:alias`: the core type sort, then the outer target.
inline constexpr auto CoreOuterAliasSort = uint8_t { 0x10 };
inline constexpr auto CoreOuterAliasTarget = uint8_t { 0x01 };

/// The bytes of `<T>?`: absent, or present and followed by a T.
inline constexpr auto Absent = uint8_t { 0x00 };
inline constexpr auto Present = uint8_t { 0x01 };

/// The bytes of `resultlist`: one result type, or none (0x01 0x00).
inline constexpr auto OneResult = uint8_t { 0x00 };
inline constexpr auto NoResults = uint8_t { 0x01 };

/// The count byte after NoResults: once a vector of named results, now always empty.
inline constexpr auto NoResultsCount = uint8_t { 0x00 };

/// The byte that ends a variant case, once its `refines`.
inline constexpr auto CaseEnd = uint8_t { 0x00 };

/// The forms of `core:instance` and `instance`: an instantiation, or inline exports; the byte is the
/// index of the expression's alternative in CoreInstanceDefinition and InstanceDefinition.
inline constexpr auto InstantiateForm = uint8_t { 0x00 };
inline constexpr auto InlineExportsForm = uint8_t { 0x01 };

/// The byte of `core:instantiatearg` that names its argument an instance, the one sort allowed.
inline constexpr auto CoreInstantiateArgumentKind = uint8_t { 0x12 };

/// The `func` sort byte that `canon lift` and `canon lower` read before their function.
inline constexpr auto CanonFuncSort = uint8_t { 0x00 };

/// The id of a component section, which opens a frame rather than holding definitions.
inline constexpr auto ComponentSectionId = uint8_t { 4 };

/// How the definitions of one Definition alternative are written into sections.
enum class SectionGrouping : uint8_t
{
    Vector,      ///< `section_N(vec(...))`: a run of adjacent definitions shares one section.
    Single,      ///< One definition per section: a custom section, a core module, a component.
    Declaration, ///< None: it is a declaration of a component or instance type only.
};

/// One row of DefinitionSectionTable: where a Definition alternative is written in a component.
struct DefinitionSectionRow
{
    uint8_t id;               ///< Its section's id; unused for SectionGrouping::Declaration.
    SectionGrouping grouping; ///< How definitions of it share sections.
};

/// The section of each Definition alternative, at the index of the alternative.
inline constexpr auto DefinitionSectionTable = std::to_array<DefinitionSectionRow>({
    { .id = 1, .grouping = SectionGrouping::Single },                  // CoreModuleDefinition
    { .id = 2, .grouping = SectionGrouping::Vector },                  // CoreInstanceDefinition
    { .id = 3, .grouping = SectionGrouping::Vector },                  // CoreTypeDefinition
    { .id = ComponentSectionId, .grouping = SectionGrouping::Single }, // NestedComponent
    { .id = 5, .grouping = SectionGrouping::Vector },                  // InstanceDefinition
    { .id = 6, .grouping = SectionGrouping::Vector },                  // AliasDefinition
    { .id = 7, .grouping = SectionGrouping::Vector },                  // TypeDefinition
    { .id = 8, .grouping = SectionGrouping::Vector },                  // CanonDefinition
    { .id = 10, .grouping = SectionGrouping::Vector },                 // ImportDefinition
    { .id = 11, .grouping = SectionGrouping::Vector },                 // ExportDefinition
    { .id = 0, .grouping = SectionGrouping::Single },                  // CustomDefinition
    { .id = 0, .grouping = SectionGrouping::Declaration },             // ExportDeclaration
});

static_assert(DefinitionSectionTable.size() == std::variant_size_v<Definition>,
              "DefinitionSectionTable holds one row per Definition alternative");

/// The declaration a Definition alternative is in a component or instance type, at the index of the
/// alternative; empty for the alternatives that are no declaration.
inline constexpr auto DefinitionDeclarationTable = std::to_array<std::optional<DeclarationKind>>({
    std::nullopt,              // CoreModuleDefinition
    std::nullopt,              // CoreInstanceDefinition
    DeclarationKind::CoreType, // CoreTypeDefinition
    std::nullopt,              // NestedComponent
    std::nullopt,              // InstanceDefinition
    DeclarationKind::Alias,    // AliasDefinition
    DeclarationKind::Type,     // TypeDefinition
    std::nullopt,              // CanonDefinition
    DeclarationKind::Import,   // ImportDefinition
    std::nullopt,              // ExportDefinition
    std::nullopt,              // CustomDefinition
    DeclarationKind::Export,   // ExportDeclaration
});

static_assert(DefinitionDeclarationTable.size() == std::variant_size_v<Definition>,
              "DefinitionDeclarationTable holds one row per Definition alternative");

} // namespace isoline::component::binary
