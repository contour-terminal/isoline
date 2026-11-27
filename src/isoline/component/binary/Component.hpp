// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
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

/// One row of CoreValueKindTable: a core value type's byte and its kind.
struct CoreValueKindRow
{
    uint8_t byte;       ///< Its byte, as `core:valtype` encodes it.
    CoreValueKind kind; ///< The kind it names.
};

/// The core value types a resource's representation and `context.get`/`context.set` name, in
/// CoreValueKind's order.
inline constexpr auto CoreValueKindTable = std::to_array<CoreValueKindRow>({
    { .byte = 0x7F, .kind = CoreValueKind::I32 },
    { .byte = 0x7E, .kind = CoreValueKind::I64 },
    { .byte = 0x7D, .kind = CoreValueKind::F32 },
    { .byte = 0x7C, .kind = CoreValueKind::F64 },
});

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
    // wasm-tools gates external-id under cm-implements (component.rs:4785-4790).
    { .byte = 0x02, .gate = ComponentFeature::Implements, .keyword = "external-id" },
});

static_assert(NameAttributeTable.size() == std::variant_size_v<Attribute>,
              "NameAttributeTable holds one row per Attribute alternative");

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: NameAttributeTable)
        {
            if (row.byte != index)
                return false;
            ++index;
        }
        return true;
    }(),
    "NameAttributeTable lists the attributes by byte, which is the index of their Attribute alternative");

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

/// What a core module type declares an import or an export to be: `core:externtype` of the core
/// binary format, decoded by isoline::binary::decodeEmbeddedExternType().
struct CoreExternType
{
    isoline::binary::ExternKind kind = isoline::binary::ExternKind::Func; ///< Selects the field below.
    uint32_t typeIndex = 0;                ///< Func and Tag: the function type's index.
    isoline::binary::TableType table {};   ///< Table: the table's type.
    isoline::binary::MemoryType memory {}; ///< Memory: the memory's type.
    isoline::binary::GlobalType global {}; ///< Global: the global's type.

    /// Two extern types are equal when every field is.
    bool operator==(CoreExternType const&) const = default;
};

/// `0x00 (import m n t)` of `core:moduledecl`: an import the module type declares.
struct CoreImportDeclaration
{
    std::string module;  ///< The import's module name.
    std::string name;    ///< The import's field name.
    CoreExternType type; ///< What it imports.

    /// Two declarations are equal when their names and types are.
    bool operator==(CoreImportDeclaration const&) const = default;
};

/// `0x01 (type rt)` of `core:moduledecl`: a type of the module type's own type index space.
struct CoreTypeDeclaration
{
    isoline::binary::FuncType type; ///< The function type; core GC's rec groups do not decode.

    /// Two declarations are equal when their types are.
    bool operator==(CoreTypeDeclaration const&) const = default;
};

/// `0x02 (alias outer ct idx (type))` of `core:moduledecl`: a core type of an enclosing scope.
struct CoreOuterAliasDeclaration
{
    uint32_t outerCount = 0; ///< The enclosing scopes to count outward.
    uint32_t outerIndex = 0; ///< The core type's index in that scope.

    /// Two aliases are equal when their counts and indices are.
    bool operator==(CoreOuterAliasDeclaration const&) const = default;
};

/// `0x03 (export n t)` of `core:moduledecl`: an export the module type declares.
struct CoreExportDeclaration
{
    std::string name;    ///< The export's name.
    CoreExternType type; ///< What it exports.

    /// Two declarations are equal when their names and types are.
    bool operator==(CoreExportDeclaration const&) const = default;
};

/// One `core:moduledecl`; the alternatives are in the order of their bytes, 0x00 to 0x03.
using CoreModuleDeclaration = std::
    variant<CoreImportDeclaration, CoreTypeDeclaration, CoreOuterAliasDeclaration, CoreExportDeclaration>;

/// What a declaration of a core module type is, in CoreModuleDeclaration's order.
enum class CoreDeclarationKind : uint8_t
{
    Import, ///< `0x00 core:import`: a CoreImportDeclaration.
    Type,   ///< `0x01 core:rectype`: a CoreTypeDeclaration.
    Alias,  ///< `0x02 core:alias`: a CoreOuterAliasDeclaration.
    Export, ///< `0x03 core:exportdecl`: a CoreExportDeclaration.
};

/// One row of CoreModuleDeclarationTable: a declarator's byte and what it declares.
struct CoreModuleDeclarationRow
{
    uint8_t byte;             ///< Its byte in `core:moduledecl`.
    CoreDeclarationKind kind; ///< What it declares; its CoreModuleDeclaration alternative.
};

/// Every declarator of `core:moduledecl`, in CoreModuleDeclaration's order.
inline constexpr auto CoreModuleDeclarationTable = std::to_array<CoreModuleDeclarationRow>({
    { .byte = 0x00, .kind = CoreDeclarationKind::Import },
    { .byte = 0x01, .kind = CoreDeclarationKind::Type },
    { .byte = 0x02, .kind = CoreDeclarationKind::Alias },
    { .byte = 0x03, .kind = CoreDeclarationKind::Export },
});

static_assert(CoreModuleDeclarationTable.size() == std::variant_size_v<CoreModuleDeclaration>,
              "CoreModuleDeclarationTable holds one row per CoreModuleDeclaration alternative");

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: CoreModuleDeclarationTable)
        {
            if (static_cast<std::size_t>(row.kind) != index)
                return false;
            ++index;
        }
        return true;
    }(),
    "CoreModuleDeclarationTable lists the declarators in CoreModuleDeclaration's order");

/// `0x50 md*` of `core:moduletype`: a core module's type. It cannot nest, so its declarations are
/// held in place rather than in a scope of the pool.
struct CoreModuleTypeDefinition
{
    std::vector<CoreModuleDeclaration> declarations; ///< In encoded order.

    /// Two module types are equal when their declarations are.
    bool operator==(CoreModuleTypeDefinition const&) const = default;
};

/// A core type: `core:type` of Binary.md. Core GC's rec groups and subtypes are not decoded (a
/// feature cut of Isoline's core): a function type, or a module type.
struct CoreTypeDefinition
{
    std::variant<isoline::binary::FuncType, CoreModuleTypeDefinition> type; ///< The type.

    /// Two definitions are equal when their types are.
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

/// A primitive value type: `primvaltype` of Binary.md.
enum class PrimitiveValueType : uint8_t
{
    Bool,
    S8,
    U8,
    S16,
    U16,
    S32,
    U32,
    S64,
    U64,
    F32,
    F64,
    Char,
    String,
    ErrorContext,
};

/// One row of PrimitiveValueTypeTable: a primitive's byte, its gate and its name.
struct PrimitiveValueTypeRow
{
    uint8_t byte;                         ///< Its byte, as `primvaltype` encodes it.
    PrimitiveValueType type;              ///< The primitive; equal to the row's index.
    std::optional<ComponentFeature> gate; ///< The feature that introduced it, if any.
    std::string_view text;                ///< Its name in the text format.
};

/// Every primitive value type, in PrimitiveValueType's order.
inline constexpr auto PrimitiveValueTypeTable = std::to_array<PrimitiveValueTypeRow>({
    { .byte = 0x7F, .type = PrimitiveValueType::Bool, .gate = std::nullopt, .text = "bool" },
    { .byte = 0x7E, .type = PrimitiveValueType::S8, .gate = std::nullopt, .text = "s8" },
    { .byte = 0x7D, .type = PrimitiveValueType::U8, .gate = std::nullopt, .text = "u8" },
    { .byte = 0x7C, .type = PrimitiveValueType::S16, .gate = std::nullopt, .text = "s16" },
    { .byte = 0x7B, .type = PrimitiveValueType::U16, .gate = std::nullopt, .text = "u16" },
    { .byte = 0x7A, .type = PrimitiveValueType::S32, .gate = std::nullopt, .text = "s32" },
    { .byte = 0x79, .type = PrimitiveValueType::U32, .gate = std::nullopt, .text = "u32" },
    { .byte = 0x78, .type = PrimitiveValueType::S64, .gate = std::nullopt, .text = "s64" },
    { .byte = 0x77, .type = PrimitiveValueType::U64, .gate = std::nullopt, .text = "u64" },
    { .byte = 0x76, .type = PrimitiveValueType::F32, .gate = std::nullopt, .text = "f32" },
    { .byte = 0x75, .type = PrimitiveValueType::F64, .gate = std::nullopt, .text = "f64" },
    { .byte = 0x74, .type = PrimitiveValueType::Char, .gate = std::nullopt, .text = "char" },
    { .byte = 0x73, .type = PrimitiveValueType::String, .gate = std::nullopt, .text = "string" },
    { .byte = 0x64,
      .type = PrimitiveValueType::ErrorContext,
      .gate = ComponentFeature::ErrorContext,
      .text = "error-context" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: PrimitiveValueTypeTable)
        {
            if (static_cast<std::size_t>(row.type) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(PrimitiveValueType::ErrorContext) + 1;
    }(),
    "PrimitiveValueTypeTable must hold one row per PrimitiveValueType, at the index of its enumerator");

/// A type index used as a value type: the defined value type at that index of the type index space.
struct TypeIndex
{
    uint32_t index = 0; ///< The index, checked by validation.

    /// Two references are equal when their indices are.
    bool operator==(TypeIndex const&) const = default;
};

/// A value type: `valtype` of Binary.md, a primitive or a type index. Value types nest only through
/// indices, so a decoded type never holds another inline.
using ValueTypeRef = std::variant<PrimitiveValueType, TypeIndex>;

/// A labelled value type: `labelvaltype`, a record field or a function parameter.
struct LabeledType
{
    std::string label; ///< Its label, as encoded; validation checks it.
    ValueTypeRef type; ///< Its type.

    /// Two labelled types are equal when their labels and types are.
    bool operator==(LabeledType const&) const = default;
};

/// `(record (field l t)+)`.
struct RecordType
{
    std::vector<LabeledType> fields; ///< In encoded order; validation requires at least one.

    /// Two records are equal when their fields are.
    bool operator==(RecordType const&) const = default;
};

/// One case of a variant: `(case l t?)`.
struct VariantCase
{
    std::string label;                ///< Its label, as encoded.
    std::optional<ValueTypeRef> type; ///< Its payload's type, if it has one.

    /// Two cases are equal when their labels and payload types are.
    bool operator==(VariantCase const&) const = default;
};

/// `(variant case+)`.
struct VariantType
{
    std::vector<VariantCase> cases; ///< In encoded order; validation requires at least one.

    /// Two variants are equal when their cases are.
    bool operator==(VariantType const&) const = default;
};

/// `(list t)`.
struct ListType
{
    ValueTypeRef element; ///< The element type.

    /// Two lists are equal when their element types are.
    bool operator==(ListType const&) const = default;
};

/// 🔧 `(list t len)`: a list of a fixed length.
struct FixedListType
{
    ValueTypeRef element; ///< The element type.
    uint32_t length = 0;  ///< The number of elements; validation requires more than zero.

    /// Two lists are equal when their element types and lengths are.
    bool operator==(FixedListType const&) const = default;
};

/// `(tuple t+)`.
struct TupleType
{
    std::vector<ValueTypeRef> elements; ///< In encoded order; validation requires at least one.

    /// Two tuples are equal when their element types are.
    bool operator==(TupleType const&) const = default;
};

/// `(flags l+)`.
struct FlagsType
{
    std::vector<std::string> labels; ///< In encoded order; validation requires one to 32.

    /// Two flags types are equal when their labels are.
    bool operator==(FlagsType const&) const = default;
};

/// `(enum l+)`.
struct EnumType
{
    std::vector<std::string> labels; ///< In encoded order; validation requires at least one.

    /// Two enums are equal when their labels are.
    bool operator==(EnumType const&) const = default;
};

/// `(option t)`.
struct OptionType
{
    ValueTypeRef type; ///< The type of the value it may hold.

    /// Two options are equal when their types are.
    bool operator==(OptionType const&) const = default;
};

/// `(result t? (error u)?)`.
struct ResultType
{
    std::optional<ValueTypeRef> ok;    ///< The success payload's type, if any.
    std::optional<ValueTypeRef> error; ///< The error payload's type, if any.

    /// Two results are equal when their payload types are.
    bool operator==(ResultType const&) const = default;
};

/// `(own i)`: an owned handle to the resource type at index i.
struct OwnType
{
    uint32_t resource = 0; ///< The resource type's index.

    /// Two handle types are equal when their resources are.
    bool operator==(OwnType const&) const = default;
};

/// `(borrow i)`: a borrowed handle to the resource type at index i.
struct BorrowType
{
    uint32_t resource = 0; ///< The resource type's index.

    /// Two handle types are equal when their resources are.
    bool operator==(BorrowType const&) const = default;
};

/// 🔀 `(stream t?)`.
struct StreamType
{
    std::optional<ValueTypeRef> element; ///< The element type, if any.

    /// Two streams are equal when their element types are.
    bool operator==(StreamType const&) const = default;
};

/// 🔀 `(future t?)`.
struct FutureType
{
    std::optional<ValueTypeRef> element; ///< The payload type, if any.

    /// Two futures are equal when their payload types are.
    bool operator==(FutureType const&) const = default;
};

/// 🗺️ `(map k v)`.
struct MapType
{
    ValueTypeRef key;   ///< The key type; validation checks it is a `keytype`.
    ValueTypeRef value; ///< The value type.

    /// Two maps are equal when their key and value types are.
    bool operator==(MapType const&) const = default;
};

/// A defined value type: `defvaltype` of Binary.md. The alternatives after the primitive are in
/// DefinedValueTypeTable's order.
using DefinedValueType = std::variant<PrimitiveValueType,
                                      RecordType,
                                      VariantType,
                                      ListType,
                                      FixedListType,
                                      TupleType,
                                      FlagsType,
                                      EnumType,
                                      OptionType,
                                      ResultType,
                                      OwnType,
                                      BorrowType,
                                      StreamType,
                                      FutureType,
                                      MapType>;

/// One row of DefinedValueTypeTable: a compound defvaltype's byte, its gate and its name.
struct DefinedValueTypeRow
{
    uint8_t byte;                         ///< Its byte in `defvaltype`.
    std::size_t alternative;              ///< Its DefinedValueType alternative: the row's index + 1.
    std::optional<ComponentFeature> gate; ///< The feature that introduced it, if any.
    std::string_view keyword;             ///< Its keyword in the text format.
};

/// Every compound defvaltype of Binary.md, in DefinedValueType's order (primitives are
/// PrimitiveValueTypeTable's).
inline constexpr auto DefinedValueTypeTable = std::to_array<DefinedValueTypeRow>({
    { .byte = 0x72, .alternative = 1, .gate = std::nullopt, .keyword = "record" },
    { .byte = 0x71, .alternative = 2, .gate = std::nullopt, .keyword = "variant" },
    { .byte = 0x70, .alternative = 3, .gate = std::nullopt, .keyword = "list" },
    { .byte = 0x67, .alternative = 4, .gate = ComponentFeature::FixedLengthLists, .keyword = "list" },
    { .byte = 0x6F, .alternative = 5, .gate = std::nullopt, .keyword = "tuple" },
    { .byte = 0x6E, .alternative = 6, .gate = std::nullopt, .keyword = "flags" },
    { .byte = 0x6D, .alternative = 7, .gate = std::nullopt, .keyword = "enum" },
    { .byte = 0x6B, .alternative = 8, .gate = std::nullopt, .keyword = "option" },
    { .byte = 0x6A, .alternative = 9, .gate = std::nullopt, .keyword = "result" },
    { .byte = 0x69, .alternative = 10, .gate = std::nullopt, .keyword = "own" },
    { .byte = 0x68, .alternative = 11, .gate = std::nullopt, .keyword = "borrow" },
    { .byte = 0x66, .alternative = 12, .gate = ComponentFeature::Async, .keyword = "stream" },
    { .byte = 0x65, .alternative = 13, .gate = ComponentFeature::Async, .keyword = "future" },
    { .byte = 0x63, .alternative = 14, .gate = ComponentFeature::Map, .keyword = "map" },
});

static_assert(DefinedValueTypeTable.size() + 1 == std::variant_size_v<DefinedValueType>,
              "DefinedValueTypeTable holds one row per compound DefinedValueType alternative");

static_assert(
    [] {
        auto index = std::size_t { 1 };
        for (auto const& row: DefinedValueTypeTable)
        {
            if (row.alternative != index)
                return false;
            ++index;
        }
        return true;
    }(),
    "DefinedValueTypeTable lists the compound alternatives in DefinedValueType's order");

/// Whether a function may be called synchronously only, or also asynchronously.
enum class Asyncness : uint8_t
{
    Sync,  ///< `(func ...)`.
    Async, ///< 🔀 `(func async ...)`.
};

/// One row of FunctionTypeForms: a `functype` byte, the asyncness it introduces and its gate.
struct FunctionTypeFormRow
{
    uint8_t byte;                         ///< The byte.
    Asyncness asyncness;                  ///< The asyncness it introduces.
    std::optional<ComponentFeature> gate; ///< The feature that introduced it, if any.
};

/// Every form of `functype`.
inline constexpr auto FunctionTypeForms = std::to_array<FunctionTypeFormRow>({
    { .byte = 0x40, .asyncness = Asyncness::Sync, .gate = std::nullopt },
    { .byte = 0x43, .asyncness = Asyncness::Async, .gate = ComponentFeature::Async },
});

/// A function type: `functype` of Binary.md.
struct FunctionTypeDefinition
{
    Asyncness asyncness = Asyncness::Sync; ///< Whether it is `async`.
    std::vector<LabeledType> params;       ///< The parameters, in encoded order.
    std::optional<ValueTypeRef> result;    ///< The result's type; empty for no result.

    /// Two function types are equal when their asyncness, parameters and results are.
    bool operator==(FunctionTypeDefinition const&) const = default;
};

/// A component type: `componenttype` of Binary.md, its declarations a scope of the pool.
struct ComponentTypeDefinition
{
    uint32_t scope = 0; ///< Its declarations' entry in ComponentPool::components.

    /// Two definitions are equal when they name the same scope.
    bool operator==(ComponentTypeDefinition const&) const = default;
};

/// An instance type: `instancetype` of Binary.md, its declarations a scope of the pool.
struct InstanceTypeDefinition
{
    uint32_t scope = 0; ///< Its declarations' entry in ComponentPool::components.

    /// Two definitions are equal when they name the same scope.
    bool operator==(InstanceTypeDefinition const&) const = default;
};

/// A resource type: `resourcetype` of Binary.md.
struct ResourceTypeDefinition
{
    CoreValueKind rep = CoreValueKind::I32; ///< Its representation; validation requires i32.
    std::optional<uint32_t> destructor;     ///< The core function that destroys one, if any.

    /// Two definitions are equal when their representations and destructors are.
    bool operator==(ResourceTypeDefinition const&) const = default;
};

/// A type: `type` of Binary.md, which `deftype` defines.
struct TypeDefinition
{
    /// The type defined.
    std::variant<DefinedValueType,
                 FunctionTypeDefinition,
                 ComponentTypeDefinition,
                 InstanceTypeDefinition,
                 ResourceTypeDefinition>
        type;

    /// Two definitions are equal when their types are.
    bool operator==(TypeDefinition const&) const = default;
};

/// A canonical built-in: one production of `canon` of Binary.md.
enum class CanonBuiltin : uint8_t
{
    Lift,
    Lower,
    ResourceNew,
    ResourceDrop,
    ResourceRep,
    BackpressureInc,
    BackpressureDec,
    TaskReturn,
    TaskCancel,
    ContextGet,
    ContextSet,
    SubtaskCancel,
    SubtaskDrop,
    StreamNew,
    StreamRead,
    StreamWrite,
    StreamCancelRead,
    StreamCancelWrite,
    StreamDropReadable,
    StreamDropWritable,
    StreamForward,
    FutureNew,
    FutureRead,
    FutureWrite,
    FutureCancelRead,
    FutureCancelWrite,
    FutureDropReadable,
    FutureDropWritable,
    FutureForward,
    ErrorContextNew,
    ErrorContextDebugMessage,
    ErrorContextDrop,
    WaitableSetNew,
    WaitableSetWait,
    WaitableSetPoll,
    WaitableSetDrop,
    WaitableJoin,
    ThreadIndex,
    ThreadNewIndirect,
    ThreadResumeLater,
    ThreadSuspend,
    ThreadYield,
    ThreadSuspendThenResume,
    ThreadYieldThenResume,
    ThreadSuspendThenPromote,
    ThreadYieldThenPromote,
    ThreadSpawnRef,
    ThreadSpawnIndirect,
    ThreadAvailableParallelism,
};

/// The last CanonBuiltin enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastCanonBuiltin = CanonBuiltin::ThreadAvailableParallelism;

/// One immediate of a canonical built-in, read after its opcode.
enum class Immediate : uint8_t
{
    None,          ///< No immediate: the end of a shape shorter than ImmediateShape.
    SortByte,      ///< The `func` sort byte, 0x00; not stored.
    Index,         ///< A u32 index; stored in CanonDefinition::immediates.
    Options,       ///< A vector of canonopts; stored in CanonDefinition::options.
    ResultList,    ///< A `resultlist`; stored in CanonDefinition::result.
    CoreValueType, ///< A `core:valtype`; stored as its CoreValueKind.
    Async,         ///< `async?`: 0 or 1; stored.
    Reserved,      ///< A byte that must be 0x00, once a boolean; not stored.
    Shared,        ///< 🧵② `sh?`: 0 or 1 (`shared`); stored. Only 🧵② built-ins take it.
};

/// The immediates of a built-in, in encoded order, padded with Immediate::None.
using ImmediateShape = std::array<Immediate, 4>;

/// One row of CanonBuiltinTable: a built-in's opcode, its immediates, its gate and its name.
struct CanonBuiltinRow
{
    uint8_t opcode;                       ///< Its opcode, the first byte of `canon`.
    CanonBuiltin builtin;                 ///< The built-in.
    ImmediateShape shape;                 ///< What follows the opcode.
    std::optional<ComponentFeature> gate; ///< The feature that introduced it, if any.
    std::string_view text;                ///< Its name, after `canon` in the text format.
};

/// Every canonical built-in of Binary.md, sorted by opcode so the decoder binary-searches it.
inline constexpr auto CanonBuiltinTable = std::to_array<CanonBuiltinRow>({
    { .opcode = 0x00,
      .builtin = CanonBuiltin::Lift,
      .shape = { Immediate::SortByte, Immediate::Index, Immediate::Options, Immediate::Index },
      .gate = std::nullopt,
      .text = "lift" },
    { .opcode = 0x01,
      .builtin = CanonBuiltin::Lower,
      .shape = { Immediate::SortByte, Immediate::Index, Immediate::Options },
      .gate = std::nullopt,
      .text = "lower" },
    { .opcode = 0x02,
      .builtin = CanonBuiltin::ResourceNew,
      .shape = { Immediate::Index },
      .gate = std::nullopt,
      .text = "resource.new" },
    { .opcode = 0x03,
      .builtin = CanonBuiltin::ResourceDrop,
      .shape = { Immediate::Index },
      .gate = std::nullopt,
      .text = "resource.drop" },
    { .opcode = 0x04,
      .builtin = CanonBuiltin::ResourceRep,
      .shape = { Immediate::Index },
      .gate = std::nullopt,
      .text = "resource.rep" },
    { .opcode = 0x05,
      .builtin = CanonBuiltin::TaskCancel,
      .shape = {},
      .gate = ComponentFeature::Async,
      .text = "task.cancel" },
    { .opcode = 0x06,
      .builtin = CanonBuiltin::SubtaskCancel,
      .shape = { Immediate::Async },
      .gate = ComponentFeature::Async,
      .text = "subtask.cancel" },
    { .opcode = 0x09,
      .builtin = CanonBuiltin::TaskReturn,
      .shape = { Immediate::ResultList, Immediate::Options },
      .gate = ComponentFeature::Async,
      .text = "task.return" },
    { .opcode = 0x0A,
      .builtin = CanonBuiltin::ContextGet,
      .shape = { Immediate::CoreValueType, Immediate::Index },
      .gate = ComponentFeature::Async,
      .text = "context.get" },
    { .opcode = 0x0B,
      .builtin = CanonBuiltin::ContextSet,
      .shape = { Immediate::CoreValueType, Immediate::Index },
      .gate = ComponentFeature::Async,
      .text = "context.set" },
    { .opcode = 0x0C,
      .builtin = CanonBuiltin::ThreadYield,
      .shape = { Immediate::Reserved },
      .gate = ComponentFeature::Async,
      .text = "thread.yield" },
    { .opcode = 0x0D,
      .builtin = CanonBuiltin::SubtaskDrop,
      .shape = {},
      .gate = ComponentFeature::Async,
      .text = "subtask.drop" },
    { .opcode = 0x0E,
      .builtin = CanonBuiltin::StreamNew,
      .shape = { Immediate::Index },
      .gate = ComponentFeature::Async,
      .text = "stream.new" },
    { .opcode = 0x0F,
      .builtin = CanonBuiltin::StreamRead,
      .shape = { Immediate::Index, Immediate::Options },
      .gate = ComponentFeature::Async,
      .text = "stream.read" },
    { .opcode = 0x10,
      .builtin = CanonBuiltin::StreamWrite,
      .shape = { Immediate::Index, Immediate::Options },
      .gate = ComponentFeature::Async,
      .text = "stream.write" },
    { .opcode = 0x11,
      .builtin = CanonBuiltin::StreamCancelRead,
      .shape = { Immediate::Index, Immediate::Async },
      .gate = ComponentFeature::Async,
      .text = "stream.cancel-read" },
    { .opcode = 0x12,
      .builtin = CanonBuiltin::StreamCancelWrite,
      .shape = { Immediate::Index, Immediate::Async },
      .gate = ComponentFeature::Async,
      .text = "stream.cancel-write" },
    { .opcode = 0x13,
      .builtin = CanonBuiltin::StreamDropReadable,
      .shape = { Immediate::Index },
      .gate = ComponentFeature::Async,
      .text = "stream.drop-readable" },
    { .opcode = 0x14,
      .builtin = CanonBuiltin::StreamDropWritable,
      .shape = { Immediate::Index },
      .gate = ComponentFeature::Async,
      .text = "stream.drop-writable" },
    { .opcode = 0x15,
      .builtin = CanonBuiltin::FutureNew,
      .shape = { Immediate::Index },
      .gate = ComponentFeature::Async,
      .text = "future.new" },
    { .opcode = 0x16,
      .builtin = CanonBuiltin::FutureRead,
      .shape = { Immediate::Index, Immediate::Options },
      .gate = ComponentFeature::Async,
      .text = "future.read" },
    { .opcode = 0x17,
      .builtin = CanonBuiltin::FutureWrite,
      .shape = { Immediate::Index, Immediate::Options },
      .gate = ComponentFeature::Async,
      .text = "future.write" },
    { .opcode = 0x18,
      .builtin = CanonBuiltin::FutureCancelRead,
      .shape = { Immediate::Index, Immediate::Async },
      .gate = ComponentFeature::Async,
      .text = "future.cancel-read" },
    { .opcode = 0x19,
      .builtin = CanonBuiltin::FutureCancelWrite,
      .shape = { Immediate::Index, Immediate::Async },
      .gate = ComponentFeature::Async,
      .text = "future.cancel-write" },
    { .opcode = 0x1A,
      .builtin = CanonBuiltin::FutureDropReadable,
      .shape = { Immediate::Index },
      .gate = ComponentFeature::Async,
      .text = "future.drop-readable" },
    { .opcode = 0x1B,
      .builtin = CanonBuiltin::FutureDropWritable,
      .shape = { Immediate::Index },
      .gate = ComponentFeature::Async,
      .text = "future.drop-writable" },
    { .opcode = 0x1C,
      .builtin = CanonBuiltin::ErrorContextNew,
      .shape = { Immediate::Options },
      .gate = ComponentFeature::ErrorContext,
      .text = "error-context.new" },
    { .opcode = 0x1D,
      .builtin = CanonBuiltin::ErrorContextDebugMessage,
      .shape = { Immediate::Options },
      .gate = ComponentFeature::ErrorContext,
      .text = "error-context.debug-message" },
    { .opcode = 0x1E,
      .builtin = CanonBuiltin::ErrorContextDrop,
      .shape = {},
      .gate = ComponentFeature::ErrorContext,
      .text = "error-context.drop" },
    { .opcode = 0x1F,
      .builtin = CanonBuiltin::WaitableSetNew,
      .shape = {},
      .gate = ComponentFeature::Async,
      .text = "waitable-set.new" },
    { .opcode = 0x20,
      .builtin = CanonBuiltin::WaitableSetWait,
      .shape = { Immediate::Reserved, Immediate::Index },
      .gate = ComponentFeature::Async,
      .text = "waitable-set.wait" },
    { .opcode = 0x21,
      .builtin = CanonBuiltin::WaitableSetPoll,
      .shape = { Immediate::Reserved, Immediate::Index },
      .gate = ComponentFeature::Async,
      .text = "waitable-set.poll" },
    { .opcode = 0x22,
      .builtin = CanonBuiltin::WaitableSetDrop,
      .shape = {},
      .gate = ComponentFeature::Async,
      .text = "waitable-set.drop" },
    { .opcode = 0x23,
      .builtin = CanonBuiltin::WaitableJoin,
      .shape = {},
      .gate = ComponentFeature::Async,
      .text = "waitable.join" },
    { .opcode = 0x24,
      .builtin = CanonBuiltin::BackpressureInc,
      .shape = {},
      .gate = ComponentFeature::Async,
      .text = "backpressure.inc" },
    { .opcode = 0x25,
      .builtin = CanonBuiltin::BackpressureDec,
      .shape = {},
      .gate = ComponentFeature::Async,
      .text = "backpressure.dec" },
    { .opcode = 0x26,
      .builtin = CanonBuiltin::ThreadIndex,
      .shape = {},
      .gate = ComponentFeature::Threading,
      .text = "thread.index" },
    { .opcode = 0x27,
      .builtin = CanonBuiltin::ThreadNewIndirect,
      .shape = { Immediate::Index, Immediate::Index },
      .gate = ComponentFeature::Threading,
      .text = "thread.new-indirect" },
    { .opcode = 0x28,
      .builtin = CanonBuiltin::ThreadResumeLater,
      .shape = {},
      .gate = ComponentFeature::Threading,
      .text = "thread.resume-later" },
    { .opcode = 0x29,
      .builtin = CanonBuiltin::ThreadSuspend,
      .shape = { Immediate::Reserved },
      .gate = ComponentFeature::Threading,
      .text = "thread.suspend" },
    { .opcode = 0x2A,
      .builtin = CanonBuiltin::ThreadSuspendThenResume,
      .shape = { Immediate::Reserved },
      .gate = ComponentFeature::Threading,
      .text = "thread.suspend-then-resume" },
    { .opcode = 0x2B,
      .builtin = CanonBuiltin::ThreadYieldThenResume,
      .shape = { Immediate::Reserved },
      .gate = ComponentFeature::Threading,
      .text = "thread.yield-then-resume" },
    { .opcode = 0x2C,
      .builtin = CanonBuiltin::ThreadSuspendThenPromote,
      .shape = { Immediate::Reserved },
      .gate = ComponentFeature::Threading,
      .text = "thread.suspend-then-promote" },
    { .opcode = 0x2D,
      .builtin = CanonBuiltin::ThreadYieldThenPromote,
      .shape = { Immediate::Reserved },
      .gate = ComponentFeature::Threading,
      .text = "thread.yield-then-promote" },
    { .opcode = 0x2E,
      .builtin = CanonBuiltin::StreamForward,
      .shape = { Immediate::Index },
      .gate = ComponentFeature::Forward,
      .text = "stream.forward" },
    { .opcode = 0x2F,
      .builtin = CanonBuiltin::FutureForward,
      .shape = { Immediate::Index },
      .gate = ComponentFeature::Forward,
      .text = "future.forward" },
    { .opcode = 0x40,
      .builtin = CanonBuiltin::ThreadSpawnRef,
      .shape = { Immediate::Shared, Immediate::Index },
      .gate = ComponentFeature::SharedThreading,
      .text = "thread.spawn-ref" },
    { .opcode = 0x41,
      .builtin = CanonBuiltin::ThreadSpawnIndirect,
      .shape = { Immediate::Shared, Immediate::Index, Immediate::Index },
      .gate = ComponentFeature::SharedThreading,
      .text = "thread.spawn-indirect" },
    { .opcode = 0x42,
      .builtin = CanonBuiltin::ThreadAvailableParallelism,
      .shape = { Immediate::Shared },
      .gate = ComponentFeature::SharedThreading,
      .text = "thread.available-parallelism" },
});

static_assert(std::ranges::adjacent_find(CanonBuiltinTable,
                                         std::ranges::greater_equal {},
                                         &CanonBuiltinRow::opcode)
                  == CanonBuiltinTable.end(),
              "CanonBuiltinTable must be sorted by opcode, each opcode once");

static_assert(
    [] {
        auto seen = std::array<bool, static_cast<std::size_t>(LastCanonBuiltin) + 1> {};
        for (auto const& row: CanonBuiltinTable)
        {
            if (seen[static_cast<std::size_t>(row.builtin)])
                return false;
            seen[static_cast<std::size_t>(row.builtin)] = true;
        }
        return CanonBuiltinTable.size() == seen.size();
    }(),
    "CanonBuiltinTable must hold one row per CanonBuiltin");

/// A canonical option: one `canonopt` of Binary.md.
enum class CanonOptionKind : uint8_t
{
    Utf8,
    Utf16,
    Latin1Utf16,
    Memory,
    Realloc,
    PostReturn,
    Async,
    Callback,
};

/// Whether a canonical option carries an index.
enum class OptionImmediate : uint8_t
{
    None,  ///< The option alone.
    Index, ///< A u32 index follows: a core memory or core function.
};

/// One row of CanonOptionTable: an option's byte, its immediate, its gate and its name.
struct CanonOptionRow
{
    uint8_t byte;                         ///< Its byte in `canonopt`; equal to the row's index.
    CanonOptionKind kind;                 ///< The option; equal to the row's index.
    OptionImmediate immediate;            ///< What follows the byte.
    std::optional<ComponentFeature> gate; ///< The feature that introduced it, if any.
    std::string_view text;                ///< Its text-format spelling.
};

/// Every canonical option, in CanonOptionKind's order, which is the order of their bytes.
inline constexpr auto CanonOptionTable = std::to_array<CanonOptionRow>({
    { .byte = 0x00,
      .kind = CanonOptionKind::Utf8,
      .immediate = OptionImmediate::None,
      .gate = std::nullopt,
      .text = "string-encoding=utf8" },
    { .byte = 0x01,
      .kind = CanonOptionKind::Utf16,
      .immediate = OptionImmediate::None,
      .gate = std::nullopt,
      .text = "string-encoding=utf16" },
    { .byte = 0x02,
      .kind = CanonOptionKind::Latin1Utf16,
      .immediate = OptionImmediate::None,
      .gate = std::nullopt,
      .text = "string-encoding=latin1+utf16" },
    { .byte = 0x03,
      .kind = CanonOptionKind::Memory,
      .immediate = OptionImmediate::Index,
      .gate = std::nullopt,
      .text = "memory" },
    { .byte = 0x04,
      .kind = CanonOptionKind::Realloc,
      .immediate = OptionImmediate::Index,
      .gate = std::nullopt,
      .text = "realloc" },
    { .byte = 0x05,
      .kind = CanonOptionKind::PostReturn,
      .immediate = OptionImmediate::Index,
      .gate = std::nullopt,
      .text = "post-return" },
    { .byte = 0x06,
      .kind = CanonOptionKind::Async,
      .immediate = OptionImmediate::None,
      .gate = ComponentFeature::Async,
      .text = "async" },
    { .byte = 0x07,
      .kind = CanonOptionKind::Callback,
      .immediate = OptionImmediate::Index,
      .gate = ComponentFeature::Async,
      .text = "callback" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: CanonOptionTable)
        {
            if (row.byte != index || static_cast<std::size_t>(row.kind) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(CanonOptionKind::Callback) + 1;
    }(),
    "CanonOptionTable must hold one row per CanonOptionKind, at the index of its enumerator and byte");

/// One canonical option as encoded.
struct CanonOption
{
    CanonOptionKind kind = CanonOptionKind::Utf8; ///< The option.
    uint32_t index = 0; ///< Its index, for an option with OptionImmediate::Index; 0 otherwise.

    /// Two options are equal when their kinds and indices are.
    bool operator==(CanonOption const&) const = default;
};

/// A canonical definition: `canon` of Binary.md, one built-in and its immediates.
struct CanonDefinition
{
    CanonBuiltin builtin = CanonBuiltin::Lift; ///< The built-in.
    /// The stored immediates of its shape, in order: each Index, the CoreValueKind of a CoreValueType,
    /// and 0 or 1 for Async and Shared.
    std::vector<uint32_t> immediates;
    std::vector<CanonOption> options;   ///< Its options, in encoded order, for a shape with Options.
    std::optional<ValueTypeRef> result; ///< For a shape with ResultList: the result's type, if any.

    /// Two definitions are equal when their built-ins, immediates, options and results are.
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

/// An export declaration of a component or instance type: `exportdecl` of Binary.md.
struct ExportDeclaration
{
    NameAttributes name; ///< The export's name.
    ExternDesc type;     ///< What it exports.

    /// Two declarations are equal when their names and types are.
    bool operator==(ExportDeclaration const&) const = default;
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
                                CustomDefinition,
                                ExportDeclaration>;

/// What one entry of ComponentPool::components is: a component, or the declarations of a component
/// or instance type, each its own scope.
enum class ScopeKind : uint8_t
{
    Component,     ///< A component; its definitions are of every section.
    ComponentType, ///< A component type: core types, types, aliases, imports and export declarations.
    InstanceType,  ///< An instance type: as a component type, without imports.
};

/// What a declaration of a component or instance type is.
enum class DeclarationKind : uint8_t
{
    CoreType, ///< `0x00 core:type`: a CoreTypeDefinition.
    Type,     ///< `0x01 type`: a TypeDefinition.
    Alias,    ///< `0x02 alias`: an AliasDefinition.
    Import,   ///< `0x03 importdecl`: an ImportDefinition.
    Export,   ///< `0x04 exportdecl`: an ExportDeclaration.
};

/// One row of DeclarationTable: a declarator's byte, what it declares and where.
struct DeclarationRow
{
    uint8_t byte;                   ///< Its byte in `componentdecl` or `instancedecl`.
    DeclarationKind kind;           ///< What it declares.
    std::optional<ScopeKind> scope; ///< The one kind of scope it may appear in; any type scope if empty.
};

/// Every declarator of `componentdecl` and `instancedecl`.
inline constexpr auto DeclarationTable = std::to_array<DeclarationRow>({
    { .byte = 0x00, .kind = DeclarationKind::CoreType, .scope = std::nullopt },
    { .byte = 0x01, .kind = DeclarationKind::Type, .scope = std::nullopt },
    { .byte = 0x02, .kind = DeclarationKind::Alias, .scope = std::nullopt },
    { .byte = 0x03, .kind = DeclarationKind::Import, .scope = ScopeKind::ComponentType },
    { .byte = 0x04, .kind = DeclarationKind::Export, .scope = std::nullopt },
});

/// A decoded scope: a component's definitions in section order, a section's vector flattened in
/// place, or a component or instance type's declarations in encoded order.
struct Component
{
    ScopeKind kind = ScopeKind::Component; ///< What the scope is.
    std::vector<Definition> definitions;   ///< Every definition or declaration, custom sections included.
};

/// The number of index spaces of a component, one per Sort.
inline constexpr auto SortCount = static_cast<std::size_t>(Sort::Instance) + 1;

/// The index space a definition adds an index to, as Binary.md's index spaces count definitions.
/// @param definition A definition or declaration.
/// @return Its sort, or nothing for a custom section, which adds none.
[[nodiscard]] inline std::optional<Sort> introducedSort(Definition const& definition)
{
    return std::visit(
        [](auto const& alternative) -> std::optional<Sort> {
            using Alternative = std::remove_cvref_t<decltype(alternative)>;
            if constexpr (std::is_same_v<Alternative, CoreModuleDefinition>)
                return Sort::CoreModule;
            else if constexpr (std::is_same_v<Alternative, CoreInstanceDefinition>)
                return Sort::CoreInstance;
            else if constexpr (std::is_same_v<Alternative, CoreTypeDefinition>)
                return Sort::CoreType;
            else if constexpr (std::is_same_v<Alternative, NestedComponent>)
                return Sort::Component;
            else if constexpr (std::is_same_v<Alternative, InstanceDefinition>)
                return Sort::Instance;
            else if constexpr (std::is_same_v<Alternative, AliasDefinition>)
                return alternative.sort;
            else if constexpr (std::is_same_v<Alternative, TypeDefinition>)
                return Sort::Type;
            else if constexpr (std::is_same_v<Alternative, CanonDefinition>)
                // `canon lift` defines a component function; every other built-in a core function.
                return alternative.builtin == CanonBuiltin::Lift ? Sort::Func : Sort::CoreFunc;
            else if constexpr (std::is_same_v<Alternative, ImportDefinition>)
                return alternative.type.sort;
            else if constexpr (std::is_same_v<Alternative, ExportDefinition>)
                return alternative.item.sort;
            else if constexpr (std::is_same_v<Alternative, ExportDeclaration>)
                return alternative.type.sort;
            else
            {
                static_assert(std::is_same_v<Alternative, CustomDefinition>, "every alternative has a case");
                return std::nullopt;
            }
        },
        definition);
}

/// A decoded component and every component and component or instance type nested in it.
///
/// Nested components and type scopes are entries of one flat pool, never members of their parent, so
/// that nothing walks or destroys them recursively. The decoder copies its input into bytes, so no
/// definition aliases the caller's memory.
struct ComponentPool
{
    /// The encoded component, as given to the decoder; the ranges of definitions index it.
    std::shared_ptr<std::vector<std::byte> const> bytes;
    /// The outermost component at index 0, then every nested component and type scope in the order the
    /// decoder met them, which is pre-order; Component::kind tells them apart.
    std::vector<Component> components;
};

} // namespace isoline::component::binary
