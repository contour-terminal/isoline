// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>

namespace isoline::component::types
{

/// A value type interned in a TypeArena: an index of its value types.
struct TypeId
{
    uint32_t value = 0; ///< The index.

    /// Ids compare by their index.
    auto operator<=>(TypeId const&) const = default;
};

/// The identity of one TypeArena, unique in the process: what Values are tagged with (D9), so that a
/// TypeId is never read in an arena it does not belong to.
struct TypeArenaId
{
    uint64_t value = 0; ///< The identity.

    /// Ids compare by their value.
    auto operator<=>(TypeArenaId const&) const = default;
};

/// A resource type: nominal, so each definition (and each `(sub resource)` import or export) mints a
/// fresh one, never equal to another however alike their representations are.
struct ResourceTypeId
{
    uint32_t value = 0; ///< The index of its ResourceTypeInfo in the arena.

    /// Ids compare by their index.
    auto operator<=>(ResourceTypeId const&) const = default;
};

/// A function type held by a TypeArena.
struct FunctionTypeId
{
    uint32_t value = 0; ///< The index.

    /// Ids compare by their index.
    auto operator<=>(FunctionTypeId const&) const = default;
};

/// A component type held by a TypeArena.
struct ComponentTypeId
{
    uint32_t value = 0; ///< The index.

    /// Ids compare by their index.
    auto operator<=>(ComponentTypeId const&) const = default;
};

/// An instance type held by a TypeArena.
struct InstanceTypeId
{
    uint32_t value = 0; ///< The index.

    /// Ids compare by their index.
    auto operator<=>(InstanceTypeId const&) const = default;
};

/// What a value type is: a primitive, in binary::PrimitiveValueType's order, then every compound
/// `defvaltype` of Binary.md.
enum class ValueKind : uint8_t
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
    List,
    FixedList,
    Record,
    Tuple,
    Variant,
    Enum,
    Option,
    Result,
    Flags,
    Map,
    Own,
    Borrow,
    Stream,
    Future,
};

/// The last ValueKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastValueKind = ValueKind::Future;

/// The number of ValueKind enumerators, as LastValueKind counts them.
inline constexpr auto ValueKindCount = static_cast<std::size_t>(LastValueKind) + 1;

/// The last primitive ValueKind: the kinds up to it are binary::PrimitiveValueType's, in its order.
inline constexpr auto LastPrimitiveKind = ValueKind::ErrorContext;

/// What a specialized value type stands for (Explainer, "Specialized value types"): the canonical ABI
/// lays it out as that type.
enum class Despecialized : uint8_t
{
    Itself,  ///< Not specialized.
    Record,  ///< `tuple`: a record whose fields are labelled "0", "1", ….
    Variant, ///< `enum`, `option` and `result`: a variant of their cases.
};

/// One row of ValueKindTable: a value kind, its name, its byte and what it despecializes to.
struct ValueKindRow
{
    ValueKind kind;              ///< The kind; equal to the row's index.
    std::string_view text;       ///< Its name in the text format.
    uint8_t binaryCode;          ///< Its byte: `primvaltype`'s for a primitive, else `defvaltype`'s.
    Despecialized despecialized; ///< What the canonical ABI lays it out as.
};

/// Every value kind, one row per ValueKind enumerator, in enumerator order.
inline constexpr auto ValueKindTable = std::to_array<ValueKindRow>({
    { .kind = ValueKind::Bool, .text = "bool", .binaryCode = 0x7F, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::S8, .text = "s8", .binaryCode = 0x7E, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::U8, .text = "u8", .binaryCode = 0x7D, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::S16, .text = "s16", .binaryCode = 0x7C, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::U16, .text = "u16", .binaryCode = 0x7B, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::S32, .text = "s32", .binaryCode = 0x7A, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::U32, .text = "u32", .binaryCode = 0x79, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::S64, .text = "s64", .binaryCode = 0x78, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::U64, .text = "u64", .binaryCode = 0x77, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::F32, .text = "f32", .binaryCode = 0x76, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::F64, .text = "f64", .binaryCode = 0x75, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::Char, .text = "char", .binaryCode = 0x74, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::String,
      .text = "string",
      .binaryCode = 0x73,
      .despecialized = Despecialized::Itself },
    { .kind = ValueKind::ErrorContext,
      .text = "error-context",
      .binaryCode = 0x64,
      .despecialized = Despecialized::Itself },
    { .kind = ValueKind::List, .text = "list", .binaryCode = 0x70, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::FixedList,
      .text = "list",
      .binaryCode = 0x67,
      .despecialized = Despecialized::Itself },
    { .kind = ValueKind::Record,
      .text = "record",
      .binaryCode = 0x72,
      .despecialized = Despecialized::Itself },
    { .kind = ValueKind::Tuple, .text = "tuple", .binaryCode = 0x6F, .despecialized = Despecialized::Record },
    { .kind = ValueKind::Variant,
      .text = "variant",
      .binaryCode = 0x71,
      .despecialized = Despecialized::Itself },
    { .kind = ValueKind::Enum, .text = "enum", .binaryCode = 0x6D, .despecialized = Despecialized::Variant },
    { .kind = ValueKind::Option,
      .text = "option",
      .binaryCode = 0x6B,
      .despecialized = Despecialized::Variant },
    { .kind = ValueKind::Result,
      .text = "result",
      .binaryCode = 0x6A,
      .despecialized = Despecialized::Variant },
    { .kind = ValueKind::Flags, .text = "flags", .binaryCode = 0x6E, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::Map, .text = "map", .binaryCode = 0x63, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::Own, .text = "own", .binaryCode = 0x69, .despecialized = Despecialized::Itself },
    { .kind = ValueKind::Borrow,
      .text = "borrow",
      .binaryCode = 0x68,
      .despecialized = Despecialized::Itself },
    { .kind = ValueKind::Stream,
      .text = "stream",
      .binaryCode = 0x66,
      .despecialized = Despecialized::Itself },
    { .kind = ValueKind::Future,
      .text = "future",
      .binaryCode = 0x65,
      .despecialized = Despecialized::Itself },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ValueKindTable)
        {
            if (static_cast<std::size_t>(row.kind) != index)
                return false;
            ++index;
        }
        return index == ValueKindCount;
    }(),
    "ValueKindTable must hold one row per ValueKind, at the index of its enumerator");

static_assert(
    [] {
        for (auto const& primitive: binary::PrimitiveValueTypeTable)
        {
            auto const& row = ValueKindTable[static_cast<std::size_t>(primitive.type)];
            if (row.binaryCode != primitive.byte || row.text != primitive.text)
                return false;
        }
        return binary::PrimitiveValueTypeTable.size() == static_cast<std::size_t>(LastPrimitiveKind) + 1;
    }(),
    "the primitive ValueKinds are binary::PrimitiveValueTypeTable's, in its order, with its bytes");

static_assert(
    [] {
        for (auto const& compound: binary::DefinedValueTypeTable)
        {
            auto const matches = [&](ValueKindRow const& row) {
                return row.binaryCode == compound.byte && row.text == compound.keyword
                       && row.kind > LastPrimitiveKind;
            };
            auto count = 0;
            for (auto const& row: ValueKindTable)
                count += matches(row) ? 1 : 0;
            if (count != 1)
                return false;
        }
        return binary::DefinedValueTypeTable.size() + static_cast<std::size_t>(LastPrimitiveKind) + 1
               == ValueKindCount;
    }(),
    "each compound ValueKind is one row of binary::DefinedValueTypeTable, with its byte and keyword");

/// The table row describing @p kind.
/// @param kind A value kind.
/// @return Its row in ValueKindTable.
[[nodiscard]] constexpr ValueKindRow const& valueKindRow(ValueKind kind) noexcept
{
    return ValueKindTable[static_cast<std::size_t>(kind)];
}

/// Whether @p kind is a primitive.
/// @param kind A value kind.
/// @return true for the kinds of binary::PrimitiveValueType.
[[nodiscard]] constexpr bool isPrimitive(ValueKind kind) noexcept
{
    return kind <= LastPrimitiveKind;
}

/// The value kind of a primitive.
/// @param primitive A primitive value type.
/// @return Its ValueKind, at the same position.
[[nodiscard]] constexpr ValueKind kindOf(binary::PrimitiveValueType primitive) noexcept
{
    return static_cast<ValueKind>(primitive);
}

/// A name or label a type holds: immutable and shared, so copying a type, as a resource substitution
/// does, copies a pointer, never the name's bytes (a name may be ComponentLimits::maxNameBytes long).
class SharedName
{
  public:
    /// The empty name.
    SharedName(): SharedName(std::string {}) {}

    /// A name of @p text, taking it over.
    /// @param text The name. Implicit, so a type is built from names as from strings.
    /// @throws std::bad_alloc When the name cannot be stored.
    SharedName(std::string text): _text { std::make_shared<std::string const>(std::move(text)) } {}

    /// A name of @p text, copied.
    /// @param text The name. Implicit, as above.
    SharedName(char const* text): SharedName(std::string { text }) {}

    /// The name's text.
    /// @return It, valid while any copy of the name lives.
    [[nodiscard]] std::string const& str() const noexcept { return *_text; }

    /// Names compare by their text.
    [[nodiscard]] bool operator==(SharedName const& other) const noexcept { return str() == other.str(); }

    /// Names order by their text.
    [[nodiscard]] std::strong_ordering operator<=>(SharedName const& other) const noexcept
    {
        return str() <=> other.str();
    }

  private:
    std::shared_ptr<std::string const> _text;
};

/// The handles a value type holds somewhere inside it: a bitmask of the two handle kinds, since a
/// type may hold both.
enum class HandleContent : uint8_t
{
    None = 0b00,   ///< No handle.
    Own = 0b01,    ///< An `own` somewhere.
    Borrow = 0b10, ///< A `borrow` somewhere.
};

/// The handles of either operand.
/// @return Their union.
[[nodiscard]] constexpr HandleContent operator|(HandleContent lhs, HandleContent rhs) noexcept
{
    return static_cast<HandleContent>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}

/// Whether @p content holds a handle of the kinds in @p kinds.
/// @param content What a type holds.
/// @param kinds The handle kinds asked about.
/// @return true when the two share a kind.
[[nodiscard]] constexpr bool holds(HandleContent content, HandleContent kinds) noexcept
{
    return (static_cast<uint8_t>(content) & static_cast<uint8_t>(kinds)) != 0;
}

/// The two measures wasm-tools bounds every type by (`TypeInfo`, wasmparser 0.263.0,
/// src/validator/types.rs:192-260): its nesting depth and its effective size, the number of nodes a
/// walk of it visits when nothing is shared.
struct TypeMeasure
{
    uint32_t depth = 1;         ///< 1 for a type that holds no other; 1 + the deepest child's otherwise.
    uint32_t effectiveSize = 1; ///< 1 + the effective sizes of its children, each counted every time.

    /// Two measures are equal when their depths and sizes are.
    bool operator==(TypeMeasure const&) const = default;
};

/// A value type, as a TypeArena holds it: a kind and what the kind takes, every inner type by TypeId.
///
/// What the fields hold, by kind:
/// - children: a record's fields, a variant's case payloads (empty for a case without one), a tuple's
///   elements; one element for list, fixed list, option, stream and future (empty for an element-less
///   stream or future); result's ok then error payloads (each possibly empty); map's key then value.
/// - labels: a record's fields, a variant's cases, a flags' or an enum's labels, as encoded.
/// - resource: an own's or a borrow's resource type.
/// - length: a fixed-length list's length.
/// depth, effectiveSize and handles are computed by TypeArena::intern(), which replaces any value given.
struct DefinedType
{
    ValueKind kind = ValueKind::Bool;               ///< What the type is.
    std::vector<std::optional<TypeId>> children {}; ///< Its inner types, in order (see above).
    std::vector<SharedName> labels {};              ///< Its labels, in order (see above).
    std::optional<ResourceTypeId> resource {};      ///< For Own and Borrow: the resource type.
    uint32_t length = 0;                            ///< For FixedList: the number of elements.
    uint32_t depth = 1;                             ///< Its nesting depth (TypeMeasure::depth).
    uint32_t effectiveSize = 1;                     ///< Its effective size (TypeMeasure::effectiveSize).
    HandleContent handles = HandleContent::None;    ///< The handles it holds, itself included.

    /// Two types are equal when every field is; the computed ones follow from the others.
    bool operator==(DefinedType const&) const = default;
};

/// Any entry of a component's type index space: a value type, a function, component or instance
/// type, or a resource type.
using AnyTypeId = std::variant<TypeId, FunctionTypeId, ComponentTypeId, InstanceTypeId, ResourceTypeId>;

/// An import or export of sort value: its value type.
struct ValueExtern
{
    TypeId type; ///< The value's type.

    /// Two externs are equal when their types are.
    bool operator==(ValueExtern const&) const = default;
};

/// An import or export of sort type: the type it binds, which for `(sub resource)` is a fresh resource.
struct TypeExtern
{
    AnyTypeId type; ///< The type bound.

    /// Two externs are equal when they bind the same type.
    bool operator==(TypeExtern const&) const = default;
};

/// What an import or an export is, interned: types::ExternType of R4, the counterpart of the AST's
/// binary::ExternDesc. Core module types join it with the core type validation of Task 7.
using ExternType = std::variant<FunctionTypeId, ValueExtern, TypeExtern, ComponentTypeId, InstanceTypeId>;

/// A named import or export of a component or instance type.
struct NamedExtern
{
    SharedName name; ///< The extern name, as encoded.
    ExternType type; ///< What it is.

    /// Two externs are equal when their names and types are.
    bool operator==(NamedExtern const&) const = default;
};

/// A function type: its parameters and result, every type interned.
struct FunctionTypeInfo
{
    binary::Asyncness asyncness = binary::Asyncness::Sync; ///< Whether it is `async`.
    std::vector<std::pair<SharedName, TypeId>> params {};  ///< The parameters, labelled, in order.
    std::optional<TypeId> result {};                       ///< The result's type, if any.
    TypeMeasure measure {};                                ///< Computed by TypeArena::add().
    /// The handles its parameters and result hold; computed by TypeArena::add().
    HandleContent handles = HandleContent::None;

    /// Two function types are equal when every field is.
    bool operator==(FunctionTypeInfo const&) const = default;
};

/// Whether a function, component or instance type names a resource type anywhere inside it.
enum class ResourceReference : uint8_t
{
    None, ///< No resource, so no resource substitution changes it.
    Some, ///< At least one, bound inside the type or not.
};

/// Where a component or instance type binds one of its resources: the resource, and the path of
/// extern indices that leads to the `(sub resource)` declaring it (wasm-tools' resource paths,
/// ComponentType::imported_resources, wasmparser 0.263.0 src/validator/component_types.rs:1015-1040).
///
/// The first index is into the type's imports or exports; each further one is into the exports of the
/// instance type the previous one names.
struct ResourcePath
{
    ResourceTypeId resource;       ///< The resource bound.
    std::vector<uint32_t> path {}; ///< The extern indices leading to its declaration.

    /// Two paths are equal when their resources and indices are.
    bool operator==(ResourcePath const&) const = default;
};

/// A component type: its imports and exports, in declaration order, and the resources it binds.
///
/// Its imported resources are universally quantified: an instantiation supplies them. Its defined
/// resources are existential: each instantiation mints them afresh (validate::freshResourcesFor()).
struct ComponentTypeInfo
{
    std::vector<NamedExtern> imports {};            ///< The imports, in order.
    std::vector<NamedExtern> exports {};            ///< The exports, in order.
    std::vector<ResourcePath> importedResources {}; ///< Its imported resources; paths into imports.
    std::vector<ResourcePath> definedResources {};  ///< Its defined resources; paths into exports.
    TypeMeasure measure {};                         ///< Computed by TypeArena::add().
    /// Whether an import or export names a resource; computed by TypeArena::add().
    ResourceReference resources = ResourceReference::None;

    /// Two component types are equal when every field is.
    bool operator==(ComponentTypeInfo const&) const = default;
};

/// An instance type: its exports, in declaration order, and the resources it defines.
///
/// Only an instance type as declared defines resources, through `(export "r" (type (sub resource)))`;
/// importing or exporting an instance of it mints them afresh, so the instance's own type defines none
/// (wasm-tools' ComponentInstanceType::defined_resources).
struct InstanceTypeInfo
{
    std::vector<NamedExtern> exports {};           ///< The exports, in order.
    std::vector<ResourcePath> definedResources {}; ///< The resources it defines; paths into exports.
    TypeMeasure measure {};                        ///< Computed by TypeArena::add().
    /// Whether an export names a resource; computed by TypeArena::add().
    ResourceReference resources = ResourceReference::None;

    /// Two instance types are equal when every field is.
    bool operator==(InstanceTypeInfo const&) const = default;
};

/// Which resource type stands for which: each mapped resource is replaced by its image, every other is
/// kept. Ordered by resource, so a lookup is O(log n) whatever the resources are.
class ResourceSubstitution
{
  public:
    /// Maps @p from to @p to, replacing an earlier image of @p from.
    /// @param from The resource replaced.
    /// @param to Its image.
    void bind(ResourceTypeId from, ResourceTypeId to) { _images.insert_or_assign(from, to); }

    /// The image of a resource.
    /// @param from A resource.
    /// @return Its image, or nothing when it is not mapped.
    [[nodiscard]] std::optional<ResourceTypeId> find(ResourceTypeId from) const
    {
        auto const entry = _images.find(from);
        if (entry == _images.end())
            return std::nullopt;
        return entry->second;
    }

    /// The image of a resource, itself when it is not mapped.
    /// @param from A resource.
    /// @return Its image.
    [[nodiscard]] ResourceTypeId apply(ResourceTypeId from) const { return find(from).value_or(from); }

    /// Every mapping, ordered by the resource replaced.
    /// @return The resources and their images.
    [[nodiscard]] std::map<ResourceTypeId, ResourceTypeId> const& images() const noexcept { return _images; }

    /// The number of resources mapped.
    /// @return The count.
    [[nodiscard]] std::size_t size() const noexcept { return _images.size(); }

  private:
    std::map<ResourceTypeId, ResourceTypeId> _images;
};

/// A resource type's definition: its representation and destructor, as its definition gives them; an
/// imported or exported `(sub resource)` is abstract, an i32 without a destructor.
struct ResourceTypeInfo
{
    binary::CoreValueKind rep = binary::CoreValueKind::I32; ///< Its representation.
    std::optional<uint32_t> destructor {};                  ///< The core function that destroys one.

    /// Two definitions are equal when their fields are; their resource types never are.
    bool operator==(ResourceTypeInfo const&) const = default;
};

/// A type past one of the arena's caps: which cap, and its value for the refusal's text.
struct TypeLimitError
{
    /// TypeDepth, TypeSize, Types, ResourceTypes or SubstitutedTypes.
    ComponentLimit limit = ComponentLimit::TypeSize;
    uint64_t value = 0; ///< The cap's value.

    /// Two errors are equal when their limits and values are.
    bool operator==(TypeLimitError const&) const = default;
};

/// The types of one component and everything nested in it: value types hash-consed, so that
/// structurally equal value types share one TypeId, found through an index ordered by structure (no
/// hash: a lookup takes O(log n) structural comparisons whatever the types, so no crafted input makes
/// interning quadratic); function, component and instance types held in
/// definition order; resource types nominal, one per newResource().
///
/// Every type is measured as it is added, bottom-up from its children's measures (wasm-tools'
/// TypeInfo::combine, wasmparser 0.263.0 src/validator/types.rs:238-247): a type whose depth exceeds
/// ComponentLimits::maxTypeDepth, or whose effective size reaches ComponentLimits::maxTypeSize, is
/// refused, so no type the arena holds is deeper or larger, and nothing ever walks it recursively. The
/// arena holds one entry per type added: at most one per definition its validation interns, plus at
/// most maxSubstitutedTypes that its resource substitutions create, so its memory is linear in the
/// component's.
///
/// An arena is movable and not copyable: its TypeArenaId names exactly one set of types.
class TypeArena
{
  public:
    /// An arena holding the primitives only, each at the TypeId of its ValueKind.
    /// @param limits The caps it enforces: maxTypeDepth, maxTypeSize, maxResourceTypes and
    ///               maxSubstitutedTypes.
    /// @throws std::bad_alloc When the primitives cannot be stored.
    explicit TypeArena(ComponentLimits const& limits);

    TypeArena(TypeArena const&) = delete;
    TypeArena& operator=(TypeArena const&) = delete;
    /// Moves the types; @p other is left empty and must not be used but to be destroyed or assigned.
    TypeArena(TypeArena&& other) noexcept;
    /// Moves the types; @p other is left empty and must not be used but to be destroyed or assigned.
    TypeArena& operator=(TypeArena&& other) noexcept;
    ~TypeArena();

    /// The arena's identity, unique in the process.
    /// @return The id it was created with.
    [[nodiscard]] TypeArenaId id() const noexcept { return _id; }

    /// Interns a value type: its measure and handles are computed from its children (which the arena
    /// must hold, checked), and a structurally equal type already held is returned instead of a new one.
    /// @param type The type; its depth, effectiveSize and handles are replaced.
    /// @return Its id, or the cap it exceeds: TypeDepth past maxTypeDepth, TypeSize at maxTypeSize.
    [[nodiscard]] std::expected<TypeId, TypeLimitError> intern(DefinedType type);

    /// A value type of the arena.
    /// @param id A TypeId the arena returned (checked).
    /// @return Its type.
    [[nodiscard]] DefinedType const& operator[](TypeId id) const;

    /// The id of a primitive, which every arena holds.
    /// @param kind A primitive kind (checked).
    /// @return Its TypeId.
    [[nodiscard]] TypeId primitive(ValueKind kind) const noexcept;

    /// The despecialized form of a value type (Explainer, "Specialized value types"): a tuple's record
    /// of fields "0", "1", …, an enum's variant of payload-less cases, an option's variant of `none`
    /// and `some`, a result's variant of `ok` and `error`; any other type itself.
    /// @param id A value type of the arena (checked).
    /// @return The despecialized type's id, interned, or the cap that interning it exceeds.
    [[nodiscard]] std::expected<TypeId, TypeLimitError> despecialize(TypeId id);

    /// Adds a function type, measured from its parameters and result.
    /// @param type The type; its measure is replaced.
    /// @return Its id, or the cap it exceeds.
    [[nodiscard]] std::expected<FunctionTypeId, TypeLimitError> add(FunctionTypeInfo type);

    /// Adds a component type, measured from its imports and exports.
    /// @param type The type; its measure is replaced.
    /// @return Its id, or the cap it exceeds.
    [[nodiscard]] std::expected<ComponentTypeId, TypeLimitError> add(ComponentTypeInfo type);

    /// Adds an instance type, measured from its exports.
    /// @param type The type; its measure is replaced.
    /// @return Its id, or the cap it exceeds.
    [[nodiscard]] std::expected<InstanceTypeId, TypeLimitError> add(InstanceTypeInfo type);

    /// Rewrites an extern type with @p substitution applied to every resource it names: each value,
    /// function, component and instance type naming a mapped resource is replaced by a copy naming its
    /// image (wasm-tools' remap_component_entity, wasmparser 0.263.0 src/validator/component_types.rs).
    /// Types naming none are kept, so the work is the number of types that name a resource, each
    /// rewritten once however often it is shared; it walks an explicit stack, never recursing.
    /// @param type An extern type whose types the arena holds.
    /// @param substitution The resources to replace.
    /// @return The rewritten type, or SubstitutedTypes once the arena's substitutions in all would
    ///         exceed maxSubstitutedTypes units (a rewritten type costs one, plus one per type it
    ///         names), or the cap a rewritten type exceeds. Names are shared with the originals.
    [[nodiscard]] std::expected<ExternType, TypeLimitError> substitute(
        ExternType const& type, ResourceSubstitution const& substitution);

    /// An instance type of @p exports with @p substitution applied, defining no resources: the type an
    /// import or export of an instance type that defines resources gets once they are minted afresh,
    /// and the type of an instantiation. Its names are shared with @p exports, never copied.
    /// @param exports The exports, whose types the arena holds.
    /// @param substitution The resources to replace.
    /// @return The instance type, or the cap it exceeds; it and its exports are charged to
    ///         maxSubstitutedTypes like the types substitute() creates.
    [[nodiscard]] std::expected<InstanceTypeId, TypeLimitError> substitutedInstance(
        std::vector<NamedExtern> exports, ResourceSubstitution const& substitution);

    /// Mints a fresh resource type, distinct from every other.
    /// @param definition Its representation and destructor.
    /// @return Its id, or ResourceTypes once maxResourceTypes are held.
    [[nodiscard]] std::expected<ResourceTypeId, TypeLimitError> newResource(ResourceTypeInfo definition);

    /// A function type of the arena. @param id An id the arena returned (checked). @return Its type.
    [[nodiscard]] FunctionTypeInfo const& operator[](FunctionTypeId id) const;
    /// A component type of the arena. @param id An id the arena returned (checked). @return Its type.
    [[nodiscard]] ComponentTypeInfo const& operator[](ComponentTypeId id) const;
    /// An instance type of the arena. @param id An id the arena returned (checked). @return Its type.
    [[nodiscard]] InstanceTypeInfo const& operator[](InstanceTypeId id) const;
    /// A resource type of the arena. @param id An id the arena returned (checked). @return Its definition.
    [[nodiscard]] ResourceTypeInfo const& operator[](ResourceTypeId id) const;

    /// The measure of any type of the arena: a resource's is that of a type holding no other.
    /// @param type A type the arena holds (checked).
    /// @return Its measure.
    [[nodiscard]] TypeMeasure measure(AnyTypeId type) const;

    /// The measure an import or export contributes to the component or instance type declaring it
    /// (ComponentEntityType::info, wasmparser 0.263.0 src/validator/component_types.rs:985-994).
    /// @param type An extern type whose types the arena holds (checked).
    /// @return The measure of the type it names.
    [[nodiscard]] TypeMeasure measure(ExternType const& type) const;

    /// Combines a child's measure into an aggregate's, as wasm-tools combines each child of a type in
    /// turn (TypeInfo::combine): the depth becomes the deeper of the two, the child one level down; the
    /// sizes add.
    /// @param aggregate The measure so far.
    /// @param child The measure of the next child.
    /// @return The combined measure, or TypeDepth past maxTypeDepth, or TypeSize at maxTypeSize.
    [[nodiscard]] std::expected<TypeMeasure, TypeLimitError> combine(TypeMeasure aggregate,
                                                                     TypeMeasure child) const noexcept;

    /// The number of value types the arena holds, the primitives included.
    /// @return The count.
    [[nodiscard]] std::size_t valueTypeCount() const noexcept { return _store->types.size(); }

    /// The structural comparisons interning has made so far: the measure of its work, which tests
    /// bound (a lookup makes O(log n) of them).
    /// @return The count.
    [[nodiscard]] std::size_t structuralComparisons() const noexcept { return _store->comparisons; }

    /// Whether a type names a resource anywhere inside it, from the flags computed when it was added:
    /// never a walk.
    /// @param type A type the arena holds (checked).
    /// @return true for a resource, and for a type that holds a handle or names a resource.
    [[nodiscard]] bool namesResource(AnyTypeId type) const;

    /// The units the arena's resource substitutions have charged so far: each type they created, plus
    /// the types it names.
    /// @return The count, at most maxSubstitutedTypes.
    [[nodiscard]] std::size_t substitutedTypes() const noexcept { return _substitutedTypes; }

  private:
    [[nodiscard]] std::expected<void, TypeLimitError> chargeSubstitution(std::size_t units);
    [[nodiscard]] std::expected<TypeMeasure, TypeLimitError> measureChildren(
        std::vector<std::optional<TypeId>> const& children) const;

    TypeArenaId _id;
    uint32_t _maxTypeDepth;
    uint32_t _maxTypeSize;
    uint32_t _maxResourceTypes;
    uint32_t _maxSubstitutedTypes;
    std::size_t _substitutedTypes = 0;
    /// The value types, on the heap so that the index's order, which reads them, survives a move.
    struct ValueStore
    {
        std::vector<DefinedType> types; ///< By TypeId.
        std::size_t comparisons = 0;    ///< See structuralComparisons().
    };

    /// Orders value type indices by their types' structure.
    struct StructuralOrder
    {
        ValueStore* store; ///< The types the indices name.

        [[nodiscard]] bool operator()(uint32_t lhs, uint32_t rhs) const;
    };

    std::unique_ptr<ValueStore> _store;
    std::set<uint32_t, StructuralOrder> _index; ///< Every value type's index, ordered by structure.
    std::vector<FunctionTypeInfo> _functions;
    std::vector<ComponentTypeInfo> _components;
    std::vector<InstanceTypeInfo> _instances;
    std::vector<ResourceTypeInfo> _resources;
};

} // namespace isoline::component::types
