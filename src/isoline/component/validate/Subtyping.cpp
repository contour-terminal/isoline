// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/validate/Subtyping.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
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

    /// How wasm-tools names a value kind in a mismatch: as the type expected (ComponentDefinedType::desc,
    /// component_types.rs:2020-2038; "primitive" for every primitive), and as the type found (the
    /// component_defined_type arms, component_types.rs:4233-4418; a primitive by its own name).
    struct ValueKindWords
    {
        std::string_view expected;
        std::string_view found;
    };

    /// Per ValueKind, in enumerator order.
    constexpr auto ValueKindWordTable = std::to_array<ValueKindWords>({
        { .expected = "primitive", .found = "bool" },   { .expected = "primitive", .found = "s8" },
        { .expected = "primitive", .found = "u8" },     { .expected = "primitive", .found = "s16" },
        { .expected = "primitive", .found = "u16" },    { .expected = "primitive", .found = "s32" },
        { .expected = "primitive", .found = "u32" },    { .expected = "primitive", .found = "s64" },
        { .expected = "primitive", .found = "u64" },    { .expected = "primitive", .found = "f32" },
        { .expected = "primitive", .found = "f64" },    { .expected = "primitive", .found = "char" },
        { .expected = "primitive", .found = "string" }, { .expected = "primitive", .found = "error-context" },
        { .expected = "list", .found = "list" },        { .expected = "fixed-length list", .found = "list" },
        { .expected = "record", .found = "record" },    { .expected = "tuple", .found = "tuple" },
        { .expected = "variant", .found = "variant" },  { .expected = "enum", .found = "enum" },
        { .expected = "option", .found = "option" },    { .expected = "result", .found = "result" },
        { .expected = "flags", .found = "flags" },      { .expected = "map", .found = "map" },
        { .expected = "own", .found = "own" },          { .expected = "borrow", .found = "borrow" },
        { .expected = "stream", .found = "stream" },    { .expected = "future", .found = "future" },
    });

    static_assert(ValueKindWordTable.size() == types::ValueKindCount, "one row of words per ValueKind");
    static_assert(
        [] {
            for (auto const& row: types::ValueKindTable)
            {
                auto const& words = ValueKindWordTable[static_cast<std::size_t>(row.kind)];
                if (types::isPrimitive(row.kind) && words.found != row.text)
                    return false;
            }
            return true;
        }(),
        "a primitive is found by its own name");

    [[nodiscard]] constexpr ValueKindWords const& wordsOf(ValueKind kind) noexcept
    {
        return ValueKindWordTable[static_cast<std::size_t>(kind)];
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

    [[nodiscard]] std::string_view kindWord(ExternKind kind) noexcept
    {
        return externKindWord(kind);
    }

    template <typename Id>
    [[nodiscard]] AnyTypeId anyOf(Id id)
    {
        return AnyTypeId { id };
    }
} // namespace

TypeRelation::TypeRelation(types::TypeArena const& sub,
                           types::TypeArena const& super,
                           ResourceMapping const& resources,
                           ComponentLimits const& limits):
    _sub { sub }, _super { super }, _sameArena { &sub == &super }, _maxSteps { limits.maxSubtypingSteps }
{
    for (auto const& [from, to]: resources.images())
        _bindings[static_cast<std::size_t>(Side::Super)].insert_or_assign(from.value, to.value);
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
            .error = RelationError { .failure = RelationFailure::StepLimit,
                                     .message = limitMessage(ComponentLimit::SubtypingSteps, _maxSteps) } } };
    }
    _steps += count;
    return {};
}

std::pair<TypeRelation::Side, uint32_t> TypeRelation::identity(Side side,
                                                               types::ResourceTypeId resource) const
{
    // A bound resource is what it is bound to; within one arena an unbound one is itself, whichever
    // side names it.
    auto const& bindings = _bindings[static_cast<std::size_t>(side)];
    auto result = std::pair { side, resource.value };
    if (auto const bound = bindings.find(resource.value); bound != bindings.end())
        result = std::pair { opposite(side), bound->second };
    if (_sameArena)
        result.first = Side::Sub;
    return result;
}

bool TypeRelation::sameResource(Side actualSide,
                                types::ResourceTypeId actual,
                                types::ResourceTypeId expected) const
{
    return identity(actualSide, actual) == identity(opposite(actualSide), expected);
}

void TypeRelation::bind(Side side, uint32_t resource, uint32_t image)
{
    auto& bindings = _bindings[static_cast<std::size_t>(side)];
    auto previous = std::optional<uint32_t> {};
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
        auto const& bindings = _bindings[static_cast<std::size_t>(Side::Super)];
        for (auto const& imported: _super[component].importedResources)
        {
            if (auto const image = bindings.find(imported.resource.value); image != bindings.end())
                bound.bind(imported.resource, types::ResourceTypeId { .value = image->second });
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
        if (failure.error.failure == RelationFailure::Mismatch)
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
        auto advanced = advance(_frames.back());
        if (!advanced)
            return unwind(std::move(advanced.error()));
        if (!*advanced)
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

TypeRelation::Outcome<bool> TypeRelation::advance(Frame& frame)
{
    auto const childFailed = [](Failure failure) {
        failure.site = FailureSite::Child;
        return std::unexpected { std::move(failure) };
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
                return true;
            auto const index = frame.next++;
            auto const started = startEntity(
                frame.actualSide, actual.exports[frame.matched[index]].type, expected.exports[index].type);
            if (!started)
                return childFailed(started.error());
            return false;
        }
        case FrameKind::Component: {
            // component_type (component_types.rs:3588-3674): the expected component's imports must
            // satisfy the actual's (contravariance, the sides swapped), binding the actual's imported
            // resources; then the actual's exports, those bindings applied, must satisfy the expected's.
            auto const actual = types::ComponentTypeId { .value = frame.actual };
            auto const expected = types::ComponentTypeId { .value = frame.expected };
            auto const actualSide = frame.actualSide;
            auto const phase = frame.phase++;
            auto started = Outcome<Progress> { Progress::Done };
            if (phase == 0)
                started = startOpen(opposite(actualSide),
                                    arena(opposite(actualSide))[expected].imports,
                                    actual,
                                    ExternKind::Import);
            else if (phase == 1)
                started =
                    startOpen(actualSide, arena(actualSide)[actual].exports, expected, ExternKind::Export);
            else
                return true;
            // startOpen may have pushed a frame: `frame` is not to be used past here.
            if (!started)
                return childFailed(started.error());
            return false;
        }
        case FrameKind::Open: {
            auto const expectedSide = opposite(frame.actualSide);
            auto const& component = arena(expectedSide)[types::ComponentTypeId { .value = frame.expected }];
            auto const& entities =
                frame.externKind == ExternKind::Import ? component.imports : component.exports;
            if (frame.next == entities.size())
                return true;
            auto const index = frame.next++;
            auto const started = startEntity(
                frame.actualSide, frame.arguments[frame.matched[index]].type, entities[index].type);
            if (!started)
                return childFailed(started.error());
            return false;
        }
    }
    Require(false);
    return true;
}

TypeRelation::Outcome<bool> TypeRelation::advanceValue(Frame& frame)
{
    // component_defined_type (component_types.rs:4233-4418), one child per call: the checks wasm-tools
    // makes of a child before comparing it, then the child.
    auto const actualSide = frame.actualSide;
    auto const& actual = arena(actualSide)[TypeId { .value = frame.actual }];
    auto const& expected = arena(opposite(actualSide))[TypeId { .value = frame.expected }];
    auto const mismatch = [](std::string message) {
        return std::unexpected { Failure { .error = RelationError { .message = std::move(message) } } };
    };
    while (frame.next < actual.children.size())
    {
        auto const index = frame.next++;
        auto const& actualChild = actual.children[index];
        auto const& expectedChild = expected.children[index];
        switch (actual.kind)
        {
            case ValueKind::Record:
                if (actual.labels[index] != expected.labels[index])
                    return mismatch(std::format("expected field name `{}`, found `{}`",
                                                expected.labels[index],
                                                actual.labels[index]));
                break;
            case ValueKind::Variant:
                if (actual.labels[index] != expected.labels[index])
                    return mismatch(std::format("expected case named `{}`, found `{}`",
                                                expected.labels[index],
                                                actual.labels[index]));
                if (!actualChild && expectedChild)
                    return mismatch(
                        std::format("expected case `{}` to have a type, found none", actual.labels[index]));
                if (actualChild && !expectedChild)
                    return mismatch(std::format("expected case `{}` to have no type", actual.labels[index]));
                break;
            case ValueKind::Result: {
                auto const part = index == 0 ? std::string_view { "ok" } : std::string_view { "err" };
                if (!actualChild && expectedChild)
                    return mismatch(std::format("expected {} type, but found none", part));
                if (actualChild && !expectedChild)
                    return mismatch(std::format("expected {} type to not be present", part));
                break;
            }
            default: break;
        }
        if (!actualChild)
            continue;
        auto started = startValue(actualSide, *actualChild, *expectedChild, PrimitiveSpelling::Inline);
        // startValue may have pushed a frame: `frame` is not to be used past here.
        if (!started)
        {
            started.error().site = FailureSite::Child;
            return std::unexpected { std::move(started.error()) };
        }
        return false;
    }
    return true;
}

TypeRelation::Outcome<bool> TypeRelation::advanceFunction(Frame& frame)
{
    // component_func_type (component_types.rs:3717-3792): each parameter's name, then its type; then
    // the result.
    auto const actualSide = frame.actualSide;
    auto const& actual = arena(actualSide)[types::FunctionTypeId { .value = frame.actual }];
    auto const& expected = arena(opposite(actualSide))[types::FunctionTypeId { .value = frame.expected }];
    auto const mismatch = [](std::string message) {
        return std::unexpected { Failure { .error = RelationError { .message = std::move(message) } } };
    };
    auto const index = frame.next++;
    auto started = Outcome<Progress> { Progress::Done };
    if (index < actual.params.size())
    {
        auto const& [actualName, actualType] = actual.params[index];
        auto const& [expectedName, expectedType] = expected.params[index];
        if (actualName != expectedName)
            return mismatch(
                std::format("expected parameter named `{}`, found `{}`", expectedName, actualName));
        started = startValue(actualSide, actualType, expectedType, PrimitiveSpelling::Inline);
    }
    else if (index == actual.params.size())
    {
        if (actual.result && !expected.result)
            return mismatch("expected a result, found none");
        if (!actual.result && expected.result)
            return mismatch("expected no result, found one");
        if (!actual.result)
            return true;
        started = startValue(actualSide, *actual.result, *expected.result, PrimitiveSpelling::Inline);
    }
    else
        return true;
    // startValue may have pushed a frame: `frame` is not to be used past here.
    if (!started)
    {
        started.error().site = FailureSite::Child;
        return std::unexpected { std::move(started.error()) };
    }
    return false;
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
            switch (actual.kind)
            {
                case ValueKind::Record:
                    return std::format("type mismatch in record field `{}`", actual.labels[index]);
                case ValueKind::Variant:
                    return std::format("type mismatch in variant case `{}`", actual.labels[index]);
                case ValueKind::Tuple: return std::format("type mismatch in tuple field {}", index);
                case ValueKind::Map:
                    return index == 0 ? "type mismatch in map key" : "type mismatch in map value";
                case ValueKind::Result:
                    return index == 0 ? "type mismatch in ok variant" : "type mismatch in err variant";
                case ValueKind::Future: return "type mismatch in future";
                case ValueKind::Stream: return "type mismatch in stream";
                default: return std::nullopt;
            }
        }
        case FrameKind::Function: {
            auto const& actual = arena(actualSide)[types::FunctionTypeId { .value = frame.actual }];
            if (index < actual.params.size())
                return std::format("type mismatch in function parameter `{}`", actual.params[index].first);
            return "type mismatch with result type";
        }
        case FrameKind::Instance: {
            auto const& expected =
                arena(opposite(actualSide))[types::InstanceTypeId { .value = frame.expected }];
            return std::format("type mismatch in instance export `{}`", expected.exports[index].name);
        }
        case FrameKind::Component: return std::nullopt;
        case FrameKind::Open: {
            auto const& component =
                arena(opposite(actualSide))[types::ComponentTypeId { .value = frame.expected }];
            auto const& entities =
                frame.externKind == ExternKind::Import ? component.imports : component.exports;
            return std::format("type mismatch for {} `{}`", kindWord(frame.externKind), entities[index].name);
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
                RelationError { .message = std::format(
                                    "expected {}, found {}", entityWord(expected), entityWord(actual)) } } };
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
        return std::unexpected { Failure {
            .error = RelationError { .message = std::format("expected {}, found {}",
                                                            AnyWordTable[expected.index()].expected,
                                                            AnyWordTable[actual.index()].found) } } };
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
                return std::unexpected { Failure {
                    .error = RelationError { .message = "resource types are not the same" } } };
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
    auto const resourceFree =
        actual.handles == types::HandleContent::None && expected.handles == types::HandleContent::None;
    // Within one arena interning makes equal types one id; one that holds no handle is equal to itself
    // under any bindings.
    if (_sameArena && actualId == expectedId && resourceFree)
        return Progress::Done;
    auto const key = MemoKey { .kind = FrameKind::Value,
                               .actualSide = actualSide,
                               .actual = actualId.value,
                               .expected = expectedId.value,
                               .epoch = resourceFree ? 0 : _epoch };
    if (memoised(key))
        return Progress::Done;
    auto const mismatch = [](std::string message) -> Outcome<Progress> {
        return std::unexpected { Failure { .error = RelationError { .message = std::move(message) } } };
    };

    // component_val_type and component_defined_type's arms (component_types.rs:4208-4440).
    if (actual.kind != expected.kind)
    {
        if (types::isPrimitive(actual.kind) && types::isPrimitive(expected.kind))
            return mismatch(std::format("expected primitive `{}` found primitive `{}`",
                                        wordsOf(expected.kind).found,
                                        wordsOf(actual.kind).found));
        if (types::isPrimitive(expected.kind) && spelling == PrimitiveSpelling::Inline)
            return mismatch(std::format(
                "expected {}, found {}", wordsOf(expected.kind).found, wordsOf(actual.kind).expected));
        return mismatch(std::format(
            "expected {}, found {}", wordsOf(expected.kind).expected, wordsOf(actual.kind).found));
    }
    switch (actual.kind)
    {
        case ValueKind::Flags:
        case ValueKind::Enum:
            if (actual.labels != expected.labels)
                return mismatch(std::format("mismatch in {} elements", wordsOf(actual.kind).found));
            return Progress::Done;
        case ValueKind::Own:
        case ValueKind::Borrow:
            if (!sameResource(actualSide, *actual.resource, *expected.resource))
                return mismatch("resource types are not the same");
            return Progress::Done;
        case ValueKind::Record:
            if (actual.children.size() != expected.children.size())
                return mismatch(std::format(
                    "expected {} fields, found {}", expected.children.size(), actual.children.size()));
            break;
        case ValueKind::Variant:
            if (actual.children.size() != expected.children.size())
                return mismatch(std::format(
                    "expected {} cases, found {}", expected.children.size(), actual.children.size()));
            break;
        case ValueKind::Tuple:
            if (actual.children.size() != expected.children.size())
                return mismatch(std::format(
                    "expected {} types, found {}", expected.children.size(), actual.children.size()));
            break;
        case ValueKind::FixedList:
            if (actual.length != expected.length)
                return mismatch(
                    std::format("expected fixed-length {}, found size {}", expected.length, actual.length));
            break;
        case ValueKind::Stream:
        case ValueKind::Future:
            if (actual.children.size() != expected.children.size())
            {
                auto const word = wordsOf(actual.kind).found;
                if (actual.children.empty())
                    return mismatch(std::format("expected {} type, but found none", word));
                return mismatch(std::format("expected {} type to not be present", word));
            }
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
                              .memoEpoch = key.epoch });
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
    auto const resourceFree =
        actual.handles == types::HandleContent::None && expected.handles == types::HandleContent::None;
    auto const key = MemoKey { .kind = FrameKind::Function,
                               .actualSide = actualSide,
                               .actual = actualId.value,
                               .expected = expectedId.value,
                               .epoch = resourceFree ? 0 : _epoch };
    if (memoised(key))
        return Progress::Done;
    if (actual.asyncness != expected.asyncness)
    {
        // wasm-tools names the provided function's asyncness first (component_types.rs:3726-3734).
        auto const word = [](binary::Asyncness asyncness) {
            return asyncness == binary::Asyncness::Async ? "async" : "sync";
        };
        return std::unexpected { Failure {
            .error = RelationError { .message = std::format("expected {} function, found {} function",
                                                            word(actual.asyncness),
                                                            word(expected.asyncness)) } } };
    }
    if (actual.params.size() != expected.params.size())
        return std::unexpected { Failure {
            .error = RelationError { .message = std::format("expected {} parameters, found {}",
                                                            expected.params.size(),
                                                            actual.params.size()) } } };
    _frames.push_back(Frame { .kind = FrameKind::Function,
                              .actualSide = actualSide,
                              .actual = actualId.value,
                              .expected = expectedId.value,
                              .memoEpoch = key.epoch });
    return Progress::Pushed;
}

TypeRelation::Outcome<std::optional<uint32_t>> TypeRelation::lookup(
    std::span<types::NamedExtern const> externs, std::string const& name)
{
    // A scan, every extern looked at a step: the relation's cap bounds it, whatever the widths.
    auto const found = std::ranges::find(externs, name, &NamedExtern::name);
    auto const scanned = static_cast<std::size_t>(found - externs.begin()) + 1;
    if (auto stepped = step(std::min(scanned, externs.size() + 1)); !stepped)
        return std::unexpected { stepped.error() };
    if (found == externs.end())
        return std::nullopt;
    return static_cast<uint32_t>(found - externs.begin());
}

TypeRelation::Outcome<std::vector<uint32_t>> TypeRelation::nameIndex(
    std::span<types::NamedExtern const> externs)
{
    // The externs' positions ordered by name, for lookups in O(log n): an extern a step.
    if (auto stepped = step(externs.size()); !stepped)
        return std::unexpected { stepped.error() };
    auto index = std::vector<uint32_t> {};
    index.reserve(externs.size());
    for (auto const position: std::views::iota(std::size_t { 0 }, externs.size()))
        index.push_back(static_cast<uint32_t>(position));
    std::ranges::stable_sort(index, std::less<> {}, [&](uint32_t position) -> std::string const& {
        return externs[position].name;
    });
    return index;
}

TypeRelation::Outcome<std::optional<uint32_t>> TypeRelation::lookup(
    std::span<types::NamedExtern const> externs, std::vector<uint32_t> const& index, std::string const& name)
{
    if (auto stepped = step(); !stepped)
        return std::unexpected { stepped.error() };
    auto const found =
        std::ranges::lower_bound(index, name, std::less<> {}, [&](uint32_t position) -> std::string const& {
            return externs[position].name;
        });
    if (found == index.end() || externs[*found].name != name)
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
                               .epoch = _epoch };
    if (memoised(key))
        return Progress::Done;
    auto const& actual = arena(actualSide)[actualId];
    auto const& expected = arena(opposite(actualSide))[expectedId];
    auto frame = Frame { .kind = FrameKind::Instance,
                         .actualSide = actualSide,
                         .actual = actualId.value,
                         .expected = expectedId.value,
                         .memoEpoch = key.epoch };
    auto const index = nameIndex(actual.exports);
    if (!index)
        return std::unexpected { index.error() };
    frame.matched.reserve(expected.exports.size());
    for (auto const& required: expected.exports)
    {
        auto const found = lookup(actual.exports, *index, required.name);
        if (!found)
            return std::unexpected { found.error() };
        if (!*found)
            return std::unexpected { Failure {
                .error = RelationError { .message =
                                             std::format("missing expected export `{}`", required.name) } } };
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
                               .epoch = _epoch };
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
    // over on this side to the resource its argument supplies, following its path by index in the
    // component and by name in the arguments (an argument of the wrong shape binds nothing: the type
    // check below reports it); then every extern must be supplied, by name; then each is compared.
    auto const expectedSide = opposite(actualSide);
    auto const& component = arena(expectedSide)[expectedId];
    auto const& entities = kind == ExternKind::Import ? component.imports : component.exports;
    auto const& resources =
        kind == ExternKind::Import ? component.importedResources : component.definedResources;
    auto const index = nameIndex(arguments);
    if (!index)
        return std::unexpected { index.error() };
    for (auto const& [resource, path]: resources)
    {
        Require(!path.empty() && path.front() < entities.size());
        auto const* entity = &entities[path.front()];
        auto found = lookup(arguments, *index, entity->name);
        if (!found)
            return std::unexpected { found.error() };
        auto const* argument = *found ? &arguments[**found] : nullptr;
        for (auto const index: path | std::views::drop(1))
        {
            auto const* nested = std::get_if<types::InstanceTypeId>(&entity->type);
            Require(nested != nullptr);
            entity = &arena(expectedSide)[*nested].exports[index];
            auto const* provided = argument ? std::get_if<types::InstanceTypeId>(&argument->type) : nullptr;
            if (!provided)
            {
                argument = nullptr;
                break;
            }
            auto const& exports = arena(actualSide)[*provided].exports;
            auto inner = lookup(exports, entity->name);
            if (!inner)
                return std::unexpected { inner.error() };
            argument = *inner ? &exports[**inner] : nullptr;
        }
        if (argument == nullptr)
            continue;
        auto const* bound = std::get_if<types::TypeExtern>(&argument->type);
        auto const* supplied = bound ? std::get_if<types::ResourceTypeId>(&bound->type) : nullptr;
        if (supplied)
            bind(expectedSide, resource.value, supplied->value);
    }

    auto frame = Frame { .kind = FrameKind::Open,
                         .actualSide = actualSide,
                         .expected = expectedId.value,
                         .arguments = arguments,
                         .externKind = kind };
    frame.matched.reserve(entities.size());
    for (auto const& entity: entities)
    {
        auto const found = lookup(arguments, *index, entity.name);
        if (!found)
            return std::unexpected { found.error() };
        if (!*found)
            return std::unexpected { Failure {
                .error = RelationError {
                    .message = std::format("missing {} named `{}`", kindWord(kind), entity.name) } } };
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
    auto exports = arena[component].exports;
    for (auto& entry: exports)
    {
        auto substituted = arena.substitute(entry.type, substitution);
        if (!substituted)
            return std::unexpected { substituted.error() };
        entry.type = *substituted;
    }
    return arena.add(types::InstanceTypeInfo { .exports = std::move(exports) });
}

} // namespace isoline::component::validate
