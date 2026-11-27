// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/validate/Subtyping.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/Limits.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::validate
{

namespace
{
    using types::AnyTypeId;
    using types::ExternType;
    using types::NamedExtern;
    using types::TypeId;
    using types::ValueKind;

    /// What a value kind's children are named by in the context of a mismatch inside one.
    enum class ContextArgument : uint8_t
    {
        None,     ///< The kind adds no context.
        Label,    ///< Its context names the child's label.
        Position, ///< Its context names the child's position.
        Offset,   ///< Each child has a context of its own: the row's, plus its position.
    };

    /// How wasm-tools names a value kind in a mismatch, and the context a mismatch inside it adds:
    /// expected is ComponentDefinedType::desc (component_types.rs:2020-2038; "primitive" for every
    /// primitive), found the word of the component_defined_type arms (component_types.rs:4233-4418; a
    /// primitive by its own name), and the context that of its with_context.
    struct ValueKindRules
    {
        std::string_view expected;
        std::string_view found;
        std::optional<RelationContext> context = std::nullopt;
        ContextArgument argument = ContextArgument::None;
    };

    /// Per ValueKind, in enumerator order.
    constexpr auto ValueKindRuleTable = std::to_array<ValueKindRules>({
        { .expected = "primitive", .found = "bool" },
        { .expected = "primitive", .found = "s8" },
        { .expected = "primitive", .found = "u8" },
        { .expected = "primitive", .found = "s16" },
        { .expected = "primitive", .found = "u16" },
        { .expected = "primitive", .found = "s32" },
        { .expected = "primitive", .found = "u32" },
        { .expected = "primitive", .found = "s64" },
        { .expected = "primitive", .found = "u64" },
        { .expected = "primitive", .found = "f32" },
        { .expected = "primitive", .found = "f64" },
        { .expected = "primitive", .found = "char" },
        { .expected = "primitive", .found = "string" },
        { .expected = "primitive", .found = "error-context" },
        { .expected = "list", .found = "list" },
        { .expected = "fixed-length list", .found = "list" },
        { .expected = "record",
          .found = "record",
          .context = RelationContext::RecordField,
          .argument = ContextArgument::Label },
        { .expected = "tuple",
          .found = "tuple",
          .context = RelationContext::TupleField,
          .argument = ContextArgument::Position },
        { .expected = "variant",
          .found = "variant",
          .context = RelationContext::VariantCase,
          .argument = ContextArgument::Label },
        { .expected = "enum", .found = "enum" },
        { .expected = "option", .found = "option" },
        { .expected = "result",
          .found = "result",
          .context = RelationContext::OkVariant,
          .argument = ContextArgument::Offset },
        { .expected = "flags", .found = "flags" },
        { .expected = "map",
          .found = "map",
          .context = RelationContext::MapKey,
          .argument = ContextArgument::Offset },
        { .expected = "own", .found = "own" },
        { .expected = "borrow", .found = "borrow" },
        { .expected = "stream", .found = "stream", .context = RelationContext::StreamElement },
        { .expected = "future", .found = "future", .context = RelationContext::FutureElement },
    });

    static_assert(ValueKindRuleTable.size() == types::ValueKindCount, "one row of rules per ValueKind");
    static_assert(
        [] {
            for (auto const& row: types::ValueKindTable)
            {
                auto const& rules = ValueKindRuleTable[static_cast<std::size_t>(row.kind)];
                if (types::isPrimitive(row.kind) && rules.found != row.text)
                    return false;
            }
            return true;
        }(),
        "a primitive is found by its own name");
    static_assert(static_cast<int>(RelationContext::ErrVariant)
                          == static_cast<int>(RelationContext::OkVariant) + 1
                      && static_cast<int>(RelationContext::MapValue)
                             == static_cast<int>(RelationContext::MapKey) + 1,
                  "an Offset context's second child has the next context");

    [[nodiscard]] constexpr ValueKindRules const& rulesOf(ValueKind kind) noexcept
    {
        return ValueKindRuleTable[static_cast<std::size_t>(kind)];
    }

    /// How an extern type is named in a mismatch (ComponentEntityType::desc, component_types.rs:974).
    [[nodiscard]] std::string_view entityWord(ExternType const& type) noexcept
    {
        return std::visit(
            [](auto const& alternative) -> std::string_view {
                using Alternative = std::remove_cvref_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Alternative, types::FunctionTypeId>)
                    return "func";
                else if constexpr (std::is_same_v<Alternative, types::ValueExtern>)
                    return "value";
                else if constexpr (std::is_same_v<Alternative, types::TypeExtern>)
                    return "type";
                else if constexpr (std::is_same_v<Alternative, types::InstanceTypeId>)
                    return "instance";
                else
                    return "component";
            },
            type);
    }

    /// How a type is named as the type expected (ComponentAnyTypeId::desc, component_types.rs:600) and
    /// as the type found (component_any_type_id, component_types.rs:3836-3886).
    struct AnyWords
    {
        std::string_view expected;
        std::string_view found;
    };

    /// Per AnyTypeId alternative, in its order.
    constexpr auto AnyWordTable = std::to_array<AnyWords>({
        { .expected = "defined type", .found = "defined type" },
        { .expected = "func", .found = "func type" },
        { .expected = "component", .found = "component type" },
        { .expected = "instance", .found = "instance type" },
        { .expected = "resource", .found = "resource" },
    });

    static_assert(AnyWordTable.size() == std::variant_size_v<AnyTypeId>, "one row of words per kind of type");
    static_assert(std::is_same_v<std::variant_alternative_t<4, AnyTypeId>, types::ResourceTypeId>,
                  "AnyWordTable follows AnyTypeId's alternatives");

    /// A row's text formatted with up to two strings.
    [[nodiscard]] std::string formatted(std::string_view text,
                                        std::string_view first,
                                        std::string_view second)
    {
        return std::vformat(text, std::make_format_args(first, second));
    }

    /// The text of a relation error.
    [[nodiscard]] RelationError relationError(RelationErrorCode code,
                                              std::string_view first = {},
                                              std::string_view second = {})
    {
        return RelationError {
            .code = code,
            .message = formatted(RelationErrorMessages[static_cast<std::size_t>(code)].text, first, second)
        };
    }

    /// The text of a context line.
    [[nodiscard]] std::string contextLine(RelationContext context,
                                          std::string_view first = {},
                                          std::string_view second = {})
    {
        return formatted(RelationContextMessages[static_cast<std::size_t>(context)].text, first, second);
    }

    /// The comparisons a binary search of @p count names makes, or a sort makes of each: ⌈log2⌉ + 1.
    [[nodiscard]] std::size_t levels(std::size_t count) noexcept
    {
        return static_cast<std::size_t>(std::bit_width(count)) + 1;
    }
} // namespace

TypeRelation::TypeRelation(types::TypeArena const& sub,
                           types::TypeArena const& super,
                           ResourceMapping const& resources,
                           ComponentLimits const& limits):
    _sub { sub }, _super { super }, _isSameArena { &sub == &super }, _maxSteps { limits.maxSubtypingSteps }
{
    for (auto const& [from, to]: resources.images())
        _bindings[static_cast<std::size_t>(Side::Super)].insert_or_assign(
            from.value, ResourceIdentity { .side = Side::Sub, .resource = to.value });
}

types::TypeArena const& TypeRelation::arena(Side side) const noexcept
{
    return side == Side::Sub ? _sub : _super;
}

TypeRelation::Outcome<void> TypeRelation::step(std::size_t count)
{
    if (count > _maxSteps - std::min(_steps, _maxSteps))
    {
        _steps = _maxSteps;
        return std::unexpected { Failure {
            .error = relationError(RelationErrorCode::StepLimit,
                                   limitMessage(ComponentLimit::SubtypingSteps, _maxSteps)) } };
    }
    _steps += count;
    return {};
}

TypeRelation::Outcome<void> TypeRelation::compareNames(std::size_t comparisons, std::size_t bytes)
{
    return step(comparisons + (bytes / SubtypingBytesPerStep));
}

uint32_t TypeRelation::memoEpoch(Side actualSide, types::AnyTypeId actual, types::AnyTypeId expected) const
{
    // A pair naming no resource is related or not under any bindings.
    if (arena(actualSide).namesResource(actual) || arena(opposite(actualSide)).namesResource(expected))
        return _epoch;
    return 0;
}

TypeRelation::ResourceIdentity TypeRelation::identity(Side side, types::ResourceTypeId resource) const
{
    // A bound resource is what it was bound to; an unbound one is itself. Within one arena a resource
    // is the same whichever side names it.
    auto const& bindings = _bindings[static_cast<std::size_t>(side)];
    auto result = ResourceIdentity { .side = side, .resource = resource.value };
    if (auto const bound = bindings.find(resource.value); bound != bindings.end())
        result = bound->second;
    if (_isSameArena)
        result.side = Side::Sub;
    return result;
}

bool TypeRelation::sameResource(Side actualSide,
                                types::ResourceTypeId actual,
                                types::ResourceTypeId expected) const
{
    return identity(actualSide, actual) == identity(opposite(actualSide), expected);
}

void TypeRelation::bind(Side side, uint32_t resource, ResourceIdentity image)
{
    auto& bindings = _bindings[static_cast<std::size_t>(side)];
    auto previous = std::optional<ResourceIdentity> {};
    if (auto const bound = bindings.find(resource); bound != bindings.end())
        previous = bound->second;
    _undo.push_back(Undo { .side = side, .resource = resource, .previous = previous });
    bindings.insert_or_assign(resource, image);
    _epoch = _nextEpoch++;
}

void TypeRelation::restore(std::size_t mark, uint32_t epoch)
{
    while (_undo.size() > mark)
    {
        auto const& undo = _undo.back();
        auto& bindings = _bindings[static_cast<std::size_t>(undo.side)];
        if (undo.previous)
            bindings.insert_or_assign(undo.resource, *undo.previous);
        else
            bindings.erase(undo.resource);
        _undo.pop_back();
    }
    _epoch = epoch;
}

std::expected<void, RelationError> TypeRelation::isSubtype(types::ExternType const& sub,
                                                           types::ExternType const& super)
{
    auto const mark = _undo.size();
    auto const epoch = _epoch;
    auto const outcome = run(startEntity(Side::Sub, sub, super));
    restore(mark, epoch);
    return outcome.transform_error([](Failure const& failure) { return failure.error; });
}

std::expected<void, RelationError> TypeRelation::isEqual(types::TypeId sub, types::TypeId super)
{
    return run(startValue(Side::Sub, sub, super, PrimitiveSpelling::Inline))
        .transform_error([](Failure const& failure) { return failure.error; });
}

std::expected<types::ResourceSubstitution, RelationError> TypeRelation::satisfyImports(
    std::span<types::NamedExtern const> arguments, types::ComponentTypeId component)
{
    auto const mark = _undo.size();
    auto const epoch = _epoch;
    auto const outcome = run(startOpen(Side::Sub, arguments, component, ExternKind::Import));
    auto bound = types::ResourceSubstitution {};
    if (outcome)
    {
        // Each imported resource stands for a resource of the sub arena, the arguments' (or of the
        // super arena, through the initial mapping, which within one arena is the same).
        for (auto const& imported: _super[component].importedResources)
        {
            auto const image = identity(Side::Super, imported.resource);
            if (image.side == Side::Sub && image.resource != imported.resource.value)
                bound.bind(imported.resource, types::ResourceTypeId { .value = image.resource });
        }
    }
    restore(mark, epoch);
    if (!outcome)
        return std::unexpected { outcome.error().error };
    return bound;
}

TypeRelation::Outcome<void> TypeRelation::run(Outcome<Progress> started)
{
    // A failure unwinds every frame, each adding the context of the child it was comparing, outermost
    // first, as wasm-tools' with_context does on its way out; the bindings are the caller's to restore.
    auto const unwind = [&](Failure failure) -> Outcome<void> {
        auto contexts = std::vector<std::string> {};
        if (failure.error.code != RelationErrorCode::StepLimit)
        {
            auto const depth = _frames.size();
            for (auto const index: std::views::iota(std::size_t { 0 }, depth))
            {
                auto const& frame = _frames[depth - 1 - index];
                if (index == 0 && failure.site == FailureSite::Frame)
                    continue;
                if (auto line = context(frame))
                    contexts.push_back(std::move(*line));
            }
        }
        _frames.clear();
        auto message = std::string {};
        for (auto const& line: contexts | std::views::reverse)
            message += line + "\n";
        failure.error.message = message + failure.error.message;
        return std::unexpected { std::move(failure) };
    };
    if (!started)
        return unwind(std::move(started.error()));
    while (!_frames.empty())
    {
        auto const advanced = advance(_frames.back());
        if (!advanced)
            return unwind(advanced.error());
        if (*advanced == Progress::Pushed)
            continue;
        auto const finished = std::move(_frames.back());
        _frames.pop_back();
        finish(finished);
    }
    return {};
}

void TypeRelation::finish(Frame const& frame)
{
    if (frame.kind == FrameKind::Component)
        restore(frame.undoMark, frame.savedEpoch);
    if (frame.kind != FrameKind::Open)
        _memo.insert(MemoKey { .kind = frame.kind,
                               .actualSide = frame.actualSide,
                               .actual = frame.actual,
                               .expected = frame.expected,
                               .epoch = frame.memoEpoch });
}

TypeRelation::Outcome<TypeRelation::Progress> TypeRelation::advance(Frame& frame)
{
    // A child's failure is the frame's to give a context. Starting a child may push a frame, so no
    // branch uses `frame` after it starts one.
    auto const inChild = [](Outcome<Progress> started) -> Outcome<Progress> {
        if (!started)
        {
            started.error().site = FailureSite::Child;
            return started;
        }
        return Progress::Pushed;
    };
    switch (frame.kind)
    {
        case FrameKind::Value: return advanceValue(frame);
        case FrameKind::Function: return advanceFunction(frame);
        case FrameKind::Instance: {
            auto const& actual = arena(frame.actualSide)[types::InstanceTypeId { .value = frame.actual }];
            auto const& expected =
                arena(opposite(frame.actualSide))[types::InstanceTypeId { .value = frame.expected }];
            if (frame.next == expected.exports.size())
                return Progress::Done;
            auto const index = frame.next++;
            return inChild(startEntity(
                frame.actualSide, actual.exports[frame.matched[index]].type, expected.exports[index].type));
        }
        case FrameKind::Component: {
            // component_type (component_types.rs:3588-3674): the expected component's imports must
            // satisfy the actual's (contravariance, the sides swapped), binding the actual's imported
            // resources; then the actual's exports, those bindings applied, must satisfy the expected's.
            auto const actual = types::ComponentTypeId { .value = frame.actual };
            auto const expected = types::ComponentTypeId { .value = frame.expected };
            auto const actualSide = frame.actualSide;
            switch (frame.phase)
            {
                case ComponentPhase::Imports:
                    frame.phase = ComponentPhase::Exports;
                    return inChild(startOpen(opposite(actualSide),
                                             arena(opposite(actualSide))[expected].imports,
                                             actual,
                                             ExternKind::Import));
                case ComponentPhase::Exports:
                    frame.phase = ComponentPhase::Finished;
                    return inChild(startOpen(
                        actualSide, arena(actualSide)[actual].exports, expected, ExternKind::Export));
                case ComponentPhase::Finished: return Progress::Done;
            }
            break;
        }
        case FrameKind::Open: {
            auto const& component =
                arena(opposite(frame.actualSide))[types::ComponentTypeId { .value = frame.expected }];
            auto const& entities =
                frame.externKind == ExternKind::Import ? component.imports : component.exports;
            if (frame.next == entities.size())
                return Progress::Done;
            auto const index = frame.next++;
            return inChild(startEntity(
                frame.actualSide, frame.arguments[frame.matched[index]].type, entities[index].type));
        }
    }
    Require(false);
    return Progress::Done;
}

TypeRelation::Outcome<TypeRelation::Progress> TypeRelation::advanceValue(Frame& frame)
{
    // component_defined_type (component_types.rs:4233-4418), one child per call: the checks wasm-tools
    // makes of a child before comparing it, then the child.
    auto const actualSide = frame.actualSide;
    auto const& actual = arena(actualSide)[TypeId { .value = frame.actual }];
    auto const& expected = arena(opposite(actualSide))[TypeId { .value = frame.expected }];
    auto const mismatch = [](RelationErrorCode code, std::string_view first, std::string_view second = {}) {
        return std::unexpected { Failure { .error = relationError(code, first, second) } };
    };
    while (frame.next < actual.children.size())
    {
        auto const index = frame.next++;
        auto const& actualChild = actual.children[index];
        auto const& expectedChild = expected.children[index];
        if (actual.kind == ValueKind::Record || actual.kind == ValueKind::Variant)
        {
            auto const& actualLabel = actual.labels[index].str();
            auto const& expectedLabel = expected.labels[index].str();
            if (auto compared = compareNames(1, std::min(actualLabel.size(), expectedLabel.size()));
                !compared)
                return std::unexpected { compared.error() };
            if (actualLabel != expectedLabel)
                return mismatch(actual.kind == ValueKind::Record ? RelationErrorCode::FieldName
                                                                 : RelationErrorCode::CaseName,
                                expectedLabel,
                                actualLabel);
            if (!actualChild && expectedChild)
                return mismatch(RelationErrorCode::CaseTypeMissing, actualLabel);
            if (actualChild && !expectedChild)
                return mismatch(RelationErrorCode::CaseTypePresent, actualLabel);
        }
        if (actual.kind == ValueKind::Result)
        {
            auto const part = index == 0 ? std::string_view { "ok" } : std::string_view { "err" };
            if (!actualChild && expectedChild)
                return mismatch(RelationErrorCode::PayloadMissing, part);
            if (actualChild && !expectedChild)
                return mismatch(RelationErrorCode::PayloadPresent, part);
        }
        if (!actualChild)
            continue;
        auto started = startValue(actualSide, *actualChild, *expectedChild, PrimitiveSpelling::Inline);
        if (!started)
        {
            started.error().site = FailureSite::Child;
            return std::unexpected { std::move(started.error()) };
        }
        return Progress::Pushed;
    }
    return Progress::Done;
}

TypeRelation::Outcome<TypeRelation::Progress> TypeRelation::advanceFunction(Frame& frame)
{
    // component_func_type (component_types.rs:3717-3792): each parameter's name, then its type; then
    // the result.
    auto const actualSide = frame.actualSide;
    auto const& actual = arena(actualSide)[types::FunctionTypeId { .value = frame.actual }];
    auto const& expected = arena(opposite(actualSide))[types::FunctionTypeId { .value = frame.expected }];
    auto const index = frame.next++;
    auto started = Outcome<Progress> { Progress::Done };
    if (index < actual.params.size())
    {
        auto const& actualName = actual.params[index].first.str();
        auto const& expectedName = expected.params[index].first.str();
        if (auto compared = compareNames(1, std::min(actualName.size(), expectedName.size())); !compared)
            return std::unexpected { compared.error() };
        if (actualName != expectedName)
            return std::unexpected { Failure {
                .error = relationError(RelationErrorCode::ParameterName, expectedName, actualName) } };
        started = startValue(actualSide,
                             actual.params[index].second,
                             expected.params[index].second,
                             PrimitiveSpelling::Inline);
    }
    else if (index == actual.params.size())
    {
        if (actual.result && !expected.result)
            return std::unexpected { Failure { .error = relationError(RelationErrorCode::ResultMissing) } };
        if (!actual.result && expected.result)
            return std::unexpected { Failure { .error = relationError(RelationErrorCode::ResultPresent) } };
        if (!actual.result)
            return Progress::Done;
        started = startValue(actualSide, *actual.result, *expected.result, PrimitiveSpelling::Inline);
    }
    else
        return Progress::Done;
    if (!started)
    {
        started.error().site = FailureSite::Child;
        return std::unexpected { std::move(started.error()) };
    }
    return Progress::Pushed;
}

std::optional<std::string> TypeRelation::context(Frame const& frame) const
{
    // The context each frame adds for the child it was comparing (frame.next - 1).
    Require(frame.next > 0 || frame.kind == FrameKind::Component);
    auto const index = frame.next - 1;
    auto const actualSide = frame.actualSide;
    switch (frame.kind)
    {
        case FrameKind::Value: {
            auto const& actual = arena(actualSide)[TypeId { .value = frame.actual }];
            auto const& rules = rulesOf(actual.kind);
            if (!rules.context)
                return std::nullopt;
            switch (rules.argument)
            {
                case ContextArgument::None: return contextLine(*rules.context);
                case ContextArgument::Label: return contextLine(*rules.context, actual.labels[index].str());
                case ContextArgument::Position: return contextLine(*rules.context, std::to_string(index));
                case ContextArgument::Offset:
                    return contextLine(
                        static_cast<RelationContext>(static_cast<std::size_t>(*rules.context) + index));
            }
            return std::nullopt;
        }
        case FrameKind::Function: {
            auto const& actual = arena(actualSide)[types::FunctionTypeId { .value = frame.actual }];
            if (index < actual.params.size())
                return contextLine(RelationContext::FunctionParameter, actual.params[index].first.str());
            return contextLine(RelationContext::FunctionResult);
        }
        case FrameKind::Instance: {
            auto const& expected =
                arena(opposite(actualSide))[types::InstanceTypeId { .value = frame.expected }];
            return contextLine(RelationContext::InstanceExport, expected.exports[index].name.str());
        }
        case FrameKind::Component: return std::nullopt;
        case FrameKind::Open: {
            auto const& component =
                arena(opposite(actualSide))[types::ComponentTypeId { .value = frame.expected }];
            auto const& entities =
                frame.externKind == ExternKind::Import ? component.imports : component.exports;
            return contextLine(
                RelationContext::Extern, externKindWord(frame.externKind), entities[index].name.str());
        }
    }
    return std::nullopt;
}

TypeRelation::Outcome<TypeRelation::Progress> TypeRelation::startEntity(Side actualSide,
                                                                        types::ExternType const& actual,
                                                                        types::ExternType const& expected)
{
    // component_entity_type (component_types.rs:3554-3584).
    if (actual.index() != expected.index())
        return std::unexpected { Failure {
            .error =
                relationError(RelationErrorCode::KindMismatch, entityWord(expected), entityWord(actual)) } };
    return std::visit(
        [&](auto const& provided) -> Outcome<Progress> {
            using Alternative = std::remove_cvref_t<decltype(provided)>;
            auto const& required = std::get<Alternative>(expected);
            if constexpr (std::is_same_v<Alternative, types::FunctionTypeId>)
                return startFunction(actualSide, provided, required);
            else if constexpr (std::is_same_v<Alternative, types::ValueExtern>)
                return startValue(actualSide, provided.type, required.type, PrimitiveSpelling::Inline);
            else if constexpr (std::is_same_v<Alternative, types::TypeExtern>)
                return startAny(actualSide, provided.type, required.type);
            else if constexpr (std::is_same_v<Alternative, types::InstanceTypeId>)
                return startInstance(actualSide, provided, required);
            else
                return startComponent(actualSide, provided, required);
        },
        actual);
}

TypeRelation::Outcome<TypeRelation::Progress> TypeRelation::startAny(Side actualSide,
                                                                     types::AnyTypeId actual,
                                                                     types::AnyTypeId expected)
{
    // component_any_type_id (component_types.rs:3836-3886).
    if (actual.index() != expected.index())
        return std::unexpected { Failure { .error = relationError(RelationErrorCode::KindMismatch,
                                                                  AnyWordTable[expected.index()].expected,
                                                                  AnyWordTable[actual.index()].found) } };
    return std::visit(
        [&](auto const provided) -> Outcome<Progress> {
            using Id = std::remove_cvref_t<decltype(provided)>;
            auto const required = std::get<Id>(expected);
            if constexpr (std::is_same_v<Id, types::ResourceTypeId>)
            {
                if (auto stepped = step(); !stepped)
                    return std::unexpected { stepped.error() };
                if (sameResource(actualSide, provided, required))
                    return Progress::Done;
                return std::unexpected { Failure { .error =
                                                       relationError(RelationErrorCode::ResourceMismatch) } };
            }
            else if constexpr (std::is_same_v<Id, TypeId>)
                return startValue(actualSide, provided, required, PrimitiveSpelling::Defined);
            else if constexpr (std::is_same_v<Id, types::FunctionTypeId>)
                return startFunction(actualSide, provided, required);
            else if constexpr (std::is_same_v<Id, types::InstanceTypeId>)
                return startInstance(actualSide, provided, required);
            else
                return startComponent(actualSide, provided, required);
        },
        actual);
}

TypeRelation::Outcome<TypeRelation::Progress> TypeRelation::startValue(Side actualSide,
                                                                       types::TypeId actualId,
                                                                       types::TypeId expectedId,
                                                                       PrimitiveSpelling spelling)
{
    if (auto stepped = step(); !stepped)
        return std::unexpected { stepped.error() };
    auto const& actual = arena(actualSide)[actualId];
    auto const& expected = arena(opposite(actualSide))[expectedId];
    auto const epoch = memoEpoch(actualSide, actualId, expectedId);
    // Within one arena interning makes equal types one id; one that names no resource is equal to
    // itself under any bindings.
    if (_isSameArena && actualId == expectedId && epoch == 0)
        return Progress::Done;
    auto const key = MemoKey { .kind = FrameKind::Value,
                               .actualSide = actualSide,
                               .actual = actualId.value,
                               .expected = expectedId.value,
                               .epoch = epoch };
    if (memoised(key))
        return Progress::Done;
    auto const mismatch = [](RelationErrorCode code,
                             std::string_view first = {},
                             std::string_view second = {}) -> Outcome<Progress> {
        return std::unexpected { Failure { .error = relationError(code, first, second) } };
    };
    auto const count = [](std::size_t value) {
        return std::to_string(value);
    };

    // component_val_type and component_defined_type's arms (component_types.rs:4208-4440).
    if (actual.kind != expected.kind)
    {
        if (types::isPrimitive(actual.kind) && types::isPrimitive(expected.kind))
            return mismatch(RelationErrorCode::PrimitiveMismatch,
                            rulesOf(expected.kind).found,
                            rulesOf(actual.kind).found);
        if (types::isPrimitive(expected.kind) && spelling == PrimitiveSpelling::Inline)
            return mismatch(
                RelationErrorCode::KindMismatch, rulesOf(expected.kind).found, rulesOf(actual.kind).expected);
        return mismatch(
            RelationErrorCode::KindMismatch, rulesOf(expected.kind).expected, rulesOf(actual.kind).found);
    }
    switch (actual.kind)
    {
        case ValueKind::Flags:
        case ValueKind::Enum: {
            auto bytes = std::size_t { 0 };
            for (auto const& label: actual.labels)
                bytes += label.str().size();
            if (auto compared = compareNames(actual.labels.size(), bytes); !compared)
                return std::unexpected { compared.error() };
            if (actual.labels != expected.labels)
                return mismatch(RelationErrorCode::ElementsMismatch, rulesOf(actual.kind).found);
            return Progress::Done;
        }
        case ValueKind::Own:
        case ValueKind::Borrow:
            if (!sameResource(actualSide, *actual.resource, *expected.resource))
                return mismatch(RelationErrorCode::ResourceMismatch);
            return Progress::Done;
        case ValueKind::Record:
            if (actual.children.size() != expected.children.size())
                return mismatch(RelationErrorCode::FieldCount,
                                count(expected.children.size()),
                                count(actual.children.size()));
            break;
        case ValueKind::Variant:
            if (actual.children.size() != expected.children.size())
                return mismatch(RelationErrorCode::CaseCount,
                                count(expected.children.size()),
                                count(actual.children.size()));
            break;
        case ValueKind::Tuple:
            if (actual.children.size() != expected.children.size())
                return mismatch(RelationErrorCode::TypeCount,
                                count(expected.children.size()),
                                count(actual.children.size()));
            break;
        case ValueKind::FixedList:
            if (actual.length != expected.length)
                return mismatch(
                    RelationErrorCode::FixedListLength, count(expected.length), count(actual.length));
            break;
        case ValueKind::Stream:
        case ValueKind::Future:
            if (actual.children.size() != expected.children.size())
                return mismatch(actual.children.empty() ? RelationErrorCode::PayloadMissing
                                                        : RelationErrorCode::PayloadPresent,
                                rulesOf(actual.kind).found);
            break;
        default:
            if (actual.children.empty())
                return Progress::Done; // a primitive: the kinds are equal
            break;
    }
    _frames.push_back(Frame { .kind = FrameKind::Value,
                              .actualSide = actualSide,
                              .actual = actualId.value,
                              .expected = expectedId.value,
                              .memoEpoch = epoch });
    return Progress::Pushed;
}

TypeRelation::Outcome<TypeRelation::Progress> TypeRelation::startFunction(Side actualSide,
                                                                          types::FunctionTypeId actualId,
                                                                          types::FunctionTypeId expectedId)
{
    if (auto stepped = step(); !stepped)
        return std::unexpected { stepped.error() };
    auto const& actual = arena(actualSide)[actualId];
    auto const& expected = arena(opposite(actualSide))[expectedId];
    auto const key = MemoKey { .kind = FrameKind::Function,
                               .actualSide = actualSide,
                               .actual = actualId.value,
                               .expected = expectedId.value,
                               .epoch = memoEpoch(actualSide, actualId, expectedId) };
    if (memoised(key))
        return Progress::Done;
    if (actual.asyncness != expected.asyncness)
    {
        // wasm-tools names the provided function's asyncness first (component_types.rs:3726-3734).
        auto const word = [](binary::Asyncness asyncness) {
            return asyncness == binary::Asyncness::Async ? std::string_view { "async" }
                                                         : std::string_view { "sync" };
        };
        return std::unexpected { Failure { .error = relationError(RelationErrorCode::Asyncness,
                                                                  word(actual.asyncness),
                                                                  word(expected.asyncness)) } };
    }
    if (actual.params.size() != expected.params.size())
        return std::unexpected { Failure { .error = relationError(RelationErrorCode::ParameterCount,
                                                                  std::to_string(expected.params.size()),
                                                                  std::to_string(actual.params.size())) } };
    _frames.push_back(Frame { .kind = FrameKind::Function,
                              .actualSide = actualSide,
                              .actual = actualId.value,
                              .expected = expectedId.value,
                              .memoEpoch = key.epoch });
    return Progress::Pushed;
}

TypeRelation::Outcome<std::optional<uint32_t>> TypeRelation::scan(std::span<types::NamedExtern const> externs,
                                                                  std::string const& name)
{
    // A linear scan, for a path step into an instance's exports: every name compared is charged.
    for (auto const position: std::views::iota(std::size_t { 0 }, externs.size()))
    {
        auto const& candidate = externs[position].name.str();
        if (auto compared = compareNames(1, std::min(candidate.size(), name.size())); !compared)
            return std::unexpected { compared.error() };
        if (candidate == name)
            return static_cast<uint32_t>(position);
    }
    return std::nullopt;
}

TypeRelation::Outcome<std::vector<uint32_t>> TypeRelation::nameIndex(
    std::span<types::NamedExtern const> externs)
{
    // The externs' positions ordered by name, for lookups in O(log n): a step per name, plus its bytes
    // for each of the about log2(n) + 1 times a merge sort compares it, charged up front.
    auto bytes = std::size_t { 0 };
    for (auto const& entry: externs)
        bytes += entry.name.str().size();
    if (auto compared = compareNames(externs.size(), bytes * levels(externs.size())); !compared)
        return std::unexpected { compared.error() };
    auto index = std::vector<uint32_t> {};
    index.reserve(externs.size());
    for (auto const position: std::views::iota(std::size_t { 0 }, externs.size()))
        index.push_back(static_cast<uint32_t>(position));
    std::ranges::stable_sort(index, std::less<> {}, [&](uint32_t position) -> std::string const& {
        return externs[position].name.str();
    });
    return index;
}

TypeRelation::Outcome<std::optional<uint32_t>> TypeRelation::lookup(
    std::span<types::NamedExtern const> externs, std::vector<uint32_t> const& index, std::string const& name)
{
    // A binary search: a step, plus the name's bytes for each of its about log2(n) + 1 comparisons.
    if (auto compared = compareNames(1, name.size() * levels(index.size())); !compared)
        return std::unexpected { compared.error() };
    auto const found =
        std::ranges::lower_bound(index, name, std::less<> {}, [&](uint32_t position) -> std::string const& {
            return externs[position].name.str();
        });
    if (found == index.end() || externs[*found].name.str() != name)
        return std::nullopt;
    return *found;
}

TypeRelation::Outcome<TypeRelation::Progress> TypeRelation::startInstance(Side actualSide,
                                                                          types::InstanceTypeId actualId,
                                                                          types::InstanceTypeId expectedId)
{
    // component_instance_type (component_types.rs:3676-3715): every export expected must be provided,
    // by name; then each is compared.
    if (auto stepped = step(); !stepped)
        return std::unexpected { stepped.error() };
    auto const key = MemoKey { .kind = FrameKind::Instance,
                               .actualSide = actualSide,
                               .actual = actualId.value,
                               .expected = expectedId.value,
                               .epoch = memoEpoch(actualSide, actualId, expectedId) };
    if (memoised(key))
        return Progress::Done;
    auto const& actual = arena(actualSide)[actualId];
    auto const& expected = arena(opposite(actualSide))[expectedId];
    auto frame = Frame { .kind = FrameKind::Instance,
                         .actualSide = actualSide,
                         .actual = actualId.value,
                         .expected = expectedId.value,
                         .memoEpoch = key.epoch };
    auto const names = nameIndex(actual.exports);
    if (!names)
        return std::unexpected { names.error() };
    frame.matched.reserve(expected.exports.size());
    for (auto const& required: expected.exports)
    {
        auto const found = lookup(actual.exports, *names, required.name.str());
        if (!found)
            return std::unexpected { found.error() };
        if (!*found)
            return std::unexpected { Failure {
                .error = relationError(RelationErrorCode::MissingExport, required.name.str()) } };
        frame.matched.push_back(**found);
    }
    _frames.push_back(std::move(frame));
    return Progress::Pushed;
}

TypeRelation::Outcome<TypeRelation::Progress> TypeRelation::startComponent(Side actualSide,
                                                                           types::ComponentTypeId actualId,
                                                                           types::ComponentTypeId expectedId)
{
    if (auto stepped = step(); !stepped)
        return std::unexpected { stepped.error() };
    auto const key = MemoKey { .kind = FrameKind::Component,
                               .actualSide = actualSide,
                               .actual = actualId.value,
                               .expected = expectedId.value,
                               .epoch = memoEpoch(actualSide, actualId, expectedId) };
    if (memoised(key))
        return Progress::Done;
    _frames.push_back(Frame { .kind = FrameKind::Component,
                              .actualSide = actualSide,
                              .actual = actualId.value,
                              .expected = expectedId.value,
                              .undoMark = _undo.size(),
                              .savedEpoch = _epoch,
                              .memoEpoch = key.epoch });
    return Progress::Pushed;
}

TypeRelation::Outcome<TypeRelation::Progress> TypeRelation::startOpen(
    Side actualSide,
    std::span<types::NamedExtern const> arguments,
    types::ComponentTypeId expectedId,
    ExternKind kind)
{
    // open_instance_type (component_types.rs:3911-4033): bind each resource the component quantifies
    // over on this side to what the resource its argument supplies stands for, following its path by
    // index in the component and by name in the arguments (an argument of the wrong shape binds
    // nothing: the type check below reports it); then every extern must be supplied, by name; then
    // each is compared.
    auto const expectedSide = opposite(actualSide);
    auto const& component = arena(expectedSide)[expectedId];
    auto const& entities = kind == ExternKind::Import ? component.imports : component.exports;
    auto const& resources =
        kind == ExternKind::Import ? component.importedResources : component.definedResources;
    auto const names = nameIndex(arguments);
    if (!names)
        return std::unexpected { names.error() };
    for (auto const& [resource, path]: resources)
    {
        Require(!path.empty() && path.front() < entities.size());
        auto const* entity = &entities[path.front()];
        auto found = lookup(arguments, *names, entity->name.str());
        if (!found)
            return std::unexpected { found.error() };
        auto const* argument = *found ? &arguments[**found] : nullptr;
        for (auto const position: path | std::views::drop(1))
        {
            auto const* nested = std::get_if<types::InstanceTypeId>(&entity->type);
            Require(nested != nullptr);
            entity = &arena(expectedSide)[*nested].exports[position];
            auto const* provided = argument ? std::get_if<types::InstanceTypeId>(&argument->type) : nullptr;
            if (!provided)
            {
                argument = nullptr;
                break;
            }
            auto const& exports = arena(actualSide)[*provided].exports;
            auto inner = scan(exports, entity->name.str());
            if (!inner)
                return std::unexpected { inner.error() };
            argument = *inner ? &exports[**inner] : nullptr;
        }
        if (argument == nullptr)
            continue;
        auto const* bound = std::get_if<types::TypeExtern>(&argument->type);
        auto const* supplied = bound ? std::get_if<types::ResourceTypeId>(&bound->type) : nullptr;
        if (supplied)
            bind(expectedSide, resource.value, identity(actualSide, *supplied));
    }

    auto frame = Frame { .kind = FrameKind::Open,
                         .actualSide = actualSide,
                         .expected = expectedId.value,
                         .arguments = arguments,
                         .externKind = kind };
    frame.matched.reserve(entities.size());
    for (auto const& entity: entities)
    {
        auto const found = lookup(arguments, *names, entity.name.str());
        if (!found)
            return std::unexpected { found.error() };
        if (!*found)
            return std::unexpected { Failure { .error = relationError(RelationErrorCode::MissingExtern,
                                                                      externKindWord(kind),
                                                                      entity.name.str()) } };
        frame.matched.push_back(**found);
    }
    _frames.push_back(std::move(frame));
    return Progress::Pushed;
}

std::expected<types::ResourceSubstitution, types::TypeLimitError> freshResourcesFor(
    types::TypeArena& arena, types::ComponentTypeInfo const& instantiated)
{
    // instantiate_component's fresh_defined_resources (component.rs:3454-3462).
    auto fresh = types::ResourceSubstitution {};
    for (auto const& defined: instantiated.definedResources)
    {
        auto const minted = arena.newResource(arena[defined.resource]);
        if (!minted)
            return std::unexpected { minted.error() };
        fresh.bind(defined.resource, *minted);
    }
    return fresh;
}

std::expected<types::InstanceTypeId, types::TypeLimitError> instantiatedExports(
    types::TypeArena& arena,
    types::ComponentTypeId component,
    types::ResourceSubstitution const& substitution)
{
    return arena.substitutedInstance(arena[component].exports, substitution);
}

} // namespace isoline::component::validate
