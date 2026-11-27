// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/runtime/ComponentStore.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/runtime/Calls.hpp>

namespace isoline::component::runtime
{

using canonical::Trap;
using canonical::TrapCode;
using canonical::trapOf;

namespace
{
    /// The index of @p sort in a frame's slots.
    [[nodiscard]] constexpr std::size_t sortIndex(binary::Sort sort) noexcept
    {
        return static_cast<std::size_t>(sort);
    }

    /// A failure of @p kind with @p detail.
    template <typename Detail = std::monostate>
    [[nodiscard]] std::unexpected<InstantiationFailure> fail(InstantiationFailureKind kind,
                                                             Detail detail = {})
    {
        return std::unexpected { InstantiationFailure { .kind = kind, .detail = std::move(detail) } };
    }

    /// The slot value an extern is.
    [[nodiscard]] SlotValue slotValueOf(isoline::api::Extern const& value)
    {
        return std::visit([](auto const& entity) { return SlotValue { entity }; }, value);
    }

    /// The entry named @p name of a list ordered by name.
    template <typename Entry>
    [[nodiscard]] Entry const* findByName(std::span<Entry const> entries, std::string_view name)
    {
        auto const found = std::ranges::lower_bound(
            entries, name, {}, [](Entry const& entry) { return std::string_view { entry.name }; });
        if (found == entries.end() || std::string_view { found->name } != name)
            return nullptr;
        return &*found;
    }

    /// A core export, by name.
    struct CoreExport
    {
        std::string_view name;      ///< Its name.
        isoline::api::Extern value; ///< The entity.
    };

    /// The store limits a component store's core store keeps to: its engine's, nesting at most
    /// maxNestedCalls invocations, which only ever tightens the engine's maxNestedInvocations: an
    /// embedder that chose a lower one for a small stack keeps it.
    [[nodiscard]] isoline::api::StoreOptions storeOptions(isoline::api::Engine const& engine,
                                                          ComponentLimits const& limits,
                                                          isoline::api::IPollObserver* observer) noexcept
    {
        auto store = engine.config().store;
        store.maxNestedInvocations = std::min(store.maxNestedInvocations, limits.maxNestedCalls);
        return isoline::api::StoreOptions { .limits = store, .observer = observer };
    }
} // namespace

/// One instantiate() in progress: the explicit stack of frames (D1), the steps counted, and the root
/// imports converted to slot values.
struct ComponentStore::Instantiation
{
    /// A frame on the stack, with the step it runs next.
    struct Pending
    {
        Frame* frame = nullptr;  ///< The frame.
        std::size_t next = 0;    ///< Its next step.
        Frame* parent = nullptr; ///< The frame instantiating it; null for the root.
        InstantiateComponent const* instantiation = nullptr; ///< The parent's step; null for the root.
    };

    ComponentStore& store;
    std::vector<SlotValue> imports {}; ///< ResolvedImports::values, converted.
    std::vector<Pending> stack {};
    uint64_t steps = 0; ///< Steps and slots charged against maxInstantiationSteps.

    /// Charges @p count steps, before what they count is done.
    [[nodiscard]] std::expected<void, InstantiationFailure> charge(uint64_t count)
    {
        if (count > store._limits.maxInstantiationSteps
                        - std::min<uint64_t>(steps, store._limits.maxInstantiationSteps))
            return fail(InstantiationFailureKind::LimitExceeded, ComponentLimit::InstantiationSteps);
        steps += count;
        return {};
    }

    /// The plan @p frame runs.
    [[nodiscard]] static ComponentPlan const& planOf(Frame const& frame) noexcept
    {
        return frame.component.code->plans->plans[frame.component.plan];
    }

    /// The index spaces of the scope @p frame runs.
    [[nodiscard]] static validate::ScopeInfo const& scopeOf(Frame const& frame) noexcept
    {
        return frame.component.code->validated->scopes[frame.component.plan];
    }

    /// The slot of @p sort and @p index of @p frame.
    [[nodiscard]] static SlotValue& slot(Frame& frame, binary::Sort sort, uint32_t index) noexcept
    {
        auto& slots = frame.slots[sortIndex(sort)];
        Require(index < slots.size());
        return slots[index];
    }

    /// Creates a frame running @p component, counting its instance and charging its slots first.
    [[nodiscard]] std::expected<Frame*, InstantiationFailure> createFrame(ComponentRef component)
    {
        if (store._resources.instances().size() >= store._limits.maxComponentInstances)
            return fail(InstantiationFailureKind::LimitExceeded, ComponentLimit::ComponentInstances);
        auto const& plan = component.code->plans->plans[component.plan];
        auto slotCount = uint64_t { 0 };
        for (auto const count: plan.slots.bySort)
            slotCount += count;
        if (auto charged = charge(slotCount); !charged)
            return std::unexpected { std::move(charged.error()) };
        auto& frame = store._frames.emplace_back();
        frame.component = component;
        for (auto const [index, count]: core::views::enumerate(plan.slots.bySort))
            frame.slots[index].resize(count);
        frame.instance = store._resources.instances().add();
        store._bindings.emplace_back();
        store._roots.push_back(UnfinishedInstance);
        return &frame;
    }

    /// Binds the resources a filled slot names (D5): a resource type slot to its runtime type; an
    /// instance slot's resource exports, through every nested instance, each walk step charged.
    [[nodiscard]] std::expected<void, InstantiationFailure> bind(Frame& frame,
                                                                 binary::Sort sort,
                                                                 uint32_t index)
    {
        auto const& scope = scopeOf(frame);
        auto& bindings = store._bindings[frame.instance];
        auto const& value = slot(frame, sort, index);
        if (sort == binary::Sort::Type)
        {
            Require(index < scope.types.size());
            auto const* resource = std::get_if<types::ResourceTypeId>(&scope.types[index].type);
            if (resource == nullptr)
                return {};
            auto const* type = std::get_if<ResourceTypeIndex>(&value);
            if (type == nullptr)
                return fail(InstantiationFailureKind::ImportMismatch); // uncovered(validated-slot)
            bindings.bind(*resource, *type);
            return {};
        }
        if (sort != binary::Sort::Instance)
            return {};
        Require(index < scope.instances.size());
        auto const& arena = frame.component.code->validated->types;
        auto const* instance = std::get_if<InstanceRef>(&value);
        if (instance == nullptr)
        {
            // An instance of types only (D8) has no value: fine while none of its types is a resource.
            if (std::holds_alternative<std::monostate>(value)
                && arena[scope.instances[index]].resources == types::ResourceReference::None)
                return {};
            return fail(InstantiationFailureKind::ImportMismatch); // uncovered(validated-slot)
        }
        auto work = std::vector<std::pair<types::InstanceTypeId, InstanceRef>> { { scope.instances[index],
                                                                                   *instance } };
        while (!work.empty())
        {
            auto const [type, ref] = work.back();
            work.pop_back();
            auto const& info = arena[type];
            if (info.resources == types::ResourceReference::None)
                continue;
            if (auto charged = charge(info.exports.size()); !charged)
                return charged;
            auto const exports = store.exportsOf(ref);
            for (auto const& entry: info.exports)
            {
                auto const* found = findByName(exports, std::string_view { entry.name.str() });
                if (auto const* typeExtern = std::get_if<types::TypeExtern>(&entry.type))
                {
                    auto const* resource = std::get_if<types::ResourceTypeId>(&typeExtern->type);
                    if (resource == nullptr)
                        continue;
                    auto const* runtimeType =
                        found != nullptr ? std::get_if<ResourceTypeIndex>(&found->value) : nullptr;
                    if (runtimeType == nullptr)
                        return fail(InstantiationFailureKind::ImportMismatch);
                    bindings.bind(*resource, *runtimeType);
                }
                else if (auto const* nested = std::get_if<types::InstanceTypeId>(&entry.type))
                {
                    auto const* nestedRef =
                        found != nullptr ? std::get_if<InstanceRef>(&found->value) : nullptr;
                    if (nestedRef == nullptr)
                        return fail(InstantiationFailureKind::ImportMismatch);
                    work.emplace_back(*nested, *nestedRef);
                }
            }
        }
        return {};
    }

    /// Fills a slot and binds what it names.
    [[nodiscard]] std::expected<void, InstantiationFailure> fill(Frame& frame,
                                                                 binary::Sort sort,
                                                                 uint32_t index,
                                                                 SlotValue value)
    {
        slot(frame, sort, index) = std::move(value);
        return bind(frame, sort, index);
    }

    /// The canonical options @p options name in @p frame.
    [[nodiscard]] static ResolvedOptions resolve(Frame& frame, std::span<binary::CanonOption const> options)
    {
        auto resolved = ResolvedOptions {};
        for (auto const& option: options)
        {
            switch (option.kind)
            {
                case binary::CanonOptionKind::Utf8:
                    resolved.encoding = canonical::StringEncoding::Utf8;
                    break;
                case binary::CanonOptionKind::Utf16:
                    resolved.encoding = canonical::StringEncoding::Utf16;
                    break;
                case binary::CanonOptionKind::Latin1Utf16:
                    resolved.encoding = canonical::StringEncoding::Latin1Utf16;
                    break;
                case binary::CanonOptionKind::Memory:
                    resolved.memory =
                        std::get<isoline::api::Memory>(slot(frame, binary::Sort::CoreMemory, option.index));
                    break;
                case binary::CanonOptionKind::Realloc:
                    resolved.realloc =
                        std::get<isoline::api::Function>(slot(frame, binary::Sort::CoreFunc, option.index));
                    break;
                case binary::CanonOptionKind::PostReturn:
                    resolved.postReturn =
                        std::get<isoline::api::Function>(slot(frame, binary::Sort::CoreFunc, option.index));
                    break;
                case binary::CanonOptionKind::Async:                           // uncovered(async-option)
                case binary::CanonOptionKind::Callback: Require(false); break; // uncovered(async-option)
            }
        }
        return resolved;
    }

    /// The side of a canonical definition of @p frame.
    [[nodiscard]] InstanceSide sideOf(Frame const& frame) const noexcept
    {
        auto const& validated = *frame.component.code->validated;
        return InstanceSide { .instance = frame.instance,
                              .arena = &validated.types,
                              .layouts = &validated.layouts,
                              .bindings = &store._bindings[frame.instance] };
    }

    /// Adds a core function to the core store.
    [[nodiscard]] std::expected<isoline::api::Function, InstantiationFailure> define(
        isoline::api::FuncType const& type, std::shared_ptr<isoline::api::IHostFunction> const& function)
    {
        auto defined = store._store->define(type, function);
        if (!defined)
            return fail(InstantiationFailureKind::CoreDefinition, defined.error());
        return *defined;
    }

    // One handler per PlanStep alternative.

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, DefineCoreModule const& step)
    {
        auto const modules = frame.component.code->modules;
        Require(step.ordinal < modules.size());
        return fill(frame, binary::Sort::CoreModule, step.slot, modules[step.ordinal]);
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, DefineComponent const& step)
    {
        return fill(frame,
                    binary::Sort::Component,
                    step.slot,
                    ComponentRef { .code = frame.component.code, .plan = step.plan, .lexical = &frame });
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, ImportSlot const& step)
    {
        auto const& pending = stack.back();
        auto const& requirement = planOf(frame).imports[step.import];
        auto value = SlotValue {};
        if (pending.parent == nullptr)
            value = imports[step.import];
        else
        {
            // Arguments are ordered by name (ComponentPlan.hpp); an absent one is an instance of types only.
            auto const arguments = std::span<NamedSlot const> { pending.instantiation->arguments };
            if (auto const* argument = findByName(arguments, requirement.name))
                value = slot(*pending.parent, argument->item.sort, argument->item.index);
        }
        return fill(frame, step.sort, step.slot, std::move(value));
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, InstantiateCore const& step)
    {
        if (store._coreInstances >= store._limits.maxCoreInstances)
            return fail(InstantiationFailureKind::LimitExceeded, ComponentLimit::CoreInstances);
        auto const module =
            std::get<isoline::api::Module>(slot(frame, binary::Sort::CoreModule, step.module));
        // The arguments by name, so each import finds its own in O(log n).
        auto arguments = std::vector<std::pair<std::string_view, uint32_t>> {};
        arguments.reserve(step.arguments.size());
        for (auto const& argument: step.arguments)
            arguments.emplace_back(argument.name, argument.coreInstance);
        std::ranges::sort(arguments);
        auto externs = std::vector<isoline::api::Extern> {};
        externs.reserve(module.imports().size());
        for (auto const& import: module.imports())
        {
            auto const argument = std::ranges::lower_bound(
                arguments, import.module, {}, [](auto const& entry) { return entry.first; });
            Require(argument != arguments.end() && argument->first == import.module);
            auto const instance =
                std::get<CoreInstanceRef>(slot(frame, binary::Sort::CoreInstance, argument->second));
            auto const& exports = store._coreInstanceExports[instance.index].exports;
            auto const found = std::ranges::lower_bound(
                exports, import.name, {}, &std::pair<std::string_view, isoline::api::Extern>::first);
            Require(found != exports.end() && found->first == import.name);
            externs.push_back(found->second);
        }
        ++store._coreInstances;
        auto instance = store._store->instantiateByPosition(module, externs);
        if (!instance)
        {
            // A start function's failure is a component trap (or the host's failure), as a call's is.
            if (auto const* call = std::get_if<isoline::api::CallError>(&instance.error()))
                return std::visit(
                    core::Overloaded {
                        [](Trap trap) { return fail(InstantiationFailureKind::Trap, std::move(trap)); },
                        [](isoline::api::HostFailure failure) {
                            return fail(InstantiationFailureKind::HostFailure, failure);
                        },
                    },
                    fromCallError(*call, store._pendingTrap));
            return fail(InstantiationFailureKind::CoreInstantiation, std::move(instance.error()));
        }
        // Defence in depth, as after a call's callee (Calls.cpp): a start function that succeeded left
        // nothing pending and the store live.
        if (auto pending = store._pendingTrap.take())
            return fail(InstantiationFailureKind::Trap, std::move(*pending)); // uncovered(defence)
        if (store._state == StoreState::LockedDown)
            return fail(InstantiationFailureKind::Trap, trapOf(TrapCode::CannotEnter)); // uncovered(defence)
        auto& value = store._coreInstanceExports.emplace_back();
        value.exports.reserve(module.exports().size());
        for (auto const [index, description]: core::views::enumerate(module.exports()))
        {
            auto entity = instance->exportAt(static_cast<uint32_t>(index));
            Require(entity.has_value());
            value.exports.emplace_back(description.name, *entity);
        }
        std::ranges::sort(value.exports, {}, &std::pair<std::string_view, isoline::api::Extern>::first);
        return fill(frame,
                    binary::Sort::CoreInstance,
                    step.slot,
                    CoreInstanceRef { static_cast<uint32_t>(store._coreInstanceExports.size() - 1) });
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame,
                                                                SynthesizeCoreInstance const& step)
    {
        auto& value = store._coreInstanceExports.emplace_back();
        value.exports.reserve(step.exports.size());
        for (auto const& entry: step.exports)
        {
            auto const& held = slot(frame, binary::sortOf(entry.sort), entry.slot);
            auto entity = std::visit(
                [](auto const& value) -> isoline::api::Extern {
                    if constexpr (std::is_constructible_v<isoline::api::Extern, decltype(value)>)
                        return isoline::api::Extern { value };
                    else
                    // uncovered-begin(validated-slot)
                    {
                        Require(false);
                        std::unreachable();
                    }
                    // uncovered-end(validated-slot)
                },
                held);
            value.exports.emplace_back(entry.name, std::move(entity));
        }
        std::ranges::sort(value.exports, {}, &std::pair<std::string_view, isoline::api::Extern>::first);
        return fill(frame,
                    binary::Sort::CoreInstance,
                    step.slot,
                    CoreInstanceRef { static_cast<uint32_t>(store._coreInstanceExports.size() - 1) });
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, LowerFunction const& step)
    {
        auto const* callee = std::get_if<ComponentFunction>(&slot(frame, binary::Sort::Func, step.function));
        if (callee == nullptr)
            return fail(InstantiationFailureKind::ImportMismatch); // uncovered(validated-slot)
        auto const side = sideOf(frame);
        auto function =
            std::make_shared<LoweredFunction>(store, side, step.type, resolve(frame, step.options), *callee);
        auto defined = define(loweredCoreType(side, step.type), function);
        if (!defined)
            return std::unexpected { std::move(defined.error()) };
        return fill(frame, binary::Sort::CoreFunc, step.slot, std::move(*defined));
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, LiftFunction const& step)
    {
        auto core = std::get<isoline::api::Function>(slot(frame, binary::Sort::CoreFunc, step.coreFunction));
        store._lifted.push_back(LiftedFunction { .side = sideOf(frame),
                                                 .core = std::move(core),
                                                 .type = step.type,
                                                 .options = resolve(frame, step.options) });
        return fill(frame,
                    binary::Sort::Func,
                    step.slot,
                    ComponentFunction { LiftedRef { static_cast<uint32_t>(store._lifted.size() - 1) } });
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, DefineBuiltin const& step)
    {
        auto const& row = builtinHandlerRow(step.builtin);
        Require(row.support == BuiltinSupport::Implemented && !step.immediates.empty());
        auto const* type =
            std::get_if<ResourceTypeIndex>(&slot(frame, binary::Sort::Type, step.immediates.front()));
        if (type == nullptr)
            return fail(InstantiationFailureKind::ImportMismatch); // uncovered(validated-slot)
        auto const builtin =
            row.factory(BuiltinBinding { .host = store, .instance = frame.instance, .type = *type });
        auto defined = define(builtin.type, builtin.function);
        if (!defined)
            return std::unexpected { std::move(defined.error()) };
        return fill(frame, binary::Sort::CoreFunc, step.slot, std::move(*defined));
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, DefineResourceType const& step)
    {
        auto destructor = std::optional<isoline::api::Function> {};
        if (step.destructor)
            destructor =
                std::get<isoline::api::Function>(slot(frame, binary::Sort::CoreFunc, *step.destructor));
        auto type = store._resources.types().create(
            GuestImplementor { .componentInstance = frame.instance, .destructor = std::move(destructor) });
        if (!type)
            return fail(InstantiationFailureKind::Trap, std::move(type.error()));
        return fill(frame, binary::Sort::Type, step.slot, *type);
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, AliasSlot const& step)
    {
        auto value = std::visit(
            core::Overloaded {
                [&](InstanceExport const& source) -> std::expected<SlotValue, InstantiationFailure> {
                    auto const& held = slot(frame, binary::Sort::Instance, source.instance);
                    // An instance of types only exports no value; fill() checks the type needs none.
                    if (std::holds_alternative<std::monostate>(held))
                        return SlotValue {};
                    auto const* instance = std::get_if<InstanceRef>(&held);
                    if (instance == nullptr)
                        return fail(InstantiationFailureKind::ImportMismatch); // uncovered(validated-slot)
                    auto const* found = findByName(store.exportsOf(*instance), source.name);
                    if (found == nullptr)
                        return fail(InstantiationFailureKind::ImportMismatch); // uncovered(validated-slot)
                    return found->value;
                },
                [&](CoreInstanceExport const& source) -> std::expected<SlotValue, InstantiationFailure> {
                    auto const instance = std::get<CoreInstanceRef>(
                        slot(frame, binary::Sort::CoreInstance, source.coreInstance));
                    auto const& exports = store._coreInstanceExports[instance.index].exports;
                    auto const found =
                        std::ranges::lower_bound(exports,
                                                 std::string_view { source.name },
                                                 {},
                                                 &std::pair<std::string_view, isoline::api::Extern>::first);
                    Require(found != exports.end() && found->first == source.name);
                    return slotValueOf(found->second);
                },
                [&](OuterSlot const& source) -> std::expected<SlotValue, InstantiationFailure> {
                    // The lexical chain: the frames the components were defined in, never the
                    // instantiating ones.
                    auto const* outer = &frame;
                    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, source.count))
                    {
                        outer = outer->component.lexical;
                        Require(outer != nullptr);
                    }
                    auto const& slots = outer->slots[sortIndex(step.sort)];
                    Require(source.slot < slots.size());
                    return slots[source.slot];
                },
            },
            step.source);
        if (!value)
            return std::unexpected { std::move(value.error()) }; // uncovered(validated-slot)
        return fill(frame, step.sort, step.slot, std::move(*value));
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame,
                                                                InstantiateComponent const& step)
    {
        auto const* component =
            std::get_if<ComponentRef>(&slot(frame, binary::Sort::Component, step.component));
        if (component == nullptr)
            return fail(InstantiationFailureKind::ImportMismatch); // uncovered(validated-slot)
        auto child = createFrame(*component);
        if (!child)
            return std::unexpected { std::move(child.error()) };
        stack.push_back(Pending { .frame = *child, .next = 0, .parent = &frame, .instantiation = &step });
        return {};
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, SynthesizeInstance const& step)
    {
        auto exports = std::vector<NamedValue> {};
        exports.reserve(step.exports.size());
        for (auto const& entry: step.exports)
            exports.push_back(
                NamedValue { .name = entry.name, .value = slot(frame, entry.item.sort, entry.item.index) });
        return fill(frame, binary::Sort::Instance, step.slot, store.addInstance(std::move(exports)));
    }

    [[nodiscard]] std::expected<void, InstantiationFailure> run(Frame& frame, ExportSlot const& step)
    {
        return fill(frame, step.sort, step.slot, slot(frame, step.sort, step.source));
    }

    /// Ends the frame on top of the stack: its exports become an instance value, held by its parent's
    /// slot.
    [[nodiscard]] std::expected<void, InstantiationFailure> finish()
    {
        auto const pending = stack.back();
        stack.pop_back();
        auto& frame = *pending.frame;
        auto const& plan = planOf(frame);
        auto exports = std::vector<NamedValue> {};
        exports.reserve(plan.exports.size());
        for (auto const& binding: plan.exports)
            exports.push_back(NamedValue { .name = binding.name,
                                           .value = slot(frame, binding.item.sort, binding.item.index) });
        auto const instance = store.addInstance(std::move(exports));
        store._roots[frame.instance] = instance;
        if (pending.parent == nullptr)
            return {};
        return fill(*pending.parent, binary::Sort::Instance, pending.instantiation->slot, instance);
    }
};

void ComponentStore::undeliver(std::span<canonical::ResourceValue const> owns, OwnHolder holder) noexcept
{
    if (owns.empty())
        return;
    if (holder == OwnHolder::Host && _hostOwns != nullptr)
        _hostOwns->returned(owns);
    else
        _resources.adoptOrphans(owns);
}

ComponentStore::ComponentStore(isoline::api::Engine const& engine,
                               isoline::api::IMemoryAllocator& allocator,
                               ComponentLimits const& limits,
                               isoline::api::IPollObserver* observer,
                               IHostOwnSink* hostOwns):
    _limits { limits },
    _hostOwns { hostOwns },
    _identity { engine.config().identity() },
    _store { std::make_unique<isoline::api::Store>(
        engine, allocator, storeOptions(engine, limits, observer)) },
    _resources { limits },
    _liveBytes { std::make_shared<canonical::LiveBytesBudget>(limits.maxLiveValueBytes) }
{
}

ComponentStore::~ComponentStore()
{
    _liveBytes->release(_hostNameBytes);
}

std::expected<uint32_t, InstantiationFailure> ComponentStore::instantiate(
    ComponentCode const& code, ResolvedImports const& imports) noexcept
{
    // Every refusal before the first step changes nothing, so none locks down.
    if (_state == StoreState::Instantiating || _activeCalls > 0)
        return fail(InstantiationFailureKind::Reentrant);
    if (_state == StoreState::LockedDown)
        return fail(InstantiationFailureKind::LockedDown);
    if (code.engine != _identity)
        return fail(InstantiationFailureKind::EngineMismatch);
    if (auto checked = checkImports(code, imports); !checked)
        return std::unexpected { std::move(checked.error()) };
    if (_resources.instances().size() >= _limits.maxComponentInstances)
        return fail(InstantiationFailureKind::LimitExceeded, ComponentLimit::ComponentInstances);
    // The host's export names are copied for the store's life: charged last of the refusals, so a
    // refusal changes nothing.
    auto nameBytes = uint64_t { 0 };
    for (auto const& value: imports.values)
        if (auto const* instance = std::get_if<HostInstanceImport>(&value))
            for (auto const& entry: instance->exports)
                nameBytes += entry.first.size();
    if (!_liveBytes->charge(nameBytes))
        return fail(InstantiationFailureKind::LimitExceeded, ComponentLimit::LiveValueBytes);
    _hostNameBytes += nameBytes;
    _state = StoreState::Instantiating;
    auto result = std::expected<uint32_t, InstantiationFailure> {};
    try
    {
        result = run(code, imports);
    }
    catch (std::bad_alloc const&)
    {
        // uncovered-begin(host-memory)
        result = fail(InstantiationFailureKind::HostMemoryExhausted);
    }
    // uncovered-end(host-memory)
    catch (std::length_error const&)
    {
        // uncovered-begin(host-memory)
        // A container asked for more than its max_size(): a memory refusal, as std::bad_alloc is.
        result = fail(InstantiationFailureKind::HostMemoryExhausted);
    }
    // uncovered-end(host-memory)
    if (_state == StoreState::Instantiating)
        _state = StoreState::Live;
    if (!result)
        lockDown();
    return result;
}

std::expected<void, InstantiationFailure> ComponentStore::checkImports(ComponentCode const& code,
                                                                       ResolvedImports const& imports) const
{
    auto const& plan = code.plans->plans.front();
    auto const& scope = code.validated->scopes.front();
    auto const mismatch = [](uint32_t import) {
        return fail(InstantiationFailureKind::ImportMismatch, import);
    };
    if (imports.imports.size() != plan.imports.size())
        return mismatch(static_cast<uint32_t>(std::min(imports.imports.size(), plan.imports.size())));
    // Each value names only values before it, so converting them in order never recurses.
    for (auto const [position, value]: core::views::enumerate(imports.values))
    {
        auto const index = static_cast<uint32_t>(position);
        if (auto const* instance = std::get_if<HostInstanceImport>(&value))
        {
            for (auto const& entry: instance->exports)
                if (entry.second >= index)
                    return mismatch(index);
        }
        else if (auto const* existing = std::get_if<InstanceRef>(&value))
        {
            if (existing->index >= _instanceValues.size())
                return mismatch(index);
        }
        else if (auto const* component = std::get_if<ComponentImport>(&value))
        {
            if (component->code.engine != _identity)
                return mismatch(index);
        }
    }
    // The host resource types the imports mint, each identity once, fit the cap: checked before any is.
    auto identities = std::vector<HostResourceIdentity const*> {};
    for (auto const& value: imports.values)
        if (auto const* resource = std::get_if<HostResourceImport>(&value);
            resource != nullptr && !_hostResources.contains(resource->identity.get()))
            identities.push_back(resource->identity.get());
    std::ranges::sort(identities);
    auto const fresh = static_cast<uint64_t>(
        std::ranges::distance(identities.begin(), std::ranges::unique(identities).begin()));
    if (fresh
        > _limits.maxResourceTypes - std::min<uint64_t>(_resources.types().size(), _limits.maxResourceTypes))
        return fail(InstantiationFailureKind::LimitExceeded, ComponentLimit::ResourceTypes);
    for (auto const [position, requirement]: core::views::enumerate(plan.imports))
    {
        auto const import = static_cast<uint32_t>(position);
        auto const valueIndex = imports.imports[position];
        if (valueIndex >= imports.values.size())
            return mismatch(import);
        auto const& value = imports.values[valueIndex];
        auto fits = false;
        switch (requirement.sort)
        {
            case binary::Sort::Func:
                fits = std::holds_alternative<std::shared_ptr<IHostCallee>>(value);
                break;
            case binary::Sort::CoreModule: fits = std::holds_alternative<isoline::api::Module>(value); break;
            case binary::Sort::Component: fits = std::holds_alternative<ComponentImport>(value); break;
            case binary::Sort::Instance:
                fits = std::holds_alternative<HostInstanceImport>(value)
                       || std::holds_alternative<InstanceRef>(value)
                       || std::holds_alternative<std::monostate>(value);
                break;
            case binary::Sort::Type:
                fits = std::holds_alternative<types::ResourceTypeId>(scope.types[requirement.slot].type)
                           ? std::holds_alternative<HostResourceImport>(value)
                           : std::holds_alternative<std::monostate>(value);
                break;
            default: fits = false; break; // uncovered(validated-slot)
        }
        if (auto const* function = std::get_if<std::shared_ptr<IHostCallee>>(&value);
            fits && function != nullptr)
            fits = *function != nullptr;
        if (!fits)
            return mismatch(import);
    }
    return {};
}

std::expected<SlotValue, InstantiationFailure> ComponentStore::importValue(
    ResolvedValue const& value, std::span<SlotValue const> converted)
{
    return std::visit(
        core::Overloaded {
            [](std::monostate) -> std::expected<SlotValue, InstantiationFailure> { return SlotValue {}; },
            [](std::shared_ptr<IHostCallee> const& callee) -> std::expected<SlotValue, InstantiationFailure> {
                return SlotValue { ComponentFunction { callee } };
            },
            [this](HostResourceImport const& resource) -> std::expected<SlotValue, InstantiationFailure> {
                return hostResourceType(resource).transform(
                    [](ResourceTypeIndex type) { return SlotValue { type }; });
            },
            [](isoline::api::Module const& module) -> std::expected<SlotValue, InstantiationFailure> {
                return SlotValue { module };
            },
            [this](ComponentImport const& component) -> std::expected<SlotValue, InstantiationFailure> {
                return SlotValue { ComponentRef {
                    .code = &keep(component.code), .plan = 0, .lexical = nullptr } };
            },
            [&](HostInstanceImport const& instance) -> std::expected<SlotValue, InstantiationFailure> {
                // Its exports precede it (checkImports()), so they are converted already.
                auto exports = std::vector<NamedValue> {};
                exports.reserve(instance.exports.size());
                for (auto const& [name, index]: instance.exports)
                    exports.push_back(
                        NamedValue { .name = _hostNames.emplace_back(name), .value = converted[index] });
                return SlotValue { addInstance(std::move(exports)) };
            },
            [](InstanceRef instance) -> std::expected<SlotValue, InstantiationFailure> {
                return SlotValue { instance };
            },
        },
        value);
}

std::expected<ResourceTypeIndex, InstantiationFailure> ComponentStore::hostResourceType(
    HostResourceImport const& resource)
{
    // One runtime type per identity token: copies of one host type are one type (D8).
    if (auto const found = _hostResources.find(resource.identity.get()); found != _hostResources.end())
        return found->second;
    auto type = _resources.types().create(
        HostImplementor { .identity = resource.identity, .destructor = resource.destructor });
    if (!type)
        return fail(InstantiationFailureKind::Trap, std::move(type.error())); // uncovered(prechecked)
    _hostIdentities.push_back(resource.identity);
    _hostResources.emplace(resource.identity.get(), *type);
    return *type;
}

ComponentCode const& ComponentStore::keep(ComponentCode const& code)
{
    auto const key = std::pair { code.owner.get(), code.plans };
    if (auto const found = _codeIndex.find(key); found != _codeIndex.end())
        return _codes[found->second];
    auto const& kept = _codes.emplace_back(code);
    _codeIndex.emplace(key, _codes.size() - 1);
    return kept;
}

InstanceRef ComponentStore::addInstance(std::vector<NamedValue> exports)
{
    std::ranges::sort(exports, {}, &NamedValue::name);
    _instanceValues.push_back(std::move(exports));
    return InstanceRef { static_cast<uint32_t>(_instanceValues.size() - 1) };
}

std::expected<uint32_t, InstantiationFailure> ComponentStore::run(ComponentCode const& code,
                                                                  ResolvedImports const& imports)
{
    // One invocation for every core instantiation: their start functions share one step budget, the
    // budget of the scope an embedder opened around this, if any.
    auto const scope = _store->openInvocation();
    auto instantiation = Instantiation { .store = *this };

    // The imports first, in order, so an instance's exports are converted before it.
    instantiation.imports.reserve(imports.values.size());
    for (auto const& value: imports.values)
    {
        auto converted = importValue(value, instantiation.imports);
        if (!converted)
            return std::unexpected { std::move(converted.error()) }; // uncovered(prechecked)
        instantiation.imports.push_back(std::move(*converted));
    }
    // ImportSlot reads the root's imports by import index.
    auto rootImports = std::vector<SlotValue> {};
    rootImports.reserve(imports.imports.size());
    for (auto const index: imports.imports)
        rootImports.push_back(instantiation.imports[index]);
    instantiation.imports = std::move(rootImports);

    auto const& kept = keep(code);
    auto root = instantiation.createFrame(ComponentRef { .code = &kept, .plan = 0, .lexical = nullptr });
    if (!root)
        return std::unexpected { std::move(root.error()) };
    auto const rootInstance = (*root)->instance;
    instantiation.stack.push_back(Instantiation::Pending { .frame = *root });
    while (!instantiation.stack.empty())
    {
        auto& top = instantiation.stack.back();
        auto& frame = *top.frame;
        auto const& plan = Instantiation::planOf(frame);
        if (top.next == plan.steps.size())
        {
            if (auto finished = instantiation.finish(); !finished)
                return std::unexpected { std::move(finished.error()) }; // uncovered(validated-slot)
            continue;
        }
        if (auto charged = instantiation.charge(1); !charged)
            return std::unexpected { std::move(charged.error()) };
        auto const& step = plan.steps[top.next];
        ++top.next; // before the step, which may push a frame and so move the stack
        auto ran =
            std::visit([&](auto const& alternative) { return instantiation.run(frame, alternative); }, step);
        if (!ran)
            return std::unexpected { std::move(ran.error()) };
        if (_state == StoreState::LockedDown)
            return fail(InstantiationFailureKind::Trap, trapOf(TrapCode::CannotEnter)); // uncovered(defence)
    }
    return rootInstance;
}

std::span<NamedValue const> ComponentStore::exportsOf(uint32_t instance) const noexcept
{
    Require(instance < _roots.size());
    if (_roots[instance] == UnfinishedInstance)
        return {};
    return exportsOf(_roots[instance]);
}

InstanceRef ComponentStore::instanceValueOf(uint32_t instance) const noexcept
{
    Require(instance < _roots.size() && _roots[instance] != UnfinishedInstance);
    return _roots[instance];
}

std::span<NamedValue const> ComponentStore::exportsOf(InstanceRef instance) const noexcept
{
    Require(instance.index < _instanceValues.size());
    return _instanceValues[instance.index];
}

std::optional<ComponentFunction> ComponentStore::function(uint32_t instance, std::string_view name) const
{
    auto const* found = findByName(exportsOf(instance), name);
    if (found == nullptr)
        return std::nullopt;
    if (auto const* function = std::get_if<ComponentFunction>(&found->value))
        return *function;
    return std::nullopt;
}

std::optional<ResourceTypeIndex> ComponentStore::hostResourceTypeOf(
    HostResourceIdentity const& identity) const
{
    if (auto const found = _hostResources.find(&identity); found != _hostResources.end())
        return found->second;
    return std::nullopt;
}

LiftedFunction const& ComponentStore::lifted(LiftedRef function) const noexcept
{
    Require(function.index < _lifted.size());
    return _lifted[function.index];
}

std::expected<void, Trap> ComponentStore::callGuestDestructor(GuestImplementor const& implementor,
                                                              uint32_t callerInstance,
                                                              uint32_t rep)
{
    // definitions.py lifts the destructor into its instance and lowers it into the caller: a
    // component call of (u32) -> (), which needs no memory, so it is the core call itself, guarded as
    // canonLift() guards one.
    // The built-in calls only a destructor there is (Builtins.cpp).
    std::ignore = callerInstance;
    Require(implementor.destructor.has_value());
    if (_state == StoreState::LockedDown)
        return std::unexpected { trapOf(TrapCode::CannotEnter) }; // uncovered(defence)
    auto const args = std::array<isoline::api::Value, 1> { isoline::api::I32 { rep } };
    enterCall();
    auto called = _store->call(*implementor.destructor, args, {});
    leaveCall();
    if (!called)
        return std::unexpected { asTrap(fromCallError(called.error(), _pendingTrap)) };
    if (auto pending = _pendingTrap.take())
        return std::unexpected { std::move(*pending) }; // uncovered(defence)
    if (_state == StoreState::LockedDown)
        return std::unexpected { trapOf(TrapCode::CannotEnter) }; // uncovered(defence)
    return {};
}

} // namespace isoline::component::runtime
