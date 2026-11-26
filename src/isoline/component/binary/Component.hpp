// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <isoline/component/Features.hpp>

namespace isoline::component::binary
{

/// A core value type, as a resource's representation and the canonical ABI's flat types name it.
enum class CoreValueKind : uint8_t
{
    I32,
    I64,
    F32,
    F64,
};

/// A core sort (`core:sort` of Binary.md): one of the index spaces of core definitions.
enum class CoreSort : uint8_t
{
    Func,
    Table,
    Memory,
    Global,
    Tag,
    Type,
    Module,
    Instance,
};

/// A sort (`sort` of Binary.md): one of the 13 index spaces of a component, the eight core ones
/// first, in CoreSort's order.
enum class Sort : uint8_t
{
    CoreFunc,
    CoreTable,
    CoreMemory,
    CoreGlobal,
    CoreTag,
    CoreType,
    CoreModule,
    CoreInstance,
    Func,
    Value,
    Type,
    Component,
    Instance,
};

/// The sort of the core index space @p sort.
/// @param sort A core sort.
/// @return The Sort of the same index space.
[[nodiscard]] constexpr Sort sortOf(CoreSort sort) noexcept
{
    static_assert(static_cast<uint8_t>(Sort::CoreInstance) == static_cast<uint8_t>(CoreSort::Instance),
                  "Sort lists the core sorts first, in CoreSort's order");
    return static_cast<Sort>(sort);
}

/// One definition of a sort's index space: `sortidx` of Binary.md.
struct SortIndex
{
    Sort sort;          ///< The index space.
    uint32_t index = 0; ///< The index in it, checked by validation.

    /// Two references are equal when their sort and index are.
    bool operator==(SortIndex const&) const = default;
};

/// What an import or an export is typed as, against a type bound: equal to a type, or a fresh
/// resource type.
enum class TypeBound : uint8_t
{
    Equal,       ///< `(eq i)`: the type at the index.
    SubResource, ///< `(sub resource)`: a fresh resource type.
};

/// An import's or an export's type: `externtype` of Binary.md. The descriptor of the AST; the
/// validator's interned form is types::ExternType.
struct ExternDesc
{
    /// What is imported or exported: Sort::CoreModule, Func, Value, Type, Component or Instance.
    Sort sort;
    /// The type index: of a core module type for CoreModule, of the type for Func, Component and
    /// Instance, and of the bound type for Type under TypeBound::Equal.
    uint32_t index = 0;
    TypeBound bound = TypeBound::Equal; ///< For Sort::Type: the type bound; Equal otherwise.

    /// Two descriptors are equal when their sort, index and bound are.
    bool operator==(ExternDesc const&) const = default;
};

/// 🏷️ `(implements "in")`: the interface an instance import or export implements.
struct Implements
{
    std::string interfaceName; ///< The interface's name, as encoded.

    /// Two attributes are equal when their names are.
    bool operator==(Implements const&) const = default;
};

/// 🔗 `(versionsuffix "vs")`: the rest of a canonical interface name's version.
struct VersionSuffix
{
    std::string suffix; ///< The suffix, as encoded.

    /// Two attributes are equal when their suffixes are.
    bool operator==(VersionSuffix const&) const = default;
};

/// 🏷️ `(external-id n)`: an identifier the embedder gives an import or export.
struct ExternalId
{
    std::string id; ///< The identifier, as encoded.

    /// Two attributes are equal when their identifiers are.
    bool operator==(ExternalId const&) const = default;
};

/// One `attribute` of a name: the alternatives are in NameAttributeTable's order.
using Attribute = std::variant<Implements, VersionSuffix, ExternalId>;

/// One row of NameAttributeTable: an attribute's byte, its gate and its name in messages.
struct NameAttributeRow
{
    uint8_t byte;             ///< Its discriminator in `attribute`; its alternative in Attribute.
    ComponentFeature gate;    ///< The feature that introduced it.
    std::string_view keyword; ///< Its name as wasm-tools' messages spell it.
};

/// Every attribute, in Attribute's order, which is the order of their bytes.
inline constexpr auto NameAttributeTable = std::to_array<NameAttributeRow>({
    { .byte = 0x00, .gate = ComponentFeature::Implements, .keyword = "implements" },
    { .byte = 0x01, .gate = ComponentFeature::CanonicalInterfaceNames, .keyword = "version-suffix" },
    { .byte = 0x02, .gate = ComponentFeature::ExternalId, .keyword = "external-id" },
});

static_assert(NameAttributeTable.size() == std::variant_size_v<Attribute>,
              "NameAttributeTable holds one row per Attribute alternative");

/// A name with its attributes: `nameattributes` of Binary.md. The name is not checked against the
/// `externname` grammar here; validation does that.
struct NameAttributes
{
    std::string name;                  ///< The name, valid UTF-8.
    std::vector<Attribute> attributes; ///< At most one of each kind, in encoded order.

    /// Two names are equal when their names and attributes are.
    bool operator==(NameAttributes const&) const = default;
};

/// An embedded core module: `section_1(core:module)`. Its bytes are not decoded with the component
/// (D10): they are a range of ComponentPool::bytes, whose preamble has been checked.
struct CoreModuleDefinition
{
    uint64_t offset = 0; ///< Where the module starts in ComponentPool::bytes.
    uint64_t size = 0;   ///< The module's size in bytes.

    /// Two definitions are equal when their ranges are.
    bool operator==(CoreModuleDefinition const&) const = default;
};

/// One argument of a core instantiation: `(with n (instance i))`.
struct CoreInstantiateArgument
{
    std::string name;      ///< The import module name it supplies.
    uint32_t instance = 0; ///< The core instance supplied.

    /// Two arguments are equal when their names and instances are.
    bool operator==(CoreInstantiateArgument const&) const = default;
};

/// `(instantiate m arg*)`: a core instance made by instantiating a core module.
struct CoreInstantiate
{
    uint32_t module = 0;                            ///< The core module instantiated.
    std::vector<CoreInstantiateArgument> arguments; ///< Its arguments, in encoded order.

    /// Two expressions are equal when their modules and arguments are.
    bool operator==(CoreInstantiate const&) const = default;
};

/// One core inline export: `(export n (sort idx))`.
struct CoreInlineExport
{
    std::string name;   ///< The export's name.
    CoreSort sort;      ///< Its sort, one of the core extern kinds (func, table, memory, global, tag).
    uint32_t index = 0; ///< The definition exported.

    /// Two exports are equal when their names, sorts and indices are.
    bool operator==(CoreInlineExport const&) const = default;
};

/// A core instance made of core inline exports.
struct CoreInlineExports
{
    std::vector<CoreInlineExport> exports; ///< The exports, in encoded order.

    /// Two expressions are equal when their exports are.
    bool operator==(CoreInlineExports const&) const = default;
};

/// A core instance: `core:instance` of Binary.md.
struct CoreInstanceDefinition
{
    std::variant<CoreInstantiate, CoreInlineExports> expression; ///< How the instance is made.

    /// Two definitions are equal when their expressions are.
    bool operator==(CoreInstanceDefinition const&) const = default;
};

/// A core type: `core:type` of Binary.md. Reserved: section 3 is refused as
/// DecodeErrorCode::UnimplementedSection until the type decoder (P11 Task 2) fills this in.
struct CoreTypeDefinition
{
    /// Two definitions are equal; there is nothing to tell them apart yet.
    bool operator==(CoreTypeDefinition const&) const = default;
};

/// A nested component: `section_4(component)`, decoded into its own entry of the pool.
struct NestedComponent
{
    uint32_t index = 0; ///< Its index in ComponentPool::components.

    /// Two definitions are equal when they name the same pool entry.
    bool operator==(NestedComponent const&) const = default;
};

/// One argument of a component instantiation: `(with n si)`.
struct InstantiateArgument
{
    std::string name; ///< The import name it supplies.
    SortIndex item;   ///< The definition supplied.

    /// Two arguments are equal when their names and items are.
    bool operator==(InstantiateArgument const&) const = default;
};

/// `(instantiate c arg*)`: a component instance made by instantiating a component.
struct Instantiate
{
    uint32_t component = 0;                     ///< The component instantiated.
    std::vector<InstantiateArgument> arguments; ///< Its arguments, in encoded order.

    /// Two expressions are equal when their components and arguments are.
    bool operator==(Instantiate const&) const = default;
};

/// One inline export: `(export na si)`.
struct InlineExport
{
    NameAttributes name; ///< The export's name.
    SortIndex item;      ///< The definition exported.

    /// Two exports are equal when their names and items are.
    bool operator==(InlineExport const&) const = default;
};

/// A component instance made of inline exports.
struct InlineExports
{
    std::vector<InlineExport> exports; ///< The exports, in encoded order.

    /// Two expressions are equal when their exports are.
    bool operator==(InlineExports const&) const = default;
};

/// A component instance: `instance` of Binary.md.
struct InstanceDefinition
{
    std::variant<Instantiate, InlineExports> expression; ///< How the instance is made.

    /// Two definitions are equal when their expressions are.
    bool operator==(InstanceDefinition const&) const = default;
};

/// Where an alias takes its definition from.
enum class AliasTarget : uint8_t
{
    Export,     ///< `(alias export i n (s))`: an export of a component instance.
    CoreExport, ///< `(alias core export i n (s))`: an export of a core instance.
    Outer,      ///< `(alias outer ct idx (s))`: a definition of an enclosing scope.
};

/// An alias: `alias` of Binary.md.
struct AliasDefinition
{
    Sort sort;               ///< The sort of the definition it introduces.
    AliasTarget target;      ///< Where it comes from.
    uint32_t instance = 0;   ///< Export and CoreExport: the (core) instance.
    std::string name;        ///< Export and CoreExport: the export's name.
    uint32_t outerCount = 0; ///< Outer: the enclosing scopes to count outward, 0 the current one.
    uint32_t outerIndex = 0; ///< Outer: the index in the target scope's index space of the sort.

    /// Two aliases are equal when every field is.
    bool operator==(AliasDefinition const&) const = default;
};

/// A type: `type` of Binary.md. Reserved: section 7 is refused as DecodeErrorCode::UnimplementedSection
/// until the type decoder (P11 Task 2) fills this in.
struct TypeDefinition
{
    /// Two definitions are equal; there is nothing to tell them apart yet.
    bool operator==(TypeDefinition const&) const = default;
};

/// A canonical definition: `canon` of Binary.md. Reserved: section 8 is refused as
/// DecodeErrorCode::UnimplementedSection until the canon decoder (P11 Task 2) fills this in.
struct CanonDefinition
{
    /// Two definitions are equal; there is nothing to tell them apart yet.
    bool operator==(CanonDefinition const&) const = default;
};

/// An import: `import` of Binary.md.
struct ImportDefinition
{
    NameAttributes name; ///< The import's name.
    ExternDesc type;     ///< What it imports.

    /// Two imports are equal when their names and types are.
    bool operator==(ImportDefinition const&) const = default;
};

/// An export: `export` of Binary.md.
struct ExportDefinition
{
    NameAttributes name;            ///< The export's name.
    SortIndex item;                 ///< The definition exported.
    std::optional<ExternDesc> type; ///< The type it is ascribed, if any.

    /// Two exports are equal when their names, items and types are.
    bool operator==(ExportDefinition const&) const = default;
};

/// A custom section: `section_0(core:custom)`. Its payload is not interpreted.
struct CustomDefinition
{
    std::string name;    ///< The section's name, valid UTF-8.
    uint64_t offset = 0; ///< Where its payload, after the name, starts in ComponentPool::bytes.
    uint64_t size = 0;   ///< The payload's size in bytes.

    /// Two sections are equal when their names and payload ranges are.
    bool operator==(CustomDefinition const&) const = default;
};

/// One definition of a component, of any section.
using Definition = std::variant<CoreModuleDefinition,
                                CoreInstanceDefinition,
                                CoreTypeDefinition,
                                NestedComponent,
                                InstanceDefinition,
                                AliasDefinition,
                                TypeDefinition,
                                CanonDefinition,
                                ImportDefinition,
                                ExportDefinition,
                                CustomDefinition>;

/// A decoded component: its definitions in section order, a section's vector flattened in place.
struct Component
{
    std::vector<Definition> definitions; ///< Every definition, custom sections included.
};

/// A decoded component and every component nested in it.
///
/// Nested components are entries of one flat pool, never members of their parent, so that nothing
/// walks or destroys them recursively. The decoder copies its input into bytes, so no definition
/// aliases the caller's memory.
struct ComponentPool
{
    /// The encoded component, as given to the decoder; the ranges of definitions index it.
    std::shared_ptr<std::vector<std::byte> const> bytes;
    /// The outermost component at index 0, then every nested one in the order the decoder met them,
    /// which is pre-order.
    std::vector<Component> components;
};

} // namespace isoline::component::binary
