// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/validate/Validator.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::validate
{

namespace
{
    using binary::DefinitionPosition;
    using types::AnyTypeId;
    using types::TypeId;
    using types::TypeMeasure;
    using types::ValueKind;

    template <typename T>
    using Result = std::expected<T, ValidationError>;

    /// The most labels a flags type may have (Binary.md; wasmparser's create_flags_type).
    constexpr auto MaxFlags = std::size_t { 32 };

    /// The most elements a fixed-length list may have: MAX_WASM_FIXED_LENGTH_LIST_ELEMENTS, wasmparser
    /// 0.263.0 src/limits.rs:78.
    constexpr auto MaxFixedListElements = uint32_t { 1'073'741'824 };

    // A stream's or future's payload rules (no `borrow`, no `stream<char>`) and a fixed-length list's
    // checks past its length are unreachable while their gates are not implemented; P12 and the
    // fixed-length lists row add what they need when they flip them.
    static_assert(componentFeatureRow(ComponentFeature::Async).p11 != Implementation::Implemented,
                  "implementing async needs the stream and future payload rules here");
    static_assert(componentFeatureRow(ComponentFeature::FixedLengthLists).p11 != Implementation::Implemented,
                  "implementing fixed-length lists needs their validation here reviewed and tested");

    /// Whether @p label is in kebab case, as wasmparser 0.263.0 defines it (KebabStr::is_kebab_case,
    /// src/validator/names.rs:68-92): words of lower- or upper-case letters and digits, one case per
    /// word, separated by single hyphens; only the first word must begin with a letter.
    [[nodiscard]] bool isKebabCase(std::string_view label) noexcept
    {
        auto lower = false;
        auto upper = false;
        auto first = true;
        auto digit = false;
        for (auto const character: label)
        {
            auto const isLower = character >= 'a' && character <= 'z';
            auto const isUpper = character >= 'A' && character <= 'Z';
            auto const isDigit = character >= '0' && character <= '9';
            auto const inWord = lower || upper;
            if (isLower && !inWord)
                lower = true;
            else if (isUpper && !inWord)
                upper = true;
            else if (isDigit && !inWord && !first)
                digit = true;
            else if ((isLower && lower) || (isUpper && upper))
                continue;
            else if (isDigit && inWord)
                digit = true;
            else if (character == '-' && (inWord || digit))
            {
                lower = false;
                upper = false;
                first = false;
                digit = false;
            }
            else
                return false;
        }
        return !label.empty() && !label.ends_with('-');
    }

    /// The form of @p label strong uniqueness compares (KebabStr::canonical_chars): hyphens dropped,
    /// ASCII letters lowered, so "a-B-c" and "abc" are the same label.
    [[nodiscard]] std::string canonicalLabel(std::string_view label)
    {
        auto canonical = std::string {};
        canonical.reserve(label.size());
        for (auto const character: label)
        {
            if (character == '-')
                continue;
            canonical.push_back(
                character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character);
        }
        return canonical;
    }

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
            auto const [entry, inserted] = _labels.try_emplace(canonicalLabel(label), label);
            if (inserted)
                return std::nullopt;
            return entry->second;
        }

      private:
        std::map<std::string, std::string, std::less<>> _labels;
    };

    /// One scope being validated: its pool entry, where in it the walk is, and its index spaces.
    struct Frame
    {
        uint32_t scope = 0;                                    ///< Its index in ComponentPool::components.
        binary::ScopeKind kind = binary::ScopeKind::Component; ///< What it is.
        DefinitionPosition origin {};                          ///< The parent's definition that opened it.
        std::size_t next = 0;                                  ///< Its next definition to validate.
        std::vector<AnyTypeId> types {};                       ///< Its type index space.
        std::vector<types::NamedExtern> imports {};            ///< Its imports, typed.
        std::vector<types::NamedExtern> exports {};            ///< Its export declarations, typed.
        TypeMeasure measure {};                                ///< Its own measure, as a type.
        /// The effective sizes of the types aliased into it from outside it (a component's only).
        uint64_t outerAliasTypeSize = 0;
        LabelSet importNames {}; ///< Its import names, for strong uniqueness.
        LabelSet exportNames {}; ///< Its export names, for strong uniqueness.
    };

    /// Whether an extern declaration imports or exports.
    enum class ExternDirection : uint8_t
    {
        Import,
        Export,
    };

    /// The attributes in the order wasm-tools checks them (validate_extern, component.rs:4721-4790),
    /// whatever their encoded order: by their Attribute alternative.
    constexpr auto AttributeCheckOrder = std::to_array<std::size_t>({ 1, 0, 2 });

    /// The kinds a map key may be: Binary.md's `keytype`, the primitives but floats and error-context.
    [[nodiscard]] constexpr bool isKeyKind(ValueKind kind) noexcept
    {
        return types::isPrimitive(kind) && kind != ValueKind::F32 && kind != ValueKind::F64
               && kind != ValueKind::ErrorContext;
    }

    /// The refusal of @p form, which Task 7 (or 5) validates and this task does not.
    [[nodiscard]] ValidationError deferredError(DeferredForm form, DefinitionPosition position)
    {
        return ValidationError { .code = ValidationErrorCode::NotYetValidated,
                                 .position = position,
                                 .subject = static_cast<uint64_t>(form) };
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
            _arena { limits }
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

      private:
        [[nodiscard]] Frame& current() { return _frames.back(); }

        [[nodiscard]] Result<void> step(binary::Definition const& definition, DefinitionPosition position);
        [[nodiscard]] Result<void> finish();
        [[nodiscard]] Result<void> open(uint32_t scope, binary::ScopeKind kind, DefinitionPosition position);

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

        [[nodiscard]] Result<void> pushType(AnyTypeId type, DefinitionPosition position);
        [[nodiscard]] bool mayReferToResource(AnyTypeId type) const;
        [[nodiscard]] Result<AnyTypeId> typeAt(uint32_t index, DefinitionPosition position);
        [[nodiscard]] Result<TypeId> valueType(binary::ValueTypeRef const& type, DefinitionPosition position);
        [[nodiscard]] Result<TypeId> intern(types::DefinedType type, DefinitionPosition position);

        [[nodiscard]] Result<void> typeDefinition(binary::TypeDefinition const& definition,
                                                  DefinitionPosition position);
        [[nodiscard]] Result<TypeId> definedType(binary::DefinedValueType const& type,
                                                 DefinitionPosition position);
        [[nodiscard]] Result<TypeId> record(binary::RecordType const& type, DefinitionPosition position);
        [[nodiscard]] Result<TypeId> variant(binary::VariantType const& type, DefinitionPosition position);
        [[nodiscard]] Result<TypeId> tuple(binary::TupleType const& type, DefinitionPosition position);
        [[nodiscard]] Result<TypeId> labelsOnly(ValueKind kind,
                                                std::vector<std::string> const& labels,
                                                DefinitionPosition position);
        [[nodiscard]] Result<TypeId> elements(ValueKind kind,
                                              std::vector<std::optional<binary::ValueTypeRef>> const& refs,
                                              DefinitionPosition position);
        [[nodiscard]] Result<TypeId> map(binary::MapType const& type, DefinitionPosition position);
        [[nodiscard]] Result<TypeId> handle(ValueKind kind, uint32_t resource, DefinitionPosition position);
        [[nodiscard]] Result<void> functionType(binary::FunctionTypeDefinition const& type,
                                                DefinitionPosition position);
        [[nodiscard]] Result<void> resourceType(binary::ResourceTypeDefinition const& type,
                                                DefinitionPosition position);

        [[nodiscard]] Result<types::ExternType> externType(binary::ExternDesc const& type,
                                                           DefinitionPosition position);
        [[nodiscard]] Result<void> declare(binary::NameAttributes const& name,
                                           binary::ExternDesc const& type,
                                           ExternDirection direction,
                                           DefinitionPosition position);
        [[nodiscard]] Result<void> attributes(binary::NameAttributes const& name,
                                              DefinitionPosition position) const;
        [[nodiscard]] Result<void> alias(binary::AliasDefinition const& alias, DefinitionPosition position);
        [[nodiscard]] Result<void> coreModule(binary::CoreModuleDefinition const& module,
                                              DefinitionPosition position);
        [[nodiscard]] Result<void> valueSort(binary::Sort sort, DefinitionPosition position) const;
        [[nodiscard]] Result<DeferredForm> gates(binary::CanonDefinition const& canon,
                                                 DefinitionPosition position) const;
        [[nodiscard]] Result<DeferredForm> gates(binary::AliasDefinition const& alias,
                                                 DefinitionPosition position) const;
        [[nodiscard]] Result<DeferredForm> gates(binary::InstanceDefinition const& instance,
                                                 DefinitionPosition position) const;
        [[nodiscard]] Result<DeferredForm> gates(binary::ExportDefinition const& definition,
                                                 DefinitionPosition position) const;
        [[nodiscard]] Result<void> deferred(binary::Definition const& definition,
                                            DefinitionPosition position) const;

        binary::ComponentPool const& _pool;
        ComponentFeatureSet _features;
        ComponentLimits const& _limits;
        ICoreModuleValidator& _coreModuleValidator;
        types::TypeArena _arena;
        std::vector<Frame> _frames;
        types::ComponentTypeInfo _rootType;
        std::vector<CoreModuleShape> _coreModules;
    };

    [[nodiscard]] ValidationError limitError(types::TypeLimitError const& error, DefinitionPosition position)
    {
        return ValidationError { .code = ValidationErrorCode::LimitExceeded,
                                 .position = position,
                                 .subject = binary::limitSubject(error.limit, error.value) };
    }

    [[nodiscard]] ValidationError indexError(ValidationErrorCode code,
                                             uint64_t index,
                                             DefinitionPosition position)
    {
        return ValidationError { .code = code, .position = position, .subject = index };
    }

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
        switch (frame.kind)
        {
            case binary::ScopeKind::Component:
                if (_frames.empty())
                    _rootType = types::ComponentTypeInfo { .imports = std::move(frame.imports),
                                                           .exports = std::move(frame.exports),
                                                           .measure = frame.measure };
                return {};
            case binary::ScopeKind::ComponentType: {
                auto const added = _arena.add(types::ComponentTypeInfo { .imports = std::move(frame.imports),
                                                                         .exports = std::move(frame.exports),
                                                                         .measure = {} });
                if (!added)
                    return std::unexpected { limitError(added.error(), frame.origin) };
                return pushType(*added, frame.origin);
            }
            case binary::ScopeKind::InstanceType: {
                Require(frame.imports.empty());
                auto const added = _arena.add(
                    types::InstanceTypeInfo { .exports = std::move(frame.exports), .measure = {} });
                if (!added)
                    return std::unexpected { limitError(added.error(), frame.origin) };
                return pushType(*added, frame.origin);
            }
        }
        Require(false);
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
                    return open(alternative.index, binary::ScopeKind::Component, position);
                else if constexpr (std::is_same_v<Alternative, binary::ImportDefinition>)
                    return declare(alternative.name, alternative.type, ExternDirection::Import, position);
                else if constexpr (std::is_same_v<Alternative, binary::ExportDeclaration>)
                    return declare(alternative.name, alternative.type, ExternDirection::Export, position);
                else if constexpr (std::is_same_v<Alternative, binary::AliasDefinition>)
                    return alias(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::CoreModuleDefinition>)
                    return coreModule(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::CustomDefinition>)
                    return {};
                else
                    return deferred(definition, position);
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

    Result<void> ComponentValidator::pushType(AnyTypeId type, DefinitionPosition position)
    {
        // check_max(type_count, 1, MAX_WASM_TYPES, "types"); core types join the count with Task 7.
        auto& space = current().types;
        if (space.size() >= _limits.maxTypes)
            return std::unexpected { limitError(
                types::TypeLimitError { .limit = ComponentLimit::Types, .value = _limits.maxTypes },
                position) };
        space.push_back(type);
        return {};
    }

    bool ComponentValidator::mayReferToResource(AnyTypeId type) const
    {
        // A value type refers to a resource only through a handle; a resource is one; function,
        // component and instance types are left to Task 7's walk.
        if (auto const* value = std::get_if<TypeId>(&type))
            return _arena[*value].handles != types::HandleContent::None;
        return true;
    }

    Result<AnyTypeId> ComponentValidator::typeAt(uint32_t index, DefinitionPosition position)
    {
        auto const& space = current().types;
        if (index >= space.size())
            return std::unexpected { indexError(ValidationErrorCode::TypeIndexOutOfBounds, index, position) };
        return space[index];
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
            return limitError(error, position);
        });
    }

    Result<void> ComponentValidator::typeDefinition(binary::TypeDefinition const& definition,
                                                    DefinitionPosition position)
    {
        return std::visit(
            [&](auto const& type) -> Result<void> {
                using Type = std::remove_cvref_t<decltype(type)>;
                if constexpr (std::is_same_v<Type, binary::DefinedValueType>)
                    return definedType(type, position).and_then([&](TypeId id) {
                        return pushType(id, position);
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
                    return valueType(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::RecordType>)
                    return record(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::VariantType>)
                    return variant(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::ListType>)
                    return elements(ValueKind::List, { alternative.element }, position);
                else if constexpr (std::is_same_v<Alternative, binary::FixedListType>)
                {
                    // uncovered-begin(fixed-length-lists): unreachable while the gate above refuses 🔧.
                    if (alternative.length < 1)
                        return std::unexpected { ValidationError {
                            .code = ValidationErrorCode::EmptyFixedList, .position = position } };
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
                                                       .length = alternative.length },
                                  position);
                    // uncovered-end(fixed-length-lists)
                }
                else if constexpr (std::is_same_v<Alternative, binary::TupleType>)
                    return tuple(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::FlagsType>)
                {
                    if (alternative.labels.empty())
                        return std::unexpected { ValidationError { .code = ValidationErrorCode::EmptyFlags,
                                                                   .position = position } };
                    if (alternative.labels.size() > MaxFlags)
                        return std::unexpected { ValidationError { .code = ValidationErrorCode::TooManyFlags,
                                                                   .position = position } };
                    return labelsOnly(ValueKind::Flags, alternative.labels, position);
                }
                else if constexpr (std::is_same_v<Alternative, binary::EnumType>)
                {
                    if (alternative.labels.empty())
                        return std::unexpected { ValidationError { .code = ValidationErrorCode::EmptyEnum,
                                                                   .position = position } };
                    return labelsOnly(ValueKind::Enum, alternative.labels, position);
                }
                else if constexpr (std::is_same_v<Alternative, binary::OptionType>)
                    return elements(ValueKind::Option, { alternative.type }, position);
                else if constexpr (std::is_same_v<Alternative, binary::ResultType>)
                    return elements(ValueKind::Result, { alternative.ok, alternative.error }, position);
                else if constexpr (std::is_same_v<Alternative, binary::OwnType>)
                    return handle(ValueKind::Own, alternative.resource, position);
                else if constexpr (std::is_same_v<Alternative, binary::BorrowType>)
                    return handle(ValueKind::Borrow, alternative.resource, position);
                else if constexpr (std::is_same_v<Alternative, binary::StreamType>)
                    // uncovered-begin(async): unreachable while the gate above refuses 🔀.
                    return elements(ValueKind::Stream, { alternative.element }, position);
                // uncovered-end(async)
                else if constexpr (std::is_same_v<Alternative, binary::FutureType>)
                    // uncovered-begin(async): unreachable while the gate above refuses 🔀.
                    return elements(ValueKind::Future, { alternative.element }, position);
                // uncovered-end(async)
                else
                {
                    static_assert(std::is_same_v<Alternative, binary::MapType>,
                                  "every defvaltype has a case");
                    return map(alternative, position);
                }
            },
            type);
    }

    Result<TypeId> ComponentValidator::record(binary::RecordType const& type, DefinitionPosition position)
    {
        // create_record_type (component.rs:4187-4224): per field its label, its type, then uniqueness.
        if (type.fields.empty())
            return std::unexpected { ValidationError { .code = ValidationErrorCode::EmptyRecord,
                                                       .position = position } };
        auto seen = LabelSet {};
        auto measure = TypeMeasure {};
        auto defined = types::DefinedType { .kind = ValueKind::Record };
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
            defined.labels.push_back(field.label);
        }
        return intern(std::move(defined), position);
    }

    Result<TypeId> ComponentValidator::variant(binary::VariantType const& type, DefinitionPosition position)
    {
        // create_variant_type (component.rs:4226-4282): per case its label, its payload, then uniqueness.
        if (type.cases.empty())
            return std::unexpected { ValidationError { .code = ValidationErrorCode::EmptyVariant,
                                                       .position = position } };
        auto seen = LabelSet {};
        auto measure = TypeMeasure {};
        auto defined = types::DefinedType { .kind = ValueKind::Variant };
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
            defined.labels.push_back(entry.label);
        }
        return intern(std::move(defined), position);
    }

    Result<TypeId> ComponentValidator::tuple(binary::TupleType const& type, DefinitionPosition position)
    {
        if (type.elements.empty())
            return std::unexpected { ValidationError { .code = ValidationErrorCode::EmptyTuple,
                                                       .position = position } };
        auto refs = std::vector<std::optional<binary::ValueTypeRef>> {};
        refs.reserve(type.elements.size());
        for (auto const& element: type.elements)
            refs.emplace_back(element);
        return elements(ValueKind::Tuple, refs, position);
    }

    Result<TypeId> ComponentValidator::elements(ValueKind kind,
                                                std::vector<std::optional<binary::ValueTypeRef>> const& refs,
                                                DefinitionPosition position)
    {
        // A list's, an option's, a tuple's and a result's inner types: each resolved, then each measured,
        // as wasmparser resolves a result's two before it combines them.
        auto defined = types::DefinedType { .kind = kind };
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
        return intern(types::DefinedType { .kind = kind, .labels = labels }, position);
    }

    Result<TypeId> ComponentValidator::map(binary::MapType const& type, DefinitionPosition position)
    {
        // create_defined_type's Map arm (component.rs:4006-4058): the key must be a `keytype`, and the
        // value may hold no handle, as bindings may drop the values of duplicate keys.
        auto const key = valueType(type.key, position);
        if (!key)
            return key;
        if (!isKeyKind(_arena[*key].kind))
            return std::unexpected { ValidationError { .code = ValidationErrorCode::InvalidMapKey,
                                                       .position = position } };
        auto const value = valueType(type.value, position);
        if (!value)
            return value;
        if (types::holds(_arena[*value].handles, types::HandleContent::Own | types::HandleContent::Borrow))
            return std::unexpected { ValidationError { .code = ValidationErrorCode::HandleInMapValue,
                                                       .position = position } };
        return elements(ValueKind::Map, { type.key, type.value }, position);
    }

    Result<TypeId> ComponentValidator::handle(ValueKind kind, uint32_t resource, DefinitionPosition position)
    {
        auto const entry = typeAt(resource, position);
        if (!entry)
            return std::unexpected { entry.error() };
        auto const* id = std::get_if<types::ResourceTypeId>(&*entry);
        if (!id)
            return std::unexpected { indexError(ValidationErrorCode::NotResourceType, resource, position) };
        return intern(types::DefinedType { .kind = kind, .resource = *id }, position);
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
                return std::unexpected { ValidationError { .code = ValidationErrorCode::BorrowInResult,
                                                           .position = position } };
            if (auto measured = combine(measure, _arena.measure(AnyTypeId { *result }), position); !measured)
                return std::unexpected { measured.error() };
            info.result = *result;
        }
        auto const added = _arena.add(std::move(info));
        if (!added)
            return std::unexpected { limitError(added.error(), position) };
        return pushType(*added, position);
    }

    Result<void> ComponentValidator::resourceType(binary::ResourceTypeDefinition const& type,
                                                  DefinitionPosition position)
    {
        // add_type's Resource arm (component.rs:610-656).
        if (current().kind != binary::ScopeKind::Component)
            return std::unexpected { ValidationError { .code = ValidationErrorCode::ResourceOutsideComponent,
                                                       .position = position } };
        if (type.rep == binary::CoreValueKind::I64
            && !validationAdmits(_features, ComponentFeature::Memory64))
            return std::unexpected { ValidationError { .code = ValidationErrorCode::ResourceRepresentationI64,
                                                       .position = position,
                                                       .disabled = ComponentFeature::Memory64 } };
        if (type.rep != binary::CoreValueKind::I32 && type.rep != binary::CoreValueKind::I64)
            return std::unexpected { ValidationError {
                .code = ValidationErrorCode::InvalidResourceRepresentation, .position = position } };
        // The destructor's core function type needs the core function index space of Task 7.
        if (type.destructor)
            return std::unexpected { deferredError(DeferredForm::ResourceDestructors, position) };
        auto const resource =
            _arena.newResource(types::ResourceTypeInfo { .rep = type.rep, .destructor = type.destructor });
        if (!resource)
            return std::unexpected { limitError(resource.error(), position) };
        return pushType(*resource, position);
    }

    Result<types::ExternType> ComponentValidator::externType(binary::ExternDesc const& type,
                                                             DefinitionPosition position)
    {
        // check_type_ref (component.rs:2876-2933).
        using binary::Sort;
        auto const kindOf = [&](ValidationErrorCode mismatch, auto expected) -> Result<types::ExternType> {
            using Expected = decltype(expected);
            auto const entry = typeAt(type.index, position);
            if (!entry)
                return std::unexpected { entry.error() };
            if (auto const* id = std::get_if<Expected>(&*entry))
                return types::ExternType { *id };
            return std::unexpected { indexError(mismatch, type.index, position) };
        };
        switch (type.sort)
        {
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
                    return types::ExternType { types::TypeExtern { .type = *resource } };
                }
                auto const entry = typeAt(type.index, position);
                if (!entry)
                    return std::unexpected { entry.error() };
                return types::ExternType { types::TypeExtern { .type = *entry } };
            }
            case Sort::Value: {
                // The decoder refuses a value extern type while 🪙 is refused; a built pool reaches here.
                auto const admitted = admit(GatedForm::Value, position);
                Require(!admitted.has_value());
                return std::unexpected { admitted.error() };
            }
            case Sort::CoreModule:
                return std::unexpected { deferredError(DeferredForm::CoreModuleTypes, position) };
            default: break;
        }
        Require(false);
        return std::unexpected { deferredError(DeferredForm::CoreModuleTypes, position) };
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

    Result<void> ComponentValidator::declare(binary::NameAttributes const& name,
                                             binary::ExternDesc const& type,
                                             ExternDirection direction,
                                             DefinitionPosition position)
    {
        // add_import/add_export: the type first (check_type_ref), then its index (add_entity), then the
        // name and the measure (validate_extern). The name's grammar and uniqueness are Tasks 5 and 7's.
        auto const declared = externType(type, position);
        if (!declared)
            return std::unexpected { declared.error() };
        if (auto const* bound = std::get_if<types::TypeExtern>(&*declared))
        {
            if (auto pushed = pushType(bound->type, position); !pushed)
                return pushed;
        }
        // The externname grammar beyond a plain label is Task 5's, and strong uniqueness of the rest
        // Task 7's: anything they would decide is refused until then, not accepted unchecked.
        if (!isKebabCase(name.name))
            return std::unexpected { deferredError(DeferredForm::ExternNames, position) };
        auto& names = direction == ExternDirection::Import ? current().importNames : current().exportNames;
        if (names.insert(name.name))
            return std::unexpected { deferredError(DeferredForm::DuplicateExternNames, position) };
        if (auto checked = attributes(name, position); !checked)
            return checked;
        auto& frame = current();
        auto measured = combine(frame.measure, _arena.measure(*declared), position);
        if (!measured)
            return std::unexpected { measured.error() };
        frame.measure = *measured;
        auto& externs = direction == ExternDirection::Import ? frame.imports : frame.exports;
        externs.push_back(types::NamedExtern { .name = name.name, .type = *declared });
        return {};
    }

    Result<void> ComponentValidator::alias(binary::AliasDefinition const& alias, DefinitionPosition position)
    {
        if (alias.target != binary::AliasTarget::Outer || alias.sort != binary::Sort::Type)
            return deferred(binary::Definition { alias }, position);
        // alias_type (component.rs:3911-3971): the count, the index in the target scope, then the charge
        // to the component the alias crosses into, which bounds the work of its resource check.
        if (alias.outerCount >= _frames.size())
            return std::unexpected { indexError(
                ValidationErrorCode::InvalidOuterAliasCount, alias.outerCount, position) };
        auto const& target = _frames[_frames.size() - 1 - alias.outerCount];
        if (alias.outerIndex >= target.types.size())
            return std::unexpected { indexError(
                ValidationErrorCode::TypeIndexOutOfBounds, alias.outerIndex, position) };
        auto const type = target.types[alias.outerIndex];
        if (alias.outerCount > 0)
        {
            // The resource check itself (a type crossing a component boundary may not refer to a
            // resource defined outside it) lands with Task 7's outer aliases.
            auto& crossed = _frames[_frames.size() - alias.outerCount];
            if (crossed.kind == binary::ScopeKind::Component)
            {
                auto const charged = crossed.outerAliasTypeSize + _arena.measure(type).effectiveSize;
                if (charged >= _limits.maxTypeSize)
                    return std::unexpected { limitError(
                        types::TypeLimitError { .limit = ComponentLimit::TypeSize,
                                                .value = _limits.maxTypeSize },
                        position) };
                crossed.outerAliasTypeSize = charged;
                // A type crossing into a component may not refer to a resource defined outside it; that
                // check (free variables) is Task 7's, so a type that may refer to one is refused here.
                if (mayReferToResource(type))
                    return std::unexpected { deferredError(DeferredForm::OuterAliasesIntoComponents,
                                                           position) };
            }
        }
        return pushType(type, position);
    }

    Result<void> ComponentValidator::coreModule(binary::CoreModuleDefinition const& module,
                                                DefinitionPosition position)
    {
        auto const& bytes = *_pool.bytes;
        Require(module.offset <= bytes.size() && module.size <= bytes.size() - module.offset);
        auto const ordinal = static_cast<uint32_t>(_coreModules.size());
        auto shape = _coreModuleValidator.validate(
            std::span<std::byte const> { bytes }.subspan(module.offset, module.size));
        if (!shape)
            return std::unexpected { ValidationError {
                .code = ValidationErrorCode::CoreModuleRefused,
                .position = position,
                .subject = ordinal,
                .coreModule = CoreModuleError { .ordinal = ordinal, .error = std::move(shape.error()) } } };
        _coreModules.push_back(std::move(*shape));
        return {};
    }

    Result<void> ComponentValidator::valueSort(binary::Sort sort, DefinitionPosition position) const
    {
        if (sort != binary::Sort::Value)
            return {};
        return admit(GatedForm::Value, position);
    }

    Result<DeferredForm> ComponentValidator::gates(binary::CanonDefinition const& canon,
                                                   DefinitionPosition position) const
    {
        auto const row =
            std::ranges::find(binary::CanonBuiltinTable, canon.builtin, &binary::CanonBuiltinRow::builtin);
        Require(row != binary::CanonBuiltinTable.end());
        if (row->gate)
        {
            auto const refusal =
                std::ranges::find(CanonRefusalTable, canon.builtin, &CanonRefusalRow::builtin);
            Require(refusal != CanonRefusalTable.end());
            if (auto admitted = admit(row->gate, refusal->text, position); !admitted)
                return std::unexpected { admitted.error() };
        }
        for (auto const& option: canon.options)
        {
            auto const form = CanonOptionGates[static_cast<std::size_t>(option.kind)];
            if (!form)
                continue;
            if (auto admitted = admit(*form, position); !admitted)
                return std::unexpected { admitted.error() };
        }
        return DeferredForm::CanonicalFunctions;
    }

    Result<DeferredForm> ComponentValidator::gates(binary::AliasDefinition const& alias,
                                                   DefinitionPosition position) const
    {
        switch (alias.target)
        {
            case binary::AliasTarget::Export:
                return valueSort(alias.sort, position).transform([] {
                    return DeferredForm::InstanceExportAliases;
                });
            case binary::AliasTarget::CoreExport: return DeferredForm::CoreInstanceExportAliases;
            case binary::AliasTarget::Outer: break;
        }
        return DeferredForm::OuterAliasesOfOtherSorts;
    }

    Result<DeferredForm> ComponentValidator::gates(binary::InstanceDefinition const& instance,
                                                   DefinitionPosition position) const
    {
        if (auto const* instantiate = std::get_if<binary::Instantiate>(&instance.expression))
        {
            for (auto const& argument: instantiate->arguments)
            {
                if (auto admitted = valueSort(argument.item.sort, position); !admitted)
                    return std::unexpected { admitted.error() };
            }
            return DeferredForm::ComponentInstances;
        }
        for (auto const& entry: std::get<binary::InlineExports>(instance.expression).exports)
        {
            if (auto admitted = attributes(entry.name, position).and_then([&] {
                    return valueSort(entry.item.sort, position);
                });
                !admitted)
                return std::unexpected { admitted.error() };
        }
        return DeferredForm::ComponentInstances;
    }

    Result<DeferredForm> ComponentValidator::gates(binary::ExportDefinition const& definition,
                                                   DefinitionPosition position) const
    {
        auto admitted = attributes(definition.name, position).and_then([&] {
            return valueSort(definition.item.sort, position);
        });
        if (admitted && definition.type)
            admitted = valueSort(definition.type->sort, position);
        return admitted.transform([] { return DeferredForm::ExportDefinitions; });
    }

    Result<void> ComponentValidator::deferred(binary::Definition const& definition,
                                              DefinitionPosition position) const
    {
        // A definition Task 7 validates: its gated forms are refused here, where wasm-tools refuses
        // them, and the rest is refused as not yet validated.
        auto const scanned = std::visit(
            [&](auto const& alternative) -> Result<DeferredForm> {
                using Alternative = std::remove_cvref_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Alternative, binary::CanonDefinition>
                              || std::is_same_v<Alternative, binary::AliasDefinition>
                              || std::is_same_v<Alternative, binary::InstanceDefinition>
                              || std::is_same_v<Alternative, binary::ExportDefinition>)
                    return gates(alternative, position);
                else if constexpr (std::is_same_v<Alternative, binary::CoreInstanceDefinition>)
                    return DeferredForm::CoreInstances;
                else
                {
                    // step() handles every other alternative itself.
                    static_assert(std::is_same_v<Alternative, binary::CoreTypeDefinition>
                                  || std::is_same_v<Alternative, binary::TypeDefinition>
                                  || std::is_same_v<Alternative, binary::NestedComponent>
                                  || std::is_same_v<Alternative, binary::ImportDefinition>
                                  || std::is_same_v<Alternative, binary::ExportDeclaration>
                                  || std::is_same_v<Alternative, binary::CoreModuleDefinition>
                                  || std::is_same_v<Alternative, binary::CustomDefinition>);
                    Require((std::is_same_v<Alternative, binary::CoreTypeDefinition>) );
                    return DeferredForm::CoreTypeDefinitions;
                }
            },
            definition);
        if (!scanned)
            return std::unexpected { scanned.error() };
        return std::unexpected { deferredError(*scanned, position) };
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
        case ValidationErrorDetail::Deferred: {
            Require(subject < DeferredFormTable.size());
            return std::vformat(row.text, std::make_format_args(DeferredFormTable[subject].text));
        }
        case ValidationErrorDetail::Refusal: return error.name;
        case ValidationErrorDetail::Label: {
            Require(subject < LabelKindTable.size());
            auto const& kind = LabelKindTable[subject];
            return std::vformat(
                row.text, std::make_format_args(kind.description, error.name, kind.noun, error.previous));
        }
        case ValidationErrorDetail::Limit: {
            auto const limit = exceededLimit(error);
            Require(limit.has_value());
            return limitMessage(*limit, subject >> 8U);
        }
    }
    return std::string { row.text };
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
                             .type = validator.takeRootType(),
                             .coreModules = validator.takeCoreModules() });
}

} // namespace isoline::component::validate
