// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/api/Linker.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/binary/Component.hpp>
#include <isoline/component/canonical/Unicode.hpp>
#include <isoline/component/validate/Names.hpp>
#include <isoline/component/validate/Subtyping.hpp>

namespace isoline::component::api
{

namespace
{

    /// The word a mismatch names a definition, or an import's sort, by: wasm-tools' words.
    enum class EntityWord : uint8_t
    {
        Function,
        Resource,
        Instance,
        Module,
        Component,
        Value,
        Type,
    };

    /// One row of EntityWords: an entity and its word.
    struct EntityWordRow
    {
        EntityWord entity;     ///< The entity; equal to the row's index.
        std::string_view word; ///< Its word.
    };

    /// Every entity's word, in EntityWord's order.
    constexpr auto EntityWords = std::to_array<EntityWordRow>({
        { .entity = EntityWord::Function, .word = "func" },
        { .entity = EntityWord::Resource, .word = "resource" },
        { .entity = EntityWord::Instance, .word = "instance" },
        { .entity = EntityWord::Module, .word = "module" },
        { .entity = EntityWord::Component, .word = "component" },
        { .entity = EntityWord::Value, .word = "value" },
        { .entity = EntityWord::Type, .word = "type" },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: EntityWords)
            {
                if (static_cast<std::size_t>(row.entity) != index)
                    return false;
                ++index;
            }
            return index == static_cast<std::size_t>(EntityWord::Type) + 1;
        }(),
        "EntityWords must hold one row per EntityWord, at the index of its enumerator");

    [[nodiscard]] constexpr std::string_view textOf(EntityWord entity) noexcept
    {
        return EntityWords[static_cast<std::size_t>(entity)].word;
    }

    /// The text of a relation row, formatted with up to two strings.
    template <typename... Args>
    [[nodiscard]] std::string relationText(validate::RelationErrorCode code, Args const&... args)
    {
        return std::vformat(validate::RelationErrorMessages[static_cast<std::size_t>(code)].text,
                            std::make_format_args(args...));
    }

    /// The text of a relation context line, formatted with up to two strings.
    template <typename... Args>
    [[nodiscard]] std::string contextText(validate::RelationContext context, Args const&... args)
    {
        return std::vformat(validate::RelationContextMessages[static_cast<std::size_t>(context)].text,
                            std::make_format_args(args...));
    }

    /// Whether @p text is well-formed UTF-8.
    [[nodiscard]] bool isUtf8(std::string_view text) noexcept
    {
        auto reader =
            canonical::CodePointReader { std::as_bytes(std::span { text }), canonical::SimpleEncoding::Utf8 };
        for (;;)
        {
            auto next = reader.next();
            if (!next)
                return false;
            if (!next->has_value())
                return true;
        }
    }

    /// @p parent's child @p segment: the path of a segment under a linker instance ("" is the root).
    [[nodiscard]] std::string childPath(std::string_view parent, std::string_view segment)
    {
        if (parent.empty())
            return std::string { segment };
        return std::format("{}{}{}", parent, PathSeparator, segment);
    }

    [[nodiscard]] LinkerError linkerError(LinkerErrorCode code, std::string path, std::string detail = {})
    {
        return LinkerError { .code = code, .path = std::move(path), .detail = std::move(detail) };
    }

    /// The `implements` interface of each root import, in import order (🏷️): what an instance import
    /// falls back to when no definition has its own name.
    [[nodiscard]] std::vector<std::optional<std::string>> implementsOf(Component const& component)
    {
        auto implements = std::vector<std::optional<std::string>> {};
        for (auto const& definition: component.validated().pool.components.front().definitions)
        {
            auto const* import = std::get_if<binary::ImportDefinition>(&definition);
            if (import == nullptr)
                continue;
            auto& entry = implements.emplace_back();
            for (auto const& attribute: import->name.attributes)
                if (auto const* interface = std::get_if<binary::Implements>(&attribute))
                    entry = interface->interfaceName;
        }
        return implements;
    }

    /// Whether every export of an instance type is a type that is not a resource: an instance of types
    /// only (D8, WIT's `use types.{…}`), which needs no definition.
    [[nodiscard]] bool isTypesOnly(types::TypeArena const& arena, types::InstanceTypeId instance)
    {
        return std::ranges::all_of(arena[instance].exports, [](types::NamedExtern const& entry) {
            auto const* type = std::get_if<types::TypeExtern>(&entry.type);
            return type != nullptr && !std::holds_alternative<types::ResourceTypeId>(type->type);
        });
    }

    /// The resource a type extern binds, if it binds one.
    [[nodiscard]] std::optional<types::ResourceTypeId> boundResource(types::ExternType const& type)
    {
        auto const* typeExtern = std::get_if<types::TypeExtern>(&type);
        if (typeExtern == nullptr)
            return std::nullopt;
        if (auto const* resource = std::get_if<types::ResourceTypeId>(&typeExtern->type))
            return *resource;
        return std::nullopt;
    }

} // namespace

std::string LinkerError::message() const
{
    return std::vformat(LinkerErrorTable[static_cast<std::size_t>(code)].text,
                        std::make_format_args(path, detail));
}

/// One resolution of a component's imports against a linker: each root import resolved on its own, an
/// instance's exports on an explicit stack (nothing recurses on the component's types), its values
/// appended so that an instance's exports precede it.
class Linker::Walk
{
  public:
    Walk(Linker const& linker, Component const& component, StoreState* store, Resolution& out):
        _linker { linker }, _component { component }, _store { store }, _out { out }
    {
    }

    void run()
    {
        auto const implements = implementsOf(_component);
        for (auto const [position, import]: core::views::enumerate(_component.imports()))
        {
            auto const& name = import.name.str();
            auto const interface =
                position < implements.size() ? implements[position] : std::optional<std::string> {};
            auto outcome = resolveImport(name, import.type, interface);
            if (_store != nullptr && !_out.refusal)
            {
                if (auto const* unsatisfied = std::get_if<Unsatisfied>(&outcome); unsatisfied != nullptr)
                    _out.refusal = UnsatisfiedImport { .name = name };
                else if (auto const* mismatch = std::get_if<TypeMismatch>(&outcome))
                    _out.refusal = ImportTypeMismatch { .name = name, .detail = mismatch->detail };
                else if (_deferred)
                {
                    if (auto* foreign = std::get_if<ForeignInstance>(&*_deferred))
                        foreign->name = name;
                    _out.refusal = std::move(*_deferred);
                }
            }
            _deferred.reset();
            _out.outcomes.push_back(ImportResolution { .name = name, .outcome = std::move(outcome) });
        }
    }

  private:
    /// A failure to resolve an extern: none defined, or one of another type.
    using Failure = std::variant<Unsatisfied, TypeMismatch>;

    /// An implicit linker instance whose exports are being resolved.
    struct Pending
    {
        std::string path;                     ///< Its linker path.
        types::InstanceTypeId type;           ///< The instance type required of it.
        std::string name {};                  ///< The export its parent records it as; empty for a root.
        std::size_t next = 0;                 ///< The next export of @ref type to resolve.
        runtime::HostInstanceImport value {}; ///< Its exports, resolved.
    };

    [[nodiscard]] types::TypeArena const& componentTypes() const noexcept { return _component.types(); }

    void defer(InstantiateComponentError refusal)
    {
        if (!_deferred)
            _deferred = std::move(refusal);
    }

    [[nodiscard]] uint32_t push(runtime::ResolvedValue value)
    {
        _out.imports.values.push_back(std::move(value));
        return static_cast<uint32_t>(_out.imports.values.size() - 1);
    }

    void record(uint32_t value) { _out.imports.imports.push_back(value); }

    [[nodiscard]] ImportOutcome resolveImport(std::string const& name,
                                              types::ExternType const& type,
                                              std::optional<std::string> const& implements)
    {
        auto matched = _linker.lookup({}, name);
        if (!matched && implements && std::holds_alternative<types::InstanceTypeId>(type))
            matched = _linker.lookup({}, *implements);
        if (!matched)
        {
            if (auto const satisfied = byTypes(type))
            {
                record(*satisfied);
                return SatisfiedByTypes {};
            }
            record(push(std::monostate {}));
            return Unsatisfied {};
        }
        auto resolved = resolveAt(*matched, type);
        if (!resolved)
        {
            record(push(std::monostate {}));
            return std::visit(core::Overloaded {
                                  [](Unsatisfied const& unsatisfied) -> ImportOutcome { return unsatisfied; },
                                  [&](TypeMismatch mismatch) -> ImportOutcome {
                                      mismatch.matched = *matched;
                                      return mismatch;
                                  },
                              },
                              std::move(resolved.error()));
        }
        record(*resolved);
        return Satisfied { .matched = *matched };
    }

    /// The value of an import of types only, which needs no definition; none for any other import.
    [[nodiscard]] std::optional<uint32_t> byTypes(types::ExternType const& type)
    {
        if (auto const* instance = std::get_if<types::InstanceTypeId>(&type);
            instance != nullptr && isTypesOnly(componentTypes(), *instance))
            return push(std::monostate {});
        if (std::holds_alternative<types::TypeExtern>(type) && !boundResource(type))
            return push(std::monostate {});
        return std::nullopt;
    }

    [[nodiscard]] static TypeMismatch mismatchOf(std::string detail)
    {
        return TypeMismatch { .matched = {}, .detail = std::move(detail) };
    }

    [[nodiscard]] static TypeMismatch kindMismatch(EntityWord expected, EntityWord found)
    {
        return mismatchOf(
            relationText(validate::RelationErrorCode::KindMismatch, textOf(expected), textOf(found)));
    }

    [[nodiscard]] static EntityWord wordOf(Definition const& definition) noexcept
    {
        return std::visit(core::Overloaded {
                              [](FunctionDefinition const&) { return EntityWord::Function; },
                              [](ResourceDefinition const&) { return EntityWord::Resource; },
                              [](InstanceDefinition const&) { return EntityWord::Instance; },
                              [](ModuleDefinition const&) { return EntityWord::Module; },
                              [](ComponentDefinition const&) { return EntityWord::Component; },
                          },
                          definition);
    }

    [[nodiscard]] static EntityWord wordOf(types::ExternType const& type) noexcept
    {
        return std::visit(core::Overloaded {
                              [](types::FunctionTypeId) { return EntityWord::Function; },
                              [](types::ValueExtern const&) { return EntityWord::Value; },
                              [](types::TypeExtern const& typeExtern) {
                                  return std::holds_alternative<types::ResourceTypeId>(typeExtern.type)
                                             ? EntityWord::Resource
                                             : EntityWord::Type;
                              },
                              [](types::ComponentTypeId) { return EntityWord::Component; },
                              [](types::InstanceTypeId) { return EntityWord::Instance; },
                              [](types::CoreModuleTypeId) { return EntityWord::Module; },
                          },
                          type);
    }

    /// Resolves the definition at @p path, or the linker instance there, against @p type.
    /// @return The index of its value; or why not.
    [[nodiscard]] std::expected<uint32_t, Failure> resolveAt(std::string const& path,
                                                             types::ExternType const& type)
    {
        auto const* definition = _linker.find(path);
        if (auto const* instance = std::get_if<types::InstanceTypeId>(&type))
        {
            if (definition == nullptr)
                return resolveImplicit(path, *instance);
            if (auto const* store = std::get_if<InstanceDefinition>(definition))
                return resolveStoreInstance(*store, *instance);
            return std::unexpected { kindMismatch(EntityWord::Instance, wordOf(*definition)) };
        }
        if (definition == nullptr)
            return std::unexpected { kindMismatch(wordOf(type), EntityWord::Instance) };
        return resolveLeaf(*definition, type);
    }

    /// Whether an unnamed host function (ParameterNames::Unnamed) satisfies a function import: the rules of
    /// TypeRelation's function comparison (wasm-tools' component_func_type) but the parameter names, which
    /// it has none of. Asyncness, then the parameter count, then each parameter's type and the result,
    /// value types compared by TypeRelation::isEqual (component_val_type).
    /// @param provided The host function's type, of the linker's arena.
    /// @param required The import's, of the component's arena.
    /// @return Nothing when it does, else why not, in wasm-tools' words.
    [[nodiscard]] std::expected<void, std::string> relateUnnamed(types::FunctionTypeId provided,
                                                                 types::FunctionTypeId required)
    {
        auto const& actual = _linker._arena[provided];
        auto const& expected = componentTypes()[required];
        // A host function is sync (FunctionTypeSpec has no asyncness), and an async import is refused at
        // validation until P12 (ComponentFeature::Async), so the two never differ yet.
        if (actual.asyncness != expected.asyncness)
        // uncovered-begin(unnamed-async)
        {
            auto const word = [](binary::Asyncness asyncness) {
                return std::string { asyncness == binary::Asyncness::Async ? "async" : "sync" };
            };
            return std::unexpected { relationText(
                validate::RelationErrorCode::Asyncness, word(actual.asyncness), word(expected.asyncness)) };
        }
        // uncovered-end(unnamed-async)
        if (actual.params.size() != expected.params.size())
            return std::unexpected { relationText(validate::RelationErrorCode::ParameterCount,
                                                  std::to_string(expected.params.size()),
                                                  std::to_string(actual.params.size())) };
        auto relation =
            validate::TypeRelation { _linker._arena, componentTypes(), _resources, _linker._config.limits };
        // A parameter's context names the import's parameter: the host function's labels name nothing.
        for (auto const index: std::views::iota(std::size_t { 0 }, actual.params.size()))
            if (auto equal = relation.isEqual(actual.params[index].second, expected.params[index].second);
                !equal)
                return std::unexpected { std::format("{}\n{}",
                                                     contextText(validate::RelationContext::FunctionParameter,
                                                                 expected.params[index].first.str()),
                                                     equal.error().message) };
        if (actual.result.has_value() != expected.result.has_value())
            return std::unexpected { relationText(actual.result
                                                      ? validate::RelationErrorCode::ResultMissing
                                                      : validate::RelationErrorCode::ResultPresent) };
        if (actual.result)
            if (auto equal = relation.isEqual(*actual.result, *expected.result); !equal)
                return std::unexpected { std::format("{}\n{}",
                                                     contextText(validate::RelationContext::FunctionResult),
                                                     equal.error().message) };
        return {};
    }

    /// A definition of anything but an instance against @p type.
    [[nodiscard]] std::expected<uint32_t, Failure> resolveLeaf(Definition const& definition,
                                                               types::ExternType const& type)
    {
        auto const expected = wordOf(type);
        auto const found = wordOf(definition);
        if (std::holds_alternative<types::TypeExtern>(type) && !boundResource(type))
            return push(std::monostate {}); // a type, not a resource: nothing at run time
        if (expected != found)
            return std::unexpected { kindMismatch(expected, found) };
        return std::visit(
            core::Overloaded {
                [&](FunctionDefinition const& function) -> std::expected<uint32_t, Failure> {
                    if (function.names == ParameterNames::Unnamed)
                    {
                        if (auto related =
                                relateUnnamed(function.type, std::get<types::FunctionTypeId>(type));
                            !related)
                            return std::unexpected { mismatchOf(std::move(related.error())) };
                        return push(callee(function.function));
                    }
                    auto relation = validate::TypeRelation {
                        _linker._arena, componentTypes(), _resources, _linker._config.limits
                    };
                    if (auto related = relation.isSubtype(types::ExternType { function.type }, type);
                        !related)
                        return std::unexpected { mismatchOf(std::move(related.error().message)) };
                    return push(callee(function.function));
                },
                [&](ResourceDefinition const& resource) -> std::expected<uint32_t, Failure> {
                    auto const required = *boundResource(type);
                    if (auto const bound = _resources.find(required); bound && *bound != resource.resource)
                        return std::unexpected { mismatchOf(
                            relationText(validate::RelationErrorCode::ResourceMismatch)) };
                    _resources.bind(required, resource.resource);
                    return push(runtime::HostResourceImport { .identity = resource.type.identity(),
                                                              .destructor = resource.type.destructor() });
                },
                [&](ModuleDefinition const& module) -> std::expected<uint32_t, Failure> {
                    // Its imports and exports are the core instantiation's to check (R7); its engine
                    // the store's, before anything runs.
                    if (_store != nullptr && module.module.engine() != _store->engine)
                        defer(isoline::api::EngineMismatch {});
                    return push(module.module);
                },
                [&](ComponentDefinition const& component) -> std::expected<uint32_t, Failure> {
                    auto const& provided = component.component;
                    auto relation = validate::TypeRelation { provided.types(),
                                                             componentTypes(),
                                                             validate::ResourceMapping {},
                                                             _linker._config.limits };
                    // defineComponent() admits only a component whose type is an arena entry.
                    auto const own = types::ExternType { *provided.validated().typeId };
                    if (auto related = relation.isSubtype(own, type); !related)
                        return std::unexpected { mismatchOf(std::move(related.error().message)) };
                    if (_store != nullptr && provided.engine() != _store->engine)
                        defer(isoline::api::EngineMismatch {});
                    return push(runtime::ComponentImport { .code = provided.code() });
                },
                [](InstanceDefinition const&) -> std::expected<uint32_t, Failure> {
                    // uncovered-begin(instance-is-not-a-leaf)
                    return std::unexpected { Unsatisfied {} };
                },
                // uncovered-end(instance-is-not-a-leaf)
            },
            definition);
    }

    /// An instance of a store against @p type: its component's exports, each a subtype of the export
    /// the instance type requires, its resources bound by name first.
    [[nodiscard]] std::expected<uint32_t, Failure> resolveStoreInstance(InstanceDefinition const& definition,
                                                                        types::InstanceTypeId type)
    {
        auto const& instance = definition.instance;
        auto const& provided = instance.component();
        auto const exports = provided.exports();
        auto const findExport = [&](std::string const& name) {
            return std::ranges::find_if(
                exports, [&](types::NamedExtern const& entry) { return entry.name.str() == name; });
        };
        auto resources = validate::ResourceMapping {};
        for (auto const& required: componentTypes()[type].exports)
        {
            auto const found = findExport(required.name.str());
            if (found == exports.end())
                return std::unexpected { mismatchOf(
                    relationText(validate::RelationErrorCode::MissingExport, required.name.str())) };
            if (auto const resource = boundResource(required.type))
                if (auto const supplied = boundResource(found->type))
                    resources.bind(*resource, *supplied);
        }
        for (auto const& required: componentTypes()[type].exports)
        {
            auto relation = validate::TypeRelation {
                provided.types(), componentTypes(), resources, _linker._config.limits
            };
            if (auto related = relation.isSubtype(findExport(required.name.str())->type, required.type);
                !related)
                return std::unexpected { mismatchOf(std::format(
                    "{}\n{}",
                    std::vformat(
                        validate::RelationContextMessages[static_cast<std::size_t>(
                                                              validate::RelationContext::InstanceExport)]
                            .text,
                        std::make_format_args(required.name.str())),
                    related.error().message)) };
        }
        if (_store == nullptr)
            return push(std::monostate {});
        if (serialOf(instance) != _store->serial)
        {
            defer(ForeignInstance {});
            return push(std::monostate {});
        }
        return push(_store->runtime.instanceValueOf(numberOf(instance)));
    }

    /// A linker instance (the definitions under @p path) against @p type, its nested instances on an
    /// explicit stack.
    [[nodiscard]] std::expected<uint32_t, Failure> resolveImplicit(std::string const& path,
                                                                   types::InstanceTypeId type)
    {
        auto stack = std::vector<Pending> {};
        stack.push_back(Pending { .path = path, .type = type });
        // A failure at the export the innermost frame resolves: every frame names the export it is at,
        // but a missing export is named by the mismatch itself, so its own frame adds no context.
        auto const fail = [&](Failure failure,
                              std::size_t contextFrames) -> std::expected<uint32_t, Failure> {
            auto* mismatch = std::get_if<TypeMismatch>(&failure);
            if (mismatch == nullptr)
                return std::unexpected { std::move(failure) }; // uncovered(unsatisfied-inside)
            // Contexts outermost first, as wasm-tools reports them.
            auto contexts = std::string {};
            for (auto const& frame: std::span { stack }.first(contextFrames))
            {
                auto const& exports = componentTypes()[frame.type].exports;
                contexts += std::vformat(
                    validate::RelationContextMessages[static_cast<std::size_t>(
                                                          validate::RelationContext::InstanceExport)]
                        .text,
                    std::make_format_args(exports[frame.next].name.str()));
                contexts += '\n';
            }
            mismatch->detail = contexts + mismatch->detail;
            return std::unexpected { std::move(failure) };
        };
        for (;;)
        {
            auto& frame = stack.back();
            auto const& exports = componentTypes()[frame.type].exports;
            if (frame.next == exports.size())
            {
                auto const value = push(std::move(frame.value));
                auto const name = std::move(frame.name);
                stack.pop_back();
                if (stack.empty())
                    return value;
                stack.back().value.exports.emplace_back(name, value);
                ++stack.back().next;
                continue;
            }
            auto const& required = exports[frame.next];
            auto const& name = required.name.str();
            auto const child = _linker.lookup(frame.path, name);
            if (!child)
            {
                if (auto const satisfied = byTypes(required.type))
                {
                    frame.value.exports.emplace_back(name, *satisfied);
                    ++frame.next;
                    continue;
                }
                return fail(mismatchOf(relationText(validate::RelationErrorCode::MissingExport, name)),
                            stack.size() - 1);
            }
            auto const* definition = _linker.find(*child);
            if (auto const* nested = std::get_if<types::InstanceTypeId>(&required.type);
                nested != nullptr && definition == nullptr)
            {
                auto const nestedType = *nested;
                auto pending = Pending { .path = *child, .type = nestedType, .name = name };
                stack.push_back(std::move(pending));
                continue;
            }
            auto resolved = resolveAt(*child, required.type);
            if (!resolved)
                return fail(std::move(resolved.error()), stack.size());
            frame.value.exports.emplace_back(name, *resolved);
            ++frame.next;
        }
    }

    [[nodiscard]] std::shared_ptr<runtime::IHostCallee> callee(
        std::shared_ptr<IHostComponentFunction> const& function)
    {
        if (_store == nullptr)
            return nullptr;
        return hostCallee(*_store, function);
    }

    Linker const& _linker;
    Component const& _component;
    StoreState* _store;
    Resolution& _out;
    validate::ResourceMapping _resources {}; ///< The component's resources bound to the linker's.
    /// What the import being resolved met that only a store refuses: an instance of another store, a
    /// module or component of another engine. The first one met.
    std::optional<InstantiateComponentError> _deferred {};
};

Linker::Linker(ComponentConfig config): _config { config }, _arena { _config.limits }
{
}

Linker::Linker(Linker&&) noexcept = default;
Linker& Linker::operator=(Linker&&) noexcept = default;
Linker::~Linker() = default;

std::expected<std::vector<std::string>, LinkerError> Linker::checkPath(std::string const& path) const
{
    auto segments = std::vector<std::string> {};
    for (auto const segment: std::views::split(std::string_view { path }, PathSeparator))
    {
        auto const text = std::string_view { segment.begin(), segment.end() };
        // The grammar refuses an empty segment too.
        if (auto parsed = validate::parseExternName(text, _config.features); !parsed)
            return std::unexpected { linkerError(
                LinkerErrorCode::InvalidPath, path, validate::nameErrorMessage(parsed.error())) };
        segments.emplace_back(text);
    }
    if (segments.empty())
        return std::unexpected { linkerError(LinkerErrorCode::InvalidPath, path, "empty segment") };
    if (_definitions.contains(path))
        return std::unexpected { linkerError(LinkerErrorCode::Redefinition, path) };
    if (_instances.contains(path))
    {
        auto const inside = _definitions.lower_bound(path + PathSeparator);
        return std::unexpected { linkerError(LinkerErrorCode::PathConflict, path, inside->first) };
    }
    auto prefix = std::string {};
    for (auto const& segment: std::span { segments }.first(segments.size() - 1))
    {
        prefix = childPath(prefix, segment);
        if (_definitions.contains(prefix))
            return std::unexpected { linkerError(LinkerErrorCode::PathConflict, path, prefix) };
    }
    return segments;
}

std::expected<void, LinkerError> Linker::define(std::string path, Definition definition)
{
    auto segments = checkPath(path);
    if (!segments)
        return std::unexpected { std::move(segments.error()) };
    auto parent = std::string {};
    for (auto const& segment: std::span { *segments }.first(segments->size() - 1))
    {
        _children[parent].insert(segment);
        parent = childPath(parent, segment);
        _instances.insert(parent);
    }
    _children[parent].insert(segments->back());
    _definitions.emplace(std::move(path), std::move(definition));
    return {};
}

Linker::Definition const* Linker::find(std::string_view path) const
{
    auto const found = _definitions.find(path);
    return found == _definitions.end() ? nullptr : &found->second;
}

std::optional<std::string> Linker::lookup(std::string const& parent, std::string_view name) const
{
    auto exact = childPath(parent, name);
    if (_definitions.contains(exact) || _instances.contains(exact))
        return exact;
    // An interface name of a version: the highest compatible version defined (D8).
    auto required = validate::parseExternName(name, _config.features);
    if (!required || required->kind != validate::NameKind::Interface || !required->version)
        return std::nullopt;
    auto const children = _children.find(parent);
    if (children == _children.end())
        return std::nullopt;
    auto best = std::optional<std::pair<validate::SemanticVersion, std::string_view>> {};
    for (auto const& segment: children->second)
    {
        auto const provided = validate::parseExternName(segment, _config.features);
        if (!provided || provided->kind != validate::NameKind::Interface || !provided->version
            || provided->package != required->package || provided->interface != required->interface
            || !validate::isSemverCompatible(*required->version, *provided->version))
            continue;
        if (!best || best->first < *provided->version)
            best.emplace(*provided->version, segment);
    }
    if (!best)
        return std::nullopt;
    return childPath(parent, best->second);
}

std::expected<types::ResourceTypeId, LinkerError> Linker::resourceOf(HostResourceType const& type,
                                                                     std::string const& path)
{
    if (!isUtf8(type.name()))
        return std::unexpected { linkerError(LinkerErrorCode::InvalidResourceName, path) };
    if (auto const found = _resources.find(type.identity().get()); found != _resources.end())
        return found->second;
    auto resource = _arena.newResource(types::ResourceTypeInfo {});
    if (!resource)
        return std::unexpected { linkerError(
            LinkerErrorCode::TypeLimit,
            path,
            std::string { LimitTable[static_cast<std::size_t>(resource.error().limit)].name }) };
    _identities.push_back(type.identity());
    _resources.emplace(type.identity().get(), *resource);
    return *resource;
}

std::expected<types::TypeId, LinkerError> Linker::handleType(HostResourceType const& type,
                                                             types::ValueKind kind)
{
    auto resource = resourceOf(type, type.name());
    if (!resource)
        return std::unexpected { std::move(resource.error()) };
    auto interned = _arena.intern(types::DefinedType { .kind = kind, .resource = *resource });
    if (!interned)
        // uncovered-begin(handle-measure)
        return std::unexpected { linkerError(
            LinkerErrorCode::TypeLimit,
            type.name(),
            std::string { LimitTable[static_cast<std::size_t>(interned.error().limit)].name }) };
    // uncovered-end(handle-measure)
    return *interned;
}

std::expected<types::TypeId, LinkerError> Linker::ownType(HostResourceType const& type)
{
    return handleType(type, types::ValueKind::Own);
}

std::expected<types::TypeId, LinkerError> Linker::borrowType(HostResourceType const& type)
{
    return handleType(type, types::ValueKind::Borrow);
}

std::expected<void, LinkerError> Linker::defineFunction(std::string path,
                                                        FunctionTypeSpec const& type,
                                                        std::shared_ptr<IHostComponentFunction> function)
{
    if (function == nullptr)
        return std::unexpected { linkerError(LinkerErrorCode::NullDefinition, std::move(path)) };
    if (auto checked = checkPath(path); !checked)
        return std::unexpected { std::move(checked.error()) };
    auto const isOfArena = [&](types::TypeId id) {
        return id.value < _arena.valueTypeCount();
    };
    auto info = types::FunctionTypeInfo {};
    for (auto const& [label, id]: type.params)
    {
        if (!validate::isKebabCase(label) || std::ranges::any_of(info.params, [&](auto const& param) {
                return param.first.str() == label;
            }))
            return std::unexpected { linkerError(
                LinkerErrorCode::InvalidParameterName, std::move(path), label) };
        if (!isOfArena(id))
            return std::unexpected { linkerError(
                LinkerErrorCode::InvalidType, std::move(path), std::format("parameter `{}`", label)) };
        info.params.emplace_back(label, id);
    }
    if (type.result && !isOfArena(*type.result))
        return std::unexpected { linkerError(LinkerErrorCode::InvalidType, std::move(path), "the result") };
    info.result = type.result;
    auto added = _arena.add(std::move(info));
    if (!added)
        return std::unexpected { linkerError(
            LinkerErrorCode::TypeLimit,
            std::move(path),
            std::string { LimitTable[static_cast<std::size_t>(added.error().limit)].name }) };
    return define(
        std::move(path),
        FunctionDefinition { .type = *added, .function = std::move(function), .names = type.names });
}

std::expected<void, LinkerError> Linker::defineResource(std::string path, HostResourceType type)
{
    if (auto checked = checkPath(path); !checked)
        return std::unexpected { std::move(checked.error()) };
    auto resource = resourceOf(type, path);
    if (!resource)
        return std::unexpected { std::move(resource.error()) };
    return define(std::move(path), ResourceDefinition { .type = std::move(type), .resource = *resource });
}

std::expected<void, LinkerError> Linker::defineInstance(std::string path, ComponentInstance const& instance)
{
    return define(std::move(path), InstanceDefinition { .instance = instance });
}

std::expected<void, LinkerError> Linker::defineModule(std::string path, isoline::api::Module const& module)
{
    return define(std::move(path), ModuleDefinition { .module = module });
}

std::expected<void, LinkerError> Linker::defineComponent(std::string path, Component const& component)
{
    if (!component.validated().typeId)
        // uncovered-begin(root-type-cap)
        return std::unexpected { linkerError(LinkerErrorCode::UntypedComponent, std::move(path)) };
    // uncovered-end(root-type-cap)
    return define(std::move(path), ComponentDefinition { .component = component });
}

std::shared_ptr<runtime::IHostCallee> Linker::hostCallee(
    StoreState& store, std::shared_ptr<IHostComponentFunction> const& function)
{
    return ComponentStore::hostCallee(store, function);
}

Linker::Resolution Linker::resolveFor(Component const& component, StoreState* store) const
{
    auto resolution = Resolution {};
    auto walk = Walk { *this, component, store, resolution };
    walk.run();
    return resolution;
}

std::vector<ImportResolution> Linker::resolve(Component const& component) const
{
    return resolveFor(component, nullptr).outcomes;
}

} // namespace isoline::component::api
