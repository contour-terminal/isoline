// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <isoline/component/Limits.hpp>
#include <isoline/component/canonical/Layout.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/types/TypeArena.hpp>

using isoline::component::ComponentLimit;
using isoline::component::ComponentLimits;
using isoline::component::limitMessage;
using isoline::component::canonical::BudgetExceeded;
using isoline::component::canonical::ChargedVector;
using isoline::component::canonical::equal;
using isoline::component::canonical::LiveBytesBudget;
using isoline::component::canonical::Ownership;
using isoline::component::canonical::ResourceTypeIndex;
using isoline::component::canonical::ResourceValue;
using isoline::component::canonical::SimpleEncoding;
using isoline::component::canonical::StringEncoding;
using isoline::component::canonical::StringError;
using isoline::component::canonical::StringValue;
using isoline::component::canonical::toText;
using isoline::component::canonical::Utf16Tag;
using isoline::component::canonical::ValueBuilder;
using isoline::component::canonical::ValueError;
using isoline::component::canonical::ValueErrorCode;
using isoline::component::canonical::Values;
using isoline::component::canonical::ValueView;
using isoline::component::types::DefinedType;
using isoline::component::types::ResourceTypeInfo;
using isoline::component::types::SharedName;
using isoline::component::types::TypeArena;
using isoline::component::types::TypeId;
using isoline::component::types::ValueKind;

namespace
{

/// Ample for every test that is not about the budget.
constexpr auto AmpleBudget = uint64_t { 1 } << 30U;

/// A fresh budget of @p limit bytes.
[[nodiscard]] std::shared_ptr<LiveBytesBudget> budgetOf(uint64_t limit = AmpleBudget)
{
    return std::make_shared<LiveBytesBudget>(limit);
}

/// The labels @p names.
[[nodiscard]] std::vector<SharedName> labelsOf(std::initializer_list<char const*> names)
{
    auto labels = std::vector<SharedName> {};
    for (auto const* name: names)
        labels.emplace_back(name);
    return labels;
}

/// Builds types in one arena.
class Types
{
  public:
    explicit Types(ComponentLimits const& limits = ComponentLimits {}): _arena { limits } {}

    [[nodiscard]] TypeId primitive(ValueKind kind) const { return _arena.primitive(kind); }

    [[nodiscard]] TypeId intern(DefinedType type)
    {
        auto const id = _arena.intern(std::move(type));
        REQUIRE(id.has_value());
        return *id;
    }

    [[nodiscard]] TypeId record(std::initializer_list<char const*> labels, std::vector<TypeId> const& fields)
    {
        auto type = DefinedType { .kind = ValueKind::Record, .labels = labelsOf(labels) };
        for (auto const field: fields)
            type.children.emplace_back(field);
        return intern(std::move(type));
    }

    [[nodiscard]] TypeId tuple(std::vector<TypeId> const& elements)
    {
        auto type = DefinedType { .kind = ValueKind::Tuple };
        for (auto const element: elements)
            type.children.emplace_back(element);
        return intern(std::move(type));
    }

    [[nodiscard]] TypeId wrap(ValueKind kind, std::vector<std::optional<TypeId>> children)
    {
        return intern(DefinedType { .kind = kind, .children = std::move(children) });
    }

    [[nodiscard]] TypeId list(TypeId element) { return wrap(ValueKind::List, { element }); }

    [[nodiscard]] TypeId option(TypeId element) { return wrap(ValueKind::Option, { element }); }

    [[nodiscard]] TypeId variant(std::initializer_list<char const*> labels,
                                 std::vector<std::optional<TypeId>> payloads)
    {
        return intern(DefinedType {
            .kind = ValueKind::Variant, .children = std::move(payloads), .labels = labelsOf(labels) });
    }

    [[nodiscard]] TypeId labelled(ValueKind kind, std::initializer_list<char const*> labels)
    {
        return intern(DefinedType { .kind = kind, .labels = labelsOf(labels) });
    }

    [[nodiscard]] TypeId fixedList(TypeId element, uint32_t length)
    {
        return intern(
            DefinedType { .kind = ValueKind::FixedList, .children = { element }, .length = length });
    }

    [[nodiscard]] TypeId handle(ValueKind kind)
    {
        if (!_resource)
        {
            auto const resource = _arena.newResource(ResourceTypeInfo {});
            REQUIRE(resource.has_value());
            _resource = *resource;
        }
        return intern(DefinedType { .kind = kind, .resource = _resource });
    }

    [[nodiscard]] TypeArena const& arena() const noexcept { return _arena; }

  private:
    TypeArena _arena;
    std::optional<isoline::component::types::ResourceTypeId> _resource {};
};

/// The values @p steps build for @p roots; they must build.
[[nodiscard]] Values build(Types const& types,
                           std::vector<TypeId> const& roots,
                           std::function<void(ValueBuilder&)> const& steps,
                           std::shared_ptr<LiveBytesBudget> budget = budgetOf())
{
    auto builder = ValueBuilder { types.arena(), roots, std::move(budget) };
    steps(builder);
    auto values = builder.finish();
    if (!values)
        FAIL(values.error().message());
    REQUIRE(values.has_value());
    return std::move(*values);
}

/// The error @p steps build for @p roots fails with; they must fail.
[[nodiscard]] ValueError refusal(Types const& types,
                                 std::vector<TypeId> const& roots,
                                 std::function<void(ValueBuilder&)> const& steps,
                                 std::shared_ptr<LiveBytesBudget> budget = budgetOf())
{
    auto builder = ValueBuilder { types.arena(), roots, std::move(budget) };
    steps(builder);
    auto const values = builder.finish();
    REQUIRE_FALSE(values.has_value());
    return values.error();
}

/// The bytes of @p text.
[[nodiscard]] std::span<std::byte const> bytesOf(std::string_view text)
{
    return std::as_bytes(std::span { text.data(), text.size() });
}

/// UTF-16LE bytes of @p units.
[[nodiscard]] std::vector<std::byte> utf16Of(std::initializer_list<uint16_t> units)
{
    auto bytes = std::vector<std::byte> {};
    for (auto const unit: units)
    {
        bytes.push_back(static_cast<std::byte>(unit & 0xFFU));
        bytes.push_back(static_cast<std::byte>(unit >> 8U));
    }
    return bytes;
}

/// The nodes and packed elements a walk through children() visits from @p root, itself included.
[[nodiscard]] std::size_t visitAll(ValueView root)
{
    auto pending = std::vector<ValueView> { root };
    auto visits = std::size_t { 0 };
    while (!pending.empty())
    {
        auto const value = pending.back();
        pending.pop_back();
        ++visits;
        for (auto const child: value.children())
            pending.push_back(child);
    }
    return visits;
}

} // namespace

TEST_CASE("Values.buildAndView")
{
    auto types = Types {};
    auto const u32 = types.primitive(ValueKind::U32);
    auto const string = types.primitive(ValueKind::String);
    auto const strings = types.list(string);
    auto const record = types.record({ "a", "b" }, { u32, strings });
    auto const budget = budgetOf();

    auto const values = build(
        types,
        { record },
        [](ValueBuilder& builder) {
            builder.begin(2).scalar(7).begin(2).string("x").string("yz").end().end();
        },
        budget);
    CHECK(values.arena() == types.arena().id());
    REQUIRE(values.rootCount() == 1);
    CHECK(values.nodeCount() == 5);
    CHECK(values.hostBytes() == budget->live());
    CHECK(values.hostBytes() > 0);

    auto const root = values.root(0);
    CHECK(root.kind() == ValueKind::Record);
    CHECK(root.type() == record);
    CHECK(root.arena() == types.arena().id());
    REQUIRE(root.childCount() == 2);
    auto fields = std::vector<ValueView> {};
    for (auto const field: root.children())
        fields.push_back(field);
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].kind() == ValueKind::U32);
    CHECK(fields[0].type() == u32);
    CHECK(fields[0].scalar<uint32_t>() == 7);
    CHECK(fields[1].type() == strings);
    REQUIRE(fields[1].childCount() == 2);
    auto texts = std::vector<std::string> {};
    for (auto const element: fields[1].children())
    {
        CHECK(element.kind() == ValueKind::String);
        auto const text = element.string();
        CHECK(text.encoding == StringEncoding::Utf8);
        CHECK(text.taggedCodeUnits == text.bytes.size());
        auto utf8 = text.utf8();
        REQUIRE(utf8.has_value());
        texts.push_back(std::move(*utf8));
    }
    CHECK(texts == std::vector<std::string> { "x", "yz" });
    CHECK(root.child(1).child(1).string().utf8() == std::string { "yz" });
    CHECK(
        toText(types.arena(), root)
        == R"((record.const (field "a" (u32.const 7)) (field "b" (list.const (str.const "x") (str.const "yz")))))");
}

TEST_CASE("Values.builderRejectsMismatch")
{
    auto types = Types {};
    auto const u8 = types.primitive(ValueKind::U8);
    auto const s8 = types.primitive(ValueKind::S8);
    auto const u32 = types.primitive(ValueKind::U32);
    auto const boolean = types.primitive(ValueKind::Bool);
    auto const character = types.primitive(ValueKind::Char);
    auto const string = types.primitive(ValueKind::String);
    auto const record = types.record({ "a", "b" }, { u32, u32 });
    auto const bytes = types.list(u8);
    auto const nested = types.list(types.list(string));
    auto const fixed = types.fixedList(u32, 2);
    auto const variant = types.variant({ "none", "some" }, { std::nullopt, u32 });
    auto const option = types.option(u32);
    auto const result = types.wrap(ValueKind::Result, { u32, std::nullopt });
    auto const enumeration = types.labelled(ValueKind::Enum, { "a", "b" });
    auto const flags = types.labelled(ValueKind::Flags, { "a", "b", "c" });
    auto const map = types.wrap(ValueKind::Map, { string, u32 });
    auto const own = types.handle(ValueKind::Own);
    auto const borrow = types.handle(ValueKind::Borrow);
    auto const stream = types.wrap(ValueKind::Stream, { u32 });
    auto const errorContext = types.primitive(ValueKind::ErrorContext);

    // The brief's two: a scalar where a record is expected, and end() with nothing begun.
    auto const mismatch = refusal(types, { record }, [](ValueBuilder& builder) { builder.scalar(7); });
    CHECK(mismatch.code == ValueErrorCode::TypeMismatch);
    CHECK(mismatch.path == "[0]");
    CHECK(mismatch.message() == "type mismatch at [0]: expected record, given a scalar");
    auto const unbalanced = refusal(types, { u32 }, [](ValueBuilder& builder) { builder.end(); });
    CHECK(unbalanced.code == ValueErrorCode::Unbalanced);
    CHECK(unbalanced.path == "[0]");

    auto const own7 =
        ResourceValue { .type = ResourceTypeIndex { 1 }, .rep = 7, .ownership = Ownership::Own };
    auto const borrow7 =
        ResourceValue { .type = ResourceTypeIndex { 1 }, .rep = 7, .ownership = Ownership::Borrow };
    struct Row
    {
        std::string_view name;
        std::vector<TypeId> roots;
        std::function<void(ValueBuilder&)> steps;
        ValueErrorCode code;
        std::string_view path;
    };
    auto const rows = std::vector<Row> {
        { .name = "string for u32",
          .roots = { u32 },
          .steps = [](ValueBuilder& builder) { builder.string("x"); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[0]" },
        { .name = "begin for u32",
          .roots = { u32, u32 },
          .steps = [](ValueBuilder& builder) { builder.scalar(1).begin(1); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[1]" },
        { .name = "resource for string",
          .roots = { string },
          .steps = [&](ValueBuilder& builder) { builder.resource(own7); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[0]" },
        { .name = "scalar for string inside a record",
          .roots = { types.record({ "s" }, { string }) },
          .steps = [](ValueBuilder& builder) { builder.begin(1).scalar(1); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[0][0]" },
        { .name = "borrow for own",
          .roots = { own },
          .steps = [&](ValueBuilder& builder) { builder.resource(borrow7); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[0]" },
        { .name = "own for borrow",
          .roots = { borrow },
          .steps = [&](ValueBuilder& builder) { builder.resource(own7); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[0]" },
        { .name = "record of three fields",
          .roots = { record },
          .steps = [](ValueBuilder& builder) { builder.begin(3); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[0]" },
        { .name = "fixed list of three",
          .roots = { fixed },
          .steps = [](ValueBuilder& builder) { builder.begin(3); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[0]" },
        { .name = "bool 2",
          .roots = { boolean },
          .steps = [](ValueBuilder& builder) { builder.scalar(2); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "u8 0x100",
          .roots = { u8 },
          .steps = [](ValueBuilder& builder) { builder.scalar(0x100); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "s8 sign-extended",
          .roots = { s8 },
          .steps = [](ValueBuilder& builder) { builder.scalar(std::numeric_limits<uint64_t>::max()); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "surrogate char",
          .roots = { character },
          .steps = [](ValueBuilder& builder) { builder.scalar(0xD800); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "char past U+10FFFF",
          .roots = { character },
          .steps = [](ValueBuilder& builder) { builder.scalar(0x11'0000); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "packed element 0x100",
          .roots = { u32, bytes },
          .steps =
              [](ValueBuilder& builder) { builder.scalar(1).begin(3).scalar(1).scalar(2).scalar(0x100); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[1][2]" },
        { .name = "enum case 2 of 2",
          .roots = { enumeration },
          .steps = [](ValueBuilder& builder) { builder.scalar(2); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "flag bit past the labels",
          .roots = { flags },
          .steps = [](ValueBuilder& builder) { builder.scalar(0b1000); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "variant case 2 of 2",
          .roots = { variant },
          .steps = [](ValueBuilder& builder) { builder.begin(2); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "option case 2",
          .roots = { option },
          .steps = [](ValueBuilder& builder) { builder.begin(2); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "result case 2",
          .roots = { result },
          .steps = [](ValueBuilder& builder) { builder.begin(2); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "variant payload of the wrong type",
          .roots = { variant },
          .steps = [](ValueBuilder& builder) { builder.begin(1).string("x"); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[0][1]" },
        { .name = "payload for a case without one",
          .roots = { variant },
          .steps = [](ValueBuilder& builder) { builder.begin(0).scalar(1); },
          .code = ValueErrorCode::Unbalanced,
          .path = "[0][0]" },
        { .name = "result error without payload given one",
          .roots = { result },
          .steps = [](ValueBuilder& builder) { builder.begin(1).scalar(1); },
          .code = ValueErrorCode::Unbalanced,
          .path = "[0][1]" },
        { .name = "invalid UTF-8",
          .roots = { string },
          .steps = [](ValueBuilder& builder) { builder.string("\xC3\x28"); },
          .code = ValueErrorCode::InvalidValue,
          .path = "[0]" },
        { .name = "map value of the wrong type",
          .roots = { map },
          .steps = [](ValueBuilder& builder) { builder.begin(1).string("k").string("v"); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[0][1]" },
        { .name = "nested list element of the wrong type",
          .roots = { nested },
          .steps =
              [](ValueBuilder& builder) { builder.begin(2).begin(0).end().begin(3).string("a").scalar(1); },
          .code = ValueErrorCode::TypeMismatch,
          .path = "[0][1][1]" },
        { .name = "too few fields",
          .roots = { record },
          .steps = [](ValueBuilder& builder) { builder.begin(2).scalar(1).end(); },
          .code = ValueErrorCode::Unbalanced,
          .path = "[0][1]" },
        { .name = "too many elements",
          .roots = { bytes },
          .steps = [](ValueBuilder& builder) { builder.begin(1).scalar(1).scalar(2); },
          .code = ValueErrorCode::Unbalanced,
          .path = "[0][1]" },
        { .name = "a value past the last root",
          .roots = { u32 },
          .steps = [](ValueBuilder& builder) { builder.scalar(1).scalar(2); },
          .code = ValueErrorCode::Unbalanced,
          .path = "[1]" },
        { .name = "a root missing",
          .roots = { u32, u32 },
          .steps = [](ValueBuilder& builder) { builder.scalar(1); },
          .code = ValueErrorCode::Unbalanced,
          .path = "[1]" },
        { .name = "a compound left open",
          .roots = { record },
          .steps = [](ValueBuilder& builder) { builder.begin(2).scalar(1).scalar(2); },
          .code = ValueErrorCode::Unbalanced,
          .path = "[0][2]" },
        { .name = "a stream",
          .roots = { stream },
          .steps = [](ValueBuilder& builder) { builder.begin(0); },
          .code = ValueErrorCode::Unsupported,
          .path = "[0]" },
        { .name = "an error context",
          .roots = { errorContext },
          .steps = [](ValueBuilder& builder) { builder.scalar(0); },
          .code = ValueErrorCode::Unsupported,
          .path = "[0]" },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const error = refusal(types, row.roots, row.steps);
        CHECK(error.code == row.code);
        CHECK(error.path == row.path);
        CHECK_FALSE(error.detail.empty());
        CHECK_FALSE(error.message().empty());
    }
}

TEST_CASE("Values.openCompoundNamedAtFinish")
{
    // A compound left open leaves its root unfinished too; finish() names the open compound, the cause.
    auto types = Types {};
    auto const u32 = types.primitive(ValueKind::U32);
    auto const record = types.record({ "a", "b" }, { u32, u32 });
    auto const error = refusal(types, { record }, [](ValueBuilder& builder) { builder.begin(2).scalar(1); });
    CHECK(error.code == ValueErrorCode::Unbalanced);
    CHECK(error.path == "[0][1]");
    CHECK(error.detail == "the record is open, 1 of its 2 children given");
    auto const missing = refusal(types, { u32, u32 }, [](ValueBuilder& builder) { builder.scalar(1); });
    CHECK(missing.detail == "1 of 2 values given");
}

TEST_CASE("Values.errorReportedAtTheStep")
{
    auto types = Types {};
    auto const u32 = types.primitive(ValueKind::U32);
    auto const string = types.primitive(ValueKind::String);
    auto const inner = types.record({ "x", "y", "z" }, { u32, u32, string });
    auto const outer = types.list(inner);
    auto const roots = std::vector<TypeId> { u32, outer };
    auto const budget = budgetOf();

    auto builder = ValueBuilder { types.arena(), roots, budget };
    builder.scalar(1).begin(2).begin(3).scalar(1).scalar(2);
    CHECK_FALSE(builder.error().has_value());
    builder.scalar(3); // the string field
    REQUIRE(builder.error().has_value());
    auto const first = *builder.error();
    CHECK(first.code == ValueErrorCode::TypeMismatch);
    CHECK(first.path == "[1][0][2]");
    CHECK(first.message() == "type mismatch at [1][0][2]: expected string, given a scalar");

    // Later steps do nothing, and finish() returns the first error.
    builder.string("ok").end().end().end().begin(9);
    CHECK(builder.error() == first);
    auto const finished = builder.finish();
    REQUIRE_FALSE(finished.has_value());
    CHECK(finished.error() == first);
}

TEST_CASE("Values.deepValueIsFlat")
{
    // An option chain far deeper than any recursion through it could survive under ASan: built,
    // walked, compared, printed and destroyed, all on explicit stacks or none.
    constexpr auto Depth = uint32_t { 250'000 };
    auto limits = ComponentLimits {};
    limits.maxTypeDepth = Depth + 1;
    limits.maxTypeSize = 4 * Depth;
    auto types = Types { limits };
    auto chain = std::vector<TypeId> { types.primitive(ValueKind::U32) };
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, Depth))
        chain.push_back(types.option(chain.back()));
    auto const budget = budgetOf();
    {
        auto const steps = [&](ValueBuilder& builder) {
            for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, Depth))
                builder.begin(1);
            builder.scalar(42);
            for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, Depth))
                builder.end();
        };
        auto const values = build(types, { chain.back() }, steps, budget);
        auto const again = build(types, { chain.back() }, steps, budget);
        CHECK(values.nodeCount() == Depth + 1);
        CHECK(visitAll(values.root(0)) == Depth + 1);
        CHECK(equal(values.root(0), again.root(0)));
        auto const text = toText(types.arena(), values.root(0));
        CHECK(text.size()
              == (Depth * std::string_view { "(option.some " }.size())
                     + std::string_view { "(u32.const 42)" }.size() + Depth);
        auto leaf = values.root(0);
        while (leaf.kind() == ValueKind::Option)
            leaf = leaf.child(0);
        CHECK(leaf.scalar<uint32_t>() == 42);
    }
    CHECK(budget->live() == 0);

    // A million-element list<u8> is one node and one pool range of a million bytes.
    static constexpr auto Elements = uint32_t { 1'000'000 };
    auto const bytes = types.list(types.primitive(ValueKind::U8));
    auto const values = build(
        types,
        { bytes },
        [](ValueBuilder& builder) {
            builder.begin(Elements);
            for (auto const index: std::views::iota(uint32_t { 0 }, Elements))
                builder.scalar(index & 0xFFU);
            builder.end();
        },
        budget);
    CHECK(values.nodeCount() == 1);
    CHECK(values.hostBytes() >= Elements);
    CHECK(values.hostBytes() < Elements + 1024);
    auto const list = values.root(0);
    CHECK(list.childCount() == Elements);
    CHECK(list.child(Elements - 1).scalar<uint8_t>() == ((Elements - 1) & 0xFFU));
    CHECK(list.child(256).type() == types.primitive(ValueKind::U8));
}

TEST_CASE("Values.budgetIsStoreWide")
{
    // Two live Values of h bytes each in a budget of 3h/2 (the brief's 60 of 100, give or take): the
    // second is refused while the first lives, and fits once it is gone. h is measured, not assumed,
    // so the case holds whatever the platform's sizeof(ValueNode). The second needs at least its
    // exact sizes, which are above h/2 since no storage grows past twice what it holds.
    auto types = Types {};
    auto const strings = types.list(types.primitive(ValueKind::String));
    auto const steps = [](ValueBuilder& builder) {
        builder.begin(3).string("alpha").string("beta").string("gamma").end();
    };
    auto const measure = budgetOf();
    auto const measured = build(types, { strings }, steps, measure).hostBytes();
    CHECK(measure->live() == 0);

    auto const budget = budgetOf(measured * 3 / 2);
    {
        auto first = std::optional<Values> { build(types, { strings }, steps, budget) };
        CHECK(budget->live() == measured);
        auto const second = refusal(types, { strings }, steps, budget);
        CHECK(second.code == ValueErrorCode::Budget);
        CHECK(second.detail == limitMessage(ComponentLimit::LiveValueBytes, budget->limit()));
        CHECK(budget->live() == measured);
        first.reset();
        CHECK(budget->live() == 0);
        auto const fits = build(types, { strings }, steps, budget);
        CHECK(budget->live() == measured);
    }
    CHECK(budget->live() == 0);
}

TEST_CASE("Values.partialBuildReleasesItsCharge")
{
    auto types = Types {};
    auto const string = types.primitive(ValueKind::String);
    auto const strings = types.list(string);
    auto const nested = types.list(strings);
    auto const budget = budgetOf(uint64_t { 64 } * 1024);
    auto const kept = build(
        types, { strings }, [](ValueBuilder& builder) { builder.begin(1).string("kept").end(); }, budget);
    auto const baseline = budget->live();
    CHECK(baseline == kept.hostBytes());

    SECTION("refused partway")
    {
        // Lists of 100-byte strings until the budget refuses a growth: every charge stays under the
        // limit, and the builder's death returns it to the baseline.
        auto const text = std::string(100, 'x');
        {
            auto builder = ValueBuilder { types.arena(), std::array { nested }, budget };
            builder.begin(1'000);
            auto lists = 0;
            while (!builder.error() && lists < 1'000)
            {
                builder.begin(100);
                for ([[maybe_unused]] auto const index: std::views::iota(0, 100))
                    builder.string(text);
                builder.end();
                ++lists;
                CHECK(budget->live() <= budget->limit());
            }
            REQUIRE(builder.error().has_value());
            CHECK(builder.error()->code == ValueErrorCode::Budget);
            auto const finished = builder.finish();
            REQUIRE_FALSE(finished.has_value());
            CHECK(finished.error().code == ValueErrorCode::Budget);
        }
        CHECK(budget->live() == baseline);
    }

    SECTION("a lying count is refused before anything grows")
    {
        auto builder = ValueBuilder { types.arena(), std::array { nested }, budget };
        auto const before = budget->live();
        builder.begin(std::numeric_limits<uint32_t>::max());
        REQUIRE(builder.error().has_value());
        CHECK(builder.error()->code == ValueErrorCode::Budget);
        CHECK(budget->live() == before);
    }

    SECTION("an unbalanced finish")
    {
        {
            auto builder = ValueBuilder { types.arena(), std::array { strings }, budget };
            builder.begin(2).string("one");
            CHECK(budget->live() > baseline);
            auto const finished = builder.finish();
            REQUIRE_FALSE(finished.has_value());
            CHECK(finished.error().code == ValueErrorCode::Unbalanced);
        }
        CHECK(budget->live() == baseline);
    }

    SECTION("moving keeps the charge, assigning releases the old")
    {
        auto values = build(
            types, { strings }, [](ValueBuilder& builder) { builder.begin(1).string("a").end(); }, budget);
        auto const held = values.hostBytes();
        auto moved = std::move(values);
        CHECK(moved.hostBytes() == held);
        CHECK(budget->live() == baseline + held);
        moved = build(types, { strings }, [](ValueBuilder& builder) { builder.begin(0).end(); }, budget);
        CHECK(budget->live() == baseline + moved.hostBytes());
    }
    CHECK(budget->live() == baseline);

    SECTION("a refused root reservation")
    {
        auto const tiny = budgetOf(1);
        auto builder = ValueBuilder { types.arena(), std::array { string }, tiny };
        REQUIRE(builder.error().has_value());
        CHECK(builder.error()->code == ValueErrorCode::Budget);
        CHECK(builder.error()->path == "[0]");
        builder.string("x");
        CHECK(tiny->live() == 0);
    }
}

TEST_CASE("Values.budgetRefusesEveryStep")
{
    // A budget that holds the root table and nothing more: each kind of step is refused by name at
    // its first growth, and the refused build releases everything. Then budgets one growth larger,
    // so that a string's and a list's later growths are refused too. The sizes are Isoline's own
    // structs, not a standard library's.
    auto types = Types {};
    auto const u32 = types.primitive(ValueKind::U32);
    auto const string = types.primitive(ValueKind::String);
    auto const strings = types.list(string);
    auto const record = types.record({ "a" }, { u32 });
    auto const own = types.handle(ValueKind::Own);
    constexpr auto RootBytes = uint64_t { sizeof(uint32_t) };
    constexpr auto NodeBytes = uint64_t { sizeof(isoline::component::canonical::ValueNode) };
    struct Row
    {
        std::string_view name;
        TypeId root;
        uint64_t limit;
        std::function<void(ValueBuilder&)> steps;
    };
    auto const rows = std::vector<Row> {
        { .name = "scalar node",
          .root = u32,
          .limit = RootBytes,
          .steps = [](ValueBuilder& b) { b.scalar(1); } },
        { .name = "string pool",
          .root = string,
          .limit = RootBytes,
          .steps = [](ValueBuilder& b) { b.string("abc"); } },
        { .name = "string range",
          .root = string,
          .limit = RootBytes + 3,
          .steps = [](ValueBuilder& b) { b.string("abc"); } },
        { .name = "string node",
          .root = string,
          .limit = RootBytes + 3 + sizeof(isoline::component::canonical::ValueRange),
          .steps = [](ValueBuilder& b) { b.string("abc"); } },
        { .name = "record node",
          .root = record,
          .limit = RootBytes,
          .steps = [](ValueBuilder& b) { b.begin(1); } },
        { .name = "list range",
          .root = strings,
          .limit = RootBytes + NodeBytes,
          .steps = [](ValueBuilder& b) { b.begin(0); } },
        { .name = "handle node",
          .root = own,
          .limit = RootBytes,
          .steps = [](ValueBuilder& b) { b.resource({ .type = ResourceTypeIndex { 1 }, .rep = 1 }); } },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const budget = budgetOf(row.limit);
        auto const error = refusal(types, { row.root }, row.steps, budget);
        CHECK(error.code == ValueErrorCode::Budget);
        CHECK(error.path == "[0]");
        CHECK(budget->live() == 0);
    }
}

TEST_CASE("Values.equalAcrossArenas")
{
    auto left = Types {};
    auto right = Types {};
    auto const makeRecord = [](Types& types) {
        auto const f32 = types.primitive(ValueKind::F32);
        auto const strings = types.list(types.primitive(ValueKind::String));
        // A padding type first, so that the record's TypeId differs between the arenas.
        return types.record({ "a", "b", "c" }, { types.primitive(ValueKind::U32), f32, strings });
    };
    std::ignore = right.list(right.primitive(ValueKind::S64));
    auto const leftRecord = makeRecord(left);
    auto const rightRecord = makeRecord(right);
    REQUIRE(leftRecord != rightRecord);

    auto const valueOf = [](Types const& types, TypeId record, uint32_t nanBits, std::string_view text) {
        return build(types, { record }, [&](ValueBuilder& builder) {
            builder.begin(3).scalar(7).scalar(nanBits).begin(1).string(text).end().end();
        });
    };
    auto const a = valueOf(left, leftRecord, 0x7FC0'0000, "x");
    auto const b = valueOf(right, rightRecord, 0x7FC0'0001, "x");
    CHECK(a.arena() != b.arena());
    CHECK(equal(a.root(0), b.root(0)));
    CHECK(equal(b.root(0), a.root(0)));
    CHECK_FALSE(equal(a.root(0), valueOf(right, rightRecord, 0x7FC0'0000, "y").root(0)));
    CHECK_FALSE(equal(a.root(0), valueOf(right, rightRecord, std::bit_cast<uint32_t>(1.5F), "x").root(0)));
    CHECK(
        toText(left.arena(), a.root(0))
        == R"((record.const (field "a" (u32.const 7)) (field "b" (f32.const nan)) (field "c" (list.const (str.const "x")))))");

    // Kinds, counts, cases and floats that differ.
    auto types = Types {};
    auto const u32 = types.primitive(ValueKind::U32);
    auto const s32 = types.primitive(ValueKind::S32);
    auto const f64 = types.primitive(ValueKind::F64);
    auto const u32s = types.list(u32);
    auto const option = types.option(u32);
    auto const flags = types.labelled(ValueKind::Flags, { "a", "b" });
    auto const own = types.handle(ValueKind::Own);
    auto const pair = [&](std::vector<TypeId> const& roots, std::function<void(ValueBuilder&)> const& steps) {
        return build(types, roots, steps);
    };
    auto const scalars = pair({ u32, s32, f64, f64, f64 }, [](ValueBuilder& builder) {
        builder.scalar(1)
            .scalar(1)
            .scalar(std::bit_cast<uint64_t>(0.0))
            .scalar(std::bit_cast<uint64_t>(-0.0))
            .scalar(0x7FF8'0000'0000'0001);
    });
    CHECK_FALSE(equal(scalars.root(0), scalars.root(1))); // u32 1 vs s32 1: kinds differ
    CHECK_FALSE(equal(scalars.root(2), scalars.root(3))); // +0 vs -0: bits differ
    CHECK_FALSE(equal(scalars.root(2), scalars.root(4))); // a number vs NaN
    CHECK(equal(scalars.root(4), scalars.root(4)));
    auto const lists = pair({ u32s, u32s, u32s }, [](ValueBuilder& builder) {
        builder.begin(2)
            .scalar(1)
            .scalar(2)
            .end()
            .begin(1)
            .scalar(1)
            .end()
            .begin(2)
            .scalar(1)
            .scalar(3)
            .end();
    });
    CHECK_FALSE(equal(lists.root(0), lists.root(1)));
    CHECK_FALSE(equal(lists.root(1), lists.root(0))); // a prefix is not equal either way
    CHECK_FALSE(equal(lists.root(0), lists.root(2)));
    auto const options = pair({ option, option, option }, [](ValueBuilder& builder) {
        builder.begin(0).end().begin(1).scalar(5).end().begin(1).scalar(5).end();
    });
    CHECK_FALSE(equal(options.root(0), options.root(1)));
    CHECK(equal(options.root(1), options.root(2)));
    auto const others = pair({ flags, flags, own, own }, [](ValueBuilder& builder) {
        builder.scalar(0b01).scalar(0b10);
        builder.resource({ .type = ResourceTypeIndex { 1 }, .rep = 2 });
        builder.resource({ .type = ResourceTypeIndex { 2 }, .rep = 2 });
    });
    CHECK_FALSE(equal(others.root(0), others.root(1)));
    CHECK_FALSE(equal(others.root(2), others.root(3)));
    CHECK(equal(others.root(2), others.root(2)));
}

TEST_CASE("Values.childrenIsLinear")
{
    // Every value is visited once by a walk through children(), and the walk steps over each child's
    // descendants in one step: visits == nodes, for a flat and for a nested list.
    static constexpr auto Elements = uint32_t { 100'000 };
    auto types = Types {};
    auto const string = types.primitive(ValueKind::String);
    auto const strings = types.list(string);
    auto const nested = types.list(types.record({ "s", "n" }, { strings, types.primitive(ValueKind::U32) }));
    auto const flat = build(types, { strings }, [](ValueBuilder& builder) {
        builder.begin(Elements);
        for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, Elements))
            builder.string("s");
        builder.end();
    });
    CHECK(flat.nodeCount() == Elements + 1);
    CHECK(visitAll(flat.root(0)) == flat.nodeCount());
    auto count = std::size_t { 0 };
    for ([[maybe_unused]] auto const element: flat.root(0).children())
        ++count;
    CHECK(count == Elements);

    auto const deep = build(types, { nested }, [](ValueBuilder& builder) {
        builder.begin(Elements / 10);
        for (auto const index: std::views::iota(uint32_t { 0 }, Elements / 10))
            builder.begin(2).begin(2).string("a").string("b").end().scalar(index).end();
        builder.end();
    });
    CHECK(deep.nodeCount() == 1 + (Elements / 10 * 5));
    CHECK(visitAll(deep.root(0)) == deep.nodeCount());
    CHECK(deep.root(0).child((Elements / 10) - 1).child(1).scalar<uint32_t>() == (Elements / 10) - 1);
}

TEST_CASE("Values.scalarsReadBack")
{
    auto types = Types {};
    auto const kinds = std::array { ValueKind::Bool, ValueKind::S8,  ValueKind::U8,  ValueKind::S16,
                                    ValueKind::U16,  ValueKind::S32, ValueKind::U32, ValueKind::S64,
                                    ValueKind::U64,  ValueKind::F32, ValueKind::F64, ValueKind::Char };
    auto const bits = std::array<uint64_t, kinds.size()> { 1,
                                                           0x80,
                                                           0xFF,
                                                           0x8000,
                                                           0xFFFF,
                                                           0x8000'0000,
                                                           0xFFFF'FFFF,
                                                           0x8000'0000'0000'0000,
                                                           0xFFFF'FFFF'FFFF'FFFF,
                                                           std::bit_cast<uint32_t>(-2.5F),
                                                           std::bit_cast<uint64_t>(0.25),
                                                           0x1F600 };
    auto roots = std::vector<TypeId> {};
    for (auto const kind: kinds)
        roots.push_back(types.primitive(kind));
    for (auto const kind: kinds)
        roots.push_back(types.list(types.primitive(kind)));
    auto const values = build(types, roots, [&](ValueBuilder& builder) {
        for (auto const value: bits)
            builder.scalar(value);
        for (auto const value: bits)
            builder.begin(2).scalar(0).scalar(value).end();
    });
    auto const check = [](ValueView view) {
        switch (view.kind())
        {
            case ValueKind::Bool: CHECK(view.scalar<bool>()); break;
            case ValueKind::S8: CHECK(view.scalar<int8_t>() == -128); break;
            case ValueKind::U8: CHECK(view.scalar<uint8_t>() == 255); break;
            case ValueKind::S16: CHECK(view.scalar<int16_t>() == -32768); break;
            case ValueKind::U16: CHECK(view.scalar<uint16_t>() == 65535); break;
            case ValueKind::S32: CHECK(view.scalar<int32_t>() == std::numeric_limits<int32_t>::min()); break;
            case ValueKind::U32: CHECK(view.scalar<uint32_t>() == 0xFFFF'FFFFU); break;
            case ValueKind::S64: CHECK(view.scalar<int64_t>() == std::numeric_limits<int64_t>::min()); break;
            case ValueKind::U64:
                CHECK(view.scalar<uint64_t>() == std::numeric_limits<uint64_t>::max());
                break;
            case ValueKind::F32: CHECK(view.scalar<float>() == -2.5F); break;
            case ValueKind::F64: CHECK(view.scalar<double>() == 0.25); break;
            case ValueKind::Char: CHECK(view.scalar<char32_t>() == U'\U0001F600'); break;
            default: FAIL("not a scalar");
        }
    };
    for (auto const index: std::views::iota(std::size_t { 0 }, kinds.size()))
    {
        INFO(std::format("kind {}", index));
        auto const scalar = values.root(index);
        CHECK(scalar.kind() == kinds[index]);
        CHECK(scalar.scalarBits() == bits[index]);
        CHECK(scalar.childCount() == 0);
        check(scalar);
        auto const list = values.root(kinds.size() + index);
        REQUIRE(list.childCount() == 2);
        auto const element = list.child(1);
        CHECK(element.kind() == kinds[index]);
        CHECK(element.type() == types.primitive(kinds[index]));
        CHECK(element.arena() == types.arena().id());
        CHECK(element.scalarBits() == bits[index]);
        CHECK(element.childCount() == 0);
        check(element);
        CHECK(list.child(0).scalarBits() == 0);
        CHECK(equal(scalar, element));
    }
    // Every packed list is one node, and its elements have no children.
    CHECK(values.nodeCount() == 2 * kinds.size());
    CHECK(visitAll(values.root(kinds.size())) == 3);
}

TEST_CASE("Values.textForms")
{
    auto types = Types {};
    auto const u32 = types.primitive(ValueKind::U32);
    auto const string = types.primitive(ValueKind::String);
    struct Row
    {
        TypeId type;
        std::function<void(ValueBuilder&)> steps;
        std::string_view text;
    };
    auto const variant = types.variant({ "none", "some" }, { std::nullopt, u32 });
    auto const result = types.wrap(ValueKind::Result, { u32, std::nullopt });
    auto const rows = std::vector<Row> {
        { .type = types.primitive(ValueKind::Bool),
          .steps = [](ValueBuilder& b) { b.scalar(0); },
          .text = "(bool.const false)" },
        { .type = types.primitive(ValueKind::Bool),
          .steps = [](ValueBuilder& b) { b.scalar(1); },
          .text = "(bool.const true)" },
        { .type = types.primitive(ValueKind::S8),
          .steps = [](ValueBuilder& b) { b.scalar(0xFF); },
          .text = "(s8.const -1)" },
        { .type = types.primitive(ValueKind::S64),
          .steps = [](ValueBuilder& b) { b.scalar(5); },
          .text = "(s64.const 5)" },
        { .type = types.primitive(ValueKind::U64),
          .steps = [](ValueBuilder& b) { b.scalar(std::numeric_limits<uint64_t>::max()); },
          .text = "(u64.const 18446744073709551615)" },
        { .type = types.primitive(ValueKind::F32),
          .steps = [](ValueBuilder& b) { b.scalar(std::bit_cast<uint32_t>(1.5F)); },
          .text = "(f32.const 1.5)" },
        { .type = types.primitive(ValueKind::F32),
          .steps =
              [](ValueBuilder& b) {
                  b.scalar(std::bit_cast<uint32_t>(-std::numeric_limits<float>::infinity()));
              },
          .text = "(f32.const -inf)" },
        { .type = types.primitive(ValueKind::F64),
          .steps =
              [](ValueBuilder& b) {
                  b.scalar(std::bit_cast<uint64_t>(std::numeric_limits<double>::infinity()));
              },
          .text = "(f64.const inf)" },
        { .type = types.primitive(ValueKind::F64),
          .steps = [](ValueBuilder& b) { b.scalar(0xFFF8'0000'0000'0000); },
          .text = "(f64.const nan)" },
        { .type = types.primitive(ValueKind::Char),
          .steps = [](ValueBuilder& b) { b.scalar('"'); },
          .text = R"((char.const "\""))" },
        { .type = types.primitive(ValueKind::Char),
          .steps = [](ValueBuilder& b) { b.scalar(0xE9); },
          .text = "(char.const \"\xC3\xA9\")" },
        { .type = string,
          .steps = [](ValueBuilder& b) { b.string("a\\b\t\n\r'\x01\x7F\xF0\x9F\x98\x80"); },
          .text = R"((str.const "a\\b\t\n\r\'\u{1}\u{7f})"
                  "\xF0\x9F\x98\x80\")" },
        { .type = types.list(string),
          .steps = [](ValueBuilder& b) { b.begin(0).end(); },
          .text = "(list.const)" },
        { .type = types.fixedList(u32, 2),
          .steps = [](ValueBuilder& b) { b.begin(2).scalar(1).scalar(2).end(); },
          .text = "(list.const (u32.const 1) (u32.const 2))" },
        { .type = types.tuple({ u32, string }),
          .steps = [](ValueBuilder& b) { b.begin(2).scalar(1).string("t").end(); },
          .text = R"((tuple.const (u32.const 1) (str.const "t")))" },
        { .type = variant,
          .steps = [](ValueBuilder& b) { b.begin(0).end(); },
          .text = R"((variant.const "none"))" },
        { .type = variant,
          .steps = [](ValueBuilder& b) { b.begin(1).scalar(3).end(); },
          .text = R"((variant.const "some" (u32.const 3)))" },
        { .type = types.labelled(ValueKind::Enum, { "red", "green" }),
          .steps = [](ValueBuilder& b) { b.scalar(1); },
          .text = R"((enum.const "green"))" },
        { .type = types.labelled(ValueKind::Flags, { "r", "w", "x" }),
          .steps = [](ValueBuilder& b) { b.scalar(0b101); },
          .text = R"((flags.const "r" "x"))" },
        { .type = types.labelled(ValueKind::Flags, { "r", "w", "x" }),
          .steps = [](ValueBuilder& b) { b.scalar(0); },
          .text = "(flags.const)" },
        { .type = types.option(u32),
          .steps = [](ValueBuilder& b) { b.begin(0).end(); },
          .text = "(option.none)" },
        { .type = types.option(u32),
          .steps = [](ValueBuilder& b) { b.begin(1).scalar(9).end(); },
          .text = "(option.some (u32.const 9))" },
        { .type = result,
          .steps = [](ValueBuilder& b) { b.begin(0).scalar(9).end(); },
          .text = "(result.ok (u32.const 9))" },
        { .type = result, .steps = [](ValueBuilder& b) { b.begin(1).end(); }, .text = "(result.err)" },
        { .type = types.wrap(ValueKind::Map, { string, u32 }),
          .steps = [](ValueBuilder& b) { b.begin(2).string("a").scalar(1).string("b").scalar(2).end(); },
          .text =
              R"((map.const (entry (str.const "a") (u32.const 1)) (entry (str.const "b") (u32.const 2))))" },
        { .type = types.handle(ValueKind::Own),
          .steps = [](ValueBuilder& b) { b.resource({ .type = ResourceTypeIndex { 3 }, .rep = 4 }); },
          .text = "(own 3 4)" },
        { .type = types.handle(ValueKind::Borrow),
          .steps =
              [](ValueBuilder& b) {
                  b.resource({ .type = ResourceTypeIndex { 3 }, .rep = 5, .ownership = Ownership::Borrow });
              },
          .text = "(borrow 3 5)" },
    };
    for (auto const& row: rows)
    {
        INFO(row.text);
        auto const values = build(types, { row.type }, row.steps);
        CHECK(toText(types.arena(), values.root(0)) == row.text);
    }
}

TEST_CASE("Values.viewAccessors")
{
    auto types = Types {};
    auto const u32 = types.primitive(ValueKind::U32);
    auto const record = types.record({ "a", "b", "c" }, { u32, u32, u32 });
    auto const variant = types.variant({ "none", "some" }, { std::nullopt, record });
    auto const enumeration = types.labelled(ValueKind::Enum, { "x", "y", "z" });
    auto const flags = types.labelled(ValueKind::Flags, { "a", "b" });
    auto const borrow = types.handle(ValueKind::Borrow);
    auto const map = types.wrap(ValueKind::Map, { u32, u32 });
    auto const resource =
        ResourceValue { .type = ResourceTypeIndex { 9 }, .rep = 0xFFFF'FFFF, .ownership = Ownership::Borrow };
    auto const values =
        build(types, { variant, variant, enumeration, flags, borrow, map }, [&](ValueBuilder& builder) {
            builder.begin(1).begin(3).scalar(10).scalar(20).scalar(30).end().end();
            builder.begin(0).end();
            builder.scalar(2).scalar(0b11).resource(resource);
            builder.begin(1).scalar(4).scalar(5).end();
        });
    auto const some = values.root(0);
    CHECK(some.caseIndex() == 1);
    CHECK(some.childCount() == 1);
    REQUIRE(some.payload().has_value());
    auto const fields = some.payload()->children();
    CHECK(some.child(0).child(2).scalar<uint32_t>() == 30);
    CHECK((*fields.begin()).scalar<uint32_t>() == 10);
    CHECK_FALSE(values.root(1).payload().has_value());
    CHECK(values.root(1).childCount() == 0);
    CHECK(values.root(2).caseIndex() == 2);
    CHECK(values.root(3).flagBits() == 0b11);
    CHECK(values.root(4).resource() == resource);
    CHECK(values.root(5).childCount() == 2);
    CHECK(values.root(5).child(1).scalar<uint32_t>() == 5);
}

TEST_CASE("Values.stringsKeepTheirEncoding")
{
    // StringValue::utf8() of each encoding, and its refusals.
    auto const utf16 = utf16Of({ 0x0061, 0x00E9, 0xD83D, 0xDE00 }); // "aé😀"
    auto const latin1 = std::array { std::byte { 0x61 }, std::byte { 0xE9 } };
    auto const expected = std::string { "a\xC3\xA9\xF0\x9F\x98\x80" };
    CHECK(StringValue { .bytes = utf16, .encoding = StringEncoding::Utf16, .taggedCodeUnits = 4 }.utf8()
          == expected);
    CHECK(StringValue {
              .bytes = utf16, .encoding = StringEncoding::Latin1Utf16, .taggedCodeUnits = 4 | Utf16Tag }
              .utf8()
          == expected);
    CHECK(
        StringValue { .bytes = latin1, .encoding = StringEncoding::Latin1Utf16, .taggedCodeUnits = 2 }.utf8()
        == std::string { "a\xC3\xA9" });
    CHECK(StringValue { .bytes = bytesOf(expected), .encoding = StringEncoding::Utf8, .taggedCodeUnits = 7 }
              .utf8()
          == expected);

    auto const refusals = std::vector<std::pair<StringValue, StringError>> {
        { StringValue { .bytes = bytesOf("\xC3\x28"), .encoding = StringEncoding::Utf8 },
          StringError::InvalidUtf8 },
        { StringValue { .bytes = bytesOf("\xED\xA0\x80"), .encoding = StringEncoding::Utf8 },
          StringError::InvalidUtf8 },
        { StringValue { .bytes = bytesOf("\xC0\xAF"), .encoding = StringEncoding::Utf8 },
          StringError::InvalidUtf8 },
        { StringValue { .bytes = bytesOf("\xE2\x82"), .encoding = StringEncoding::Utf8 },
          StringError::InvalidUtf8 },
        { StringValue { .bytes = bytesOf("\xF4\x90\x80\x80"), .encoding = StringEncoding::Utf8 },
          StringError::InvalidUtf8 },
        { StringValue { .bytes = bytesOf("abc"), .encoding = StringEncoding::Utf16 },
          StringError::InvalidUtf16 },
    };
    for (auto const& [value, error]: refusals)
    {
        auto const text = value.utf8();
        REQUIRE_FALSE(text.has_value());
        CHECK(text.error() == error);
    }
    auto const loneHigh = utf16Of({ 0xD800 });
    auto const loneLow = utf16Of({ 0xDC00, 0x0061 });
    auto const highThenLetter = utf16Of({ 0xD800, 0x0061 });
    for (auto const* bytes: { &loneHigh, &loneLow, &highThenLetter })
    {
        auto const text = StringValue { .bytes = *bytes, .encoding = StringEncoding::Utf16 }.utf8();
        REQUIRE_FALSE(text.has_value());
        CHECK(text.error() == StringError::InvalidUtf16);
    }

    // A builder's UTF-8 strings, and every UTF-8 sequence length, read back.
    auto types = Types {};
    auto const string = types.primitive(ValueKind::String);
    auto const text = std::string { "$\xC2\xA2\xE2\x82\xAC\xF0\x90\x8D\x88" };
    auto const values = build(types, { string }, [&](ValueBuilder& builder) { builder.string(text); });
    CHECK(values.root(0).string().utf8() == text);
    CHECK(values.root(0).string().taggedCodeUnits == text.size());
}

TEST_CASE("LiveBytesBudget.chargesWholeOrNothing")
{
    auto budget = LiveBytesBudget { 100 };
    CHECK(budget.limit() == 100);
    CHECK(budget.live() == 0);
    REQUIRE(budget.charge(60).has_value());
    CHECK(budget.available() == 40);
    auto const refused = budget.charge(41);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == BudgetExceeded { .requested = 41, .live = 60, .limit = 100 });
    CHECK(refused.error().message() == "live value bytes exceed the limit of 100");
    CHECK(budget.live() == 60);
    // A charge that would wrap around is refused, not wrapped.
    CHECK_FALSE(budget.charge(std::numeric_limits<uint64_t>::max()).has_value());
    REQUIRE(budget.charge(40).has_value());
    CHECK(budget.available() == 0);
    CHECK_FALSE(budget.charge(1).has_value());
    budget.release(100);
    CHECK(budget.live() == 0);
}

TEST_CASE("ChargedVector.growthIsChargedFirst")
{
    constexpr auto Unit = uint64_t { sizeof(uint64_t) };
    auto const budget = budgetOf(900);
    {
        auto vector = ChargedVector<uint64_t> { budget };
        REQUIRE(vector.reserveFor(10).has_value());
        CHECK(vector.chargedBytes() == 80);
        CHECK(budget->live() == 80);
        for (auto const value: std::views::iota(uint64_t { 0 }, uint64_t { 10 }))
            vector.append(value);
        // Doubling to 20 fits: 160 charged beside the old 80, which is then released.
        REQUIRE(vector.reserveFor(1).has_value());
        CHECK(vector.chargedBytes() == 160);
        CHECK(budget->live() == 160);
        auto const tens = std::array<uint64_t, 10> {};
        vector.append(tens);
        REQUIRE(vector.reserveFor(1).has_value());
        CHECK(vector.chargedBytes() == 320);
        // Doubling to 80 would need 640 beside the 320 held, more than 900: the growth is as large
        // as the budget affords, (900 - 320) / 8 = 72 elements.
        vector.append(tens);
        vector.append(tens);
        REQUIRE(vector.reserveFor(1).has_value());
        CHECK(vector.chargedBytes() == 72 * Unit);
        CHECK(budget->live() == 72 * Unit);
        // More than the budget has left is refused, changing nothing.
        auto const refused = vector.reserveFor(100);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().limit == 900);
        CHECK(refused.error().live == 72 * Unit);
        CHECK(vector.chargedBytes() == 72 * Unit);
        CHECK(budget->live() == 72 * Unit);
        // A count whose bytes overflow 64 bits is refused before any arithmetic wraps.
        CHECK_FALSE(vector.reserveFor(std::numeric_limits<std::size_t>::max()).has_value());
        CHECK(vector.items().size() == 40);
        CHECK(vector[39] == 0);
        CHECK(std::as_const(vector)[9] == 9);

        auto moved = std::move(vector);
        CHECK(moved.chargedBytes() == 72 * Unit);
        CHECK(budget->live() == 72 * Unit);
        auto other = ChargedVector<uint64_t> { budget };
        REQUIRE(other.reserveFor(1).has_value());
        CHECK(budget->live() == 73 * Unit);
        other = std::move(moved);
        CHECK(budget->live() == 72 * Unit);
        CHECK(other.size() == 40);
    }
    CHECK(budget->live() == 0);

    // A growth the budget allows but the host cannot make: the charge is released before the
    // exception leaves. Bytes, so that max_size() + 1 fits 64 bits on every standard library.
    auto const unlimited = budgetOf(std::numeric_limits<uint64_t>::max());
    {
        auto vector = ChargedVector<std::byte> { unlimited };
        auto const pastMax = std::vector<std::byte> {}.max_size() + 1;
        CHECK_THROWS_AS(std::ignore = vector.reserveFor(pastMax), std::length_error);
        CHECK(unlimited->live() == 0);
        CHECK(vector.chargedBytes() == 0);
    }
}

TEST_CASE("LiveBytesBudget.chargesAcrossThreads", "[threading]")
{
    // Four threads charge and release one byte at a time against a limit of 2: the budget never holds
    // more than its limit, and ends empty.
    auto budget = LiveBytesBudget { 2 };
    auto refusals = std::array<uint32_t, 4> {};
    auto overLimit = std::array<uint32_t, 4> {};
    {
        auto threads = std::vector<std::thread> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, refusals.size()))
        {
            threads.emplace_back([&budget, &refusals, &overLimit, index] {
                for ([[maybe_unused]] auto const round: std::views::iota(0, 10'000))
                {
                    if (!budget.charge(1))
                    {
                        ++refusals[index];
                        continue;
                    }
                    if (budget.live() > budget.limit())
                        ++overLimit[index];
                    budget.release(1);
                }
            });
        }
        for (auto& thread: threads)
            thread.join();
    }
    CHECK(budget.live() == 0);
    for (auto const count: overLimit)
        CHECK(count == 0);

    // A Values built on one thread is destroyed on another, releasing its charge there.
    auto types = Types {};
    auto const strings = types.list(types.primitive(ValueKind::String));
    auto const shared = budgetOf();
    auto values = std::optional<Values> { build(
        types,
        { strings },
        [](ValueBuilder& builder) { builder.begin(1).string("elsewhere").end(); },
        shared) };
    CHECK(shared->live() > 0);
    auto destroyer = std::thread { [&values] { values.reset(); } };
    destroyer.join();
    CHECK(shared->live() == 0);
}

TEST_CASE("Values.flagsPastThirtyTwoLabelsRefused")
{
    // Validation refuses a flags type of more than 32 labels, but an embedder's arena may hold one: a
    // value of it is refused by name, before the bits are shifted by the label count (undefined from
    // 64 labels on, and silently truncating from 33 to 63).
    auto types = Types {};
    auto const flagsOf = [&types](uint32_t count) {
        auto labels = std::vector<SharedName> {};
        for (auto const index: std::views::iota(uint32_t { 0 }, count))
            labels.emplace_back(std::format("f{}", index));
        return types.intern(DefinedType { .kind = ValueKind::Flags, .labels = std::move(labels) });
    };
    auto const most = flagsOf(isoline::component::canonical::MaxFlagLabels);
    auto const all = build(types, { most }, [](ValueBuilder& builder) { builder.scalar(0xFFFF'FFFFU); });
    CHECK(all.root(0).flagBits() == 0xFFFF'FFFFU);
    CHECK(refusal(types, { most }, [](ValueBuilder& builder) { builder.scalar(uint64_t { 1 } << 32U); }).code
          == ValueErrorCode::InvalidValue);

    for (auto const count: { 33U, 40U, 63U, 64U, 65U, 1000U })
    {
        INFO(count);
        auto const refused =
            refusal(types, { flagsOf(count) }, [](ValueBuilder& builder) { builder.scalar(0); });
        CHECK(refused.code == ValueErrorCode::TooManyFlags);
        CHECK(refused.message()
              == std::format("too many flags at [0]: a flags type of {} labels, more than the 32", count));
    }
}

TEST_CASE("Values.builderIsNeitherCopiedNorMoved")
{
    // A moved-from builder's storage would hold no budget, which its next step would dereference.
    STATIC_CHECK_FALSE(std::is_copy_constructible_v<ValueBuilder>);
    STATIC_CHECK_FALSE(std::is_move_constructible_v<ValueBuilder>);
    STATIC_CHECK_FALSE(std::is_copy_assignable_v<ValueBuilder>);
    STATIC_CHECK_FALSE(std::is_move_assignable_v<ValueBuilder>);
}

TEST_CASE("LiveBytesBudget.chargesWithinTheHeadroom", "[threading]")
{
    // As much as is left, up to the most wanted, never less than the least: one decision.
    auto budget = LiveBytesBudget { 100 };
    auto const all = budget.chargeWithin(10, 40, 1);
    REQUIRE(all.has_value());
    CHECK(*all == 40);
    REQUIRE(budget.charge(50).has_value());
    auto const rest = budget.chargeWithin(5, 40, 1);
    REQUIRE(rest.has_value());
    CHECK(*rest == 10);
    CHECK(budget.live() == 100);
    budget.release(30);
    auto const refused = budget.chargeWithin(31, 40, 1);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().requested == 31);
    CHECK(refused.error().live == 70);
    CHECK(budget.live() == 70);
    auto const exact = budget.chargeWithin(30, 30, 1);
    REQUIRE(exact.has_value());
    CHECK(*exact == 30);
    budget.release(30);
    CHECK_FALSE(budget.chargeWithin(11, 10, 1).has_value());
    CHECK(budget.live() == 70);

    // In whole units: of the 30 left, 3 units of 8 bytes, the 6 bytes of a partial one never charged;
    // and a least of 4 units refused, though 30 bytes are left.
    auto const units = budget.chargeWithin(8, 40, 8);
    REQUIRE(units.has_value());
    CHECK(*units == 24);
    CHECK(budget.live() == 94);
    budget.release(24);
    auto const partial = budget.chargeWithin(32, 40, 8);
    REQUIRE_FALSE(partial.has_value());
    CHECK(partial.error().requested == 32);
    CHECK(budget.live() == 70);

    // Four threads take what is left of a limit of 3, one or two bytes at a time: the budget never holds
    // more than its limit, and ends empty.
    auto shared = LiveBytesBudget { 3 };
    auto overLimit = std::atomic<uint32_t> { 0 };
    {
        auto threads = std::vector<std::thread> {};
        for ([[maybe_unused]] auto const index: std::views::iota(0, 4))
        {
            threads.emplace_back([&shared, &overLimit] {
                for ([[maybe_unused]] auto const round: std::views::iota(0, 10'000))
                {
                    auto const granted = shared.chargeWithin(1, 2, 1);
                    if (!granted)
                        continue;
                    if (shared.live() > shared.limit())
                        ++overLimit;
                    shared.release(*granted);
                }
            });
        }
        for (auto& thread: threads)
            thread.join();
    }
    CHECK(overLimit == 0);
    CHECK(shared.live() == 0);
}

TEST_CASE("ChargedVector.growthTakesTheHeadroomLeft")
{
    // Headroom between what is needed and the doubled capacity: the growth takes all of it, in whole
    // elements, and gives back the bytes of a partial one.
    constexpr auto Unit = uint64_t { sizeof(uint64_t) };
    auto const budget = budgetOf(905);
    auto vector = ChargedVector<uint64_t> { budget };
    REQUIRE(vector.reserveFor(40).has_value());
    for (auto const value: std::views::iota(uint64_t { 0 }, uint64_t { 40 }))
        vector.append(value);
    // Needed 41, doubled 80: (905 - 320) / 8 = 73 elements; the last byte, a partial element, is never
    // charged, not even for a moment.
    REQUIRE(vector.reserveFor(1).has_value());
    CHECK(vector.chargedBytes() == 73 * Unit);
    CHECK(budget->live() == 73 * Unit);
    // With the headroom another charge took, exactly what is needed still fits.
    auto const other = budgetOf(2000);
    auto held = ChargedVector<std::byte> { other };
    REQUIRE(held.reserveFor(400).has_value());
    held.append(std::vector<std::byte>(400));
    REQUIRE(other->charge(1100).has_value());
    REQUIRE(held.reserveFor(1).has_value());
    CHECK(held.chargedBytes() == 500);
    CHECK(other->live() == 1600);
}

TEST_CASE("ChargedVector.growsWhileAnotherThreadCharges", "[threading]")
{
    // A stress case for TSan: 400 bytes held and one more needed, while a second thread toggles a 1100-byte
    // charge on a 2000-byte budget. The exact growth always fits, so whatever the interleaving, no growth
    // is refused, and the budget ends empty.
    constexpr auto Held = std::size_t { 400 };
    constexpr auto Other = uint64_t { 1100 };
    auto const budget = budgetOf(2000);
    auto stop = std::atomic<bool> { false };
    auto other = std::thread { [&budget, &stop] {
        while (!stop.load(std::memory_order_relaxed))
            if (budget->charge(Other))
                budget->release(Other);
    } };
    auto const held = std::vector<std::byte>(Held);
    auto refusals = uint64_t { 0 };
    for ([[maybe_unused]] auto const round: std::views::iota(0, 2000))
    {
        auto vector = ChargedVector<std::byte> { budget };
        if (!vector.reserveFor(Held))
        {
            ++refusals;
            continue;
        }
        vector.append(held);
        if (!vector.reserveFor(1))
            ++refusals;
    }
    stop.store(true, std::memory_order_relaxed);
    other.join();
    CHECK(refusals == 0);
    CHECK(budget->live() == 0);
}

TEMPLATE_TEST_CASE("ChargedVector.everyPathOfEveryElementType",
                   "",
                   std::byte,
                   uint32_t,
                   isoline::component::canonical::ValueNode,
                   isoline::component::canonical::ValueRange)
{
    // The element types Values hold, each through room already made, a growth, a refused growth, a
    // count whose bytes overflow, and a growth the host cannot make.
    constexpr auto Unit = uint64_t { sizeof(TestType) };
    auto const budget = budgetOf(10 * Unit);
    {
        auto vector = ChargedVector<TestType> { budget };
        REQUIRE(vector.reserveFor(4).has_value());
        CHECK(vector.reserveFor(4).has_value());
        CHECK(vector.chargedBytes() == 4 * Unit);
        vector.append(std::vector<TestType>(4));
        // Needed 5, doubled 8: 6 of the 10 are left beside the 4 held, so the growth takes 6.
        REQUIRE(vector.reserveFor(1).has_value());
        CHECK(vector.chargedBytes() == 6 * Unit);
        CHECK_FALSE(vector.reserveFor(5).has_value());
        CHECK_FALSE(vector.reserveFor(std::numeric_limits<std::size_t>::max()).has_value());
        CHECK(budget->live() == 6 * Unit);
    }
    CHECK(budget->live() == 0);
    {
        // Assigned to itself, it keeps its storage and its charge.
        auto vector = ChargedVector<TestType> { budget };
        REQUIRE(vector.reserveFor(3).has_value());
        auto& same = vector;
        vector = std::move(same);
        CHECK(vector.chargedBytes() == 3 * Unit);
        CHECK(budget->live() == 3 * Unit);
    }
    CHECK(budget->live() == 0);
    auto const unlimited = budgetOf(std::numeric_limits<uint64_t>::max());
    auto vector = ChargedVector<TestType> { unlimited };
    // One past max_size(): the host refuses it (libstdc++, libc++), or its bytes overflow 64 bits and
    // the vector refuses it first (the MSVC STL, whose max_size() is SIZE_MAX / sizeof(T)).
    auto const pastMax = std::vector<TestType> {}.max_size() + 1;
    auto outcome = std::string_view { "accepted" };
    try
    {
        auto const reserved = vector.reserveFor(pastMax);
        if (!reserved)
        {
            outcome = "refused";
            CHECK(reserved.error().requested == std::numeric_limits<uint64_t>::max());
        }
    }
    catch (std::length_error const&)
    {
        outcome = "thrown";
    }
    CHECK(outcome != "accepted");
    CHECK(unlimited->live() == 0);
    CHECK(vector.chargedBytes() == 0);
}

TEST_CASE("Values.inPlaceSteps")
{
    auto types = Types {};
    auto const string = types.primitive(ValueKind::String);
    auto const u16 = types.primitive(ValueKind::U16);
    auto const shorts = types.list(u16);
    auto const strings = types.list(string);

    // encodedString(): the bytes stringByteLength() gives, in place, in their encoding.
    auto builder = ValueBuilder { types.arena(), std::vector { string, shorts }, budgetOf() };
    auto const bytes = builder.encodedString(StringEncoding::Latin1Utf16, 2 | Utf16Tag);
    REQUIRE(bytes.size() == 4);
    std::ranges::copy(std::array { std::byte { 'h' }, std::byte {}, std::byte { 'i' }, std::byte {} },
                      bytes.begin());
    // packedElements(): every element of the open list of scalars, in host byte order.
    builder.begin(2);
    auto const elements = builder.packedElements();
    REQUIRE(elements.size() == 4);
    auto const words = std::array { uint16_t { 7 }, uint16_t { 0xbeef } };
    std::memcpy(elements.data(), words.data(), elements.size());
    builder.end();
    auto values = builder.finish();
    REQUIRE(values.has_value());
    CHECK(values->root(0).string().utf8() == "hi");
    CHECK(values->root(0).string().simpleEncoding() == SimpleEncoding::Utf16);
    CHECK(values->root(1).child(1).scalar<uint16_t>() == 0xbeef);

    // Refusals: a string past 2^32 - 1 bytes; a string where another value goes; packed elements with no
    // list open, for a list of strings, or after an element was given.
    auto const refused = [&](std::vector<TypeId> const& roots, auto const& steps) {
        auto refusing = ValueBuilder { types.arena(), roots, budgetOf() };
        steps(refusing);
        REQUIRE(refusing.error().has_value());
        return refusing.error()->code;
    };
    CHECK(refused({ string },
                  [](ValueBuilder& builder) {
                      CHECK(builder.encodedString(StringEncoding::Utf16, ~uint32_t { 0 }).empty());
                  })
          == ValueErrorCode::TooLarge);
    CHECK(
        refused({ u16 },
                [](ValueBuilder& builder) { CHECK(builder.encodedString(StringEncoding::Utf8, 1).empty()); })
        == ValueErrorCode::TypeMismatch);
    CHECK(refused({ shorts }, [](ValueBuilder& builder) { CHECK(builder.packedElements().empty()); })
          == ValueErrorCode::Unbalanced);
    CHECK(refused({ strings },
                  [](ValueBuilder& builder) {
                      builder.begin(1);
                      CHECK(builder.packedElements().empty());
                  })
          == ValueErrorCode::Unbalanced);
    CHECK(refused({ shorts },
                  [](ValueBuilder& builder) {
                      builder.begin(2).scalar(1);
                      CHECK(builder.packedElements().empty());
                  })
          == ValueErrorCode::Unbalanced);
}
