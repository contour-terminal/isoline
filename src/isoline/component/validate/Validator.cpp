// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/validate/Validator.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/CoreTypes.hpp>
#include <isoline/component/validate/Names.hpp>
#include <isoline/component/validate/Subtyping.hpp>

namespace isoline::component::validate
{

namespace
{
    using binary::DefinitionPosition;
    using binary::Sort;
    using types::AnyTypeId;
    using types::CoreEntityType;
    using types::CoreFunctionTypeId;
    using types::CoreInstanceTypeId;
    using types::CoreModuleTypeId;
    using types::ExternType;
    using types::TypeId;
    using types::TypeIdentity;
    using types::TypeMeasure;
    using types::ValueKind;

    template <typename T>
    using Result = std::expected<T, ValidationError>;

    /// The most labels a flags type may have (Binary.md; wasmparser's create_flags_type).
    constexpr auto MaxFlags = std::size_t { 32 };

    /// The most elements a fixed-length list may have: MAX_WASM_FIXED_LENGTH_LIST_ELEMENTS, wasmparser
    /// 0.263.0 src/limits.rs:78.
    constexpr auto MaxFixedListElements = uint32_t { 1'073'741'824 };

    /// The most core parameters a lowered or lifted function passes flat, and results it returns flat
    /// (MAX_FLAT_FUNC_PARAMS and MAX_FLAT_FUNC_RESULTS, wasmparser 0.263.0
    /// src/validator/component_types.rs:31-39; CanonicalABI.md's MAX_FLAT_PARAMS and MAX_FLAT_RESULTS).
    constexpr auto MaxFlatParams = std::size_t { 16 };
    constexpr auto MaxFlatResults = std::size_t { 1 };

    /// The type of every address of a 32-bit memory, the only one P11 admits (🐘 is refused).
    constexpr auto PointerType = isoline::binary::I32;

    // A stream's or future's payload rules (no `borrow`, no `stream<char>`) and a fixed-length list's
    // checks past its length are unreachable while their gates are not implemented; P12 and the
    // fixed-length lists row add what they need when they flip them.
    static_assert(componentFeatureRow(ComponentFeature::Async).p11 != Implementation::Implemented,
                  "implementing async needs the stream and future payload rules here");
    static_assert(componentFeatureRow(ComponentFeature::FixedLengthLists).p11 != Implementation::Implemented,
                  "implementing fixed-length lists needs their validation here reviewed and tested");
    // 🐘 is refused, so a canonical memory is a 32-bit one and every pointer an i32.
    static_assert(componentFeatureRow(ComponentFeature::Memory64).p11 != Implementation::Implemented,
                  "implementing cm64 needs 64-bit pointers in the canonical options here");

    /// The labels of one type, or the names of one scope's imports or exports, so far, for the
    /// uniqueness rule: each by its canonical form, mapped to its first spelling, which a conflict
    /// names. Ordered, not hashed, so no crafted set of labels makes the check quadratic.
    class LabelSet
    {
      public:
        /// Adds a label, or names the earlier one it conflicts with.
        /// @param label The label, as encoded.
        /// @return Nothing when it is new, else the earlier label's spelling.
        [[nodiscard]] std::optional<std::string> insert(std::string const& label)
        {
            return insertKeyed(canonicalLabel(label), label);
        }

        /// Adds a name under the key uniqueness compares it by, or names the earlier one of that key.
        /// @param key The name's key (strongUniquenessKey(), or the name itself for exact uniqueness).
        /// @param name The name, as encoded.
        /// @return Nothing when it is new, else the earlier name's spelling.
        [[nodiscard]] std::optional<std::string> insertKeyed(std::string key, std::string const& name)
        {
            auto const [entry, inserted] = _labels.try_emplace(std::move(key), name);
            if (inserted)
                return std::nullopt;
            return entry->second;
        }

      private:
        std::map<std::string, std::string, std::less<>> _labels;
    };

    /// The resources a scope's top-level imports or exports name (wasm-tools' ComponentNameContext):
    /// which resource entry each name binds, for the rules of `[constructor]`, `[method]` and
    /// `[static]` names. Ordered by the arena's identities and by name, never hashed.
    class ResourceNames
    {
      public:
        /// Records that @p resource is named @p name.
        void add(std::string const& name, TypeIdentity resource)
        {
            _byResource.insert_or_assign(resource, name);
            _names.insert(name);
        }

        /// The name a resource entry is bound to.
        /// @return It, or nothing when the entry has none here.
        [[nodiscard]] std::string const* nameOf(TypeIdentity resource) const
        {
            auto const entry = _byResource.find(resource);
            return entry == _byResource.end() ? nullptr : &entry->second;
        }

        /// Whether a resource of @p name is known here.
        [[nodiscard]] bool knows(std::string_view name) const { return _names.contains(name); }

      private:
        std::map<TypeIdentity, std::string> _byResource;
        std::set<std::string, std::less<>> _names;
    };

    /// One scope being validated: its pool entry, where in it the walk is, and its index spaces.
    struct Frame
    {
        uint32_t scope = 0;                                    ///< Its index in ComponentPool::components.
        binary::ScopeKind kind = binary::ScopeKind::Component; ///< What it is.
        DefinitionPosition origin {};                          ///< The parent's definition that opened it.
        std::size_t next = 0;                                  ///< Its next definition to validate.
        ScopeInfo spaces {};                                   ///< Its index spaces.
        std::vector<types::NamedExtern> imports {};            ///< Its imports, typed.
        std::vector<types::NamedExtern> exports {};            ///< Its exports, typed.
        TypeMeasure measure {};                                ///< Its own measure, as a type.
        /// The effective sizes of the types aliased into it from outside it (a component's only).
        uint64_t outerAliasTypeSize = 0;
        LabelSet importNames {};      ///< Its import names, for strong uniqueness.
        LabelSet exportNames {};      ///< Its export names, for strong uniqueness.
        LabelSet exactImportNames {}; ///< Its import names, for exact uniqueness.
        LabelSet exactExportNames {}; ///< Its export names, for exact uniqueness.
        /// Where its imports bind resources: `(sub resource)` imports and the fresh resources of
        /// imported instances.
        std::vector<types::ResourcePath> importedResources {};
        /// Where its export declarations bind resources, likewise (type scopes only).
        std::vector<types::ResourcePath> definedResources {};
        /// The resources defined in it (wasm-tools' defined_resources), in order, each with its
        /// representation where the component knows it: defined here, minted for an instantiation or
        /// an exported instance, or declared by a type scope's `(sub resource)` export.
        std::vector<std::pair<types::ResourceTypeId, std::optional<binary::CoreValueKind>>> localResources {};
        /// Each local resource's position in localResources, by its id.
        std::map<types::ResourceTypeId, std::size_t> localResourceIndex {};
        /// The types its imports make nameable (imported_types): imported types are exportable too.
        std::set<TypeIdentity> importedTypes {};
        /// The types its exports and imports make nameable by an export (exported_types).
        std::set<TypeIdentity> exportedTypes {};
        ResourceNames importedResourceNames {}; ///< toplevel_imported_resources.
        ResourceNames exportedResourceNames {}; ///< toplevel_exported_resources.
        /// The instance types whose exports an import or export registered already (per ExternKind):
        /// registering one again adds nothing and fails nothing, as the sets it reads only grow.
        std::array<std::set<types::InstanceTypeId>, 2> registeredInstances {};
        /// The instance types whose exports were found nameable (per ExternKind), likewise.
        std::array<std::set<types::InstanceTypeId>, 2> nameableInstances {};
    };

    // A version suffix is refused while 🔗 is, so the rules past its gate (only instances, not empty,
    // an interface name or implements, the canonical version) are not here yet.
    static_assert(componentFeatureRow(ComponentFeature::CanonicalInterfaceNames).p11
                      != Implementation::Implemented,
                  "implementing canonical interface names needs the version suffix rules here");

    /// The attributes in the order wasm-tools checks them (validate_extern, component.rs:4721-4790),
    /// whatever their encoded order: by their Attribute alternative.
    constexpr auto AttributeCheckOrder = std::to_array<std::size_t>({ 1, 0, 2 });

    /// The Attribute alternative of `implements`, whose rules follow its gate.
    constexpr auto ImplementsAlternative = std::size_t { 0 };

    static_assert(std::is_same_v<std::variant_alternative_t<1, binary::Attribute>, binary::VersionSuffix>
                      && std::is_same_v<std::variant_alternative_t<0, binary::Attribute>, binary::Implements>
                      && std::is_same_v<std::variant_alternative_t<2, binary::Attribute>, binary::ExternalId>,
                  "AttributeCheckOrder checks the version suffix, then implements, then the external id");

    /// The kinds a map key may be: Binary.md's `keytype`, the primitives but floats and error-context.
    [[nodiscard]] constexpr bool isKeyKind(ValueKind kind) noexcept
    {
        return types::isPrimitive(kind) && kind != ValueKind::F32 && kind != ValueKind::F64
               && kind != ValueKind::ErrorContext;
    }

    // The one gated primitive is error-context, refused as GatedForm::ErrorContextType.
    static_assert(
        [] {
            for (auto const& row: binary::PrimitiveValueTypeTable)
            {
                auto const isErrorContext = row.type == binary::PrimitiveValueType::ErrorContext;
                if (row.gate.has_value() != isErrorContext
                    || (row.gate && *row.gate != gatedFormRow(GatedForm::ErrorContextType).feature))
                    return false;
            }
            return true;
        }(),
        "error-context is the only gated primitive, under GatedForm::ErrorContextType's feature");

    // The one gated function type form is `async`, refused as GatedForm::AsyncFunction.
    static_assert(
        [] {
            for (auto const& row: binary::FunctionTypeForms)
            {
                auto const isAsync = row.asyncness == binary::Asyncness::Async;
                if (row.gate.has_value() != isAsync
                    || (row.gate && *row.gate != gatedFormRow(GatedForm::AsyncFunction).feature))
                    return false;
            }
            return true;
        }(),
        "`async` is the only gated function type form, under GatedForm::AsyncFunction's feature");

    /// Which index space, or pair of spaces, a sort counts against, and the cap (wasm-tools' check_max
    /// of each section and alias, component.rs and validator.rs).
    struct SortLimitRow
    {
        Sort sort;                           ///< The sort; equal to the row's index.
        std::optional<ComponentLimit> limit; ///< Its cap; none for a value, which 🪙 refuses first.
        std::optional<Sort> sharedWith;      ///< The sort whose space counts against the same cap.
    };

    /// Per Sort, in its order.
    constexpr auto SortLimitTable = std::to_array<SortLimitRow>({
        { .sort = Sort::CoreFunc, .limit = ComponentLimit::Functions, .sharedWith = Sort::Func },
        { .sort = Sort::CoreTable, .limit = ComponentLimit::CoreTables, .sharedWith = std::nullopt },
        { .sort = Sort::CoreMemory, .limit = ComponentLimit::CoreMemories, .sharedWith = std::nullopt },
        { .sort = Sort::CoreGlobal, .limit = ComponentLimit::CoreGlobals, .sharedWith = std::nullopt },
        { .sort = Sort::CoreTag, .limit = ComponentLimit::CoreTags, .sharedWith = std::nullopt },
        { .sort = Sort::CoreType, .limit = ComponentLimit::Types, .sharedWith = Sort::Type },
        { .sort = Sort::CoreModule, .limit = ComponentLimit::Modules, .sharedWith = std::nullopt },
        { .sort = Sort::CoreInstance, .limit = ComponentLimit::Instances, .sharedWith = Sort::Instance },
        { .sort = Sort::Func, .limit = ComponentLimit::Functions, .sharedWith = Sort::CoreFunc },
        { .sort = Sort::Value, .limit = std::nullopt, .sharedWith = std::nullopt },
        { .sort = Sort::Type, .limit = ComponentLimit::Types, .sharedWith = Sort::CoreType },
        { .sort = Sort::Component, .limit = ComponentLimit::Components, .sharedWith = std::nullopt },
        { .sort = Sort::Instance, .limit = ComponentLimit::Instances, .sharedWith = Sort::CoreInstance },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: SortLimitTable)
            {
                if (static_cast<std::size_t>(row.sort) != index)
                    return false;
                if (row.sharedWith
                    && SortLimitTable[static_cast<std::size_t>(*row.sharedWith)].limit != row.limit)
                    return false;
                ++index;
            }
            return index == static_cast<std::size_t>(Sort::Instance) + 1;
        }(),
        "SortLimitTable holds one row per Sort, a shared space under the same cap");

    /// The number of entries of @p sort's index space in @p spaces.
    [[nodiscard]] std::size_t spaceSize(ScopeInfo const& spaces, Sort sort) noexcept
    {
        switch (sort)
        {
            case Sort::CoreFunc: return spaces.coreFunctions.size();
            case Sort::CoreTable: return spaces.coreTables.size();
            case Sort::CoreMemory: return spaces.coreMemories.size();
            case Sort::CoreGlobal: return spaces.coreGlobals.size();
            case Sort::CoreTag: return spaces.coreTags.size();
            case Sort::CoreType: return spaces.coreTypes.size();
            case Sort::CoreModule: return spaces.coreModules.size();
            case Sort::CoreInstance: return spaces.coreInstances.size();
            case Sort::Func: return spaces.functions.size();
            case Sort::Value: return 0;
            case Sort::Type: return spaces.types.size();
            case Sort::Component: return spaces.components.size();
            case Sort::Instance: return spaces.instances.size();
        }
        return 0; // uncovered(exhaustive-switch)
    }

    /// What each component sort is called in messages, by Sort from CoreModule on (ComponentExternalKind::
    /// desc, wasmparser 0.263.0 src/readers/component/exports.rs): the sorts an export alias names.
    [[nodiscard]] constexpr std::string_view externalSortWord(Sort sort) noexcept
    {
        switch (sort)
        {
            case Sort::CoreModule: return "module";
            case Sort::Func: return "func";
            case Sort::Value: return "value";
            case Sort::Type: return "type";
            case Sort::Component: return "component";
            case Sort::Instance: return "instance";
            default: return "";
        }
    }

    /// What each core sort is called by a core export alias' refusal (alias_core_instance_export,
    /// component.rs:3723-3810), by isoline::binary::ExternKind.
    constexpr auto CoreAliasWords =
        std::to_array<std::string_view>({ "function", "table", "memory", "global", "tag" });

    /// How an extern type is named in a refusal (ComponentEntityType::desc), by ExternType alternative.
    constexpr auto EntityWords =
        std::to_array<std::string_view>({ "func", "value", "type", "component", "instance", "module" });

    static_assert(EntityWords.size() == std::variant_size_v<ExternType>, "one word per kind of extern type");

    /// The core extern kind a core sort names (func, table, memory, global, tag).
    [[nodiscard]] constexpr isoline::binary::ExternKind coreKindOf(Sort sort) noexcept
    {
        static_assert(static_cast<uint8_t>(Sort::CoreTag)
                          == static_cast<uint8_t>(isoline::binary::ExternKind::Tag),
                      "the first core sorts are the core extern kinds, in their order");
        return static_cast<isoline::binary::ExternKind>(sort);
    }

    /// How a value type flattens into core types (CanonicalABI.md's flatten_type; wasm-tools'
    /// push_wasm_types, component_types.rs:308-331 and 1923-2016), by ValueKind.
    enum class FlatRule : uint8_t
    {
        I32,         ///< One i32.
        I64,         ///< One i64.
        F32,         ///< One f32.
        F64,         ///< One f64.
        Pointers,    ///< A pointer and a length: string, list, map.
        Concatenate, ///< Each child's flattening in turn: record, tuple.
        Repeat,      ///< The element's flattening, length times: a fixed-length list.
        Variant,     ///< An i32 discriminant, then the cases' flattenings joined: variant, option, result.
        Flags,       ///< One i32 per 32 labels.
    };

    /// Per ValueKind, in its order.
    constexpr auto FlatRuleTable = std::to_array<FlatRule>({
        FlatRule::I32,         // bool
        FlatRule::I32,         // s8
        FlatRule::I32,         // u8
        FlatRule::I32,         // s16
        FlatRule::I32,         // u16
        FlatRule::I32,         // s32
        FlatRule::I32,         // u32
        FlatRule::I64,         // s64
        FlatRule::I64,         // u64
        FlatRule::F32,         // f32
        FlatRule::F64,         // f64
        FlatRule::I32,         // char
        FlatRule::Pointers,    // string
        FlatRule::I32,         // error-context
        FlatRule::Pointers,    // list
        FlatRule::Repeat,      // list N
        FlatRule::Concatenate, // record
        FlatRule::Concatenate, // tuple
        FlatRule::Variant,     // variant
        FlatRule::I32,         // enum
        FlatRule::Variant,     // option
        FlatRule::Variant,     // result
        FlatRule::Flags,       // flags
        FlatRule::Pointers,    // map
        FlatRule::I32,         // own
        FlatRule::I32,         // borrow
        FlatRule::I32,         // stream
        FlatRule::I32,         // future
    });

    static_assert(FlatRuleTable.size() == types::ValueKindCount, "one flattening rule per ValueKind");

    /// A value type's flattening, at most MaxFlatParams core types long, and whether it holds a
    /// pointer (wasm-tools' contains_ptr: a string, a list or a map somewhere in it).
    struct FlatShape
    {
        std::vector<isoline::binary::ValueType> types {}; ///< Its flat core types, up to MaxFlatParams.
        bool isOverflowing = false;                       ///< Whether it flattens to more than that.
        bool holdsPointer = false;                        ///< Whether a string, list or map is in it.
    };

    /// The join of two flat types at one position of a variant (CanonicalABI.md's join).
    [[nodiscard]] isoline::binary::ValueType joined(isoline::binary::ValueType first,
                                                    isoline::binary::ValueType second) noexcept
    {
        using isoline::binary::ValueKind;
        if (first.kind == second.kind)
            return first;
        auto const isWord = [](ValueKind kind) {
            return kind == ValueKind::I32 || kind == ValueKind::F32;
        };
        if (isWord(first.kind) && isWord(second.kind))
            return isoline::binary::I32;
        return isoline::binary::I64;
    }

    /// The core value type of a resource's representation.
    [[nodiscard]] constexpr isoline::binary::ValueType coreValueOf(binary::CoreValueKind kind) noexcept
    {
        switch (kind)
        {
            case binary::CoreValueKind::I32: return isoline::binary::I32;
            case binary::CoreValueKind::I64: return isoline::binary::I64;
            case binary::CoreValueKind::F32: return isoline::binary::F32;
            case binary::CoreValueKind::F64: return isoline::binary::F64;
        }
        return isoline::binary::I32; // uncovered(exhaustive-switch)
    }

    /// The flattening of @p defined from its children's (CanonicalABI.md's flatten_type), by its
    /// kind's FlatRuleTable row: at most MaxFlatParams types, marked overflowing past them.
    /// @param defined A value type.
    /// @param shapeOf The shape of each of its children, already made.
    /// @return Its shape.
    template <typename ShapeOf>
    [[nodiscard]] FlatShape shapeFrom(types::DefinedType const& defined, ShapeOf const& shapeOf)
    {
        auto shape = FlatShape {};
        auto const push = [&](isoline::binary::ValueType flat) {
            if (shape.types.size() == MaxFlatParams)
                shape.isOverflowing = true;
            else
                shape.types.push_back(flat);
        };
        auto const append = [&](FlatShape const& child) {
            shape.holdsPointer = shape.holdsPointer || child.holdsPointer;
            shape.isOverflowing = shape.isOverflowing || child.isOverflowing;
            for (auto const& flat: child.types)
                push(flat);
        };
        switch (FlatRuleTable[static_cast<std::size_t>(defined.kind)])
        {
            case FlatRule::I32: push(isoline::binary::I32); break;
            case FlatRule::I64: push(isoline::binary::I64); break;
            case FlatRule::F32: push(isoline::binary::F32); break;
            case FlatRule::F64: push(isoline::binary::F64); break;
            case FlatRule::Pointers:
                push(PointerType);
                push(PointerType);
                shape.holdsPointer = true;
                break;
            case FlatRule::Concatenate:
                for (auto const& child: defined.children)
                    append(shapeOf(*child));
                break;
            case FlatRule::Repeat:
                // uncovered-begin(fixed-length-lists): unreachable while 🔧 is refused.
                for ([[maybe_unused]] auto const element: std::views::iota(uint32_t { 0 }, defined.length))
                {
                    append(shapeOf(*defined.children.front()));
                    if (shape.isOverflowing)
                        break;
                }
                break;
                // uncovered-end(fixed-length-lists)
            case FlatRule::Variant: {
                push(isoline::binary::I32);
                auto const start = shape.types.size();
                for (auto const& child: defined.children)
                {
                    if (!child)
                        continue;
                    auto const& payload = shapeOf(*child);
                    shape.holdsPointer = shape.holdsPointer || payload.holdsPointer;
                    shape.isOverflowing = shape.isOverflowing || payload.isOverflowing;
                    for (auto const index: std::views::iota(std::size_t { 0 }, payload.types.size()))
                    {
                        if (start + index < shape.types.size())
                            shape.types[start + index] =
                                joined(shape.types[start + index], payload.types[index]);
                        else
                            push(payload.types[index]);
                    }
                }
                break;
            }
            case FlatRule::Flags:
                for ([[maybe_unused]] auto const word:
                     std::views::iota(std::size_t { 0 }, (defined.labels.size() + 31) / 32))
                    push(isoline::binary::I32);
                break;
        }
        return shape;
    }

    /// The steps of a binary search for a name of @p bytes bytes among @p entries names: at most
    /// bit_width(entries) comparisons, each of the name and one byte more at most, a step per
    /// SubtypingBytesPerStep bytes, as the relation charges the names it compares.
    [[nodiscard]] uint64_t nameSearchSteps(std::size_t bytes, std::size_t entries) noexcept
    {
        return (uint64_t { bytes } + 1) * static_cast<uint64_t>(std::bit_width(entries))
               / SubtypingBytesPerStep;
    }

    [[nodiscard]] ValidationError limitError(types::TypeLimitError const& error, DefinitionPosition position)
    {
        return ValidationError { .code = ValidationErrorCode::LimitExceeded,
                                 .position = position,
                                 .subject = binary::limitSubject(error.limit, error.value) };
    }

    [[nodiscard]] ValidationError capError(ComponentLimit limit,
                                           ComponentLimits const& limits,
                                           DefinitionPosition position)
    {
        return limitError(types::TypeLimitError { .limit = limit, .value = limitValue(limits, limit) },
                          position);
    }

    [[nodiscard]] ValidationError indexError(ValidationErrorCode code,
                                             uint64_t index,
                                             DefinitionPosition position)
    {
        return ValidationError { .code = code, .position = position, .subject = index };
    }

    [[nodiscard]] ValidationError plainError(ValidationErrorCode code, DefinitionPosition position)
    {
        return ValidationError { .code = code, .position = position };
    }

    [[nodiscard]] ValidationError namedError(ValidationErrorCode code,
                                             std::string name,
                                             std::string previous,
                                             DefinitionPosition position)
    {
        return ValidationError {
            .code = code, .position = position, .name = std::move(name), .previous = std::move(previous)
        };
    }

    [[nodiscard]] ValidationError indexNamedError(ValidationErrorCode code,
                                                  uint64_t index,
                                                  std::string name,
                                                  std::string previous,
                                                  DefinitionPosition position)
    {
        auto error = namedError(code, std::move(name), std::move(previous), position);
        error.subject = index;
        return error;
    }

    [[nodiscard]] ValidationError externError(ValidationErrorCode code,
                                              ExternKind kind,
                                              std::string const& name,
                                              std::string previous,
                                              DefinitionPosition position)
    {
        return ValidationError { .code = code,
                                 .position = position,
                                 .subject = static_cast<uint64_t>(kind),
                                 .name = name,
                                 .previous = std::move(previous) };
    }

    /// The text of a resource name rule broken.
    [[nodiscard]] std::string ruleText(ResourceNameRule rule, std::string_view expected = {})
    {
        return std::vformat(ResourceNameRuleMessages[static_cast<std::size_t>(rule)].text,
                            std::make_format_args(expected));
    }

    /// How a list of core types reads in a lifting mismatch: Rust's Debug of a slice of ValType, `[I32,
    /// F64]`, a reference by its spelling.
    [[nodiscard]] std::string coreTypeListText(std::vector<isoline::binary::ValueType> const& types)
    {
        constexpr auto Words = std::to_array<std::string_view>({ "I32", "I64", "F32", "F64", "V128" });
        auto text = std::string { "[" };
        for (auto const& type: types)
        {
            if (text.size() > 1)
                text += ", ";
            if (type.kind == isoline::binary::ValueKind::Ref)
                text += std::format("Ref({})", coreValueTypeText(type));
            else
                text += Words[static_cast<std::size_t>(type.kind)];
        }
        return text + "]";
    }

    /// The canonical options of one `canon lift` or `canon lower`, checked (wasm-tools' CanonicalOptions).
    struct CanonicalOptions
    {
        std::optional<binary::CanonOptionKind> encoding {}; ///< The string encoding given, if any.
        std::optional<uint32_t> memory {};                  ///< The core memory, if given.
        std::optional<uint32_t> realloc {};                 ///< The core realloc function, if given.
        std::optional<uint32_t> postReturn {};              ///< The core post-return function, if given.
    };

    /// Whether an item of sort type made an extern of its own entry, as an export does, or shares the
    /// entry it names, as an instantiation argument and an inline export do (TypeAlloc::with_unique).
    enum class TypeCopy : uint8_t
    {
        Shared, ///< `created` is `referenced`.
        Fresh,  ///< `created` is a fresh copy of `referenced`.
    };

    /// Which side of the canonical ABI a lowering is for (wasm-tools' Abi).
    enum class Abi : uint8_t
    {
        Lift,  ///< A core function lifted to a component function, which a caller calls into.
        Lower, ///< A component function lowered to a core function, which core code calls.
    };

    /// The walk over one pool: its frames, its arena, and what it found so far.
    class ComponentValidator
    {
      public:
        ComponentValidator(binary::ComponentPool const& pool,
                           ComponentFeatureSet features,
                           ComponentLimits const& limits,
                           ICoreModuleValidator& coreModules):
            _pool { pool },
            _features { features },
            _limits { limits },
            _coreModuleValidator { coreModules },
            _arena { limits },
            _relation { _arena, _arena, _noBindings, limits },
            _scopes(pool.components.size())
        {
        }

        /// Validates the pool.
        [[nodiscard]] Result<void> run();

        /// The arena, once run() succeeded.
        [[nodiscard]] types::TypeArena takeArena() { return std::move(_arena); }

        /// The root's type, once run() succeeded.
        [[nodiscard]] types::ComponentTypeInfo takeRootType() { return std::move(_rootType); }

        /// The core modules, once run() succeeded.
        [[nodiscard]] std::vector<CoreModuleShape> takeCoreModules() { return std::move(_coreModules); }

        /// What the validation cost, once run() succeeded.
        [[nodiscard]] ValidationWork work() const noexcept
        {
            auto counted = _work;
            counted.subtypingSteps = _relation.steps();
            return counted;
        }

        /// Every scope's index spaces, once run() succeeded.
        [[nodiscard]] std::vector<ScopeInfo> takeScopes() { return std::move(_scopes); }

      private:
        [[nodiscard]] Frame& current() { return _frames.back(); }
        [[nodiscard]] uint32_t freshNominal() { return _nextNominal++; }

        // The walk.
        [[nodiscard]] Result<void> step(binary::Definition const& definition, DefinitionPosition position);
        [[nodiscard]] Result<void> finish();
        [[nodiscard]] Result<void> finishComponent(Frame& frame);
        [[nodiscard]] Result<void> open(uint32_t scope, binary::ScopeKind kind, DefinitionPosition position);

        // Gates, labels and measures.
        [[nodiscard]] Result<void> admit(GatedForm form, DefinitionPosition position) const;
        [[nodiscard]] Result<void> admit(std::optional<ComponentFeature> gate,
                                         std::string_view form,
                                         DefinitionPosition position) const;
        [[nodiscard]] Result<void> label(LabelKind kind,
                                         std::string const& text,
                                         DefinitionPosition position) const;
        [[nodiscard]] static Result<void> unique(LabelSet& seen,
                                                 LabelKind kind,
                                                 std::string const& text,
                                                 DefinitionPosition position);
        [[nodiscard]] Result<TypeMeasure> combine(TypeMeasure aggregate,
                                                  TypeMeasure child,
                                                  DefinitionPosition position) const;

        // Index spaces.
        [[nodiscard]] Result<void> reserve(Sort sort, DefinitionPosition position);
        [[nodiscard]] Result<void> pushType(TypeIdentity type, DefinitionPosition position);
        [[nodiscard]] Result<TypeIdentity> slotAt(uint32_t index, DefinitionPosition position);
        [[nodiscard]] Result<AnyTypeId> typeAt(uint32_t index, DefinitionPosition position);
        [[nodiscard]] Result<CoreTypeEntry> coreTypeAt(Frame const& frame,
                                                       uint32_t index,
                                                       DefinitionPosition position) const;
        [[nodiscard]] Result<CoreFunctionTypeId> coreFunctionAt(uint32_t index, DefinitionPosition position);
        [[nodiscard]] Result<types::FunctionTypeId> functionAt(uint32_t index, DefinitionPosition position);
        [[nodiscard]] Result<types::InstanceTypeId> instanceAt(uint32_t index, DefinitionPosition position);
        [[nodiscard]] Result<types::ComponentTypeId> componentAt(Frame const& frame,
                                                                 uint32_t index,
                                                                 DefinitionPosition position) const;
        [[nodiscard]] Result<CoreModuleTypeId> moduleAt(Frame const& frame,
                                                        uint32_t index,
                                                        DefinitionPosition position) const;
        [[nodiscard]] Result<CoreInstanceTypeId> coreInstanceAt(uint32_t index, DefinitionPosition position);
        [[nodiscard]] Result<CoreEntityType> coreEntityAt(Sort sort,
                                                          uint32_t index,
                                                          DefinitionPosition position);
        [[nodiscard]] Result<Frame*> outerFrame(uint32_t count, DefinitionPosition position);
        [[nodiscard]] TypeIdentity withUnique(TypeIdentity type);

        // Types.
        [[nodiscard]] Result<TypeId> valueType(binary::ValueTypeRef const& type, DefinitionPosition position);
        [[nodiscard]] Result<TypeId> intern(types::DefinedType type, DefinitionPosition position);
        [[nodiscard]] Result<void> typeDefinition(binary::TypeDefinition const& definition,
                                                  DefinitionPosition position);
        [[nodiscard]] Result<TypeId> definedType(binary::DefinedValueType const& type,
                                                 uint32_t nominal,
                                                 DefinitionPosition position);
        [[nodiscard]] Result<TypeId> record(binary::RecordType const& type,
                                            uint32_t nominal,
                                            DefinitionPosition position);
        [[nodiscard]] Result<TypeId> variant(binary::VariantType const& type,
                                             uint32_t nominal,
                                             DefinitionPosition position);
        [[nodiscard]] Result<TypeId> tuple(binary::TupleType const& type,
                                           uint32_t nominal,
                                           DefinitionPosition position);
        [[nodiscard]] Result<TypeId> labelsOnly(ValueKind kind,
                                                std::vector<std::string> const& labels,
                                                uint32_t nominal,
                                                DefinitionPosition position);
        [[nodiscard]] Result<TypeId> elements(ValueKind kind,
                                              std::vector<std::optional<binary::ValueTypeRef>> const& refs,
                                              uint32_t nominal,
                                              DefinitionPosition position);
        [[nodiscard]] Result<TypeId> map(binary::MapType const& type,
                                         uint32_t nominal,
                                         DefinitionPosition position);
        [[nodiscard]] Result<TypeId> handle(ValueKind kind,
                                            uint32_t resource,
                                            uint32_t nominal,
                                            DefinitionPosition position);
        [[nodiscard]] Result<void> functionType(binary::FunctionTypeDefinition const& type,
                                                DefinitionPosition position);
        [[nodiscard]] Result<void> resourceType(binary::ResourceTypeDefinition const& type,
                                                DefinitionPosition position);

        // Core types, modules and instances.
        [[nodiscard]] Result<void> coreType(binary::CoreTypeDefinition const& definition,
                                            DefinitionPosition position);
        [[nodiscard]] Result<CoreFunctionTypeId> coreFunctionType(isoline::binary::FuncType type,
                                                                  std::span<CoreFunctionTypeId const> scope,
                                                                  DefinitionPosition position);
        [[nodiscard]] Result<isoline::binary::ValueType> coreValueType(
            isoline::binary::ValueType type,
            std::span<CoreFunctionTypeId const> scope,
            DefinitionPosition position) const;
        [[nodiscard]] Result<CoreEntityType> declaredEntity(binary::CoreExternType const& type,
                                                            std::span<CoreFunctionTypeId const> scope,
                                                            DefinitionPosition position);
        [[nodiscard]] Result<CoreModuleTypeId> moduleType(binary::CoreModuleTypeDefinition const& type,
                                                          DefinitionPosition position);
        [[nodiscard]] Result<CoreModuleTypeId> embeddedModuleType(isoline::binary::Module const& module,
                                                                  DefinitionPosition position);
        [[nodiscard]] Result<CoreModuleTypeId> finishModuleType(types::CoreModuleTypeInfo type,
                                                                DefinitionPosition position);
        [[nodiscard]] Result<void> coreModule(binary::CoreModuleDefinition const& module,
                                              DefinitionPosition position);
        [[nodiscard]] Result<void> coreInstance(binary::CoreInstanceDefinition const& instance,
                                                DefinitionPosition position);
        [[nodiscard]] Result<CoreInstanceTypeId> coreInstantiate(binary::CoreInstantiate const& instantiate,
                                                                 DefinitionPosition position);
        [[nodiscard]] Result<CoreInstanceTypeId> coreInlineExports(binary::CoreInlineExports const& exports,
                                                                   DefinitionPosition position);

        // Imports, exports and names.
        [[nodiscard]] Result<ExternType> checkTypeRef(binary::ExternDesc const& type,
                                                      DefinitionPosition position);
        [[nodiscard]] Result<void> addEntity(ExternType& type,
                                             std::optional<std::pair<std::string const*, ExternKind>> named,
                                             DefinitionPosition position);
        [[nodiscard]] Result<ExternType> freshen(types::InstanceTypeId instance,
                                                 ExternKind kind,
                                                 uint32_t externIndex,
                                                 DefinitionPosition position);
        [[nodiscard]] Result<void> declare(binary::NameAttributes const& name,
                                           binary::ExternDesc const& type,
                                           ExternKind kind,
                                           DefinitionPosition position);
        [[nodiscard]] Result<void> exportDefinition(binary::ExportDefinition const& definition,
                                                    DefinitionPosition position);
        [[nodiscard]] Result<ExternType> itemEntity(binary::SortIndex const& item,
                                                    TypeCopy copy,
                                                    DefinitionPosition position);
        [[nodiscard]] Result<void> name(binary::NameAttributes const& name,
                                        ExternType const& type,
                                        ExternKind kind,
                                        ResourceNames const& resources,
                                        LabelSet& names,
                                        DefinitionPosition position);
        [[nodiscard]] Result<void> implements(binary::NameAttributes const& name,
                                              std::optional<ExternName> const& parsed,
                                              ExternType const& type,
                                              DefinitionPosition position) const;
        [[nodiscard]] Result<void> attributes(binary::NameAttributes const& name,
                                              DefinitionPosition position) const;
        [[nodiscard]] std::optional<std::string> resourceFunctionRule(ExternName const& parsed,
                                                                      ExternType const& type,
                                                                      ResourceNames const& resources) const;

        // Visibility (validate_and_register_named_types).
        [[nodiscard]] bool registerNamed(std::string const* name, ExternKind kind, ExternType const& type);
        [[nodiscard]] bool typeNamed(TypeId type, std::set<TypeIdentity> const& named) const;
        [[nodiscard]] bool definedNamed(TypeId type, std::set<TypeIdentity> const& named) const;
        [[nodiscard]] bool functionNamed(types::FunctionTypeId function,
                                         std::set<TypeIdentity> const& named) const;
        [[nodiscard]] bool instanceNamed(types::InstanceTypeId instance, std::set<TypeIdentity>& named) const;
        [[nodiscard]] bool anyNamed(TypeIdentity type, std::set<TypeIdentity>& named) const;

        // Resources.
        [[nodiscard]] std::set<types::ResourceTypeId> freeResources(std::span<AnyTypeId const> roots);
        [[nodiscard]] std::map<types::ResourceTypeId, std::vector<uint32_t>> explicitResourcePaths(
            std::vector<types::NamedExtern> const& exports, std::set<types::ResourceTypeId> remaining) const;
        void addLocalResource(types::ResourceTypeId resource, std::optional<binary::CoreValueKind> rep);

        // Aliases and instances.
        [[nodiscard]] Result<void> alias(binary::AliasDefinition const& alias, DefinitionPosition position);
        [[nodiscard]] Result<void> outerAlias(binary::AliasDefinition const& alias,
                                              DefinitionPosition position);
        [[nodiscard]] Result<void> exportAlias(binary::AliasDefinition const& alias,
                                               DefinitionPosition position);
        [[nodiscard]] Result<void> coreExportAlias(binary::AliasDefinition const& alias,
                                                   DefinitionPosition position);
        [[nodiscard]] Result<void> chargeCoreMatching(uint64_t steps, DefinitionPosition position);
        [[nodiscard]] ValidationError relationRefusal(RelationError error,
                                                      ValidationErrorCode code,
                                                      DefinitionPosition position) const;
        [[nodiscard]] types::NamedExtern const* instanceExport(types::InstanceTypeId instance,
                                                               std::string const& name);
        [[nodiscard]] Result<void> instance(binary::InstanceDefinition const& instance,
                                            DefinitionPosition position);
        [[nodiscard]] Result<types::InstanceTypeId> instantiate(binary::Instantiate const& instantiate,
                                                                DefinitionPosition position);
        [[nodiscard]] Result<types::InstanceTypeId> inlineExports(binary::InlineExports const& exports,
                                                                  DefinitionPosition position);

        // Canonical definitions.
        [[nodiscard]] Result<void> canon(binary::CanonDefinition const& canon, DefinitionPosition position);
        [[nodiscard]] Result<CanonicalOptions> options(std::vector<binary::CanonOption> const& options,
                                                       DefinitionPosition position);
        [[nodiscard]] Result<isoline::binary::FuncType> lowered(types::FunctionTypeId function,
                                                                CanonicalOptions const& options,
                                                                Abi abi,
                                                                DefinitionPosition position);
        [[nodiscard]] FlatShape const& flatShape(TypeId type);
        [[nodiscard]] Result<types::ResourceTypeId> resourceAt(uint32_t index, DefinitionPosition position);
        [[nodiscard]] Result<binary::CoreValueKind> localResourceAt(uint32_t index,
                                                                    DefinitionPosition position);
        [[nodiscard]] Result<void> pushCoreFunction(isoline::binary::FuncType type,
                                                    DefinitionPosition position);

        binary::ComponentPool const& _pool;
        ComponentFeatureSet _features;
        ComponentLimits const& _limits;
        ICoreModuleValidator& _coreModuleValidator;
        types::TypeArena _arena;
        ResourceMapping _noBindings {};
        /// One relation for the whole validation: its memo and name indices serve every check.
        TypeRelation _relation;
        std::vector<Frame> _frames {};
        std::vector<ScopeInfo> _scopes;
        types::ComponentTypeInfo _rootType {};
        std::vector<CoreModuleShape> _coreModules {};
        uint32_t _nextNominal = 1;
        uint32_t _nextAlias = 1;
        /// The flattening of each value type met by a canonical definition, by TypeId: each type is
        /// flattened once, so no lowering walks a type more than once.
        std::map<TypeId, FlatShape> _flatShapes {};
        /// The component and instance types found to name no resource they do not bind: a later walk
        /// skips them, so a closed type shared by many definitions is walked once.
        std::set<AnyTypeId> _closedTypes {};
        ValidationWork _work {};
        /// The exports of each instance type an export alias looked into, ordered by name, by its id.
        std::map<uint32_t, std::vector<uint32_t>> _exportIndices {};
    };

    Result<void> ComponentValidator::run()
    {
        Require(!_pool.components.empty() && _pool.components.front().kind == binary::ScopeKind::Component);
        _frames.push_back(Frame { .scope = 0, .kind = binary::ScopeKind::Component });
        while (!_frames.empty())
        {
            auto& frame = _frames.back();
            auto const& definitions = _pool.components[frame.scope].definitions;
            if (frame.next == definitions.size())
            {
                if (auto finished = finish(); !finished)
                    return finished;
                continue;
            }
            auto const position = DefinitionPosition { .scope = frame.scope, .definition = frame.next++ };
            // step() may open a frame, so nothing past this point refers to frame.
            if (auto stepped = step(definitions[position.definition], position); !stepped)
                return stepped;
        }
        return {};
    }

    Result<void> ComponentValidator::open(uint32_t scope, binary::ScopeKind kind, DefinitionPosition position)
    {
        Require(scope > position.scope && scope < _pool.components.size());
        Require(_pool.components[scope].kind == kind);
        _frames.push_back(Frame { .scope = scope, .kind = kind, .origin = position });
        return {};
    }

    Result<void> ComponentValidator::finish()
    {
        auto frame = std::move(_frames.back());
        _frames.pop_back();
        if (frame.kind != binary::ScopeKind::InstanceType)
        {
            // finish (component.rs:4560-4651): no import may name a resource the scope defines.
            auto importTypes = std::vector<AnyTypeId> {};
            for (auto const& entry: frame.imports)
            {
                if (auto const* bound = std::get_if<types::TypeExtern>(&entry.type))
                    importTypes.push_back(bound->type);
                else if (!std::holds_alternative<CoreModuleTypeId>(entry.type)
                         && !std::holds_alternative<types::ValueExtern>(entry.type))
                    std::visit(
                        [&](auto const& id) {
                            using Id = std::remove_cvref_t<decltype(id)>;
                            if constexpr (std::is_constructible_v<AnyTypeId, Id>)
                                importTypes.emplace_back(id);
                        },
                        entry.type);
            }
            auto const free = freeResources(importTypes);
            for (auto const& [resource, rep]: frame.localResources)
            {
                if (free.contains(resource))
                    return std::unexpected { plainError(ValidationErrorCode::LocalResourceInImports,
                                                        frame.origin) };
            }
        }
        _scopes[frame.scope] = std::move(frame.spaces);
        switch (frame.kind)
        {
            case binary::ScopeKind::Component: return finishComponent(frame);
            case binary::ScopeKind::ComponentType: {
                auto const added = _arena.add(
                    types::ComponentTypeInfo { .imports = std::move(frame.imports),
                                               .exports = std::move(frame.exports),
                                               .importedResources = std::move(frame.importedResources),
                                               .definedResources = std::move(frame.definedResources) });
                if (!added)
                    // uncovered-begin(arena-refold)
                    return std::unexpected { limitError(added.error(), frame.origin) };
                // uncovered-end(arena-refold)
                return pushType(TypeIdentity { .type = *added }, frame.origin);
            }
            case binary::ScopeKind::InstanceType: {
                Require(frame.imports.empty() && frame.importedResources.empty());
                auto const added = _arena.add(
                    types::InstanceTypeInfo { .exports = std::move(frame.exports),
                                              .definedResources = std::move(frame.definedResources) });
                if (!added)
                    // uncovered-begin(arena-refold)
                    return std::unexpected { limitError(added.error(), frame.origin) };
                // uncovered-end(arena-refold)
                return pushType(TypeIdentity { .type = *added }, frame.origin);
            }
        }
        Require(false);
        return {}; // uncovered(exhaustive-switch)
    }

    Result<void> ComponentValidator::finishComponent(Frame& frame)
    {
        // finish (component.rs:4615-4651): the component defines the resources of its own that its
        // exports name, each exported somewhere by a path of exports; one named but not exported is
        // refused.
        auto exportTypes = std::vector<AnyTypeId> {};
        for (auto const& entry: frame.exports)
        {
            std::visit(
                [&](auto const& alternative) {
                    using Alternative = std::remove_cvref_t<decltype(alternative)>;
                    if constexpr (std::is_same_v<Alternative, types::TypeExtern>)
                        exportTypes.push_back(alternative.type);
                    else if constexpr (std::is_same_v<Alternative, types::ValueExtern>)
                        exportTypes.emplace_back(alternative.type);
                    else if constexpr (std::is_constructible_v<AnyTypeId, Alternative>)
                        exportTypes.emplace_back(alternative);
                },
                entry.type);
        }
        auto const free = freeResources(exportTypes);
        auto used = std::set<types::ResourceTypeId> {};
        auto order = std::vector<types::ResourceTypeId> {};
        for (auto const& [resource, rep]: frame.localResources)
        {
            if (free.contains(resource))
            {
                used.insert(resource);
                order.push_back(resource);
            }
        }
        auto paths = explicitResourcePaths(frame.exports, used);
        auto defined = std::vector<types::ResourcePath> {};
        defined.reserve(order.size());
        for (auto const resource: order)
        {
            auto const path = paths.find(resource);
            // The visibility rule refuses an export naming a resource it does not export first, so no
            // valid pool reaches this; wasm-tools keeps the check "out of an abundance of caution".
            if (path == paths.end())
                // uncovered-begin(visibility-first)
                return std::unexpected { plainError(ValidationErrorCode::LocalResourceNotExported,
                                                    frame.origin) };
            // uncovered-end(visibility-first)
            defined.push_back(types::ResourcePath { .resource = resource, .path = std::move(path->second) });
        }
        auto type = types::ComponentTypeInfo { .imports = std::move(frame.imports),
                                               .exports = std::move(frame.exports),
                                               .importedResources = std::move(frame.importedResources),
                                               .definedResources = std::move(defined) };
        if (_frames.empty())
        {
            type.measure = frame.measure;
            _rootType = std::move(type);
            return {};
        }
        // add_component (component.rs:2508-2512): the nested component's type, an entry of its parent's
        // component index space, which its section checked room for.
        auto const added = _arena.add(std::move(type));
        if (!added)
            // uncovered-begin(arena-refold)
            return std::unexpected { limitError(added.error(), frame.origin) };
        // uncovered-end(arena-refold)
        current().spaces.components.push_back(*added);
        return {};
    }

    Result<void> ComponentValidator::step(binary::Definition const& definition, DefinitionPosition position)
    {
        return std::visit(
            [&](auto const& alternative) -> Result<void> {
                using Alternative = std::remove_cvref_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Alternative, binary::TypeDefinition>)
                    return typeDefinition(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::NestedComponent>)
                    // component_section (validator.rs:1140-1158): room in the component space first.
                    return reserve(Sort::Component, position).and_then([&] {
                        return open(alternative.index, binary::ScopeKind::Component, position);
                    });
                else if constexpr (std::is_same_v<Alternative, binary::ImportDefinition>)
                    return declare(alternative.name, alternative.type, ExternKind::Import, position);
                else if constexpr (std::is_same_v<Alternative, binary::ExportDeclaration>)
                    return declare(alternative.name, alternative.type, ExternKind::Export, position);
                else if constexpr (std::is_same_v<Alternative, binary::ExportDefinition>)
                    return exportDefinition(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::AliasDefinition>)
                    return alias(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::CoreModuleDefinition>)
                    return coreModule(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::CoreTypeDefinition>)
                    return coreType(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::CoreInstanceDefinition>)
                    return coreInstance(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::InstanceDefinition>)
                    return instance(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::CanonDefinition>)
                    return canon(alternative, position);
                else
                {
                    static_assert(std::is_same_v<Alternative, binary::CustomDefinition>,
                                  "every definition has a case");
                    return {};
                }
            },
            definition);
    }

    Result<void> ComponentValidator::admit(GatedForm form, DefinitionPosition position) const
    {
        auto const& row = gatedFormRow(form);
        return admit(row.feature, row.text, position);
    }

    Result<void> ComponentValidator::admit(std::optional<ComponentFeature> gate,
                                           std::string_view form,
                                           DefinitionPosition position) const
    {
        if (!gate || validationAdmits(_features, *gate))
            return {};
        return std::unexpected { ValidationError { .code = ValidationErrorCode::FeatureDisabled,
                                                   .position = position,
                                                   .name = std::string { form },
                                                   .disabled = gate } };
    }

    Result<void> ComponentValidator::label(LabelKind kind,
                                           std::string const& text,
                                           DefinitionPosition position) const
    {
        // wasmparser's to_kebab_string (component.rs:28-39): empty first, then kebab case.
        if (isKebabCase(text))
            return {};
        return std::unexpected { ValidationError {
            .code = text.empty() ? ValidationErrorCode::EmptyLabel : ValidationErrorCode::LabelNotKebabCase,
            .position = position,
            .subject = static_cast<uint64_t>(kind),
            .name = text } };
    }

    Result<void> ComponentValidator::unique(LabelSet& seen,
                                            LabelKind kind,
                                            std::string const& text,
                                            DefinitionPosition position)
    {
        auto previous = seen.insert(text);
        if (!previous)
            return {};
        return std::unexpected { ValidationError { .code = ValidationErrorCode::DuplicateLabel,
                                                   .position = position,
                                                   .subject = static_cast<uint64_t>(kind),
                                                   .name = text,
                                                   .previous = std::move(*previous) } };
    }

    Result<TypeMeasure> ComponentValidator::combine(TypeMeasure aggregate,
                                                    TypeMeasure child,
                                                    DefinitionPosition position) const
    {
        return _arena.combine(aggregate, child).transform_error([&](types::TypeLimitError const& error) {
            return limitError(error, position);
        });
    }

    Result<void> ComponentValidator::reserve(Sort sort, DefinitionPosition position)
    {
        // check_max(len, 1, max, desc): one more entry of @p sort's space (and of the space it shares
        // its cap with) must stay within the cap.
        auto const& row = SortLimitTable[static_cast<std::size_t>(sort)];
        if (!row.limit)
            return {};
        auto const& spaces = current().spaces;
        auto count = spaceSize(spaces, sort);
        if (row.sharedWith)
            count += spaceSize(spaces, *row.sharedWith);
        if (count + 1 > limitValue(_limits, *row.limit))
            return std::unexpected { capError(*row.limit, _limits, position) };
        return {};
    }

    Result<void> ComponentValidator::pushType(TypeIdentity type, DefinitionPosition position)
    {
        if (auto reserved = reserve(Sort::Type, position); !reserved)
            return reserved;
        current().spaces.types.push_back(type);
        return {};
    }

    Result<TypeIdentity> ComponentValidator::slotAt(uint32_t index, DefinitionPosition position)
    {
        auto const& space = current().spaces.types;
        if (index >= space.size())
            return std::unexpected { indexError(ValidationErrorCode::TypeIndexOutOfBounds, index, position) };
        return space[index];
    }

    Result<AnyTypeId> ComponentValidator::typeAt(uint32_t index, DefinitionPosition position)
    {
        return slotAt(index, position).transform([](TypeIdentity slot) { return slot.type; });
    }

    Result<CoreTypeEntry> ComponentValidator::coreTypeAt(Frame const& frame,
                                                         uint32_t index,
                                                         DefinitionPosition position) const
    {
        auto const& space = frame.spaces.coreTypes;
        if (index >= space.size())
            return std::unexpected { indexError(ValidationErrorCode::TypeIndexOutOfBounds, index, position) };
        return space[index];
    }

    /// The entry of @p space at @p index, or @p code's refusal.
    template <typename Entry>
    [[nodiscard]] Result<Entry> entryAt(std::vector<Entry> const& space,
                                        uint32_t index,
                                        ValidationErrorCode code,
                                        DefinitionPosition position)
    {
        if (index >= space.size())
            return std::unexpected { indexError(code, index, position) };
        return space[index];
    }

    Result<CoreFunctionTypeId> ComponentValidator::coreFunctionAt(uint32_t index, DefinitionPosition position)
    {
        return entryAt(current().spaces.coreFunctions,
                       index,
                       ValidationErrorCode::CoreFunctionIndexOutOfBounds,
                       position);
    }

    Result<types::FunctionTypeId> ComponentValidator::functionAt(uint32_t index, DefinitionPosition position)
    {
        return entryAt(
            current().spaces.functions, index, ValidationErrorCode::FunctionIndexOutOfBounds, position);
    }

    Result<types::InstanceTypeId> ComponentValidator::instanceAt(uint32_t index, DefinitionPosition position)
    {
        return entryAt(
            current().spaces.instances, index, ValidationErrorCode::InstanceIndexOutOfBounds, position);
    }

    Result<types::ComponentTypeId> ComponentValidator::componentAt(Frame const& frame,
                                                                   uint32_t index,
                                                                   DefinitionPosition position) const
    {
        return entryAt(
            frame.spaces.components, index, ValidationErrorCode::ComponentIndexOutOfBounds, position);
    }

    Result<CoreModuleTypeId> ComponentValidator::moduleAt(Frame const& frame,
                                                          uint32_t index,
                                                          DefinitionPosition position) const
    {
        return entryAt(
            frame.spaces.coreModules, index, ValidationErrorCode::ModuleIndexOutOfBounds, position);
    }

    Result<CoreInstanceTypeId> ComponentValidator::coreInstanceAt(uint32_t index, DefinitionPosition position)
    {
        return entryAt(current().spaces.coreInstances,
                       index,
                       ValidationErrorCode::CoreInstanceIndexOutOfBounds,
                       position);
    }

    Result<CoreEntityType> ComponentValidator::coreEntityAt(Sort sort,
                                                            uint32_t index,
                                                            DefinitionPosition position)
    {
        // An entry of a core extern index space, as the core inline exports of an instance name it
        // (instantiate_core_exports, component.rs:3633-3721).
        using isoline::binary::ExternKind;
        auto const& spaces = current().spaces;
        auto entity = CoreEntityType { .kind = coreKindOf(sort) };
        switch (entity.kind)
        {
            case ExternKind::Func:
                return entryAt(spaces.coreFunctions,
                               index,
                               ValidationErrorCode::CoreFunctionIndexOutOfBounds,
                               position)
                    .transform([&](CoreFunctionTypeId id) {
                        entity.typeIndex = id.value;
                        return entity;
                    });
            case ExternKind::Table:
                return entryAt(spaces.coreTables, index, ValidationErrorCode::TableIndexOutOfBounds, position)
                    .transform([&](isoline::binary::TableType const& table) {
                        entity.table = table;
                        return entity;
                    });
            case ExternKind::Memory:
                return entryAt(
                           spaces.coreMemories, index, ValidationErrorCode::MemoryIndexOutOfBounds, position)
                    .transform([&](isoline::binary::MemoryType const& memory) {
                        entity.memory = memory;
                        return entity;
                    });
            case ExternKind::Global:
                return entryAt(
                           spaces.coreGlobals, index, ValidationErrorCode::GlobalIndexOutOfBounds, position)
                    .transform([&](isoline::binary::GlobalType const& global) {
                        entity.global = global;
                        return entity;
                    });
            case ExternKind::Tag:
                return entryAt(spaces.coreTags, index, ValidationErrorCode::TagIndexOutOfBounds, position)
                    .transform([&](CoreFunctionTypeId id) {
                        entity.typeIndex = id.value;
                        return entity;
                    });
        }
        Require(false);
        return entity; // uncovered(exhaustive-switch)
    }

    Result<Frame*> ComponentValidator::outerFrame(uint32_t count, DefinitionPosition position)
    {
        // check_alias_count (component.rs:3973-3980).
        if (count >= _frames.size())
            return std::unexpected { indexError(
                ValidationErrorCode::InvalidOuterAliasCount, count, position) };
        return &_frames[_frames.size() - 1 - count];
    }

    TypeIdentity ComponentValidator::withUnique(TypeIdentity type)
    {
        // TypeAlloc::with_unique (component_types.rs:2734): the same type under a fresh identity, which
        // the visibility rule tells apart from the entry it copies. A value type's copy is a type of its
        // own (DefinedType::nominal), as it is a field of the types naming it.
        if (auto const* value = std::get_if<TypeId>(&type.type))
        {
            auto copy = _arena[*value];
            copy.nominal = freshNominal();
            auto const added = _arena.intern(std::move(copy));
            Require(added.has_value()); // the copy measures as the type it copies, which was admitted
            return TypeIdentity { .type = *added };
        }
        return TypeIdentity { .type = type.type, .alias = _nextAlias++ };
    }

    Result<TypeId> ComponentValidator::valueType(binary::ValueTypeRef const& type,
                                                 DefinitionPosition position)
    {
        if (auto const* primitive = std::get_if<binary::PrimitiveValueType>(&type))
        {
            auto const& row = binary::PrimitiveValueTypeTable[static_cast<std::size_t>(*primitive)];
            auto const admitted = row.gate ? admit(GatedForm::ErrorContextType, position) : Result<void> {};
            return admitted.transform([&] { return _arena.primitive(types::kindOf(*primitive)); });
        }
        auto const index = std::get<binary::TypeIndex>(type).index;
        auto const entry = typeAt(index, position);
        if (!entry)
            return std::unexpected { entry.error() };
        if (auto const* defined = std::get_if<TypeId>(&*entry))
            return *defined;
        return std::unexpected { indexError(ValidationErrorCode::NotDefinedType, index, position) };
    }

    Result<TypeId> ComponentValidator::intern(types::DefinedType type, DefinitionPosition position)
    {
        return _arena.intern(std::move(type)).transform_error([&](types::TypeLimitError const& error) {
            return limitError(error, position); // uncovered(premeasured)
        });                                     // uncovered(premeasured)
    }

    Result<void> ComponentValidator::typeDefinition(binary::TypeDefinition const& definition,
                                                    DefinitionPosition position)
    {
        return std::visit(
            [&](auto const& type) -> Result<void> {
                using Type = std::remove_cvref_t<decltype(type)>;
                if constexpr (std::is_same_v<Type, binary::DefinedValueType>)
                    return definedType(type, freshNominal(), position).and_then([&](TypeId id) {
                        return pushType(TypeIdentity { .type = id }, position);
                    });
                else if constexpr (std::is_same_v<Type, binary::FunctionTypeDefinition>)
                    return functionType(type, position);
                else if constexpr (std::is_same_v<Type, binary::ComponentTypeDefinition>)
                    return open(type.scope, binary::ScopeKind::ComponentType, position);
                else if constexpr (std::is_same_v<Type, binary::InstanceTypeDefinition>)
                    return open(type.scope, binary::ScopeKind::InstanceType, position);
                else
                {
                    static_assert(std::is_same_v<Type, binary::ResourceTypeDefinition>,
                                  "every type has a case");
                    return resourceType(type, position);
                }
            },
            definition.type);
    }

    Result<TypeId> ComponentValidator::definedType(binary::DefinedValueType const& type,
                                                   uint32_t nominal,
                                                   DefinitionPosition position)
    {
        // The gate of a compound comes first, as wasmparser's create_defined_type checks it first.
        if (type.index() > 0)
        {
            if (auto const form = DefinedTypeGates[type.index() - 1])
            {
                if (auto admitted = admit(*form, position); !admitted)
                    return std::unexpected { admitted.error() };
            }
        }
        return std::visit(
            [&](auto const& alternative) -> Result<TypeId> {
                using Alternative = std::remove_cvref_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Alternative, binary::PrimitiveValueType>)
                    // A defined primitive is a type of its own (wasm-tools' ComponentDefinedType::Primitive).
                    return valueType(alternative, position).and_then([&](TypeId primitive) {
                        return intern(
                            types::DefinedType { .kind = _arena[primitive].kind, .nominal = nominal },
                            position);
                    });
                else if constexpr (std::is_same_v<Alternative, binary::RecordType>)
                    return record(alternative, nominal, position);
                else if constexpr (std::is_same_v<Alternative, binary::VariantType>)
                    return variant(alternative, nominal, position);
                else if constexpr (std::is_same_v<Alternative, binary::ListType>)
                    return elements(ValueKind::List, { alternative.element }, nominal, position);
                else if constexpr (std::is_same_v<Alternative, binary::FixedListType>)
                // uncovered-begin(fixed-length-lists): unreachable while the gate above refuses 🔧.
                {
                    if (alternative.length < 1)
                        return std::unexpected { plainError(ValidationErrorCode::EmptyFixedList, position) };
                    if (alternative.length > MaxFixedListElements)
                        return std::unexpected { indexError(
                            ValidationErrorCode::FixedListTooLong, MaxFixedListElements, position) };
                    auto const element = valueType(alternative.element, position);
                    if (!element)
                        return element;
                    if (auto measured =
                            combine(TypeMeasure {}, _arena.measure(AnyTypeId { *element }), position);
                        !measured)
                        return std::unexpected { measured.error() };
                    return intern(types::DefinedType { .kind = ValueKind::FixedList,
                                                       .children = { *element },
                                                       .length = alternative.length,
                                                       .nominal = nominal },
                                  position);
                }
                // uncovered-end(fixed-length-lists)
                else if constexpr (std::is_same_v<Alternative, binary::TupleType>)
                    return tuple(alternative, nominal, position);
                else if constexpr (std::is_same_v<Alternative, binary::FlagsType>)
                {
                    if (alternative.labels.empty())
                        return std::unexpected { plainError(ValidationErrorCode::EmptyFlags, position) };
                    if (alternative.labels.size() > MaxFlags)
                        return std::unexpected { plainError(ValidationErrorCode::TooManyFlags, position) };
                    return labelsOnly(ValueKind::Flags, alternative.labels, nominal, position);
                }
                else if constexpr (std::is_same_v<Alternative, binary::EnumType>)
                {
                    if (alternative.labels.empty())
                        return std::unexpected { plainError(ValidationErrorCode::EmptyEnum, position) };
                    return labelsOnly(ValueKind::Enum, alternative.labels, nominal, position);
                }
                else if constexpr (std::is_same_v<Alternative, binary::OptionType>)
                    return elements(ValueKind::Option, { alternative.type }, nominal, position);
                else if constexpr (std::is_same_v<Alternative, binary::ResultType>)
                    return elements(
                        ValueKind::Result, { alternative.ok, alternative.error }, nominal, position);
                else if constexpr (std::is_same_v<Alternative, binary::OwnType>)
                    return handle(ValueKind::Own, alternative.resource, nominal, position);
                else if constexpr (std::is_same_v<Alternative, binary::BorrowType>)
                    return handle(ValueKind::Borrow, alternative.resource, nominal, position);
                else if constexpr (std::is_same_v<Alternative, binary::StreamType>)
                    // uncovered-begin(async): unreachable while the gate above refuses 🔀.
                    return elements(ValueKind::Stream, { alternative.element }, nominal, position);
                // uncovered-end(async)
                else if constexpr (std::is_same_v<Alternative, binary::FutureType>)
                    // uncovered-begin(async): unreachable while the gate above refuses 🔀.
                    return elements(ValueKind::Future, { alternative.element }, nominal, position);
                // uncovered-end(async)
                else
                {
                    static_assert(std::is_same_v<Alternative, binary::MapType>,
                                  "every defvaltype has a case");
                    return map(alternative, nominal, position);
                }
            },
            type);
    }

    Result<TypeId> ComponentValidator::record(binary::RecordType const& type,
                                              uint32_t nominal,
                                              DefinitionPosition position)
    {
        // create_record_type (component.rs:4187-4224): per field its label, its type, then uniqueness.
        if (type.fields.empty())
            return std::unexpected { plainError(ValidationErrorCode::EmptyRecord, position) };
        auto seen = LabelSet {};
        auto measure = TypeMeasure {};
        auto defined = types::DefinedType { .kind = ValueKind::Record, .nominal = nominal };
        defined.children.reserve(type.fields.size());
        defined.labels.reserve(type.fields.size());
        for (auto const& field: type.fields)
        {
            if (auto checked = label(LabelKind::RecordField, field.label, position); !checked)
                return std::unexpected { checked.error() };
            auto const child = valueType(field.type, position);
            if (!child)
                return child;
            if (auto checked = unique(seen, LabelKind::RecordField, field.label, position); !checked)
                return std::unexpected { checked.error() };
            auto measured = combine(measure, _arena.measure(AnyTypeId { *child }), position);
            if (!measured)
                return std::unexpected { measured.error() };
            measure = *measured;
            defined.children.emplace_back(*child);
            defined.labels.emplace_back(field.label);
        }
        return intern(std::move(defined), position);
    }

    Result<TypeId> ComponentValidator::variant(binary::VariantType const& type,
                                               uint32_t nominal,
                                               DefinitionPosition position)
    {
        // create_variant_type (component.rs:4226-4282): per case its label, its payload, then uniqueness.
        if (type.cases.empty())
            return std::unexpected { plainError(ValidationErrorCode::EmptyVariant, position) };
        auto seen = LabelSet {};
        auto measure = TypeMeasure {};
        auto defined = types::DefinedType { .kind = ValueKind::Variant, .nominal = nominal };
        defined.children.reserve(type.cases.size());
        defined.labels.reserve(type.cases.size());
        for (auto const& entry: type.cases)
        {
            if (auto checked = label(LabelKind::VariantCase, entry.label, position); !checked)
                return std::unexpected { checked.error() };
            auto payload = std::optional<TypeId> {};
            if (entry.type)
            {
                auto const child = valueType(*entry.type, position);
                if (!child)
                    return child;
                payload = *child;
            }
            if (auto checked = unique(seen, LabelKind::VariantCase, entry.label, position); !checked)
                return std::unexpected { checked.error() };
            if (payload)
            {
                auto measured = combine(measure, _arena.measure(AnyTypeId { *payload }), position);
                if (!measured)
                    return std::unexpected { measured.error() };
                measure = *measured;
            }
            defined.children.push_back(payload);
            defined.labels.emplace_back(entry.label);
        }
        return intern(std::move(defined), position);
    }

    Result<TypeId> ComponentValidator::tuple(binary::TupleType const& type,
                                             uint32_t nominal,
                                             DefinitionPosition position)
    {
        if (type.elements.empty())
            return std::unexpected { plainError(ValidationErrorCode::EmptyTuple, position) };
        auto refs = std::vector<std::optional<binary::ValueTypeRef>> {};
        refs.reserve(type.elements.size());
        for (auto const& element: type.elements)
            refs.emplace_back(element);
        return elements(ValueKind::Tuple, refs, nominal, position);
    }

    Result<TypeId> ComponentValidator::elements(ValueKind kind,
                                                std::vector<std::optional<binary::ValueTypeRef>> const& refs,
                                                uint32_t nominal,
                                                DefinitionPosition position)
    {
        // A list's, an option's, a tuple's and a result's inner types: each resolved, then each measured,
        // as wasmparser resolves a result's two before it combines them.
        auto defined = types::DefinedType { .kind = kind, .nominal = nominal };
        defined.children.reserve(refs.size());
        for (auto const& ref: refs)
        {
            if (!ref)
            {
                defined.children.emplace_back();
                continue;
            }
            auto const child = valueType(*ref, position);
            if (!child)
                return child;
            defined.children.emplace_back(*child);
        }
        auto measure = TypeMeasure {};
        for (auto const& child: defined.children)
        {
            if (!child)
                continue;
            auto measured = combine(measure, _arena.measure(AnyTypeId { *child }), position);
            if (!measured)
                return std::unexpected { measured.error() };
            measure = *measured;
        }
        return intern(std::move(defined), position);
    }

    Result<TypeId> ComponentValidator::labelsOnly(ValueKind kind,
                                                  std::vector<std::string> const& labels,
                                                  uint32_t nominal,
                                                  DefinitionPosition position)
    {
        // create_flags_type and create_enum_type (component.rs:4307-4360): each label, then uniqueness.
        auto const labelKind = kind == ValueKind::Flags ? LabelKind::Flag : LabelKind::EnumTag;
        auto seen = LabelSet {};
        for (auto const& text: labels)
        {
            if (auto checked = label(labelKind, text, position); !checked)
                return std::unexpected { checked.error() };
            if (auto checked = unique(seen, labelKind, text, position); !checked)
                return std::unexpected { checked.error() };
        }
        return intern(types::DefinedType { .kind = kind,
                                           .labels = { labels.begin(), labels.end() },
                                           .nominal = nominal },
                      position);
    }

    Result<TypeId> ComponentValidator::map(binary::MapType const& type,
                                           uint32_t nominal,
                                           DefinitionPosition position)
    {
        // create_defined_type's Map arm (component.rs:4006-4058): the key must be a `keytype`, and the
        // value may hold no handle, as bindings may drop the values of duplicate keys.
        auto const key = valueType(type.key, position);
        if (!key)
            return key;
        if (!isKeyKind(_arena[*key].kind))
            return std::unexpected { plainError(ValidationErrorCode::InvalidMapKey, position) };
        auto const value = valueType(type.value, position);
        if (!value)
            return value;
        if (types::holds(_arena[*value].handles, types::HandleContent::Own | types::HandleContent::Borrow))
            return std::unexpected { plainError(ValidationErrorCode::HandleInMapValue, position) };
        return elements(ValueKind::Map, { type.key, type.value }, nominal, position);
    }

    Result<TypeId> ComponentValidator::handle(ValueKind kind,
                                              uint32_t resource,
                                              uint32_t nominal,
                                              DefinitionPosition position)
    {
        // The handle names the resource entry it was given, which the visibility rule reads.
        auto const entry = slotAt(resource, position);
        if (!entry)
            return std::unexpected { entry.error() };
        auto const* id = std::get_if<types::ResourceTypeId>(&entry->type);
        if (!id)
            return std::unexpected { indexError(ValidationErrorCode::NotResourceType, resource, position) };
        return intern(
            types::DefinedType {
                .kind = kind, .resource = *id, .nominal = nominal, .resourceAlias = entry->alias },
            position);
    }

    Result<void> ComponentValidator::functionType(binary::FunctionTypeDefinition const& type,
                                                  DefinitionPosition position)
    {
        // create_function_type (component.rs:3141-3199): the async gate, then per parameter its label,
        // its uniqueness and its type; then the result, which may hold no borrow.
        auto const form = std::ranges::find(
            binary::FunctionTypeForms, type.asyncness, &binary::FunctionTypeFormRow::asyncness);
        Require(form != binary::FunctionTypeForms.end());
        if (form->gate)
        {
            if (auto admitted = admit(GatedForm::AsyncFunction, position); !admitted)
                return admitted;
        }
        auto info = types::FunctionTypeInfo { .asyncness = type.asyncness };
        info.params.reserve(type.params.size());
        auto seen = LabelSet {};
        auto measure = TypeMeasure {};
        for (auto const& param: type.params)
        {
            if (auto checked = label(LabelKind::FunctionParameter, param.label, position); !checked)
                return checked;
            if (auto checked = unique(seen, LabelKind::FunctionParameter, param.label, position); !checked)
                return checked;
            auto const child = valueType(param.type, position);
            if (!child)
                return std::unexpected { child.error() };
            auto measured = combine(measure, _arena.measure(AnyTypeId { *child }), position);
            if (!measured)
                return std::unexpected { measured.error() };
            measure = *measured;
            info.params.emplace_back(param.label, *child);
        }
        if (type.result)
        {
            auto const result = valueType(*type.result, position);
            if (!result)
                return std::unexpected { result.error() };
            if (types::holds(_arena[*result].handles, types::HandleContent::Borrow))
                return std::unexpected { plainError(ValidationErrorCode::BorrowInResult, position) };
            if (auto measured = combine(measure, _arena.measure(AnyTypeId { *result }), position); !measured)
                return std::unexpected { measured.error() };
            info.result = *result;
        }
        auto const added = _arena.add(std::move(info));
        if (!added)
            return std::unexpected { limitError(added.error(), position) }; // uncovered(premeasured)
        return pushType(TypeIdentity { .type = *added }, position);
    }

    Result<void> ComponentValidator::resourceType(binary::ResourceTypeDefinition const& type,
                                                  DefinitionPosition position)
    {
        // add_type's Resource arm (component.rs:610-656): a concrete component's only; an i32 (i64 under
        // 🐘); a destructor of core type [rep] -> []; then a fresh resource the component defines.
        if (current().kind != binary::ScopeKind::Component)
            return std::unexpected { plainError(ValidationErrorCode::ResourceOutsideComponent, position) };
        if (type.rep == binary::CoreValueKind::I64
            && !validationAdmits(_features, ComponentFeature::Memory64))
            return std::unexpected { ValidationError { .code = ValidationErrorCode::ResourceRepresentationI64,
                                                       .position = position,
                                                       .disabled = ComponentFeature::Memory64 } };
        if (type.rep != binary::CoreValueKind::I32 && type.rep != binary::CoreValueKind::I64)
            return std::unexpected { plainError(ValidationErrorCode::InvalidResourceRepresentation,
                                                position) };
        if (type.destructor)
        {
            auto const destructor = coreFunctionAt(*type.destructor, position);
            if (!destructor)
                return std::unexpected { destructor.error() };
            auto const& signature = _arena[*destructor];
            if (signature.params != std::vector { coreValueOf(type.rep) } || !signature.results.empty())
                return std::unexpected { indexError(
                    ValidationErrorCode::DestructorSignature, *type.destructor, position) };
        }
        auto const resource =
            _arena.newResource(types::ResourceTypeInfo { .rep = type.rep, .destructor = type.destructor });
        if (!resource)
            return std::unexpected { limitError(resource.error(), position) };
        if (auto pushed = pushType(TypeIdentity { .type = *resource }, position); !pushed)
            return pushed;
        addLocalResource(*resource, type.rep);
        return {};
    }

    void ComponentValidator::addLocalResource(types::ResourceTypeId resource,
                                              std::optional<binary::CoreValueKind> rep)
    {
        auto& frame = current();
        if (frame.localResourceIndex.try_emplace(resource, frame.localResources.size()).second)
            frame.localResources.emplace_back(resource, rep);
    }

    Result<isoline::binary::ValueType> ComponentValidator::coreValueType(
        isoline::binary::ValueType type,
        std::span<CoreFunctionTypeId const> scope,
        DefinitionPosition position) const
    {
        // check_heap_type (core.rs:858-879): a concrete reference names a type of the scope, rewritten
        // to the arena's canonical id.
        if (type.kind != isoline::binary::ValueKind::Ref
            || type.heap.kind != isoline::binary::AbstractHeapType::Concrete)
            return type;
        if (type.heap.typeIndex >= scope.size())
            return std::unexpected { indexError(
                ValidationErrorCode::TypeIndexOutOfBounds, type.heap.typeIndex, position) };
        type.heap.typeIndex = scope[type.heap.typeIndex].value;
        return type;
    }

    Result<CoreFunctionTypeId> ComponentValidator::coreFunctionType(isoline::binary::FuncType type,
                                                                    std::span<CoreFunctionTypeId const> scope,
                                                                    DefinitionPosition position)
    {
        for (auto* list: { &type.params, &type.results })
        {
            for (auto& entry: *list)
            {
                auto const canonical = coreValueType(entry, scope, position);
                if (!canonical)
                    return std::unexpected { canonical.error() };
                entry = *canonical;
            }
        }
        return _arena.intern(std::move(type));
    }

    Result<CoreEntityType> ComponentValidator::declaredEntity(binary::CoreExternType const& type,
                                                              std::span<CoreFunctionTypeId const> scope,
                                                              DefinitionPosition position)
    {
        // A core module type's import or export (Module::check_type_ref, core.rs:680-930), its types
        // the declaration scope's, rewritten to canonical ids. The core features are the core
        // validator's: a declared type is checked by the rules every core feature set shares.
        using isoline::binary::ExternKind;
        auto entity = CoreEntityType { .kind = type.kind };
        auto const limitsOrdered = [&](isoline::binary::Limits const& limits) -> Result<void> {
            if (limits.max && limits.min > *limits.max)
                return std::unexpected { plainError(ValidationErrorCode::CoreLimitsOrder, position) };
            return {};
        };
        auto const functionType = [&](uint32_t index) -> Result<CoreFunctionTypeId> {
            if (index >= scope.size())
                return std::unexpected { indexError(
                    ValidationErrorCode::TypeIndexOutOfBounds, index, position) };
            return scope[index];
        };
        switch (type.kind)
        {
            case ExternKind::Func:
                return functionType(type.typeIndex).transform([&](CoreFunctionTypeId id) {
                    entity.typeIndex = id.value;
                    return entity;
                });
            case ExternKind::Tag: {
                auto const id = functionType(type.typeIndex);
                if (!id)
                    return std::unexpected { id.error() };
                if (!_arena[*id].results.empty())
                    return std::unexpected { plainError(ValidationErrorCode::CoreTagResults, position) };
                entity.typeIndex = id->value;
                return entity;
            }
            case ExternKind::Table: {
                auto const element = coreValueType(type.table.element, scope, position);
                if (!element)
                    return std::unexpected { element.error() };
                if (auto ordered = limitsOrdered(type.table.limits); !ordered)
                    return std::unexpected { ordered.error() };
                auto const largest = type.table.limits.addressType == isoline::binary::AddressType::I64
                                         ? ~uint64_t { 0 }
                                         : uint64_t { 0xFFFF'FFFF };
                if (type.table.limits.min > largest
                    || (type.table.limits.max && *type.table.limits.max > largest))
                    return std::unexpected { namedError(ValidationErrorCode::CoreTableTooLarge,
                                                        std::format("{:#x}", largest),
                                                        {},
                                                        position) };
                entity.table =
                    isoline::binary::TableType { .element = *element, .limits = type.table.limits };
                return entity;
            }
            case ExternKind::Memory: {
                if (auto ordered = limitsOrdered(type.memory.limits); !ordered)
                    return std::unexpected { ordered.error() };
                if (std::ranges::find(isoline::binary::ValidPageSizesLog2, type.memory.pageSizeLog2)
                    == isoline::binary::ValidPageSizesLog2.end())
                    return std::unexpected { plainError(ValidationErrorCode::InvalidPageSize, position) };
                auto const largest = isoline::binary::maxPages(type.memory);
                if (type.memory.limits.min > largest
                    || (type.memory.limits.max && *type.memory.limits.max > largest))
                    return std::unexpected { namedError(
                        ValidationErrorCode::CoreMemoryTooLarge,
                        std::format("{:#x}", largest),
                        std::to_string(isoline::binary::pageSize(type.memory)),
                        position) };
                entity.memory = type.memory;
                return entity;
            }
            case ExternKind::Global: {
                auto const value = coreValueType(type.global.type, scope, position);
                if (!value)
                    return std::unexpected { value.error() };
                entity.global =
                    isoline::binary::GlobalType { .type = *value, .mutability = type.global.mutability };
                return entity;
            }
        }
        Require(false);
        return entity; // uncovered(exhaustive-switch)
    }

    Result<CoreModuleTypeId> ComponentValidator::finishModuleType(types::CoreModuleTypeInfo type,
                                                                  DefinitionPosition position)
    {
        // A module's type size (Module::type_size, core.rs:598 and 630): 1, then each import's and
        // export's size, refused once it reaches maxTypeSize. Then imports_for_module_type (core.rs:
        // 824-843): every (module, name) once, the first repeated one named.
        auto size = uint64_t { 1 };
        auto const charge = [&](CoreEntityType const& entity) -> Result<void> {
            size += coreEntitySize(_arena, entity);
            if (size >= _limits.maxTypeSize)
                return std::unexpected { capError(ComponentLimit::TypeSize, _limits, position) };
            return {};
        };
        for (auto const& import: type.imports)
        {
            if (auto charged = charge(import.type); !charged)
                return std::unexpected { charged.error() };
        }
        for (auto const& entry: type.exports)
        {
            if (auto charged = charge(entry.type); !charged)
                return std::unexpected { charged.error() };
        }
        auto seen = LabelSet {};
        auto repeated = std::optional<std::size_t> {};
        auto firstOf = std::map<std::pair<std::string_view, std::string_view>, std::size_t> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, type.imports.size()))
        {
            auto const& import = type.imports[index];
            auto const [entry, inserted] =
                firstOf.try_emplace(std::pair { std::string_view { import.module.str() },
                                                std::string_view { import.name.str() } },
                                    index);
            if (!inserted && (!repeated || entry->second < *repeated))
                repeated = entry->second;
        }
        if (repeated)
            return std::unexpected { namedError(ValidationErrorCode::DuplicateCoreImport,
                                                type.imports[*repeated].module.str(),
                                                type.imports[*repeated].name.str(),
                                                position) };
        type.measure = TypeMeasure { .depth = 1, .effectiveSize = static_cast<uint32_t>(size) };
        return _arena.add(std::move(type));
    }

    Result<CoreModuleTypeId> ComponentValidator::moduleType(binary::CoreModuleTypeDefinition const& type,
                                                            DefinitionPosition position)
    {
        // create_module_type (component.rs:2987-3045): the declarations in order over a type index
        // space of the module type's own.
        auto scope = std::vector<CoreFunctionTypeId> {};
        auto info = types::CoreModuleTypeInfo {};
        auto exportNames = std::set<std::string_view> {};
        for (auto const& declaration: type.declarations)
        {
            auto declared = std::visit(
                [&](auto const& entry) -> Result<void> {
                    using Entry = std::remove_cvref_t<decltype(entry)>;
                    if constexpr (std::is_same_v<Entry, binary::CoreTypeDeclaration>)
                    {
                        if (scope.size() + 1 > _limits.maxTypes)
                            return std::unexpected { capError(ComponentLimit::Types, _limits, position) };
                        return coreFunctionType(entry.type, scope, position)
                            .transform([&](CoreFunctionTypeId id) { scope.push_back(id); });
                    }
                    else if constexpr (std::is_same_v<Entry, binary::CoreOuterAliasDeclaration>)
                    {
                        auto aliased = CoreFunctionTypeId {};
                        if (entry.outerCount == 0)
                        {
                            if (entry.outerIndex >= scope.size())
                                return std::unexpected { indexError(
                                    ValidationErrorCode::TypeIndexOutOfBounds, entry.outerIndex, position) };
                            aliased = scope[entry.outerIndex];
                        }
                        else
                        {
                            // The module type counts as a scope of its own: count 1 is this scope.
                            auto const target = outerFrame(entry.outerCount - 1, position);
                            if (!target)
                                return std::unexpected { target.error() };
                            auto const found = coreTypeAt(**target, entry.outerIndex, position);
                            if (!found)
                                return std::unexpected { found.error() };
                            auto const* function = std::get_if<CoreFunctionTypeId>(&*found);
                            if (!function)
                                return std::unexpected { plainError(
                                    ValidationErrorCode::CoreAliasOfModuleType, position) };
                            aliased = *function;
                        }
                        if (scope.size() + 1 > _limits.maxTypes)
                            return std::unexpected { capError(ComponentLimit::Types, _limits, position) };
                        scope.push_back(aliased);
                        return {};
                    }
                    else if constexpr (std::is_same_v<Entry, binary::CoreImportDeclaration>)
                        return declaredEntity(entry.type, scope, position)
                            .transform([&](CoreEntityType entity) {
                                info.imports.push_back(types::CoreImportType {
                                    .module = entry.module, .name = entry.name, .type = entity });
                            });
                    else
                    {
                        static_assert(std::is_same_v<Entry, binary::CoreExportDeclaration>,
                                      "every declaration has a case");
                        auto const entity = declaredEntity(entry.type, scope, position);
                        if (!entity)
                            return std::unexpected { entity.error() };
                        // add_export (core.rs:604-640): the size is charged before the name's uniqueness.
                        if (!exportNames.insert(entry.name).second)
                            return std::unexpected { namedError(
                                ValidationErrorCode::DuplicateCoreExport, entry.name, {}, position) };
                        info.exports.push_back(types::CoreExportType { .name = entry.name, .type = *entity });
                        return {};
                    }
                },
                declaration);
            if (!declared)
                return std::unexpected { declared.error() };
        }
        return finishModuleType(std::move(info), position);
    }

    Result<CoreModuleTypeId> ComponentValidator::embeddedModuleType(isoline::binary::Module const& module,
                                                                    DefinitionPosition position)
    {
        // add_core_module (component.rs:536-557): the module's imports and exports, its types interned
        // into the component's one canonical space (carried: once per module, never per query).
        using isoline::binary::ExternKind;
        auto scope = std::vector<CoreFunctionTypeId> {};
        scope.reserve(module.types.size());
        for (auto const& type: module.types)
        {
            auto const id = coreFunctionType(type, scope, position);
            Require(id.has_value()); // the core validator checked its references
            scope.push_back(*id);
        }
        auto const canonical = [&](isoline::validate::CoreExternType entity) {
            auto const rewrite = [&](isoline::binary::ValueType value) {
                auto const rewritten = coreValueType(value, scope, position);
                Require(rewritten.has_value());
                return *rewritten;
            };
            if (entity.kind == ExternKind::Func || entity.kind == ExternKind::Tag)
                entity.typeIndex = scope[entity.typeIndex].value;
            entity.table.element = rewrite(entity.table.element);
            entity.global.type = rewrite(entity.global.type);
            return entity;
        };
        auto info = types::CoreModuleTypeInfo {};
        info.imports.reserve(module.imports.size());
        for (auto const& import: module.imports)
            info.imports.push_back(types::CoreImportType {
                .module = std::string { isoline::binary::importModuleOf(module, import) },
                .name = import.name,
                .type = canonical(isoline::validate::externTypeOf(import)) });
        // Each index space: the imports of its kind first, then the module's own definitions.
        auto functions = std::vector<uint32_t> {};
        auto tables = std::vector<isoline::binary::TableType> {};
        auto memories = std::vector<isoline::binary::MemoryType> {};
        auto globals = std::vector<isoline::binary::GlobalType> {};
        auto tags = std::vector<uint32_t> {};
        for (auto const& import: module.imports)
        {
            switch (import.kind)
            {
                case ExternKind::Func: functions.push_back(import.typeIndex); break;
                case ExternKind::Table: tables.push_back(import.table); break;
                case ExternKind::Memory: memories.push_back(import.memory); break;
                case ExternKind::Global: globals.push_back(import.global); break;
                case ExternKind::Tag: tags.push_back(import.typeIndex); break;
            }
        }
        functions.insert(functions.end(), module.functions.begin(), module.functions.end());
        for (auto const& table: module.tables)
            tables.push_back(table.type);
        memories.insert(memories.end(), module.memories.begin(), module.memories.end());
        for (auto const& global: module.globals)
            globals.push_back(global.type);
        for (auto const& tag: module.tags)
            tags.push_back(tag.typeIndex);
        info.exports.reserve(module.exports.size());
        for (auto const& entry: module.exports)
        {
            auto entity = isoline::validate::CoreExternType { .kind = entry.kind };
            switch (entry.kind)
            {
                case ExternKind::Func: entity.typeIndex = functions.at(entry.index); break;
                case ExternKind::Table: entity.table = tables.at(entry.index); break;
                case ExternKind::Memory: entity.memory = memories.at(entry.index); break;
                case ExternKind::Global: entity.global = globals.at(entry.index); break;
                case ExternKind::Tag: entity.typeIndex = tags.at(entry.index); break;
            }
            info.exports.push_back(types::CoreExportType { .name = entry.name, .type = canonical(entity) });
        }
        return finishModuleType(std::move(info), position);
    }

    Result<void> ComponentValidator::coreModule(binary::CoreModuleDefinition const& module,
                                                DefinitionPosition position)
    {
        auto const& bytes = *_pool.bytes;
        Require(module.offset <= bytes.size() && module.size <= bytes.size() - module.offset);
        if (auto reserved = reserve(Sort::CoreModule, position); !reserved)
            return reserved;
        auto const ordinal = static_cast<uint32_t>(_coreModules.size());
        auto shape = _coreModuleValidator.validate(
            std::span<std::byte const> { bytes }.subspan(module.offset, module.size));
        if (!shape)
            return std::unexpected { ValidationError {
                .code = ValidationErrorCode::CoreModuleRefused,
                .position = position,
                .subject = ordinal,
                .coreModule = CoreModuleError { .ordinal = ordinal, .error = std::move(shape.error()) } } };
        auto const type = embeddedModuleType(shape->module->module(), position);
        if (!type)
            return std::unexpected { type.error() };
        shape->type = *type;
        current().spaces.coreModules.push_back(*type);
        _coreModules.push_back(std::move(*shape));
        return {};
    }

    Result<void> ComponentValidator::coreType(binary::CoreTypeDefinition const& definition,
                                              DefinitionPosition position)
    {
        // add_core_type (component.rs:510-534): a function type of the scope's core type space, or a
        // module type.
        if (auto reserved = reserve(Sort::CoreType, position); !reserved)
            return reserved;
        auto entry = std::visit(
            [&](auto const& type) -> Result<CoreTypeEntry> {
                using Type = std::remove_cvref_t<decltype(type)>;
                if constexpr (std::is_same_v<Type, isoline::binary::FuncType>)
                {
                    // The scope's earlier core types, of which a reference must name a function type.
                    for (auto const* list: { &type.params, &type.results })
                    {
                        for (auto const& ref: *list)
                        {
                            if (ref.kind != isoline::binary::ValueKind::Ref
                                || ref.heap.kind != isoline::binary::AbstractHeapType::Concrete)
                                continue;
                            auto const found = coreTypeAt(current(), ref.heap.typeIndex, position);
                            if (!found)
                                return std::unexpected { found.error() };
                            if (!std::holds_alternative<CoreFunctionTypeId>(*found))
                                return std::unexpected { indexError(ValidationErrorCode::ModuleTypeNotSubType,
                                                                    ref.heap.typeIndex,
                                                                    position) };
                        }
                    }
                    auto rewritten = type;
                    for (auto* list: { &rewritten.params, &rewritten.results })
                    {
                        for (auto& value: *list)
                        {
                            if (value.kind == isoline::binary::ValueKind::Ref
                                && value.heap.kind == isoline::binary::AbstractHeapType::Concrete)
                                value.heap.typeIndex = std::get<CoreFunctionTypeId>(
                                                           current().spaces.coreTypes[value.heap.typeIndex])
                                                           .value;
                        }
                    }
                    return CoreTypeEntry { _arena.intern(std::move(rewritten)) };
                }
                else
                    return moduleType(type, position).transform([](CoreModuleTypeId id) {
                        return CoreTypeEntry { id };
                    });
            },
            definition.type);
        if (!entry)
            return std::unexpected { entry.error() };
        current().spaces.coreTypes.push_back(*entry);
        return {};
    }

    Result<void> ComponentValidator::coreInstance(binary::CoreInstanceDefinition const& instance,
                                                  DefinitionPosition position)
    {
        // instance_section (validator.rs:1091-1115): room among the instances first.
        if (auto reserved = reserve(Sort::CoreInstance, position); !reserved)
            return reserved;
        auto const made = std::visit(
            [&](auto const& expression) -> Result<CoreInstanceTypeId> {
                using Expression = std::remove_cvref_t<decltype(expression)>;
                if constexpr (std::is_same_v<Expression, binary::CoreInstantiate>)
                    return coreInstantiate(expression, position);
                else
                    return coreInlineExports(expression, position);
            },
            instance.expression);
        if (!made)
            return std::unexpected { made.error() };
        current().spaces.coreInstances.push_back(*made);
        return {};
    }

    Result<CoreInstanceTypeId> ComponentValidator::coreInstantiate(binary::CoreInstantiate const& instantiate,
                                                                   DefinitionPosition position)
    {
        // instantiate_core_module (component.rs:3204-3279): each argument a core instance, each name
        // once; then every import of the module supplied by its module's argument, by an export of its
        // name that matches it in the component's canonical core types.
        auto const module = moduleAt(current(), instantiate.module, position);
        if (!module)
            return std::unexpected { module.error() };
        auto arguments = std::map<std::string_view, CoreInstanceTypeId> {};
        for (auto const& argument: instantiate.arguments)
        {
            auto const supplied = coreInstanceAt(argument.instance, position);
            if (!supplied)
                return std::unexpected { supplied.error() };
            if (!arguments.try_emplace(argument.name, *supplied).second)
                return std::unexpected { namedError(
                    ValidationErrorCode::DuplicateModuleArgument, argument.name, {}, position) };
        }
        auto measure = TypeMeasure {};
        auto const& type = _arena[*module];
        for (auto const& import: type.imports)
        {
            if (auto charged = chargeCoreMatching(
                    1 + nameSearchSteps(import.module.str().size(), arguments.size()), position);
                !charged)
                return std::unexpected { charged.error() };
            auto const argument = arguments.find(import.module.str());
            if (argument == arguments.end())
                return std::unexpected { namedError(
                    ValidationErrorCode::MissingModuleArgument, import.module.str(), {}, position) };
            if (auto charged = chargeCoreMatching(
                    nameSearchSteps(import.name.str().size(), _arena.exportsOf(argument->second).size()),
                    position);
                !charged)
                return std::unexpected { charged.error() };
            auto const* provided = _arena.findExport(argument->second, import.name.str());
            if (provided == nullptr)
                return std::unexpected { namedError(ValidationErrorCode::ArgumentMissingExport,
                                                    import.module.str(),
                                                    import.name.str(),
                                                    position) };
            auto const matched = matchCoreEntity(
                _arena, provided->type, _arena, import.type, [](uint32_t sub, uint32_t super) {
                    return sub == super;
                });
            if (!matched)
            {
                auto refused = namedError(ValidationErrorCode::CoreArgumentMismatch,
                                          import.module.str(),
                                          import.name.str(),
                                          position);
                refused.detail = matched.error().message;
                return std::unexpected { std::move(refused) };
            }
        }
        for (auto const& entry: type.exports)
        {
            if (auto charged = chargeCoreMatching(1, position); !charged)
                return std::unexpected { charged.error() };
            auto combined =
                combine(measure,
                        TypeMeasure { .depth = 1, .effectiveSize = coreEntitySize(_arena, entry.type) },
                        position);
            if (!combined)
                return std::unexpected { combined.error() };
            measure = *combined;
        }
        return _arena.add(types::CoreInstanceTypeInfo { .instantiated = *module, .measure = measure });
    }

    Result<CoreInstanceTypeId> ComponentValidator::coreInlineExports(binary::CoreInlineExports const& exports,
                                                                     DefinitionPosition position)
    {
        // instantiate_core_exports (component.rs:3633-3721): each export's size, then its name once.
        auto info = types::CoreInstanceTypeInfo {};
        auto names = std::set<std::string_view> {};
        for (auto const& entry: exports.exports)
        {
            if (auto charged =
                    chargeCoreMatching(1 + nameSearchSteps(entry.name.size(), names.size()), position);
                !charged)
                return std::unexpected { charged.error() };
            auto const entity = coreEntityAt(binary::sortOf(entry.sort), entry.index, position);
            if (!entity)
                return std::unexpected { entity.error() };
            auto combined =
                combine(info.measure,
                        TypeMeasure { .depth = 1, .effectiveSize = coreEntitySize(_arena, *entity) },
                        position);
            if (!combined)
                return std::unexpected { combined.error() };
            info.measure = *combined;
            if (!names.insert(entry.name).second)
                return std::unexpected { namedError(
                    ValidationErrorCode::DuplicateCoreInstanceExport, entry.name, {}, position) };
            info.exports.push_back(types::CoreExportType { .name = entry.name, .type = *entity });
        }
        return _arena.add(std::move(info));
    }

    Result<ExternType> ComponentValidator::checkTypeRef(binary::ExternDesc const& type,
                                                        DefinitionPosition position)
    {
        // check_type_ref (component.rs:2876-2933).
        auto const kindOf = [&](ValidationErrorCode mismatch, auto expected) -> Result<ExternType> {
            using Expected = decltype(expected);
            auto const entry = typeAt(type.index, position);
            if (!entry)
                return std::unexpected { entry.error() };
            if (auto const* id = std::get_if<Expected>(&*entry))
                return ExternType { *id };
            return std::unexpected { indexError(mismatch, type.index, position) };
        };
        switch (type.sort)
        {
            case Sort::CoreModule: {
                auto const entry = coreTypeAt(current(), type.index, position);
                if (!entry)
                    return std::unexpected { entry.error() };
                if (auto const* module = std::get_if<CoreModuleTypeId>(&*entry))
                    return ExternType { *module };
                return std::unexpected { indexError(
                    ValidationErrorCode::NotModuleType, type.index, position) };
            }
            case Sort::Func: return kindOf(ValidationErrorCode::NotFunctionType, types::FunctionTypeId {});
            case Sort::Component:
                return kindOf(ValidationErrorCode::NotComponentType, types::ComponentTypeId {});
            case Sort::Instance:
                return kindOf(ValidationErrorCode::NotInstanceType, types::InstanceTypeId {});
            case Sort::Type: {
                if (type.bound == binary::TypeBound::SubResource)
                {
                    auto const resource = _arena.newResource(types::ResourceTypeInfo {});
                    if (!resource)
                        return std::unexpected { limitError(resource.error(), position) };
                    return ExternType { types::TypeExtern { .type = *resource } };
                }
                auto const referenced = slotAt(type.index, position);
                if (!referenced)
                    return std::unexpected { referenced.error() };
                auto const created = withUnique(*referenced);
                return ExternType { types::TypeExtern {
                    .type = created.type, .alias = created.alias, .referenced = *referenced } };
            }
            case Sort::Value: {
                // The decoder refuses a value extern type while 🪙 is refused; a built pool reaches here.
                auto const admitted = admit(GatedForm::Value, position);
                Require(!admitted.has_value());
                return std::unexpected { admitted.error() };
            }
            default: break; // uncovered(core-sort-extern)
        }
        // An extern type names no other core sort (binary::ExternalKinds).
        Require(false);
        return std::unexpected { plainError(ValidationErrorCode::NotModuleType,
                                            position) }; // uncovered(core-sort-extern)
    }

    Result<void> ComponentValidator::addEntity(ExternType& type,
                                               std::optional<std::pair<std::string const*, ExternKind>> named,
                                               DefinitionPosition position)
    {
        // add_entity (component.rs:697-814): the entry it adds to its sort's index space, an imported or
        // exported instance's resources made fresh, a type import's or export's own resource bound; then,
        // for a named extern, the rule that it names only nameable types.
        auto const kind = named ? std::optional<ExternKind> { named->second } : std::nullopt;
        auto added = std::visit(
            [&](auto const& entity) -> Result<void> {
                using Entity = std::remove_cvref_t<decltype(entity)>;
                auto& frame = current();
                if constexpr (std::is_same_v<Entity, CoreModuleTypeId>)
                    return reserve(Sort::CoreModule, position).transform([&] {
                        frame.spaces.coreModules.push_back(entity);
                    });
                else if constexpr (std::is_same_v<Entity, types::ComponentTypeId>)
                    return reserve(Sort::Component, position).transform([&] {
                        frame.spaces.components.push_back(entity);
                    });
                else if constexpr (std::is_same_v<Entity, types::FunctionTypeId>)
                    return reserve(Sort::Func, position).transform([&] {
                        frame.spaces.functions.push_back(entity);
                    });
                else if constexpr (std::is_same_v<Entity, types::InstanceTypeId>)
                {
                    auto instance = entity;
                    if (kind)
                    {
                        auto const externIndex = static_cast<uint32_t>(
                            (*kind == ExternKind::Import ? frame.imports : frame.exports).size());
                        auto fresh = freshen(entity, *kind, externIndex, position);
                        if (!fresh)
                            return std::unexpected { fresh.error() };
                        instance = std::get<types::InstanceTypeId>(*fresh);
                        type = *fresh;
                    }
                    return reserve(Sort::Instance, position).transform([&] {
                        current().spaces.instances.push_back(instance);
                    });
                }
                else if constexpr (std::is_same_v<Entity, types::TypeExtern>)
                {
                    if (auto pushed = pushType(entity.created(), position); !pushed)
                        return pushed;
                    // A `(sub resource)` introduces its resource where it is imported or exported.
                    auto const* resource = std::get_if<types::ResourceTypeId>(&entity.type);
                    if (resource == nullptr || entity.referenced || !kind)
                        return {};
                    if (*kind == ExternKind::Import)
                        frame.importedResources.push_back(types::ResourcePath {
                            .resource = *resource, .path = { static_cast<uint32_t>(frame.imports.size()) } });
                    else
                    {
                        if (frame.kind != binary::ScopeKind::Component)
                            frame.definedResources.push_back(types::ResourcePath {
                                .resource = *resource,
                                .path = { static_cast<uint32_t>(frame.exports.size()) } });
                        addLocalResource(*resource, std::nullopt);
                    }
                    return {};
                }
                else
                {
                    // 🪙's value sort is refused before an entity of it is made.
                    static_assert(std::is_same_v<Entity, types::ValueExtern>, "every extern type has a case");
                    Require(false);
                    return {}; // uncovered(value-entity)
                }
            },
            type);
        if (!added)
            return added;
        if (!named || registerNamed(named->first, named->second, type))
            return {};
        return std::unexpected { namedError(ValidationErrorCode::NotValidAs,
                                            std::string { EntityWords[type.index()] },
                                            std::string { externKindWord(named->second) },
                                            position) };
    }

    Result<ExternType> ComponentValidator::freshen(types::InstanceTypeId instance,
                                                   ExternKind kind,
                                                   uint32_t externIndex,
                                                   DefinitionPosition position)
    {
        // prepare_instance_import and prepare_instance_export (component.rs:1060-1168): the resources an
        // instance type defines are minted afresh for each import or export of an instance of it, and
        // become the scope's own, imported or defined, at their paths below this extern; the instance
        // itself then defines none.
        auto const defined = _arena[instance].definedResources;
        if (defined.empty())
            return ExternType { instance };
        auto fresh = types::ResourceSubstitution {};
        for (auto const& [resource, path]: defined)
        {
            auto const minted = _arena.newResource(_arena[resource]);
            if (!minted)
                return std::unexpected { limitError(minted.error(), position) };
            fresh.bind(resource, *minted);
            auto prefixed = types::ResourcePath { .resource = *minted, .path = { externIndex } };
            prefixed.path.insert(prefixed.path.end(), path.begin(), path.end());
            if (kind == ExternKind::Import)
                current().importedResources.push_back(std::move(prefixed));
            else
            {
                current().definedResources.push_back(std::move(prefixed));
                addLocalResource(*minted, std::nullopt);
            }
        }
        auto const added = _arena.substitutedInstance(_arena[instance].exports, fresh);
        if (!added)
            return std::unexpected { limitError(added.error(), position) };
        return ExternType { *added };
    }

    Result<void> ComponentValidator::declare(binary::NameAttributes const& name,
                                             binary::ExternDesc const& type,
                                             ExternKind kind,
                                             DefinitionPosition position)
    {
        // add_import and add_export (component.rs:670-695, 1170-1199): an export's room first; the type
        // (check_type_ref); its entry (add_entity); then the name (validate_extern), then the measure.
        if (kind == ExternKind::Export && current().exports.size() + 1 > _limits.maxExports)
            return std::unexpected { capError(ComponentLimit::Exports, _limits, position) };
        auto declared = checkTypeRef(type, position);
        if (!declared)
            return std::unexpected { declared.error() };
        if (auto added = addEntity(*declared, std::pair { &name.name, kind }, position); !added)
            return added;
        auto& frame = current();
        auto const& resources =
            kind == ExternKind::Import ? frame.importedResourceNames : frame.exportedResourceNames;
        auto& names = kind == ExternKind::Import ? frame.importNames : frame.exportNames;
        if (auto named = this->name(name, *declared, kind, resources, names, position); !named)
            return named;
        auto measured = combine(frame.measure, _arena.measure(*declared), position);
        if (!measured)
            return std::unexpected { measured.error() };
        frame.measure = *measured;
        auto& externs = kind == ExternKind::Import ? frame.imports : frame.exports;
        externs.push_back(types::NamedExtern { .name = name.name, .type = *declared });
        return {};
    }

    Result<ExternType> ComponentValidator::itemEntity(binary::SortIndex const& item,
                                                      TypeCopy copy,
                                                      DefinitionPosition position)
    {
        // An indexed item as an extern (export_to_entity_type, component.rs:2936-2985; the arguments of
        // instantiate_component and the exports of instantiate_component_exports).
        auto& frame = current();
        switch (item.sort)
        {
            case Sort::CoreModule:
                return moduleAt(frame, item.index, position).transform([](CoreModuleTypeId id) {
                    return ExternType { id };
                });
            case Sort::Func:
                return functionAt(item.index, position).transform([](types::FunctionTypeId id) {
                    return ExternType { id };
                });
            case Sort::Component:
                return componentAt(frame, item.index, position).transform([](types::ComponentTypeId id) {
                    return ExternType { id };
                });
            case Sort::Instance:
                return instanceAt(item.index, position).transform([](types::InstanceTypeId id) {
                    return ExternType { id };
                });
            case Sort::Type: {
                auto const referenced = slotAt(item.index, position);
                if (!referenced)
                    return std::unexpected { referenced.error() };
                if (copy == TypeCopy::Shared)
                    return ExternType { types::TypeExtern { .type = referenced->type,
                                                            .alias = referenced->alias } };
                auto const created = withUnique(*referenced);
                return ExternType { types::TypeExtern {
                    .type = created.type, .alias = created.alias, .referenced = *referenced } };
            }
            case Sort::Value: {
                auto const admitted = admit(GatedForm::Value, position);
                Require(!admitted.has_value());
                return std::unexpected { admitted.error() };
            }
            default: break; // uncovered(core-sort-item)
        }
        // A sortidx names no other core sort (binary::ExternalKinds).
        Require(false);
        return std::unexpected { plainError(ValidationErrorCode::NotModuleType,
                                            position) }; // uncovered(core-sort-item)
    }

    Result<void> ComponentValidator::exportDefinition(binary::ExportDefinition const& definition,
                                                      DefinitionPosition position)
    {
        // component_export_section (validator.rs:1363-1392): room among the exports, the item's type, an
        // ascribed type it must be a subtype of, which it takes; then add_export.
        if (current().exports.size() + 1 > _limits.maxExports)
            return std::unexpected { capError(ComponentLimit::Exports, _limits, position) };
        auto exported = itemEntity(definition.item, TypeCopy::Fresh, position);
        if (!exported)
            return std::unexpected { exported.error() };
        if (definition.type)
        {
            auto ascribed = checkTypeRef(*definition.type, position);
            if (!ascribed)
                return std::unexpected { ascribed.error() };
            if (auto related = _relation.isSubtype(*exported, *ascribed); !related)
                return std::unexpected { relationRefusal(
                    std::move(related.error()), ValidationErrorCode::AscriptionMismatch, position) };
            exported = std::move(ascribed);
        }
        if (auto added =
                addEntity(*exported, std::pair { &definition.name.name, ExternKind::Export }, position);
            !added)
            return added;
        auto& frame = current();
        if (auto named = name(definition.name,
                              *exported,
                              ExternKind::Export,
                              frame.exportedResourceNames,
                              frame.exportNames,
                              position);
            !named)
            return named;
        auto measured = combine(frame.measure, _arena.measure(*exported), position);
        if (!measured)
            return std::unexpected { measured.error() };
        frame.measure = *measured;
        frame.exports.push_back(types::NamedExtern { .name = definition.name.name, .type = *exported });
        return {};
    }

    Result<void> ComponentValidator::attributes(binary::NameAttributes const& name,
                                                DefinitionPosition position) const
    {
        // validate_extern (component.rs:4709-4790) gates each attribute, in its own order.
        for (auto const alternative: AttributeCheckOrder)
        {
            if (std::ranges::none_of(name.attributes, [&](binary::Attribute const& attribute) {
                    return attribute.index() == alternative;
                }))
                continue;
            if (auto admitted = admit(AttributeGates[alternative], position); !admitted)
                return admitted;
        }
        return {};
    }

    Result<void> ComponentValidator::name(binary::NameAttributes const& name,
                                          ExternType const& type,
                                          ExternKind kind,
                                          ResourceNames const& resources,
                                          LabelSet& names,
                                          DefinitionPosition position)
    {
        // validate_extern (component.rs:4709-4880): the grammar; the attributes, in wasm-tools' order,
        // with implements' rules; the name's own rules (an interface's version, a resource function's
        // signature); strong uniqueness among the scope's imports, or its exports. Exact-string
        // uniqueness, which wasm-tools checks last, cannot fail once strong uniqueness holds: equal names
        // have equal keys.
        auto parsed = parseExternName(name.name, _features);
        if (!parsed && parsed.error().code != NameErrorCode::InvalidVersion)
        {
            auto refused = externError(ValidationErrorCode::InvalidExternName,
                                       kind,
                                       name.name,
                                       nameErrorMessage(parsed.error()),
                                       position);
            refused.disabled = parsed.error().disabled;
            return std::unexpected { std::move(refused) };
        }
        auto const grammatical = parsed ? std::optional<ExternName> { *parsed } : std::nullopt;
        for (auto const alternative: AttributeCheckOrder)
        {
            if (std::ranges::none_of(name.attributes, [&](binary::Attribute const& attribute) {
                    return attribute.index() == alternative;
                }))
                continue;
            if (auto admitted = admit(AttributeGates[alternative], position); !admitted)
                return admitted;
            if (alternative == ImplementsAlternative)
            {
                if (auto checked = implements(name, grammatical, type, position); !checked)
                    return checked;
            }
        }
        if (!parsed)
            return std::unexpected { externError(ValidationErrorCode::InvalidExternNameUse,
                                                 kind,
                                                 name.name,
                                                 nameErrorMessage(parsed.error()),
                                                 position) };
        if (auto broken = resourceFunctionRule(*parsed, type, resources))
            return std::unexpected { externError(
                ValidationErrorCode::InvalidExternNameUse, kind, name.name, std::move(*broken), position) };
        if (auto previous = names.insertKeyed(strongUniquenessKey(*parsed), name.name))
            return std::unexpected { externError(ValidationErrorCode::ConflictingExternName,
                                                 kind,
                                                 name.name,
                                                 std::move(*previous),
                                                 position) };
        return {};
    }

    std::optional<std::string> ComponentValidator::resourceFunctionRule(ExternName const& parsed,
                                                                        ExternType const& type,
                                                                        ResourceNames const& resources) const
    {
        // ComponentNameContext::validate's resource function arms and validate_resource_name
        // (component.rs:4885-5090): a function, whose signature names the resource its name does, as
        // the scope's imports or exports have named it.
        if (parsed.kind != NameKind::Constructor && parsed.kind != NameKind::Method
            && parsed.kind != NameKind::Static)
            return std::nullopt;
        auto const* function = std::get_if<types::FunctionTypeId>(&type);
        if (function == nullptr)
            return ruleText(ResourceNameRule::NotAFunction);
        auto const& signature = _arena[*function];
        auto const resourceNamed = [&](types::DefinedType const& handle) -> std::optional<std::string> {
            auto const* bound =
                resources.nameOf(TypeIdentity { .type = *handle.resource, .alias = handle.resourceAlias });
            if (bound == nullptr)
                return ruleText(ResourceNameRule::ResourceUnnamed);
            if (*bound != parsed.resource)
                return ruleText(ResourceNameRule::ResourceNameMismatch, *bound);
            return std::nullopt;
        };
        switch (parsed.kind)
        {
            case NameKind::Constructor: {
                // An async constructor is refused with its function type, while 🔀 is.
                if (!signature.result)
                    return ruleText(ResourceNameRule::ConstructorWithoutResult);
                auto const* result = &_arena[*signature.result];
                if (result->kind == ValueKind::Result && !result->children.empty()
                    && result->children.front())
                    result = &_arena[*result->children.front()];
                if (result->kind != ValueKind::Own)
                    return ruleText(ResourceNameRule::ConstructorResultNotOwn);
                return resourceNamed(*result);
            }
            case NameKind::Method: {
                if (signature.params.empty())
                    return ruleText(ResourceNameRule::MethodWithoutParameters);
                auto const& [label, self] = signature.params.front();
                if (label.str() != "self")
                    return ruleText(ResourceNameRule::MethodFirstNotSelf);
                auto const& handle = _arena[self];
                if (handle.kind != ValueKind::Borrow)
                    return ruleText(ResourceNameRule::MethodSelfNotBorrow);
                return resourceNamed(handle);
            }
            default:
                if (!resources.knows(parsed.resource))
                    return ruleText(ResourceNameRule::StaticResourceUnknown);
                return std::nullopt;
        }
    }

    Result<void> ComponentValidator::implements(binary::NameAttributes const& name,
                                                std::optional<ExternName> const& parsed,
                                                ExternType const& type,
                                                DefinitionPosition position) const
    {
        // validate_extern's implements arm (component.rs:4757-4783): a bare plain name, an instance,
        // and an interface name implemented, its version valid. A name whose version is invalid is an
        // interface name, so not plain.
        if (!parsed || parsed->kind != NameKind::Plain)
            return std::unexpected { namedError(
                ValidationErrorCode::ImplementsNameNotPlain, name.name, {}, position) };
        if (!std::holds_alternative<types::InstanceTypeId>(type))
            return std::unexpected { namedError(
                ValidationErrorCode::ImplementsNotInstance, name.name, {}, position) };
        auto const implemented =
            std::ranges::find_if(name.attributes, [](binary::Attribute const& attribute) {
                return std::holds_alternative<binary::Implements>(attribute);
            });
        Require(implemented != name.attributes.end());
        auto const& interfaceName = std::get<binary::Implements>(*implemented).interfaceName;
        auto const interface = parseExternName(interfaceName, _features);
        if (!interface && interface.error().code != NameErrorCode::InvalidVersion)
            return std::unexpected { namedError(ValidationErrorCode::InvalidImplementsName,
                                                interfaceName,
                                                nameErrorMessage(interface.error()),
                                                position) };
        if (interface && interface->kind != NameKind::Interface)
            return std::unexpected { namedError(
                ValidationErrorCode::ImplementsNotInterface, interfaceName, {}, position) };
        if (!interface)
            return std::unexpected { namedError(ValidationErrorCode::InvalidImplementsVersion,
                                                interfaceName,
                                                nameErrorMessage(interface.error()),
                                                position) };
        return {};
    }

    bool ComponentValidator::typeNamed(TypeId type, std::set<TypeIdentity> const& named) const
    {
        // type_named_valtype and type_named_type_id (component_types.rs:3108-3165): a record, variant,
        // flags or enum must be a named entry itself; a handle's resource must be; any other compound
        // is anonymous, and its children must be named. An explicit stack; a type met twice in one
        // query is checked once.
        auto pending = std::vector<TypeId> { type };
        auto visited = std::set<TypeId> {};
        while (!pending.empty())
        {
            auto const current = pending.back();
            pending.pop_back();
            if (!visited.insert(current).second)
                continue;
            auto const& defined = _arena[current];
            switch (defined.kind)
            {
                case ValueKind::Record:
                case ValueKind::Variant:
                case ValueKind::Flags:
                case ValueKind::Enum:
                    if (!named.contains(TypeIdentity { .type = current }))
                        return false;
                    break;
                case ValueKind::Own:
                case ValueKind::Borrow:
                    if (!named.contains(
                            TypeIdentity { .type = *defined.resource, .alias = defined.resourceAlias }))
                        return false;
                    break;
                default:
                    for (auto const& child: defined.children)
                    {
                        if (child)
                            pending.push_back(*child);
                    }
                    break;
            }
        }
        return true;
    }

    bool ComponentValidator::definedNamed(TypeId type, std::set<TypeIdentity> const& named) const
    {
        // all_valtypes_named_in_defined (component.rs:979-1031): what a defined type names directly must
        // be named; the type itself need not be.
        auto const& defined = _arena[type];
        switch (defined.kind)
        {
            case ValueKind::Flags:
            case ValueKind::Enum: return true;
            case ValueKind::Own:
            case ValueKind::Borrow:
                return named.contains(
                    TypeIdentity { .type = *defined.resource, .alias = defined.resourceAlias });
            default:
                return std::ranges::all_of(defined.children, [&](std::optional<TypeId> const& child) {
                    return !child || typeNamed(*child, named);
                });
        }
    }

    bool ComponentValidator::functionNamed(types::FunctionTypeId function,
                                           std::set<TypeIdentity> const& named) const
    {
        // all_valtypes_named_in_func (component.rs:1033-1045).
        auto const& signature = _arena[function];
        return std::ranges::all_of(signature.params,
                                   [&](auto const& param) { return typeNamed(param.second, named); })
               && (!signature.result || typeNamed(*signature.result, named));
    }

    bool ComponentValidator::instanceNamed(types::InstanceTypeId instance,
                                           std::set<TypeIdentity>& named) const
    {
        // all_valtypes_named_in_instance (component.rs:941-977): the types an instance type exports are
        // named within it, so they join @p named while it is checked and leave it after. An explicit
        // stack of the instance types open.
        struct Open
        {
            types::InstanceTypeId instance;
            std::size_t next = 0;
            std::vector<TypeIdentity> added {};
            /// The entry the instance was exported as, which its parent names once it is checked.
            std::optional<TypeIdentity> exportedAs {};
        };
        auto stack = std::vector<Open> { Open { .instance = instance } };
        auto const unwind = [&] {
            for (auto const& open: stack)
            {
                for (auto const& entry: open.added)
                    named.erase(entry);
            }
        };
        while (!stack.empty())
        {
            auto& top = stack.back();
            auto const& exports = _arena[top.instance].exports;
            if (top.next == exports.size())
            {
                for (auto const& entry: top.added)
                    named.erase(entry);
                auto const exportedAs = top.exportedAs;
                stack.pop_back();
                if (exportedAs && !stack.empty() && named.insert(*exportedAs).second)
                    stack.back().added.push_back(*exportedAs);
                continue;
            }
            auto const& entry = exports[top.next++].type;
            auto isNamed = true;
            if (auto const* function = std::get_if<types::FunctionTypeId>(&entry))
                isNamed = functionNamed(*function, named);
            else if (auto const* value = std::get_if<types::ValueExtern>(&entry))
                isNamed = definedNamed(value->type, named);
            else if (auto const* nested = std::get_if<types::InstanceTypeId>(&entry))
            {
                stack.push_back(Open { .instance = *nested });
                continue;
            }
            else if (auto const* bound = std::get_if<types::TypeExtern>(&entry))
            {
                auto const created = bound->created();
                if (auto const* inner = std::get_if<types::InstanceTypeId>(&created.type))
                {
                    stack.push_back(Open { .instance = *inner, .exportedAs = created });
                    continue;
                }
                if (auto const* createdValue = std::get_if<TypeId>(&created.type))
                    isNamed = definedNamed(*createdValue, named);
                else if (auto const* createdFunction = std::get_if<types::FunctionTypeId>(&created.type))
                    isNamed = functionNamed(*createdFunction, named);
                if (isNamed && named.insert(created).second)
                    top.added.push_back(created);
            }
            if (!isNamed)
            {
                unwind();
                return false;
            }
        }
        return true;
    }

    bool ComponentValidator::anyNamed(TypeIdentity type, std::set<TypeIdentity>& named) const
    {
        // all_valtypes_named (component.rs:915-939): a resource or a component type is always nameable.
        if (auto const* value = std::get_if<TypeId>(&type.type))
            return definedNamed(*value, named);
        if (auto const* function = std::get_if<types::FunctionTypeId>(&type.type))
            return functionNamed(*function, named);
        if (auto const* instance = std::get_if<types::InstanceTypeId>(&type.type))
            return instanceNamed(*instance, named);
        return true;
    }

    bool ComponentValidator::registerNamed(std::string const* name, ExternKind kind, ExternType const& type)
    {
        // validate_and_register_named_types (component.rs:816-913): a top-level resource names itself for
        // the resource function rules; then, outside an instance type, every type an import or export
        // names must be nameable by it, and the types it binds become nameable. An imported or exported
        // instance does this for each of its exports, in order, on an explicit stack.
        auto& frame = current();
        if (auto const* bound = std::get_if<types::TypeExtern>(&type);
            bound != nullptr && name != nullptr && std::holds_alternative<types::ResourceTypeId>(bound->type))
            (kind == ExternKind::Import ? frame.importedResourceNames : frame.exportedResourceNames)
                .add(*name, bound->created());
        if (frame.kind == binary::ScopeKind::InstanceType)
            return true;
        auto& named = kind == ExternKind::Import ? frame.importedTypes : frame.exportedTypes;
        auto const visit = [&](ExternType const& entity,
                               std::vector<std::pair<types::InstanceTypeId, std::size_t>>& open) {
            if (auto const* bound = std::get_if<types::TypeExtern>(&entity))
            {
                auto const referenced = bound->referencedIdentity();
                if (kind == ExternKind::Import && frame.exportedTypes.contains(referenced)
                    && !frame.importedTypes.contains(referenced))
                    return false;
                auto& nameable = frame.nameableInstances[static_cast<std::size_t>(kind)];
                auto const* instance = std::get_if<types::InstanceTypeId>(&referenced.type);
                if (instance == nullptr || !nameable.contains(*instance))
                {
                    if (!anyNamed(referenced, named))
                        return false;
                    if (instance != nullptr)
                        nameable.insert(*instance);
                }
                frame.exportedTypes.insert(bound->created());
                if (kind == ExternKind::Import)
                    frame.importedTypes.insert(bound->created());
                return true;
            }
            if (auto const* instance = std::get_if<types::InstanceTypeId>(&entity))
            {
                if (frame.registeredInstances[static_cast<std::size_t>(kind)].insert(*instance).second)
                    open.emplace_back(*instance, 0);
                return true;
            }
            if (auto const* function = std::get_if<types::FunctionTypeId>(&entity))
                return functionNamed(*function, named);
            // A component or a module is closed; a value is refused before it is named.
            return true;
        };
        auto open = std::vector<std::pair<types::InstanceTypeId, std::size_t>> {};
        if (!visit(type, open))
            return false;
        while (!open.empty())
        {
            auto& [instance, next] = open.back();
            auto const& exports = _arena[instance].exports;
            if (next == exports.size())
            {
                open.pop_back();
                continue;
            }
            auto const& entity = exports[next++].type;
            if (!visit(entity, open)) // may grow open
                return false;
        }
        return true;
    }

    /// One binding scope of a free resource walk: the component or instance type it walks (none for
    /// the roots), what is left to walk under it, the resources found, and those it binds itself.
    struct FreeScope
    {
        std::vector<AnyTypeId> pending {};
        std::set<types::ResourceTypeId> found {};
        std::vector<types::ResourceTypeId> bound {};
        std::set<AnyTypeId> visited {};
        std::optional<AnyTypeId> type {};
    };

    /// Adds the types @p externs name to what @p scope walks.
    void pushExterns(FreeScope& scope, std::vector<types::NamedExtern> const& externs)
    {
        for (auto const& entry: externs)
        {
            std::visit(
                [&](auto const& alternative) {
                    using Alternative = std::remove_cvref_t<decltype(alternative)>;
                    if constexpr (std::is_same_v<Alternative, types::TypeExtern>)
                        scope.pending.push_back(alternative.type);
                    else if constexpr (std::is_same_v<Alternative, types::ValueExtern>)
                        scope.pending.emplace_back(alternative.type);
                    else if constexpr (std::is_constructible_v<AnyTypeId, Alternative>)
                        scope.pending.emplace_back(alternative);
                },
                entry.type);
        }
    }

    /// Walks one type of the top scope of @p stack: a resource is found, a value or function type's
    /// children join the scope, a component or instance type opens a scope of its own.
    void expandFree(types::TypeArena const& arena, std::vector<FreeScope>& stack, AnyTypeId type)
    {
        auto& top = stack.back();
        if (auto const* resource = std::get_if<types::ResourceTypeId>(&type))
            top.found.insert(*resource);
        else if (auto const* value = std::get_if<TypeId>(&type))
        {
            auto const& defined = arena[*value];
            if (defined.resource)
                top.found.insert(*defined.resource);
            for (auto const& child: defined.children)
            {
                if (child)
                    top.pending.emplace_back(*child);
            }
        }
        else if (auto const* function = std::get_if<types::FunctionTypeId>(&type))
        {
            auto const& signature = arena[*function];
            for (auto const& [label, param]: signature.params)
                top.pending.emplace_back(param);
            if (signature.result)
                top.pending.emplace_back(*signature.result);
        }
        else if (auto const* instance = std::get_if<types::InstanceTypeId>(&type))
        {
            auto const& info = arena[*instance];
            auto scope = FreeScope { .type = type };
            pushExterns(scope, info.exports);
            for (auto const& defined: info.definedResources)
                scope.bound.push_back(defined.resource);
            stack.push_back(std::move(scope)); // invalidates top
        }
        else
        {
            auto const& info = arena[std::get<types::ComponentTypeId>(type)];
            auto scope = FreeScope { .type = type };
            pushExterns(scope, info.imports);
            pushExterns(scope, info.exports);
            for (auto const& imported: info.importedResources)
                scope.bound.push_back(imported.resource);
            for (auto const& defined: info.definedResources)
                scope.bound.push_back(defined.resource);
            stack.push_back(std::move(scope)); // invalidates top
        }
    }

    std::set<types::ResourceTypeId> ComponentValidator::freeResources(std::span<AnyTypeId const> roots)
    {
        // free_variables_* (component_types.rs:2855-3031): the resources the types name, less those a
        // component or instance type binds itself. An explicit stack of the binding types open, each
        // collecting its own; a type met twice under one of them is walked once, a type naming no
        // resource or found closed (binding all it names) not at all, so a closed type shared by many
        // definitions is walked once per validation.
        auto stack = std::vector<FreeScope> { FreeScope { .pending = { roots.begin(), roots.end() } } };
        while (true)
        {
            auto& top = stack.back();
            if (top.pending.empty())
            {
                for (auto const resource: top.bound)
                    top.found.erase(resource);
                if (top.type && top.found.empty())
                    _closedTypes.insert(*top.type);
                if (stack.size() == 1)
                    return std::move(top.found);
                auto found = std::move(top.found);
                stack.pop_back();
                stack.back().found.merge(found);
                continue;
            }
            auto const type = top.pending.back();
            top.pending.pop_back();
            ++_work.resourceWalkSteps;
            if (!_arena.namesResource(type) || _closedTypes.contains(type)
                || !top.visited.insert(type).second)
                continue;
            expandFree(_arena, stack, type); // may invalidate top
        }
    }

    std::map<types::ResourceTypeId, std::vector<uint32_t>> ComponentValidator::explicitResourcePaths(
        std::vector<types::NamedExtern> const& exports, std::set<types::ResourceTypeId> remaining) const
    {
        // explicit_resource_paths (component_types.rs:3034-3106): where each resource is exported as a
        // type, directly or inside an exported instance, by the first path found; each instance type is
        // searched once.
        auto paths = std::map<types::ResourceTypeId, std::vector<uint32_t>> {};
        auto visited = std::set<types::InstanceTypeId> {};
        struct Open
        {
            std::vector<types::NamedExtern> const* exports;
            std::size_t next = 0;
        };
        auto stack = std::vector<Open> { Open { .exports = &exports } };
        auto path = std::vector<uint32_t> {};
        while (!stack.empty() && !remaining.empty())
        {
            auto& top = stack.back();
            if (top.next == top.exports->size())
            {
                stack.pop_back();
                if (!path.empty())
                    path.pop_back();
                continue;
            }
            auto const index = top.next++;
            auto const& entity = (*top.exports)[index].type;
            if (auto const* bound = std::get_if<types::TypeExtern>(&entity))
            {
                auto const* resource = std::get_if<types::ResourceTypeId>(&bound->type);
                if (resource != nullptr && remaining.erase(*resource) > 0)
                {
                    auto found = path;
                    found.push_back(static_cast<uint32_t>(index));
                    paths.emplace(*resource, std::move(found));
                }
            }
            else if (auto const* instance = std::get_if<types::InstanceTypeId>(&entity))
            {
                if (visited.insert(*instance).second)
                {
                    path.push_back(static_cast<uint32_t>(index));
                    stack.push_back(Open { .exports = &_arena[*instance].exports }); // invalidates top
                }
            }
        }
        return paths;
    }

    Result<void> ComponentValidator::alias(binary::AliasDefinition const& alias, DefinitionPosition position)
    {
        // add_alias (component.rs:2535-2592): a type scope aliases only types and instances.
        if (current().kind != binary::ScopeKind::Component)
        {
            auto const isAllowed = (alias.target == binary::AliasTarget::Export
                                    && (alias.sort == Sort::Type || alias.sort == Sort::Instance))
                                   || (alias.target == binary::AliasTarget::Outer
                                       && (alias.sort == Sort::Type || alias.sort == Sort::CoreType));
            if (!isAllowed)
                return std::unexpected { plainError(ValidationErrorCode::AliasInTypeScope, position) };
        }
        switch (alias.target)
        {
            case binary::AliasTarget::Export: return exportAlias(alias, position);
            case binary::AliasTarget::CoreExport: return coreExportAlias(alias, position);
            case binary::AliasTarget::Outer: return outerAlias(alias, position);
        }
        Require(false);
        return {}; // uncovered(exhaustive-switch)
    }

    Result<void> ComponentValidator::chargeCoreMatching(uint64_t steps, DefinitionPosition position)
    {
        // Isoline's own bound: wasm-tools matches every core instantiation without one.
        _work.coreMatchingSteps += steps;
        if (_work.coreMatchingSteps > _limits.maxCoreMatchingSteps)
            return std::unexpected { capError(ComponentLimit::CoreMatchingSteps, _limits, position) };
        return {};
    }

    ValidationError ComponentValidator::relationRefusal(RelationError error,
                                                        ValidationErrorCode code,
                                                        DefinitionPosition position) const
    {
        // A relation out of steps is the validation's budget spent, not a mismatch: it reports the limit.
        if (error.code == RelationErrorCode::StepLimit)
            return capError(ComponentLimit::SubtypingSteps, _limits, position);
        auto refused = plainError(code, position);
        refused.detail = std::move(error.message);
        return refused;
    }

    types::NamedExtern const* ComponentValidator::instanceExport(types::InstanceTypeId instance,
                                                                 std::string const& name)
    {
        // An instance type's exports ordered by name, built once per instance type an alias reads: an
        // alias costs O(log n), never a scan of every export.
        auto const& exports = _arena[instance].exports;
        auto [entry, inserted] = _exportIndices.try_emplace(instance.value);
        ++_work.exportLookupSteps;
        if (inserted)
        {
            _work.exportLookupSteps += exports.size();
            entry->second.reserve(exports.size());
            for (auto const position: std::views::iota(std::size_t { 0 }, exports.size()))
                entry->second.push_back(static_cast<uint32_t>(position));
            std::ranges::stable_sort(
                entry->second, std::less<> {}, [&](uint32_t position) -> std::string const& {
                    return exports[position].name.str();
                });
        }
        auto const found = std::ranges::lower_bound(
            entry->second, name, std::less<> {}, [&](uint32_t position) -> std::string const& {
                return exports[position].name.str();
            });
        if (found == entry->second.end() || exports[*found].name.str() != name)
            return nullptr;
        return &exports[*found];
    }

    Result<void> ComponentValidator::exportAlias(binary::AliasDefinition const& alias,
                                                 DefinitionPosition position)
    {
        // alias_instance_export (component.rs:3813-3861): the value gate, the instance, its export of the
        // name, which must be of the sort; then its entry, as it is (no name, no fresh resources).
        if (alias.sort == Sort::Value)
        {
            if (auto admitted = admit(GatedForm::Value, position); !admitted)
                return admitted;
        }
        auto const instance = instanceAt(alias.instance, position);
        if (!instance)
            return std::unexpected { instance.error() };
        auto const* item = instanceExport(*instance, alias.name);
        if (item == nullptr)
            return std::unexpected { indexNamedError(
                ValidationErrorCode::InstanceNoExport, alias.instance, alias.name, {}, position) };
        static_assert(std::is_same_v<std::variant_alternative_t<5, ExternType>, CoreModuleTypeId>,
                      "ExternSorts follows ExternType's alternatives");
        constexpr auto ExternSorts = std::to_array<Sort>(
            { Sort::Func, Sort::Value, Sort::Type, Sort::Component, Sort::Instance, Sort::CoreModule });
        if (ExternSorts[item->type.index()] != alias.sort)
            return std::unexpected { indexNamedError(ValidationErrorCode::ExportNotOfSort,
                                                     alias.instance,
                                                     alias.name,
                                                     std::string { externalSortWord(alias.sort) },
                                                     position) };
        auto entity = item->type;
        return addEntity(entity, std::nullopt, position);
    }

    Result<void> ComponentValidator::coreExportAlias(binary::AliasDefinition const& alias,
                                                     DefinitionPosition position)
    {
        // alias_core_instance_export (component.rs:3723-3811): room in the space first; the core
        // instance's export of the name, which must be of the sort.
        if (auto reserved = reserve(alias.sort, position); !reserved)
            return reserved;
        auto const instance = coreInstanceAt(alias.instance, position);
        if (!instance)
            return std::unexpected { instance.error() };
        auto const* item = _arena.findExport(*instance, alias.name);
        if (item == nullptr)
            return std::unexpected { indexNamedError(
                ValidationErrorCode::CoreInstanceNoExport, alias.instance, alias.name, {}, position) };
        auto const kind = coreKindOf(alias.sort);
        if (item->type.kind != kind)
            return std::unexpected { indexNamedError(
                ValidationErrorCode::CoreExportNotOfSort,
                alias.instance,
                alias.name,
                std::string { CoreAliasWords[static_cast<std::size_t>(kind)] },
                position) };
        auto& spaces = current().spaces;
        using isoline::binary::ExternKind;
        switch (kind)
        {
            case ExternKind::Func:
                spaces.coreFunctions.push_back(CoreFunctionTypeId { .value = item->type.typeIndex });
                break;
            case ExternKind::Table: spaces.coreTables.push_back(item->type.table); break;
            case ExternKind::Memory: spaces.coreMemories.push_back(item->type.memory); break;
            case ExternKind::Global: spaces.coreGlobals.push_back(item->type.global); break;
            case ExternKind::Tag:
                spaces.coreTags.push_back(CoreFunctionTypeId { .value = item->type.typeIndex });
                break;
        }
        return {};
    }

    Result<void> ComponentValidator::outerAlias(binary::AliasDefinition const& alias,
                                                DefinitionPosition position)
    {
        // alias_module, alias_component, alias_core_type and alias_type (component.rs:3863-3971): the
        // count, the index in the target scope, then room here.
        auto const target = outerFrame(alias.outerCount, position);
        if (!target)
            return std::unexpected { target.error() };
        switch (alias.sort)
        {
            case Sort::CoreModule: {
                auto const module = moduleAt(**target, alias.outerIndex, position);
                if (!module)
                    return std::unexpected { module.error() };
                return reserve(Sort::CoreModule, position).transform([&] {
                    current().spaces.coreModules.push_back(*module);
                });
            }
            case Sort::Component: {
                auto const component = componentAt(**target, alias.outerIndex, position);
                if (!component)
                    return std::unexpected { component.error() };
                return reserve(Sort::Component, position).transform([&] {
                    current().spaces.components.push_back(*component);
                });
            }
            case Sort::CoreType: {
                auto const type = coreTypeAt(**target, alias.outerIndex, position);
                if (!type)
                    return std::unexpected { type.error() };
                return reserve(Sort::CoreType, position).transform([&] {
                    current().spaces.coreTypes.push_back(*type);
                });
            }
            case Sort::Type: break;
            default: Require(false); return {}; // uncovered(outer-sort)
        }
        auto const& types = (*target)->spaces.types;
        if (alias.outerIndex >= types.size())
            return std::unexpected { indexError(
                ValidationErrorCode::TypeIndexOutOfBounds, alias.outerIndex, position) };
        auto const type = types[alias.outerIndex];
        if (alias.outerCount > 0)
        {
            // A type crossing into a component may not refer to a resource defined outside it; the walk
            // that checks it is charged to that component's budget of maxTypeSize first.
            auto& crossed = _frames[_frames.size() - alias.outerCount];
            if (crossed.kind == binary::ScopeKind::Component)
            {
                auto const charged = crossed.outerAliasTypeSize + _arena.measure(type.type).effectiveSize;
                if (charged >= _limits.maxTypeSize)
                    return std::unexpected { capError(ComponentLimit::TypeSize, _limits, position) };
                crossed.outerAliasTypeSize = charged;
                auto const roots = std::array { type.type };
                if (!freeResources(roots).empty())
                    return std::unexpected { plainError(ValidationErrorCode::OuterAliasResource, position) };
            }
        }
        return pushType(type, position);
    }

    Result<void> ComponentValidator::instance(binary::InstanceDefinition const& instance,
                                              DefinitionPosition position)
    {
        // component_instance_section (validator.rs:1160-1188): room among the instances first.
        if (auto reserved = reserve(Sort::Instance, position); !reserved)
            return reserved;
        auto const made = std::visit(
            [&](auto const& expression) -> Result<types::InstanceTypeId> {
                using Expression = std::remove_cvref_t<decltype(expression)>;
                if constexpr (std::is_same_v<Expression, binary::Instantiate>)
                    return instantiate(expression, position);
                else
                    return inlineExports(expression, position);
            },
            instance.expression);
        if (!made)
            return std::unexpected { made.error() };
        current().spaces.instances.push_back(*made);
        return {};
    }

    Result<types::InstanceTypeId> ComponentValidator::instantiate(binary::Instantiate const& instantiate,
                                                                  DefinitionPosition position)
    {
        // instantiate_component (component.rs:3281-3530): the component, its arguments by name, each
        // name once; every import supplied by a subtype, its imported resources bound to the arguments';
        // its defined resources fresh; its exports with both substituted, the fresh resources the
        // component's own.
        auto const component = componentAt(current(), instantiate.component, position);
        if (!component)
            return std::unexpected { component.error() };
        auto arguments = std::vector<types::NamedExtern> {};
        arguments.reserve(instantiate.arguments.size());
        auto names = std::map<std::string_view, std::string_view> {};
        for (auto const& argument: instantiate.arguments)
        {
            auto const entity = itemEntity(argument.item, TypeCopy::Shared, position);
            if (!entity)
                return std::unexpected { entity.error() };
            if (auto const [entry, inserted] = names.try_emplace(argument.name, argument.name); !inserted)
                return std::unexpected { namedError(ValidationErrorCode::ArgumentConflict,
                                                    argument.name,
                                                    std::string { entry->second },
                                                    position) };
            arguments.push_back(types::NamedExtern { .name = argument.name, .type = *entity });
        }
        auto measure = TypeMeasure {};
        // The instance type measures what the component exports, which the component type's own
        // measure already admitted, so this never refuses; wasm-tools combines it here all the same.
        for (auto const& entry: _arena[*component].exports)
        {
            auto combined = combine(measure, _arena.measure(entry.type), position);
            if (!combined)
                return std::unexpected { combined.error() }; // uncovered(premeasured)
            measure = *combined;
        }
        auto bound = _relation.satisfyImports(arguments, *component);
        if (!bound)
            return std::unexpected { relationRefusal(
                std::move(bound.error()), ValidationErrorCode::InstantiationMismatch, position) };
        auto const fresh = freshResourcesFor(_arena, _arena[*component]);
        if (!fresh)
            return std::unexpected { limitError(fresh.error(), position) };
        for (auto const& [defined, image]: fresh->images())
        {
            bound->bind(defined, image);
            addLocalResource(image, std::nullopt);
        }
        auto const instance = instantiatedExports(_arena, *component, *bound);
        if (!instance)
            return std::unexpected { limitError(instance.error(), position) };
        return *instance;
    }

    Result<types::InstanceTypeId> ComponentValidator::inlineExports(binary::InlineExports const& exports,
                                                                    DefinitionPosition position)
    {
        // instantiate_component_exports (component.rs:3532-3631): each export's item as it is, its name
        // validated in a context of no resources, the names unique among the instance's exports.
        auto info = types::InstanceTypeInfo {};
        auto names = LabelSet {};
        auto const noResources = ResourceNames {};
        auto measure = TypeMeasure {};
        for (auto const& entry: exports.exports)
        {
            auto const entity = itemEntity(entry.item, TypeCopy::Shared, position);
            if (!entity)
                return std::unexpected { entity.error() };
            if (auto named = name(entry.name, *entity, ExternKind::Export, noResources, names, position);
                !named)
                return std::unexpected { named.error() };
            auto combined = combine(measure, _arena.measure(*entity), position);
            if (!combined)
                return std::unexpected { combined.error() };
            measure = *combined;
            info.exports.push_back(types::NamedExtern { .name = entry.name.name, .type = *entity });
        }
        auto const added = _arena.add(std::move(info));
        if (!added)
            return std::unexpected { limitError(added.error(), position) }; // uncovered(premeasured)
        return *added;
    }

    Result<void> ComponentValidator::canon(binary::CanonDefinition const& canon, DefinitionPosition position)
    {
        // canonical_function (component.rs:1201-1325), after the built-in's gate and room among the
        // functions (component_canonical_section, validator.rs:1288-1312).
        auto const row =
            std::ranges::find(binary::CanonBuiltinTable, canon.builtin, &binary::CanonBuiltinRow::builtin);
        Require(row != binary::CanonBuiltinTable.end());
        if (row->gate)
        {
            auto const refusal =
                std::ranges::find(CanonRefusalTable, canon.builtin, &CanonRefusalRow::builtin);
            Require(refusal != CanonRefusalTable.end());
            if (auto admitted = admit(row->gate, refusal->text, position); !admitted)
                return admitted;
        }
        if (auto reserved = reserve(Sort::Func, position); !reserved)
            return reserved;
        auto const immediate = [&](std::size_t index) {
            Require(index < canon.immediates.size());
            return canon.immediates[index];
        };
        using binary::CanonBuiltin;
        switch (canon.builtin)
        {
            case CanonBuiltin::Lift: {
                // lift_function (component.rs:1327-1384): the type, the core function, the options and
                // the lifting rules; the core function's type must be the lowered one.
                auto const coreFunction = immediate(0);
                auto const typeIndex = immediate(1);
                auto const type = typeAt(typeIndex, position);
                if (!type)
                    return std::unexpected { type.error() };
                auto const* function = std::get_if<types::FunctionTypeId>(&*type);
                if (function == nullptr)
                    return std::unexpected { indexError(
                        ValidationErrorCode::NotFunctionType, typeIndex, position) };
                auto const core = coreFunctionAt(coreFunction, position);
                if (!core)
                    return std::unexpected { core.error() };
                auto const checked = options(canon.options, position);
                if (!checked)
                    return std::unexpected { checked.error() };
                auto const& signature = _arena[*core];
                if (checked->postReturn)
                {
                    // check_lift (component.rs:355-413): post-return takes the results, returns nothing.
                    auto const postReturn = coreFunctionAt(*checked->postReturn, position);
                    if (!postReturn)
                        return std::unexpected { postReturn.error() };
                    auto const& cleanup = _arena[*postReturn];
                    if (cleanup.params != signature.results || !cleanup.results.empty())
                        return std::unexpected { plainError(ValidationErrorCode::PostReturnSignature,
                                                            position) };
                }
                auto const expected = lowered(*function, *checked, Abi::Lift, position);
                if (!expected)
                    return std::unexpected { expected.error() };
                if (expected->params != signature.params)
                    return std::unexpected { indexNamedError(ValidationErrorCode::LiftParamsMismatch,
                                                             coreFunction,
                                                             coreTypeListText(expected->params),
                                                             coreTypeListText(signature.params),
                                                             position) };
                if (expected->results != signature.results)
                    return std::unexpected { indexNamedError(ValidationErrorCode::LiftResultsMismatch,
                                                             coreFunction,
                                                             coreTypeListText(expected->results),
                                                             coreTypeListText(signature.results),
                                                             position) };
                current().spaces.functions.push_back(*function);
                return {};
            }
            case CanonBuiltin::Lower: {
                // lower_function (component.rs:1386-1406).
                auto const function = functionAt(immediate(0), position);
                if (!function)
                    return std::unexpected { function.error() };
                auto const checked = options(canon.options, position);
                if (!checked)
                    return std::unexpected { checked.error() };
                // check_lower (component.rs:330-353); a callback without `async` was refused above.
                if (checked->postReturn)
                    return std::unexpected { plainError(ValidationErrorCode::PostReturnInLower, position) };
                return lowered(*function, *checked, Abi::Lower, position)
                    .and_then([&](isoline::binary::FuncType type) {
                        return pushCoreFunction(std::move(type), position);
                    });
            }
            case CanonBuiltin::ResourceNew:
                // resource_new (component.rs:1408-1413): [rep] -> [i32], of a resource of this component.
                return localResourceAt(immediate(0), position).and_then([&](binary::CoreValueKind rep) {
                    return pushCoreFunction(isoline::binary::FuncType { .params = { coreValueOf(rep) },
                                                                        .results = { isoline::binary::I32 } },
                                            position);
                });
            case CanonBuiltin::ResourceDrop:
                // resource_drop (component.rs:1415-1420): [i32] -> [], of any resource.
                return resourceAt(immediate(0), position).and_then([&](types::ResourceTypeId) {
                    return pushCoreFunction(
                        isoline::binary::FuncType { .params = { isoline::binary::I32 }, .results = {} },
                        position);
                });
            case CanonBuiltin::ResourceRep:
                // resource_rep (component.rs:1422-1427): [i32] -> [rep], of a resource of this component.
                return localResourceAt(immediate(0), position).and_then([&](binary::CoreValueKind rep) {
                    return pushCoreFunction(isoline::binary::FuncType { .params = { isoline::binary::I32 },
                                                                        .results = { coreValueOf(rep) } },
                                            position);
                });
            default: break; // uncovered(gated-builtin)
        }
        // Every other built-in is gated by a feature P11 does not implement, refused above.
        Require(false);
        return {}; // uncovered(gated-builtin)
    }

    Result<void> ComponentValidator::pushCoreFunction(isoline::binary::FuncType type, DefinitionPosition)
    {
        current().spaces.coreFunctions.push_back(_arena.intern(std::move(type)));
        return {};
    }

    Result<types::ResourceTypeId> ComponentValidator::resourceAt(uint32_t index, DefinitionPosition position)
    {
        // resource_at (component.rs:2370-2380).
        auto const entry = typeAt(index, position);
        if (!entry)
            return std::unexpected { entry.error() };
        if (auto const* resource = std::get_if<types::ResourceTypeId>(&*entry))
            return *resource;
        return std::unexpected { indexError(ValidationErrorCode::NotResourceType, index, position) };
    }

    Result<binary::CoreValueKind> ComponentValidator::localResourceAt(uint32_t index,
                                                                      DefinitionPosition position)
    {
        // check_local_resource (component.rs:2358-2368): a resource this component defined, whose
        // representation it knows.
        auto const resource = resourceAt(index, position);
        if (!resource)
            return std::unexpected { resource.error() };
        auto const& frame = current();
        auto const local = frame.localResourceIndex.find(*resource);
        if (local == frame.localResourceIndex.end() || !frame.localResources[local->second].second)
            return std::unexpected { indexError(ValidationErrorCode::NotLocalResource, index, position) };
        return *frame.localResources[local->second].second;
    }

    Result<CanonicalOptions> ComponentValidator::options(std::vector<binary::CanonOption> const& options,
                                                         DefinitionPosition position)
    {
        // check_options (component.rs:2652-2875): each option in order, each at most once, one string
        // encoding, a 32-bit memory; `async` is gated and a callback needs it; a realloc needs the
        // memory and has core type [i32 i32 i32 i32] -> [i32].
        using binary::CanonOptionKind;
        auto checked = CanonicalOptions {};
        auto hasCallback = false;
        auto const once =
            [&](std::optional<uint32_t>& slot, std::string_view word, uint32_t index) -> Result<void> {
            if (slot)
                return std::unexpected { namedError(
                    ValidationErrorCode::RepeatedOption, std::string { word }, {}, position) };
            slot = index;
            return {};
        };
        auto callback = std::optional<uint32_t> {};
        for (auto const& option: options)
        {
            auto handled = Result<void> {};
            switch (option.kind)
            {
                case CanonOptionKind::Utf8:
                case CanonOptionKind::Utf16:
                case CanonOptionKind::Latin1Utf16:
                    if (checked.encoding)
                        return std::unexpected { namedError(
                            ValidationErrorCode::EncodingConflict,
                            std::string { StringEncodingWords[static_cast<std::size_t>(*checked.encoding)] },
                            std::string { StringEncodingWords[static_cast<std::size_t>(option.kind)] },
                            position) };
                    checked.encoding = option.kind;
                    break;
                case CanonOptionKind::Memory: {
                    if (checked.memory)
                        return std::unexpected { namedError(
                            ValidationErrorCode::RepeatedOption, "memory", {}, position) };
                    // cabi_memory_at (component.rs:4529-4558): a plain memory, 32-bit unless 🐘.
                    auto const& memories = current().spaces.coreMemories;
                    if (option.index >= memories.size())
                        return std::unexpected { indexError(
                            ValidationErrorCode::MemoryIndexOutOfBounds, option.index, position) };
                    if (memories[option.index].limits.addressType == isoline::binary::AddressType::I64)
                    {
                        if (auto admitted = admit(GatedForm::Memory64CanonicalMemory, position); !admitted)
                            return std::unexpected { admitted.error() };
                    }
                    checked.memory = option.index;
                    break;
                }
                case CanonOptionKind::Realloc:
                    handled = once(checked.realloc, "realloc", option.index);
                    break;
                case CanonOptionKind::PostReturn:
                    handled = once(checked.postReturn, "post-return", option.index);
                    break;
                case CanonOptionKind::Async: handled = admit(GatedForm::AsyncOption, position); break;
                case CanonOptionKind::Callback:
                    handled = once(callback, "callback", option.index);
                    hasCallback = true;
                    break;
            }
            if (!handled)
                return std::unexpected { handled.error() };
        }
        // `async` is refused at its option, so a callback here is one without it.
        if (hasCallback)
            return std::unexpected { plainError(ValidationErrorCode::CallbackWithoutAsync, position) };
        if (checked.realloc)
        {
            if (!checked.memory)
                return std::unexpected { plainError(ValidationErrorCode::ReallocWithoutMemory, position) };
            auto const realloc = coreFunctionAt(*checked.realloc, position);
            if (!realloc)
                return std::unexpected { realloc.error() };
            auto const& signature = _arena[*realloc];
            if (signature.params != std::vector(4, PointerType)
                || signature.results != std::vector { PointerType })
                return std::unexpected { plainError(ValidationErrorCode::ReallocSignature, position) };
        }
        return checked;
    }

    FlatShape const& ComponentValidator::flatShape(TypeId type)
    {
        // Bottom-up over an explicit stack: a type's shape is made once its children's are, then kept,
        // so that no type is flattened twice however many definitions lower it. Kept by TypeId, the
        // arena's own ids, for the types canonical definitions reach only.
        auto pending = std::vector<TypeId> { type };
        while (!pending.empty())
        {
            auto const current = pending.back();
            if (_flatShapes.contains(current))
            {
                pending.pop_back();
                continue;
            }
            auto const& defined = _arena[current];
            auto isReady = true;
            for (auto const& child: defined.children)
            {
                if (child && !_flatShapes.contains(*child))
                {
                    pending.push_back(*child);
                    isReady = false;
                }
            }
            if (!isReady)
                continue;
            pending.pop_back();
            _flatShapes.emplace(current, shapeFrom(defined, [&](TypeId child) -> FlatShape const& {
                                    return _flatShapes.at(child);
                                }));
        }
        return _flatShapes.at(type);
    }

    Result<isoline::binary::FuncType> ComponentValidator::lowered(types::FunctionTypeId function,
                                                                  CanonicalOptions const& options,
                                                                  Abi abi,
                                                                  DefinitionPosition position)
    {
        // ComponentFuncType::lower (component_types.rs:1236-1359), synchronously: the parameters flat
        // up to MaxFlatParams, else one pointer; the result flat up to MaxFlatResults, else a pointer
        // (a lowered import's last parameter, a lifted export's result). A pointer anywhere needs the
        // memory; a lift's parameters and a lower's result that hold one need the realloc, as does a
        // lift whose parameters spill.
        auto const requireMemory = [&]() -> Result<void> {
            if (!options.memory)
                return std::unexpected { plainError(ValidationErrorCode::MemoryRequired, position) };
            return {};
        };
        auto const requireRealloc = [&]() -> Result<void> {
            return requireMemory().and_then([&]() -> Result<void> {
                if (!options.realloc)
                    return std::unexpected { plainError(ValidationErrorCode::ReallocRequired, position) };
                return {};
            });
        };
        auto const& signature = _arena[function];
        auto lowered = isoline::binary::FuncType {};
        for (auto const& [label, param]: signature.params)
        {
            auto const& shape = flatShape(param);
            if (shape.holdsPointer)
            {
                auto required = abi == Abi::Lower ? requireMemory() : requireRealloc();
                if (!required)
                    return std::unexpected { required.error() };
            }
            auto const fits =
                !shape.isOverflowing && lowered.params.size() + shape.types.size() <= MaxFlatParams;
            if (!fits)
            {
                lowered.params = { PointerType };
                auto required = abi == Abi::Lower ? requireMemory() : requireRealloc();
                if (!required)
                    return std::unexpected { required.error() };
                break;
            }
            lowered.params.insert(lowered.params.end(), shape.types.begin(), shape.types.end());
        }
        if (signature.result)
        {
            auto const& shape = flatShape(*signature.result);
            if (abi == Abi::Lower && shape.holdsPointer)
            {
                if (auto required = requireRealloc(); !required)
                    return std::unexpected { required.error() };
            }
            if (shape.isOverflowing || shape.types.size() > MaxFlatResults)
            {
                if (auto required = requireMemory(); !required)
                    return std::unexpected { required.error() };
                if (abi == Abi::Lower)
                    lowered.params.push_back(PointerType);
                else
                    lowered.results = { PointerType };
            }
            else
                lowered.results = shape.types;
        }
        return lowered;
    }
} // namespace

std::optional<ComponentLimit> exceededLimit(ValidationError const& error) noexcept
{
    if (error.code != ValidationErrorCode::LimitExceeded || !error.subject)
        return std::nullopt;
    return static_cast<ComponentLimit>(*error.subject & 0xFFU);
}

std::string specMessage(ValidationError const& error)
{
    auto const& row = ValidationErrorMessages[static_cast<std::size_t>(error.code)];
    auto const subject = error.subject.value_or(0);
    switch (row.detail)
    {
        case ValidationErrorDetail::None: return std::string { row.text };
        case ValidationErrorDetail::Index: return std::vformat(row.text, std::make_format_args(subject));
        case ValidationErrorDetail::Refusal: return error.name;
        case ValidationErrorDetail::Extern:
            Require(subject < ExternKindWords.size());
            return std::vformat(row.text,
                                std::make_format_args(ExternKindWords[subject], error.name, error.previous));
        case ValidationErrorDetail::Named:
            return std::vformat(row.text, std::make_format_args(error.name, error.previous));
        case ValidationErrorDetail::IndexNamed:
            return std::vformat(row.text, std::make_format_args(subject, error.name, error.previous));
        case ValidationErrorDetail::Mismatch:
            return std::vformat(row.text, std::make_format_args(error.name, error.previous)) + "\n"
                   + error.detail;
        case ValidationErrorDetail::Verbatim: return error.detail;
        case ValidationErrorDetail::Label:
            Require(subject < LabelKindTable.size());
            return std::vformat(row.text,
                                std::make_format_args(LabelKindTable[subject].description,
                                                      error.name,
                                                      LabelKindTable[subject].noun,
                                                      error.previous));
        case ValidationErrorDetail::Limit:
            Require(exceededLimit(error).has_value());
            return limitMessage(*exceededLimit(error), subject >> 8U);
    }
    return std::string { row.text }; // uncovered(exhaustive-switch)
}

std::expected<std::shared_ptr<ValidatedComponent const>, ValidationError> validateComponent(
    binary::ComponentPool pool,
    ComponentFeatureSet features,
    ComponentLimits const& limits,
    ICoreModuleValidator& coreModules)
{
    auto validator = ComponentValidator { pool, features, limits, coreModules };
    if (auto validated = validator.run(); !validated)
        return std::unexpected { std::move(validated.error()) };
    return std::make_shared<ValidatedComponent const>(
        ValidatedComponent { .pool = std::move(pool),
                             .types = validator.takeArena(),
                             .scopes = validator.takeScopes(),
                             .type = validator.takeRootType(),
                             .coreModules = validator.takeCoreModules(),
                             .work = validator.work() });
}

} // namespace isoline::component::validate
