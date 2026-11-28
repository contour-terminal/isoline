// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/api/CallError.hpp>
#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/api/Typed.hpp>
#include <isoline/component/api/testing/ApiFixtures.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/ComponentFixtures.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/testing/ResourceDoubles.hpp>
#include <isoline/component/runtime/testing/StoreFixtures.hpp>

using isoline::component::ComponentLimits;
using isoline::component::api::ComponentCallError;
using isoline::component::api::ComponentCallFailure;
using isoline::component::api::ComponentConfig;
using isoline::component::api::ComponentStore;
using isoline::component::api::FunctionTypeSpec;
using isoline::component::api::HostComponentContext;
using isoline::component::api::HostResourceType;
using isoline::component::api::Linker;
using isoline::component::api::LinkerErrorCode;
using isoline::component::api::testing::ApiHarness;
using isoline::component::api::testing::instanceOf;
using isoline::component::api::testing::require;
using isoline::component::api::typed::Borrow;
using isoline::component::api::typed::canMakeTypedHostFunction;
using isoline::component::api::typed::Case;
using isoline::component::api::typed::functionType;
using isoline::component::api::typed::hostBorrow;
using isoline::component::api::typed::hostOwn;
using isoline::component::api::typed::HostResourceTable;
using isoline::component::api::typed::makeTypedHostFunction;
using isoline::component::api::typed::Own;
using isoline::component::api::typed::ResourceTypes;
using isoline::component::api::typed::StructuralMatch;
using isoline::component::api::typed::TypeContext;
using isoline::component::api::typed::TypedFunction;
using isoline::component::api::typed::TypeOf;
using isoline::component::api::typed::Unit;
using isoline::component::binary::Asyncness;
using isoline::component::binary::BorrowType;
using isoline::component::binary::CanonBuiltin;
using isoline::component::binary::CanonDefinition;
using isoline::component::binary::CanonOption;
using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreSort;
using isoline::component::binary::DefinedValueType;
using isoline::component::binary::EnumType;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::FlagsType;
using isoline::component::binary::FunctionTypeDefinition;
using isoline::component::binary::LabeledType;
using isoline::component::binary::ListType;
using isoline::component::binary::MapType;
using isoline::component::binary::OptionType;
using isoline::component::binary::OwnType;
using isoline::component::binary::PrimitiveValueType;
using isoline::component::binary::RecordType;
using isoline::component::binary::ResultType;
using isoline::component::binary::Sort;
using isoline::component::binary::TupleType;
using isoline::component::binary::TypeBound;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::TypeIndex;
using isoline::component::binary::ValueTypeRef;
using isoline::component::binary::VariantCase;
using isoline::component::binary::VariantType;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::liftedIdentity;
using isoline::component::canonical::LiveBytesBudget;
using isoline::component::canonical::Ownership;
using isoline::component::canonical::StringEncoding;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::ValueBuilder;
using isoline::component::canonical::Values;
using isoline::component::runtime::testing::coreModule;
using isoline::component::runtime::testing::embedderFailure;
using isoline::component::runtime::testing::option;
using isoline::component::types::DefinedType;
using isoline::component::types::TypeArena;
using isoline::component::types::TypeId;
using isoline::component::types::ValueKind;

namespace
{

/// A command's output: a record of two fields.
struct CommandOutput
{
    int32_t exitCode = 0;
    std::string output;

    bool operator==(CommandOutput const&) const = default;
};

/// CommandOutput with its second field labelled otherwise.
struct RenamedOutput
{
    int32_t exitCode = 0;
    std::string text;
};

/// CommandOutput with its second field of another type.
struct WideOutput
{
    int32_t exitCode = 0;
    uint32_t output = 0;
};

/// Why a command failed: an enum.
enum class CommandError : uint8_t
{
    NotFound,
    Denied,
};

/// CommandError labelled otherwise.
enum class OtherError : uint8_t
{
    NotFound,
    Refused,
};

/// A host resource type's C++ tag.
struct Pane
{
};

/// Another host resource type's C++ tag.
struct Tab
{
};

/// A guest resource type's C++ tag: no HostResourceType stands for it.
struct GuestThing
{
};

/// A status-line segment's severity.
enum class Severity : uint8_t
{
    Info,
    Warning,
    Error,
};

/// A status-line segment's style: a flags type.
struct Style
{
    uint32_t bits = 0;

    bool operator==(Style const&) const = default;
};

/// Style labelled otherwise.
struct OtherStyle
{
    uint32_t bits = 0;
};

/// A status-line segment: a record of every row's type.
struct Segment
{
    std::string text;
    Severity severity = Severity::Info;
    Style style {};
    std::optional<uint32_t> width;
    std::vector<std::string> tags;
    float ratio = 0;
    double weight = 0;
    char32_t glyph = 0;
    bool visible = false;
    int8_t tiny = 0;
    int16_t small = 0;
    int64_t stamp = 0;
    uint8_t byte = 0;
    uint16_t word = 0;
    uint64_t large = 0;
    std::expected<Unit, std::string> state;
    std::map<std::string, int32_t> counters;
    std::tuple<uint32_t, int32_t> position;

    bool operator==(Segment const&) const = default;
};

/// A status-line event: a variant of a record, no payload and a tuple.
using Event = std::
    variant<Case<"segment", Segment>, Case<"clear", Unit>, Case<"progress", std::tuple<uint32_t, uint32_t>>>;

/// Event with its second case labelled otherwise.
using OtherEvent = std::
    variant<Case<"segment", Segment>, Case<"reset", Unit>, Case<"progress", std::tuple<uint32_t, uint32_t>>>;

/// Event with its third case of another payload.
using NarrowEvent = std::variant<Case<"segment", Segment>, Case<"clear", Unit>, Case<"progress", uint32_t>>;

} // namespace

namespace isoline::component::api::typed
{

template <>
struct Record<CommandOutput>
{
    static constexpr auto Fields =
        std::tuple { field("exit-code", &CommandOutput::exitCode), field("output", &CommandOutput::output) };
};

template <>
struct Record<RenamedOutput>
{
    static constexpr auto Fields =
        std::tuple { field("exit-code", &RenamedOutput::exitCode), field("text", &RenamedOutput::text) };
};

template <>
struct Record<WideOutput>
{
    static constexpr auto Fields =
        std::tuple { field("exit-code", &WideOutput::exitCode), field("output", &WideOutput::output) };
};

template <>
struct Enum<CommandError>
{
    static constexpr auto Labels = std::to_array<std::string_view>({ "not-found", "denied" });
};

template <>
struct Enum<OtherError>
{
    static constexpr auto Labels = std::to_array<std::string_view>({ "not-found", "refused" });
};

template <>
struct Enum<Severity>
{
    static constexpr auto Labels = std::to_array<std::string_view>({ "info", "warning", "error" });
};

template <>
struct Flags<Style>
{
    static constexpr auto Labels = std::to_array<std::string_view>({ "bold", "italic", "underline" });
};

template <>
struct Flags<OtherStyle>
{
    static constexpr auto Labels = std::to_array<std::string_view>({ "bold", "italic", "blink" });
};

template <>
struct Record<Segment>
{
    static constexpr auto Fields = std::tuple {
        field("text", &Segment::text),         field("severity", &Segment::severity),
        field("style", &Segment::style),       field("width", &Segment::width),
        field("tags", &Segment::tags),         field("ratio", &Segment::ratio),
        field("weight", &Segment::weight),     field("glyph", &Segment::glyph),
        field("visible", &Segment::visible),   field("tiny", &Segment::tiny),
        field("small", &Segment::small),       field("stamp", &Segment::stamp),
        field("byte", &Segment::byte),         field("word", &Segment::word),
        field("large", &Segment::large),       field("state", &Segment::state),
        field("counters", &Segment::counters), field("position", &Segment::position),
    };
};

} // namespace isoline::component::api::typed

namespace
{

/// The value of an expected that must have one (REQUIREd).
template <typename T, typename E>
[[nodiscard]] T valueOf(std::expected<T, E> outcome)
{
    if constexpr (requires { outcome.error().message(); })
        if (!outcome)
            FAIL(outcome.error().message());
    REQUIRE(outcome.has_value());
    return std::move(*outcome);
}

/// The failure of an expected that must have failed (REQUIREd).
template <typename T>
[[nodiscard]] ComponentCallFailure failureOf(std::expected<T, ComponentCallError> const& outcome)
{
    REQUIRE_FALSE(outcome.has_value());
    return outcome.error().failure;
}

/// The types of a linker's arena, encoded into a component: anonymous types as type definitions, and
/// records, variants, enums and flags imported under fresh names, as an import's or export's type must
/// name them.
class GuestTypes
{
  public:
    GuestTypes(ComponentBuilder& builder, TypeArena const& arena): _builder { &builder }, _arena { &arena } {}

    /// The guest type of @p id.
    [[nodiscard]] ValueTypeRef of(TypeId id)
    {
        if (auto const found = _defined.find(id.value); found != _defined.end())
            return found->second;
        auto ref = define((*_arena)[id]);
        _defined.emplace(id.value, ref);
        return ref;
    }

    /// The guest function type of @p spec.
    [[nodiscard]] uint32_t function(FunctionTypeSpec const& spec)
    {
        auto params = std::vector<LabeledType> {};
        for (auto const& [label, id]: spec.params)
            params.push_back(LabeledType { .label = label, .type = of(id) });
        auto result = std::optional<ValueTypeRef> {};
        if (spec.result)
            result = of(*spec.result);
        return _builder->type(TypeDefinition { .type = FunctionTypeDefinition { .asyncness = Asyncness::Sync,
                                                                                .params = std::move(params),
                                                                                .result = result } });
    }

    /// Encodes the linker resource of @p handle (an own or borrow type) as the guest's @p resource.
    void bindResource(TypeId handle, uint32_t resource)
    {
        _resources.emplace((*_arena)[handle].resource->value, resource);
    }

  private:
    [[nodiscard]] std::optional<ValueTypeRef> optionalOf(std::optional<TypeId> id)
    {
        if (!id)
            return std::nullopt;
        return of(*id);
    }

    [[nodiscard]] ValueTypeRef anonymous(DefinedValueType type)
    {
        return TypeIndex { .index = _builder->type(TypeDefinition { .type = std::move(type) }) };
    }

    [[nodiscard]] ValueTypeRef named(DefinedValueType type)
    {
        auto const defined = _builder->type(TypeDefinition { .type = std::move(type) });
        auto const imported = _builder->import(std::format("t{}", _named++),
                                               ExternDesc { .sort = Sort::Type, .index = defined });
        return TypeIndex { .index = imported };
    }

    [[nodiscard]] std::vector<std::string> labelsOf(DefinedType const& type) const
    {
        auto labels = std::vector<std::string> {};
        for (auto const& label: type.labels)
            labels.push_back(label.str());
        return labels;
    }

    [[nodiscard]] ValueTypeRef define(DefinedType const& type)
    {
        if (type.kind <= ValueKind::String)
            return static_cast<PrimitiveValueType>(type.kind);
        if (type.kind == ValueKind::List)
            return anonymous(ListType { .element = of(*type.children[0]) });
        if (type.kind == ValueKind::Option)
            return anonymous(OptionType { .type = of(*type.children[0]) });
        if (type.kind == ValueKind::Result)
            return anonymous(
                ResultType { .ok = optionalOf(type.children[0]), .error = optionalOf(type.children[1]) });
        if (type.kind == ValueKind::Map)
            return anonymous(MapType { .key = of(*type.children[0]), .value = of(*type.children[1]) });
        if (type.kind == ValueKind::Tuple)
        {
            auto elements = std::vector<ValueTypeRef> {};
            for (auto const& child: type.children)
                elements.push_back(of(*child));
            return anonymous(TupleType { .elements = std::move(elements) });
        }
        if (type.kind == ValueKind::Own)
            return anonymous(OwnType { .resource = _resources.at(type.resource->value) });
        if (type.kind == ValueKind::Borrow)
            return anonymous(BorrowType { .resource = _resources.at(type.resource->value) });
        if (type.kind == ValueKind::Enum)
            return named(EnumType { .labels = labelsOf(type) });
        if (type.kind == ValueKind::Flags)
            return named(FlagsType { .labels = labelsOf(type) });
        auto const labels = labelsOf(type);
        if (type.kind == ValueKind::Record)
        {
            auto fields = std::vector<LabeledType> {};
            for (auto const index: std::views::iota(std::size_t { 0 }, labels.size()))
                fields.push_back(LabeledType { .label = labels[index], .type = of(*type.children[index]) });
            return named(RecordType { .fields = std::move(fields) });
        }
        REQUIRE(type.kind == ValueKind::Variant);
        auto cases = std::vector<VariantCase> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, labels.size()))
            cases.push_back(VariantCase { .label = labels[index], .type = optionalOf(type.children[index]) });
        return named(VariantType { .cases = std::move(cases) });
    }

    ComponentBuilder* _builder;
    TypeArena const* _arena;
    std::map<uint32_t, ValueTypeRef> _defined {};
    std::map<uint32_t, uint32_t> _resources {};
    uint32_t _named = 0;
};

/// A core module with a memory and a bump `realloc` from 8192 up.
constexpr auto MemoryModule = R"((module
  (memory (export "mem") 1)
  (global $next (mut i32) (i32.const 8192))
  (func (export "realloc") (param i32 i32 i32 i32) (result i32)
    (local $at i32)
    (local.set $at (i32.and (i32.add (global.get $next) (i32.sub (local.get 2) (i32.const 1)))
                            (i32.sub (i32.const 0) (local.get 2))))
    (global.set $next (i32.add (local.get $at) (local.get 3)))
    (local.get $at)))
)";

/// The memory and realloc options of the memory module's instance.
[[nodiscard]] std::vector<CanonOption> memoryOptions(ComponentBuilder& builder)
{
    auto const memory = builder.coreInstantiate(builder.coreModule(coreModule(MemoryModule)), {});
    return {
        option(isoline::component::binary::CanonOptionKind::Memory,
               builder.aliasCoreExport(memory, "mem", CoreSort::Memory)),
        option(isoline::component::binary::CanonOptionKind::Realloc,
               builder.aliasCoreExport(memory, "realloc", CoreSort::Func)),
    };
}

/// Each function of @p functions imported under its name and exported as `<name>-relay`, the lift of
/// its lowering: a call of the export lowers its arguments into the guest and lifts them back to the
/// host function, whose results take the way back. Every function flattens to at most 16 parameters and
/// one result, so lift and lowering share their core type.
std::map<std::string, uint32_t> relays(ComponentBuilder& builder,
                                       GuestTypes& guest,
                                       std::vector<std::pair<std::string, FunctionTypeSpec>> const& functions)
{
    auto const options = memoryOptions(builder);
    auto imports = std::map<std::string, uint32_t> {};
    for (auto const& [name, spec]: functions)
    {
        auto const type = guest.function(spec);
        auto const imported = builder.import(name, ExternDesc { .sort = Sort::Func, .index = type });
        imports.emplace(name, imported);
        std::ignore =
            builder.exportItem(name + "-relay",
                               Sort::Func,
                               builder.canonLift(builder.canonLower(imported, options), options, type));
    }
    return imports;
}

/// A host function of a signature, defined in @p linker under @p name.
template <typename Signature, typename F, typename... Resources>
[[nodiscard]] std::pair<std::string, FunctionTypeSpec> defineTyped(
    Linker& linker, std::string name, F body, ResourceTypes<Resources...> const& resources = {})
{
    auto spec = valueOf(functionType<Signature>(linker, resources));
    require(linker.defineFunction(name, spec, makeTypedHostFunction<Signature>(std::move(body))));
    return { std::move(name), std::move(spec) };
}

/// The function bound at @p path (REQUIREd to bind).
template <typename Signature>
[[nodiscard]] TypedFunction<Signature> bound(isoline::component::api::ComponentInstance& instance,
                                             std::string_view path)
{
    auto function = TypedFunction<Signature>::bind(instance, path);
    REQUIRE(function.has_value());
    return std::move(*function);
}

/// @p from's value, moved out where the caller cannot see it, so that reading @p from afterwards (to check
/// what a move leaves) is no use after a move.
template <typename T>
[[nodiscard]] T moveOut(T& from)
{
    return T { std::move(from) };
}

/// A builder of one value of each of @p roots in @p arena.
[[nodiscard]] std::unique_ptr<ValueBuilder> builderOf(TypeArena const& arena,
                                                      std::vector<TypeId> const& roots)
{
    return std::make_unique<ValueBuilder>(arena, roots, std::make_shared<LiveBytesBudget>(1U << 20));
}

/// The type of @p T in @p linker's arena (REQUIREd).
template <typename T>
[[nodiscard]] TypeId typeIn(Linker& linker)
{
    auto const resources = ResourceTypes<> {};
    auto context = TypeContext<> { linker, resources };
    return valueOf(TypeOf<T>::type(context));
}

/// One value of @p T, written by its row into @p linker's arena.
template <typename T>
[[nodiscard]] Values valuesOf(Linker& linker, T const& value)
{
    auto const builder = builderOf(linker.types(), { typeIn<T>(linker) });
    TypeOf<T>::write(*builder, value);
    return valueOf(builder->finish());
}

} // namespace

TEST_CASE("Typed.typeOfMatchesDynamic")
{
    using Signature = std::expected<CommandOutput, CommandError>(
        std::string, std::vector<std::tuple<std::string, std::string>>);
    auto linker = Linker { ComponentConfig {} };
    auto const typed = valueOf(functionType<Signature>(linker));

    auto& types = linker.types();
    auto const interned = [&](DefinedType type) {
        return valueOf(types.intern(std::move(type)));
    };
    auto const string = types.primitive(ValueKind::String);
    auto const output = interned(DefinedType { .kind = ValueKind::Record,
                                               .children = { types.primitive(ValueKind::S32), string },
                                               .labels = { "exit-code", "output" } });
    auto const error = interned(DefinedType { .kind = ValueKind::Enum, .labels = { "not-found", "denied" } });
    auto const result = interned(DefinedType { .kind = ValueKind::Result, .children = { output, error } });
    auto const pair = interned(DefinedType { .kind = ValueKind::Tuple, .children = { string, string } });
    auto const list = interned(DefinedType { .kind = ValueKind::List, .children = { pair } });
    CHECK(typed.params == std::vector<std::pair<std::string, TypeId>> { { "p0", string }, { "p1", list } });
    CHECK(typed.result == result);
    require(linker.defineFunction(
        "run", typed, makeTypedHostFunction<Signature>([](std::string const&, auto const&) noexcept {
            return std::expected<CommandOutput, CommandError> {};
        })));

    // The parameters may be labelled; a void or Unit result is none; Unit is a case without payload.
    auto const names = std::to_array<std::string_view>({ "command", "env" });
    auto const labelled = valueOf(functionType<Signature>(linker, {}, names));
    CHECK(labelled.params
          == std::vector<std::pair<std::string, TypeId>> { { "command", string }, { "env", list } });
    CHECK(valueOf(functionType<void(uint8_t)>(linker)).result == std::nullopt);
    CHECK(valueOf(functionType<Unit()>(linker)).result == std::nullopt);
    CHECK(valueOf(functionType<std::expected<Unit, std::string>()>(linker)).result
          == interned(DefinedType { .kind = ValueKind::Result, .children = { std::nullopt, string } }));
    CHECK(valueOf(functionType<std::map<std::string, std::optional<char32_t>>()>(linker)).result
          == interned(DefinedType {
              .kind = ValueKind::Map,
              .children = { string,
                            interned(DefinedType { .kind = ValueKind::Option,
                                                   .children = { types.primitive(ValueKind::Char) } }) } }));

    // Own and Borrow name the host resource types bound to their C++ types.
    auto const pane = HostResourceType { "pane", nullptr };
    auto const resources = ResourceTypes<Pane> { pane };
    auto const handles = valueOf(functionType<Own<Pane>(Borrow<Pane>)>(linker, resources));
    CHECK(handles.params.front().second == valueOf(linker.borrowType(pane)));
    CHECK(handles.result == valueOf(linker.ownType(pane)));

    // A type past the linker's caps is refused, whether a parameter's, inside a tuple, or the result's.
    auto shallow = Linker { ComponentConfig { .limits = { .maxTypeDepth = 2 } } };
    using Deep = std::vector<std::vector<uint32_t>>;
    for (auto const& refused: { functionType<void(Deep)>(shallow),
                                functionType<void(std::tuple<Deep, uint32_t>)>(shallow),
                                functionType<Deep()>(shallow) })
    {
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == LinkerErrorCode::TypeLimit);
        CHECK(refused.error().detail == "maxTypeDepth");
    }
}

TEST_CASE("Typed.hostFunctionRoundTrip")
{
    auto harness = ApiHarness {};
    auto linker = Linker { ComponentConfig {} };
    auto seen = std::vector<std::string> {};
    auto notes = std::vector<uint32_t> {};
    auto const functions = std::vector {
        defineTyped<uint32_t(std::string)>(linker,
                                           "len",
                                           [&seen](std::string const& text) noexcept {
                                               seen.push_back(text);
                                               return static_cast<uint32_t>(text.size());
                                           }),
        defineTyped<uint32_t(char32_t)>(
            linker, "glyph", [](char32_t glyph) noexcept { return static_cast<uint32_t>(glyph); }),
        defineTyped<void(uint32_t)>(
            linker, "note", [&notes](uint32_t note) noexcept { notes.push_back(note); }),
        defineTyped<Unit(uint32_t)>(linker,
                                    "mark",
                                    [&notes](HostComponentContext&, uint32_t note) noexcept
                                        -> std::expected<Unit, isoline::api::HostFailure> {
                                        notes.push_back(note + 100);
                                        return Unit {};
                                    }),
        defineTyped<uint32_t(std::string)>(linker,
                                           "fail",
                                           [](HostComponentContext& context, std::string const&) noexcept
                                               -> std::expected<uint32_t, isoline::api::HostFailure> {
                                               CHECK_FALSE(context.store().isLockedDown());
                                               return std::unexpected { embedderFailure(7) };
                                           }),
    };
    auto builder = ComponentBuilder {};
    auto guest = GuestTypes { builder, linker.types() };
    relays(builder, guest, functions);
    auto const component = harness.load(builder.build());
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, linker));

    auto len = bound<uint32_t(std::string)>(instance, "len-relay");
    CHECK(valueOf(len("hello")) == 5);
    CHECK(valueOf(len(std::string { "wörld" })) == 6);
    CHECK(seen == std::vector<std::string> { "hello", "wörld" });
    auto glyph = bound<uint32_t(char32_t)>(instance, "glyph-relay");
    CHECK(valueOf(glyph(U'x')) == 120);
    // A surrogate is no char: the argument is refused before anything runs.
    auto const surrogate = glyph(char32_t { 0xD800 });
    CHECK(failureOf(surrogate) == ComponentCallFailure::ArgumentMismatch);
    CHECK_FALSE(store.isLockedDown());
    REQUIRE(bound<void(uint32_t)>(instance, "note-relay")(3U).has_value());
    CHECK(valueOf(bound<Unit(uint32_t)>(instance, "mark-relay")(4U)) == Unit {});
    CHECK(notes == std::vector<uint32_t> { 3, 104 });

    // A host body's failure ends the call and locks the store down.
    auto const failed = bound<uint32_t(std::string)>(instance, "fail-relay")("x");
    REQUIRE(failureOf(failed) == ComponentCallFailure::HostFailure);
    auto const* const failure = std::get_if<isoline::api::HostFailure>(&failed.error().detail);
    REQUIRE(failure != nullptr);
    CHECK(failure->code == 7);
    CHECK(failureOf(len("again")) == ComponentCallFailure::LockedDown);
}

namespace
{

/// A callable that may throw.
struct ThrowingBody
{
    uint32_t operator()() const { return 1; }
};

} // namespace

TEST_CASE("Typed.throwingLambdaDoesNotCompile")
{
    auto const throwing = [] {
        return uint32_t { 1 };
    };
    auto const nothrow = []() noexcept {
        return uint32_t { 1 };
    };
    auto const withContext = [](HostComponentContext&, std::string const&) noexcept {
        return uint32_t { 1 };
    };
    auto const fallible = []() noexcept -> std::expected<uint32_t, isoline::api::HostFailure> {
        return 1U;
    };
    STATIC_REQUIRE_FALSE(canMakeTypedHostFunction<uint32_t(), decltype(throwing)>);
    STATIC_REQUIRE_FALSE(canMakeTypedHostFunction<uint32_t(), ThrowingBody>);
    STATIC_REQUIRE(canMakeTypedHostFunction<uint32_t(), decltype(nothrow)>);
    STATIC_REQUIRE(canMakeTypedHostFunction<uint32_t(std::string), decltype(withContext)>);
    STATIC_REQUIRE(canMakeTypedHostFunction<uint32_t(), decltype(fallible)>);
    // A generic body is judged by its call with the parameters.
    auto const genericThrowing = [](auto value) {
        return value;
    };
    auto const genericNothrow = [](auto value) noexcept {
        return value;
    };
    STATIC_REQUIRE_FALSE(canMakeTypedHostFunction<uint32_t(uint32_t), decltype(genericThrowing)>);
    STATIC_REQUIRE(canMakeTypedHostFunction<uint32_t(uint32_t), decltype(genericNothrow)>);
    // A body taking a value whose move may throw is still noexcept: its declaration says so.
    auto const byValue = [](std::map<std::string, std::string> entries) noexcept {
        auto const kept = std::move(entries);
        return static_cast<uint32_t>(kept.size());
    };
    STATIC_REQUIRE(canMakeTypedHostFunction<uint32_t(std::map<std::string, std::string>), decltype(byValue)>);
    // The result must be the signature's, and the parameters the body's.
    STATIC_REQUIRE_FALSE(canMakeTypedHostFunction<uint64_t(), decltype(nothrow)>);
    STATIC_REQUIRE_FALSE(canMakeTypedHostFunction<uint32_t(uint32_t), decltype(nothrow)>);
    STATIC_REQUIRE_FALSE(canMakeTypedHostFunction<uint32_t(), decltype(withContext)>);
    // An own is move-only, a borrow is copyable.
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Own<Pane>>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<Own<Pane>>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<Own<Pane>>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<Own<Pane>>);
    STATIC_REQUIRE(std::is_copy_constructible_v<Borrow<Pane>>);
}

TEST_CASE("Typed.exportBindChecksType")
{
    auto harness = ApiHarness {};
    auto const component = harness.load(liftedIdentity().build());
    auto const linker = Linker { ComponentConfig {} };
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, linker));

    auto id = bound<uint32_t(uint32_t)>(instance, "id");
    CHECK(valueOf(id(7U)) == 7);
    CHECK_FALSE(TypedFunction<uint64_t(uint32_t)>::bind(instance, "id").has_value());
    CHECK_FALSE(TypedFunction<uint32_t(uint64_t)>::bind(instance, "id").has_value());
    CHECK_FALSE(TypedFunction<uint32_t(uint32_t, uint32_t)>::bind(instance, "id").has_value());
    CHECK_FALSE(TypedFunction<uint32_t()>::bind(instance, "id").has_value());
    CHECK_FALSE(TypedFunction<void(uint32_t)>::bind(instance, "id").has_value());
    CHECK_FALSE(TypedFunction<uint32_t(uint32_t)>::bind(instance, "missing").has_value());

    // Once the store is gone, nothing binds, and a bound function's call is refused.
    auto other = std::optional { harness.store() };
    auto orphan = instanceOf(other->instantiate(component, linker));
    auto orphaned = bound<uint32_t(uint32_t)>(orphan, "id");
    auto const function = isoline::component::api::testing::lookup(orphan, "id");
    REQUIRE(function.has_value());
    CHECK(function->types() == &component.types());
    other.reset();
    CHECK(function->types() == nullptr);
    CHECK_FALSE(TypedFunction<uint32_t(uint32_t)>::bind(orphan, "id").has_value());
    CHECK(failureOf(orphaned(1U)) == ComponentCallFailure::StoreDeleted);
}

TEST_CASE("Typed.recordEnumFlagsVariant")
{
    // relay(e: event) -> event calls the host's echo(e: event) -> event: both flatten past 16 values, so
    // the event goes to the guest's memory and back through a parameter pointer and a return pointer.
    auto harness = ApiHarness {};
    auto linker = Linker { ComponentConfig {} };
    auto echoed = 0;
    auto const echo = defineTyped<Event(Event)>(linker, "echo", [&echoed](Event event) noexcept {
        ++echoed;
        return event;
    });
    auto builder = ComponentBuilder {};
    auto guest = GuestTypes { builder, linker.types() };
    auto const type = guest.function(echo.second);
    auto const imported = builder.import("echo", ExternDesc { .sort = Sort::Func, .index = type });
    auto const options = memoryOptions(builder);
    auto const lowered = builder.coreInlineExports({ CoreInlineExport {
        .name = "echo", .sort = CoreSort::Func, .index = builder.canonLower(imported, options) } });
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "h" "echo" (func $echo (param i32 i32)))
      (func (export "relay") (param i32) (result i32)
        (call $echo (local.get 0) (i32.const 4096))
        (i32.const 4096))))")),
                                              { { "h", lowered } });
    std::ignore = builder.exportItem(
        "relay",
        Sort::Func,
        builder.canonLift(builder.aliasCoreExport(core, "relay", CoreSort::Func), options, type));
    auto const component = harness.load(builder.build());
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, linker));

    auto relay = bound<Event(Event)>(instance, "relay");
    auto const events = std::vector<Event> {
        Case<"segment", Segment> { Segment {
            .text = "build ✓",
            .severity = Severity::Warning,
            .style = Style { .bits = 0b101 },
            .width = 12,
            .tags = { "ci", "", "lane-a" },
            .ratio = 0.25F,
            .weight = -1.5,
            .glyph = U'é',
            .visible = true,
            .tiny = -8,
            .small = -300,
            .stamp = -(int64_t { 1 } << 40),
            .byte = 200,
            .word = 60000,
            .large = uint64_t { 1 } << 63,
            .state = Unit {},
            .counters = { { "errors", -1 }, { "warnings", 3 } },
            .position = { 4, -2 },
        } },
        Case<"segment", Segment> { Segment {
            .text = "",
            .severity = Severity::Error,
            .style = Style {},
            .width = std::nullopt,
            .tags = {},
            .ratio = 0,
            .weight = 0,
            .glyph = 0,
            .visible = false,
            .tiny = 0,
            .small = 0,
            .stamp = 0,
            .byte = 0,
            .word = 0,
            .large = 0,
            .state = std::unexpected { std::string { "offline" } },
            .counters = {},
            .position = {},
        } },
        Case<"clear", Unit> {},
        Case<"progress", std::tuple<uint32_t, uint32_t>> { { 3, 7 } },
    };
    for (auto const& event: events)
        CHECK(valueOf(relay(event)) == event);
    CHECK(echoed == 4);
    // Labels and types are the binding's: another case's label, or another payload, binds nothing.
    CHECK_FALSE(TypedFunction<OtherEvent(Event)>::bind(instance, "relay").has_value());
    CHECK_FALSE(TypedFunction<Event(NarrowEvent)>::bind(instance, "relay").has_value());
}

TEST_CASE("Typed.mapKeepsLastDuplicate")
{
    // m() -> map<string, u32> returns the guest's [("a", 1), ("a", 2)].
    using Counts = std::map<std::string, uint32_t>;
    auto harness = ApiHarness {};
    auto linker = Linker { ComponentConfig {} };
    auto builder = ComponentBuilder {};
    auto guest = GuestTypes { builder, linker.types() };
    auto const type = guest.function(valueOf(functionType<Counts()>(linker)));
    auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (memory (export "mem") 1)
      (data (i32.const 16) "a")
      (data (i32.const 32) "\10\00\00\00\01\00\00\00\01\00\00\00\10\00\00\00\01\00\00\00\02\00\00\00")
      (data (i32.const 64) "\20\00\00\00\02\00\00\00")
      (func (export "m") (result i32) (i32.const 64))))")),
                                              {});
    std::ignore = builder.exportItem(
        "m",
        Sort::Func,
        builder.canonLift(builder.aliasCoreExport(core, "m", CoreSort::Func),
                          { option(isoline::component::binary::CanonOptionKind::Memory,
                                   builder.aliasCoreExport(core, "mem", CoreSort::Memory)) },
                          type));
    auto const component = harness.load(builder.build());
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, linker));

    CHECK(valueOf(bound<Counts()>(instance, "m")()) == Counts { { "a", 2 } });
}

TEST_CASE("Typed.hostResourceTableGenerations")
{
    auto table = HostResourceTable<std::string> { 2 };
    auto const first = valueOf(table.insert("one"));
    REQUIRE(table.find(first) != nullptr);
    CHECK(*table.find(first) == "one");
    table.erase(first);
    CHECK(table.find(first) == nullptr);
    auto const second = valueOf(table.insert("two"));
    CHECK(table.slotOf(second) == table.slotOf(first));
    CHECK(second != first);
    CHECK(table.find(first) == nullptr);
    auto const* const two = table.find(second);
    REQUIRE(two != nullptr);
    auto const third = valueOf(table.insert("three"));
    CHECK(table.slotOf(third) != table.slotOf(second));
    CHECK(table.find(second) == two); // an insert moves no object
    auto const full = table.insert("four");
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error().code == TrapCode::Handles);
    CHECK(full.error().message() == "handles count exceeds limit of 2");
    // A stale rep erases nothing; erasing frees the slot for the next insert.
    table.erase(first);
    CHECK(*table.find(second) == "two");
    table.erase(third);
    CHECK(table.slotOf(valueOf(table.insert("five"))) == table.slotOf(third));

    // A rep of a slot never used finds nothing.
    auto wide = HostResourceTable<std::string> { 4 };
    auto const only = valueOf(wide.insert("only"));
    CHECK(wide.find(only | 2) == nullptr);

    // With 2^31 slots a rep keeps one generation bit: a slot is reused once, then retired.
    auto huge = HostResourceTable<std::string> { uint32_t { 1 } << 31 };
    auto const a = valueOf(huge.insert("a"));
    huge.erase(a);
    auto const b = valueOf(huge.insert("b"));
    CHECK(huge.slotOf(b) == huge.slotOf(a));
    CHECK(b != a);
    huge.erase(b);
    auto const c = valueOf(huge.insert("c"));
    CHECK(huge.slotOf(c) != huge.slotOf(a));
    CHECK(huge.find(a) == nullptr);
    CHECK(huge.find(b) == nullptr);
}

namespace
{

/// An rvalue of @p value, made where a use after it cannot be seen: the call it is passed to decides
/// whether it is consumed, which the test then reads.
template <typename T>
[[nodiscard]] T&& passed(T& value) noexcept
{
    return std::move(value);
}

/// A host destructor that records each rep it destroys, then runs the test's hook, which may call back into
/// the store as a parent resource's destructor dropping its children would.
class CallbackDestructor final: public isoline::component::runtime::IHostResourceDestructor
{
  public:
    void destroy(uint32_t rep) noexcept override
    {
        destroyed.push_back(rep);
        if (onDestroy)
            onDestroy(rep);
    }

    std::vector<uint32_t> destroyed {};         ///< Each representation destroyed, in order.
    std::function<void(uint32_t)> onDestroy {}; ///< Runs after each destroy; none when empty.
};

/// A component of the host resource types pane and tab, each of whose host functions is relayed as
/// `<name>-relay`, plus peek-relay (it drops the borrow it lends on) and hoard(), which takes two owns from
/// give() and then hands both to forget().
class PaneWorld
{
  public:
    explicit PaneWorld(ComponentLimits const& limits = {}): _limits { limits }
    {
        require(linker.defineResource("pane", pane));
        require(linker.defineResource("tab", tab));
        auto const functions = std::vector {
            defineTyped<uint32_t(Own<Pane>)>(
                linker,
                "take",
                [this](HostComponentContext& context, Own<Pane> own) noexcept {
                    auto const rep = own.rep();
                    drops.push_back(isoline::component::api::typed::drop(context.store(), own).has_value());
                    drops.push_back(isoline::component::api::typed::drop(context.store(), own).has_value());
                    return rep;
                },
                resources),
            defineTyped<uint32_t(Own<Pane>)>(
                linker, "forget", [](Own<Pane> const& own) noexcept { return own.rep(); }, resources),
            defineTyped<Own<Pane>(uint32_t)>(
                linker,
                "give",
                [this](HostComponentContext& context,
                       uint32_t rep) noexcept -> std::expected<Own<Pane>, isoline::api::HostFailure> {
                    auto made = hostOwn<Pane>(context.store(), resources, rep);
                    if (!made)
                        return std::unexpected { embedderFailure() };
                    return std::move(*made);
                },
                resources),
            defineTyped<Own<Pane>(uint32_t)>(
                linker, "empty", [](uint32_t) noexcept { return Own<Pane> {}; }, resources),
            defineTyped<uint32_t(Own<Pane>)>(
                linker,
                "fail",
                [](Own<Pane> const&) noexcept -> std::expected<uint32_t, isoline::api::HostFailure> {
                    return std::unexpected { embedderFailure(9) };
                },
                resources),
            defineTyped<uint32_t(Own<Tab>)>(
                linker, "take-tab", [](Own<Tab> const& own) noexcept { return own.rep(); }, resources),
        };
        auto builder = ComponentBuilder {};
        auto guest = GuestTypes { builder, linker.types() };
        auto const paneIndex =
            builder.import("pane", ExternDesc { .sort = Sort::Type, .bound = TypeBound::SubResource });
        guest.bindResource(valueOf(linker.ownType(pane)), paneIndex);
        guest.bindResource(
            valueOf(linker.ownType(tab)),
            builder.import("tab", ExternDesc { .sort = Sort::Type, .bound = TypeBound::SubResource }));
        auto const imports = relays(builder, guest, functions);
        auto const peekType = guest.function(
            defineTyped<uint32_t(Borrow<Pane>)>(
                linker, "peek", [](Borrow<Pane> lent) noexcept { return lent.rep(); }, resources)
                .second);
        auto const peekImport = builder.import("peek", ExternDesc { .sort = Sort::Func, .index = peekType });
        auto const lowered = builder.coreInlineExports({
            CoreInlineExport {
                .name = "peek", .sort = CoreSort::Func, .index = builder.canonLower(peekImport, {}) },
            CoreInlineExport {
                .name = "give", .sort = CoreSort::Func, .index = builder.canonLower(imports.at("give"), {}) },
            CoreInlineExport { .name = "forget",
                               .sort = CoreSort::Func,
                               .index = builder.canonLower(imports.at("forget"), {}) },
            CoreInlineExport { .name = "drop",
                               .sort = CoreSort::Func,
                               .index = builder.canon(CanonDefinition { .builtin = CanonBuiltin::ResourceDrop,
                                                                        .immediates = { paneIndex },
                                                                        .options = {},
                                                                        .result = std::nullopt }) },
        });
        auto const core = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
          (import "h" "peek" (func $peek (param i32) (result i32)))
          (import "h" "give" (func $give (param i32) (result i32)))
          (import "h" "forget" (func $forget (param i32) (result i32)))
          (import "h" "drop" (func $drop (param i32)))
          (func (export "peek") (param i32) (result i32) (local $rep i32)
            (local.set $rep (call $peek (local.get 0)))
            (call $drop (local.get 0))
            (local.get $rep))
          (func (export "hoard") (local $first i32) (local $second i32)
            (local.set $first (call $give (i32.const 1)))
            (local.set $second (call $give (i32.const 2)))
            (drop (call $forget (local.get $first)))
            (drop (call $forget (local.get $second))))))")),
                                                  { { "h", lowered } });
        std::ignore = builder.exportItem(
            "peek-relay",
            Sort::Func,
            builder.canonLift(builder.aliasCoreExport(core, "peek", CoreSort::Func), {}, peekType));
        std::ignore =
            builder.exportItem("hoard",
                               Sort::Func,
                               builder.canonLift(builder.aliasCoreExport(core, "hoard", CoreSort::Func),
                                                 {},
                                                 guest.function(valueOf(functionType<void()>(linker)))));
        component.emplace(harness.load(builder.build()));
    }

    PaneWorld(PaneWorld const&) = delete;
    PaneWorld& operator=(PaneWorld const&) = delete;
    PaneWorld(PaneWorld&&) = delete;
    PaneWorld& operator=(PaneWorld&&) = delete;
    ~PaneWorld() = default;

    /// A store of the world's limits.
    [[nodiscard]] ComponentStore store() { return harness.store(_limits); }

    ApiHarness harness {};
    Linker linker { ComponentConfig {} };
    std::shared_ptr<CallbackDestructor> destructor = std::make_shared<CallbackDestructor>();
    HostResourceType pane { "pane", destructor };
    HostResourceType tab { "tab", destructor };
    ResourceTypes<Pane, Tab> resources { pane, tab };
    std::vector<bool> drops {}; ///< take()'s two drops of its own, each whether it succeeded.
    std::optional<isoline::component::api::Component> component {};

  private:
    ComponentLimits _limits;
};

/// A component of the guest resource type r, whose destructor traps for rep 13: make() -> own<r> gives
/// rep 9, make-bad() rep 13.
[[nodiscard]] std::vector<std::byte> guestThings()
{
    auto builder = ComponentBuilder {};
    auto const first = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (func (export "dtor") (param $rep i32)
        (if (i32.eq (local.get $rep) (i32.const 13)) (then unreachable)))))")),
                                               {});
    auto const r = builder.resource(builder.aliasCoreExport(first, "dtor", CoreSort::Func));
    auto const exported = builder.exportItem("r", Sort::Type, r);
    auto const own =
        builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = exported } } });
    auto const builtins = builder.coreInlineExports(
        { CoreInlineExport { .name = "new",
                             .sort = CoreSort::Func,
                             .index = builder.canon(CanonDefinition { .builtin = CanonBuiltin::ResourceNew,
                                                                      .immediates = { r },
                                                                      .options = {},
                                                                      .result = std::nullopt }) } });
    auto const second = builder.coreInstantiate(builder.coreModule(coreModule(R"((module
      (import "r" "new" (func $new (param i32) (result i32)))
      (func (export "make") (result i32) (call $new (i32.const 9)))
      (func (export "make-bad") (result i32) (call $new (i32.const 13)))))")),
                                                { { "r", builtins } });
    for (auto const* name: { "make", "make-bad" })
        std::ignore =
            builder.exportItem(name,
                               Sort::Func,
                               builder.canonLift(builder.aliasCoreExport(second, name, CoreSort::Func),
                                                 {},
                                                 isoline::component::runtime::testing::functionType(
                                                     builder, {}, TypeIndex { .index = own })));
    return builder.build();
}

} // namespace

TEST_CASE("Typed.ownsAreConsumedOnce")
{
    auto world = PaneWorld {};
    auto& resources = world.resources;
    auto& destructor = *world.destructor;
    auto store = world.store();
    auto instance = instanceOf(store.instantiate(*world.component, world.linker));
    auto take = bound<uint32_t(Own<Pane>)>(instance, "take-relay");
    auto peek = bound<uint32_t(Borrow<Pane>)>(instance, "peek-relay");

    // Borrows are lent, of an own or made apart; an own made by the host comes back through the guest.
    auto seven = valueOf(hostOwn<Pane>(store, resources, 7));
    CHECK(valueOf(peek(seven.borrow())) == 7);
    CHECK(valueOf(peek(valueOf(hostBorrow<Pane>(store, resources, 8)))) == 8);
    auto given = valueOf(bound<Own<Pane>(uint32_t)>(instance, "give-relay")(9U));
    CHECK(given.rep() == 9);

    // Passing an own consumes it; the host function's second drop of it is refused, so it is destroyed once.
    CHECK(valueOf(take(passed(seven))) == 7);
    CHECK(seven.isEmpty());
    CHECK(world.drops == std::vector<bool> { true, false });
    CHECK(destructor.destroyed == std::vector<uint32_t> { 7 });
    // An empty own is refused before anything runs, and an lvalue own does not compile.
    CHECK(failureOf(take(passed(seven))) == ComponentCallFailure::ArgumentMismatch);
    STATIC_REQUIRE_FALSE(std::is_invocable_v<TypedFunction<uint32_t(Own<Pane>)>&, Own<Pane>&>);
    STATIC_REQUIRE_FALSE(std::is_invocable_v<TypedFunction<uint32_t(Own<Pane>)>&, Own<Pane> const&&>);
    STATIC_REQUIRE(std::is_invocable_v<TypedFunction<uint32_t(Own<Pane>)>&, Own<Pane>&&>);
    // A host body returning an empty own fails ResultMismatch, and locks the store down.
    {
        auto other = world.store();
        auto otherInstance = instanceOf(other.instantiate(*world.component, world.linker));
        auto const emptied = bound<Own<Pane>(uint32_t)>(otherInstance, "empty-relay")(1U);
        REQUIRE(failureOf(emptied) == ComponentCallFailure::HostFailure);
        CHECK(std::get<isoline::api::HostFailure>(emptied.error().detail).kind
              == isoline::api::HostFailureKind::ResultMismatch);
    }

    auto moved = Own<Pane> {};
    CHECK(moved.isEmpty());
    moved = moveOut(given);
    CHECK(given.isEmpty());
    CHECK(moved.rep() == 9);
    REQUIRE(isoline::component::api::typed::drop(store, moved).has_value());
    CHECK(moved.isEmpty());
    CHECK(failureOf(isoline::component::api::typed::drop(store, moved))
          == ComponentCallFailure::ArgumentMismatch);
    CHECK(destructor.destroyed == std::vector<uint32_t> { 7, 9 });

    // After a failed call, the owns the store still holds stay: twelve came back to the host, whose body
    // failed without dropping it; a refused call leaves its owns as they were.
    auto eleven = valueOf(hostOwn<Pane>(store, resources, 11));
    auto twelve = valueOf(hostOwn<Pane>(store, resources, 12));
    CHECK(failureOf(bound<uint32_t(Own<Pane>)>(instance, "fail-relay")(passed(twelve)))
          == ComponentCallFailure::HostFailure);
    CHECK_FALSE(twelve.isEmpty());
    CHECK(failureOf(take(passed(eleven))) == ComponentCallFailure::LockedDown);
    REQUIRE_FALSE(eleven.isEmpty());
    REQUIRE(isoline::component::api::typed::drop(store, eleven).has_value());
    REQUIRE(isoline::component::api::typed::drop(store, twelve).has_value());
    CHECK(destructor.destroyed == std::vector<uint32_t> { 7, 9, 11, 12 });

    // A refused drop leaves the own as it was too.
    auto thirteen = valueOf(hostOwn<Pane>(store, resources, 13));
    auto kept = moveOut(store);
    CHECK(failureOf(isoline::component::api::typed::drop(store, thirteen))
          == ComponentCallFailure::StoreDeleted);
    CHECK_FALSE(thirteen.isEmpty());
    REQUIRE(isoline::component::api::typed::drop(kept, thirteen).has_value());
    CHECK(destructor.destroyed == std::vector<uint32_t> { 7, 9, 11, 12, 13 });
}

TEST_CASE("Typed.bindChecksResourceIdentity")
{
    auto world = PaneWorld {};
    auto store = world.store();
    auto instance = instanceOf(store.instantiate(*world.component, world.linker));
    auto const& resources = world.resources;
    // The resource bound to a C++ type must be the one the function names: a parameter's and a result's.
    CHECK(TypedFunction<uint32_t(Own<Pane>)>::bind(instance, "take-relay", resources).has_value());
    CHECK_FALSE(TypedFunction<uint32_t(Own<Tab>)>::bind(instance, "take-relay", resources).has_value());
    CHECK(TypedFunction<uint32_t(Own<Tab>)>::bind(instance, "take-tab-relay", resources).has_value());
    CHECK_FALSE(TypedFunction<uint32_t(Own<Pane>)>::bind(instance, "take-tab-relay", resources).has_value());
    CHECK(TypedFunction<Own<Pane>(uint32_t)>::bind(instance, "give-relay", resources).has_value());
    CHECK_FALSE(TypedFunction<Own<Tab>(uint32_t)>::bind(instance, "give-relay", resources).has_value());
    CHECK_FALSE(TypedFunction<uint32_t(Borrow<Tab>)>::bind(instance, "peek-relay", resources).has_value());
    // A type bound to no host resource type is a guest's: it matches by kind.
    CHECK(TypedFunction<uint32_t(Own<GuestThing>)>::bind(instance, "take-relay", resources).has_value());
    // A host resource type the store has not imported names no handle.
    auto const stranger = ResourceTypes<Pane> { HostResourceType { "pane", nullptr } };
    CHECK_FALSE(TypedFunction<uint32_t(Own<Pane>)>::bind(instance, "take-relay", stranger).has_value());
}

TEST_CASE("Typed.hostOwnsAreBoundedAndTornDown")
{
    SECTION("the store's end destroys the owns the host still holds, once each")
    {
        auto world = PaneWorld {};
        {
            auto store = world.store();
            auto instance = instanceOf(store.instantiate(*world.component, world.linker));
            auto seven = valueOf(hostOwn<Pane>(store, world.resources, 7));
            std::ignore = valueOf(hostOwn<Pane>(store, world.resources, 8));
            CHECK(valueOf(bound<uint32_t(Own<Pane>)>(instance, "forget-relay")(passed(seven))) == 7);
            CHECK_FALSE(seven.isEmpty()); // forget() left it with the host, which still holds it
            REQUIRE(bound<void()>(instance, "hoard")().has_value());
            CHECK(world.destructor->destroyed.empty());
        }
        CHECK(world.destructor->destroyed == std::vector<uint32_t> { 1, 2, 7, 8 });
    }
    SECTION("each entry of the table is charged to the live-bytes budget")
    {
        auto world = PaneWorld {};
        auto store = world.store();
        std::ignore = instanceOf(store.instantiate(*world.component, world.linker));
        auto const& budget = *store.runtime().liveBytes();
        auto const before = budget.live();
        auto first = valueOf(hostOwn<Pane>(store, world.resources, 7));
        CHECK(budget.live() == before + isoline::component::api::HostOwnEntryBytes);
        auto second = valueOf(hostOwn<Pane>(store, world.resources, 7));
        CHECK(budget.live() == before + isoline::component::api::HostOwnEntryBytes);
        CHECK(store.hostHolds(isoline::component::canonical::ResourceValue {
                  .type = valueOf(store.hostResource(world.pane, 7, Ownership::Borrow)).type,
                  .rep = 7,
                  .ownership = Ownership::Own })
              == 2);
        REQUIRE(isoline::component::api::typed::drop(store, first).has_value());
        CHECK(budget.live() == before + isoline::component::api::HostOwnEntryBytes);
        REQUIRE(isoline::component::api::typed::drop(store, second).has_value());
        CHECK(budget.live() == before);
    }
    SECTION("an own past maxHostOwns is refused, wherever it would be held")
    {
        auto world = PaneWorld { ComponentLimits { .maxHostOwns = 1 } };
        auto store = world.store();
        auto instance = instanceOf(store.instantiate(*world.component, world.linker));
        auto one = valueOf(hostOwn<Pane>(store, world.resources, 7));
        auto const refused = hostOwn<Pane>(store, world.resources, 8);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message() == "host-held owns exceed the limit of 1");
        REQUIRE(isoline::component::api::typed::drop(store, one).has_value());
        // hoard()'s second forget() hands the host a second own while it holds the first: the call fails
        // ResourceExhausted and locks the store down; the store's end destroys both.
        auto const hoarded = bound<void()>(instance, "hoard")();
        REQUIRE(failureOf(hoarded) == ComponentCallFailure::ResourceExhausted);
        CHECK(hoarded.error().message()
              == "component store resources exhausted: host-held owns exceed the limit of 1");
        CHECK(store.isLockedDown());
        store = world.store();
        CHECK(world.destructor->destroyed == std::vector<uint32_t> { 7, 1, 2 });
    }
    SECTION("owns a call returns past maxHostOwns fail it")
    {
        auto harness = ApiHarness {};
        auto const component = harness.load(guestThings());
        auto store = harness.store(ComponentLimits { .maxHostOwns = 0 });
        auto instance = instanceOf(store.instantiate(component, Linker { ComponentConfig {} }));
        auto const made = bound<Own<GuestThing>()>(instance, "make")();
        REQUIRE(failureOf(made) == ComponentCallFailure::ResourceExhausted);
        CHECK(store.isLockedDown());
    }
}

TEST_CASE("Typed.hostOwnEntriesWithinTheBudget")
{
    // The live-bytes budget holds one entry of the host's owns table: a second own of the held rep needs
    // no entry, an own of another rep is refused, and a dropped entry frees the budget for it.
    auto world =
        PaneWorld { ComponentLimits { .maxLiveValueBytes = isoline::component::api::HostOwnEntryBytes } };
    auto store = world.store();
    std::ignore = instanceOf(store.instantiate(*world.component, world.linker));
    auto first = valueOf(hostOwn<Pane>(store, world.resources, 7));
    auto second = valueOf(hostOwn<Pane>(store, world.resources, 7));
    auto const refused = hostOwn<Pane>(store, world.resources, 8);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == isoline::component::canonical::TrapCode::LiveValueBytes);
    REQUIRE(isoline::component::api::typed::drop(store, first).has_value());
    REQUIRE(isoline::component::api::typed::drop(store, second).has_value());
    CHECK(hostOwn<Pane>(store, world.resources, 8).has_value());
}

TEST_CASE("Typed.handlesOfAStoreThatIsGone")
{
    auto world = PaneWorld {};
    auto store = std::optional { world.store() };
    auto instance = instanceOf(store->instantiate(*world.component, world.linker));
    auto otherStore = world.store();
    auto other = instanceOf(otherStore.instantiate(*world.component, world.linker));
    // Each export the test names, which it must find.
    auto const exported = [](auto const& of, std::string_view name) {
        auto found = of.function(name);
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        return std::move(**found);
    };
    auto const take = std::optional { exported(instance, "take-relay") };
    auto const forget = std::optional { exported(instance, "forget-relay") };
    auto const foreign = std::optional { exported(other, "take-relay") };
    CHECK(*take == exported(instance, "take-relay"));
    CHECK_FALSE(*take == *forget);
    CHECK_FALSE(*take == *foreign);
    REQUIRE(take->type() != nullptr);
    auto const handle = take->type()->params.front().second;
    auto const number = *take->type()->result;
    CHECK(instance.resourceTypeOf(*take, handle).has_value());
    // A type that names no resource, and a function of another store, name no runtime type.
    CHECK_FALSE(instance.resourceTypeOf(*take, number).has_value());
    CHECK_FALSE(instance.resourceTypeOf(*foreign, handle).has_value());
    CHECK(instance.hostResourceTypeOf(world.pane).has_value());

    // A moved-from store is gone for what it names: no interrupter, no instantiation.
    auto moved = std::move(*store);
    CHECK(store->interrupter() == nullptr);
    auto const refused = store->instantiate(*world.component, world.linker);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(std::holds_alternative<isoline::component::runtime::InstantiationFailure>(refused.error()));
    CHECK(std::get<isoline::component::runtime::InstantiationFailure>(refused.error()).kind
          == isoline::component::runtime::InstantiationFailureKind::Reentrant);

    // Once the store is gone, its handles name nothing.
    store.reset();
    {
        [[maybe_unused]] auto const ended = std::move(moved);
    }
    CHECK(take->types() == nullptr);
    CHECK_FALSE(instance.resourceTypeOf(*take, handle).has_value());
    CHECK_FALSE(instance.hostResourceTypeOf(world.pane).has_value());
}

TEST_CASE("Typed.teardownRefusesReentrantCalls")
{
    // pane 1's destructor drops pane 2, calls a function of the store and makes an own while the store
    // tears down: each is refused, and each destructor runs once.
    auto world = PaneWorld {};
    auto refusals = std::vector<std::string> {};
    auto const tearDown = [&](auto&& end) {
        auto store = std::optional { world.store() };
        auto instance = instanceOf(store->instantiate(*world.component, world.linker));
        auto take = bound<uint32_t(Own<Pane>)>(instance, "take-relay");
        auto first = valueOf(hostOwn<Pane>(*store, world.resources, 1));
        auto second = valueOf(hostOwn<Pane>(*store, world.resources, 2));
        auto* const reached = &*store;
        world.destructor->onDestroy = [&, reached](uint32_t rep) {
            if (rep != 1)
                return;
            auto const dropped = isoline::component::api::typed::drop(*reached, second);
            refusals.push_back(dropped ? "dropped" : dropped.error().message());
            refusals.emplace_back(second.isEmpty() ? "consumed" : "kept");
            auto const called = take(passed(first));
            refusals.push_back(called ? "called" : called.error().message());
            auto const made = hostOwn<Pane>(*reached, world.resources, 3);
            refusals.push_back(made ? "made" : made.error().message());
            refusals.emplace_back(reached->isLockedDown() ? "locked" : "live");
        };
        end(store);
        world.destructor->onDestroy = {};
    };
    auto const expected = std::vector<std::string> {
        "component store is gone", "kept", "component store is gone", "component store is gone", "locked",
    };

    SECTION("by its destruction")
    {
        tearDown([](auto& store) { store.reset(); });
        CHECK(refusals == expected);
        CHECK(world.destructor->destroyed == std::vector<uint32_t> { 1, 2 });
    }
    SECTION("by a move-assignment, which never reaches the store taking its place")
    {
        auto other = world.store();
        std::ignore = instanceOf(other.instantiate(*world.component, world.linker));
        auto kept = valueOf(hostOwn<Pane>(other, world.resources, 2));
        tearDown([&](auto& store) {
            *store = std::move(other);
            // The refused drop did not take the new store's own of the same rep: it is still held there.
            CHECK(isoline::component::api::typed::drop(*store, kept).has_value());
        });
        CHECK(refusals == expected);
        CHECK(world.destructor->destroyed == std::vector<uint32_t> { 1, 2, 2 });
        CHECK(kept.isEmpty());
    }
}

TEST_CASE("Typed.dropThatTrapsConsumes")
{
    // A guest destructor that traps took the own before it ran: the own is consumed, and the store locks
    // down.
    auto harness = ApiHarness {};
    auto const component = harness.load(guestThings());
    auto store = harness.store();
    auto instance = instanceOf(store.instantiate(component, Linker { ComponentConfig {} }));
    auto good = valueOf(bound<Own<GuestThing>()>(instance, "make")());
    auto bad = valueOf(bound<Own<GuestThing>()>(instance, "make-bad")());
    REQUIRE(isoline::component::api::typed::drop(store, good).has_value());
    CHECK(good.isEmpty());
    CHECK(failureOf(isoline::component::api::typed::drop(store, bad)) == ComponentCallFailure::Trap);
    CHECK(bad.isEmpty());
    CHECK(store.isLockedDown());
}

TEST_CASE("Typed.matchesComparesLabels")
{
    auto linker = Linker { ComponentConfig {} };
    auto const structural = StructuralMatch { linker.types() };
    CHECK(TypeOf<CommandOutput>::matches(structural, typeIn<CommandOutput>(linker)));
    CHECK_FALSE(TypeOf<CommandOutput>::matches(structural, typeIn<RenamedOutput>(linker)));
    CHECK_FALSE(TypeOf<CommandOutput>::matches(structural, typeIn<WideOutput>(linker)));
    CHECK_FALSE(TypeOf<CommandOutput>::matches(structural, typeIn<std::tuple<int32_t, std::string>>(linker)));
    CHECK(TypeOf<CommandError>::matches(structural, typeIn<CommandError>(linker)));
    CHECK_FALSE(TypeOf<CommandError>::matches(structural, typeIn<OtherError>(linker)));
    CHECK_FALSE(TypeOf<CommandError>::matches(structural, typeIn<Style>(linker)));
    CHECK(TypeOf<Style>::matches(structural, typeIn<Style>(linker)));
    CHECK_FALSE(TypeOf<Style>::matches(structural, typeIn<OtherStyle>(linker)));
    CHECK_FALSE(TypeOf<Style>::matches(structural, typeIn<CommandError>(linker)));
    CHECK(TypeOf<Event>::matches(structural, typeIn<Event>(linker)));
    CHECK_FALSE(TypeOf<Event>::matches(structural, typeIn<OtherEvent>(linker)));
    CHECK_FALSE(TypeOf<Event>::matches(structural, typeIn<NarrowEvent>(linker)));
    // Kinds, arities and absent payloads.
    CHECK_FALSE(
        TypeOf<std::tuple<uint32_t>>::matches(structural, typeIn<std::tuple<uint32_t, uint32_t>>(linker)));
    CHECK_FALSE(TypeOf<std::optional<uint32_t>>::matches(structural, typeIn<std::vector<uint32_t>>(linker)));
    CHECK_FALSE(TypeOf<std::expected<Unit, uint32_t>>::matches(
        structural, typeIn<std::expected<uint32_t, uint32_t>>(linker)));
    CHECK_FALSE(TypeOf<std::expected<uint32_t, uint32_t>>::matches(
        structural, typeIn<std::expected<Unit, uint32_t>>(linker)));
    CHECK(TypeOf<std::map<std::string, Event>>::matches(structural,
                                                        typeIn<std::map<std::string, Event>>(linker)));
    CHECK_FALSE(TypeOf<std::string>::matches(structural, typeIn<uint32_t>(linker)));
    auto const pane = HostResourceType { "pane", nullptr };
    CHECK(TypeOf<Own<Pane>>::matches(structural, valueOf(linker.ownType(pane))));
    CHECK_FALSE(TypeOf<Own<Pane>>::matches(structural, valueOf(linker.borrowType(pane))));
    CHECK(TypeOf<Borrow<Pane>>::matches(structural, valueOf(linker.borrowType(pane))));
}

TEST_CASE("Typed.readsRefuseOtherTypes")
{
    // Every row reads only a value of its own type, the children included.
    auto linker = Linker { ComponentConfig {} };
    auto const number = valuesOf(linker, uint32_t { 7 });
    auto const view = number.root(0);
    CHECK_FALSE(TypeOf<bool>::read(view));
    CHECK_FALSE(TypeOf<std::string>::read(view));
    CHECK_FALSE(TypeOf<std::vector<uint32_t>>::read(view));
    CHECK_FALSE(TypeOf<std::optional<uint32_t>>::read(view));
    // Clang 19 cannot compare an optional tuple inside Catch2's decomposition.
    CHECK_FALSE(TypeOf<std::tuple<uint32_t>>::read(view).has_value());
    CHECK_FALSE((TypeOf<std::expected<uint32_t, uint32_t>>::read(view)));
    CHECK_FALSE((TypeOf<std::map<uint32_t, uint32_t>>::read(view)));
    CHECK_FALSE(TypeOf<CommandOutput>::read(view));
    CHECK_FALSE(TypeOf<CommandError>::read(view));
    CHECK_FALSE(TypeOf<Style>::read(view));
    CHECK_FALSE(TypeOf<Event>::read(view));
    CHECK_FALSE(TypeOf<Own<Pane>>::read(view));
    CHECK_FALSE(TypeOf<Borrow<Pane>>::read(view));

    CHECK_FALSE(
        TypeOf<std::vector<std::string>>::read(valuesOf(linker, std::vector<uint32_t> { 1 }).root(0)));
    CHECK_FALSE(
        TypeOf<std::optional<std::string>>::read(valuesOf(linker, std::optional<uint32_t> { 1 }).root(0)));
    auto const pair = valuesOf(linker, std::tuple<uint32_t, uint32_t> { 1, 2 });
    // Clang 19 cannot compare an optional tuple inside Catch2's decomposition.
    CHECK_FALSE(TypeOf<std::tuple<std::string, uint32_t>>::read(pair.root(0)).has_value());
    CHECK_FALSE(TypeOf<std::tuple<uint32_t>>::read(pair.root(0)).has_value());
    CHECK_FALSE((TypeOf<std::expected<Unit, std::string>>::read(
        valuesOf(linker, std::expected<uint32_t, std::string> { 1U }).root(0))));
    CHECK_FALSE((TypeOf<std::expected<uint32_t, uint32_t>>::read(
        valuesOf(linker, std::expected<uint32_t, std::string> { std::unexpect, "x" }).root(0))));
    CHECK((TypeOf<std::expected<uint32_t, Unit>>::read(
               valuesOf(linker, std::expected<uint32_t, Unit> { std::unexpect }).root(0))
           == std::expected<uint32_t, Unit> { std::unexpect }));
    CHECK_FALSE((TypeOf<std::map<std::string, std::string>>::read(
        valuesOf(linker, std::map<std::string, uint32_t> { { "a", 1 } }).root(0))));
    auto const output = valuesOf(linker, CommandOutput { .exitCode = 1, .output = "x" });
    CHECK_FALSE(TypeOf<WideOutput>::read(output.root(0)));
    CHECK_FALSE(TypeOf<CommandOutput>::read(valuesOf(linker, Segment {}).root(0)));
    CHECK_FALSE(TypeOf<NarrowEvent>::read(
        valuesOf(linker, Event { Case<"progress", std::tuple<uint32_t, uint32_t>> { { 1, 2 } } }).root(0)));
    CHECK(TypeOf<NarrowEvent>::read(valuesOf(linker, Event { Case<"clear", Unit> {} }).root(0))
          == NarrowEvent { Case<"clear", Unit> {} });

    // A string whose bytes are not text reads as none.
    auto const strings = builderOf(linker.types(), { linker.types().primitive(ValueKind::String) });
    auto bytes = strings->encodedString(StringEncoding::Utf8, 1);
    REQUIRE(bytes.size() == 1);
    bytes[0] = std::byte { 0xFF };
    auto const invalid = valueOf(strings->finish());
    CHECK_FALSE(TypeOf<std::string>::read(invalid.root(0)));
}

TEST_CASE("Typed.hostFunctionChecksArguments")
{
    // A typed host function refuses arguments of another count or type, before its body runs.
    auto harness = ApiHarness {};
    auto linker = Linker { ComponentConfig {} };
    auto store = harness.store();
    auto context = HostComponentContext { store };
    auto runs = 0;
    auto const function = makeTypedHostFunction<uint32_t(std::string)>([&runs](std::string const&) noexcept {
        ++runs;
        return uint32_t { 1 };
    });
    auto const u32 = linker.types().primitive(ValueKind::U32);
    auto const results = builderOf(linker.types(), { u32 });
    auto const isArgumentMismatch = [](std::expected<void, isoline::api::HostFailure> const& outcome) {
        return !outcome.has_value()
               && outcome.error().kind == isoline::api::HostFailureKind::ArgumentMismatch;
    };
    auto const none = valueOf(builderOf(linker.types(), {})->finish());
    CHECK(isArgumentMismatch(function->call(context, none, *results)));
    CHECK(isArgumentMismatch(function->call(context, valuesOf(linker, uint32_t { 3 }), *results)));
    CHECK(runs == 0);
    REQUIRE(function->call(context, valuesOf(linker, std::string { "abc" }), *results).has_value());
    CHECK(runs == 1);
    CHECK(valueOf(results->finish()).root(0).scalar<uint32_t>() == 1);
}

TEST_CASE("Typed.labelsAreConstantText")
{
    // Labels and fields are made at compile time; made at run time, they are the same.
    auto const label = isoline::component::api::typed::FixedString { "clear" };
    CHECK(label.view() == "clear");
    auto const row = isoline::component::api::typed::field("exit-code", &CommandOutput::exitCode);
    CHECK(row.name == "exit-code");
    CHECK(row.member == &CommandOutput::exitCode);
}
