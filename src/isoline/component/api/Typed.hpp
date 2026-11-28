// SPDX-License-Identifier: Apache-2.0
#pragma once

// Typed C++ bindings over the embedding API (P9): component values as C++ values, host functions as
// noexcept callables, exports as typed function objects, and resources as move-only owns and copyable
// borrows. Every conversion goes through canonical::Values (one copy each way), driven by one TypeOf row
// per C++ type: type() interns its component type in a linker's arena, write() gives a ValueBuilder its
// steps, read() takes a ValueView apart (iterating children()), and matches() checks a type of another
// arena, such as a component's, against it.

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Types.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/api/CallError.hpp>
#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/canonical/Layout.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::api::typed
{

/// No value: the payload of a result's or a variant's case that has none, or a function's absent result
/// (as void is). It is no component type of its own.
struct Unit
{
    /// All units are equal.
    bool operator==(Unit const&) const = default;
};

/// A string literal as a template argument: a variant case's label (Case).
template <std::size_t Size>
struct FixedString
{
    /// The literal's characters. Implicit, so a literal is the template argument.
    /// @param text A string literal.
    consteval FixedString(char const (&text)[Size]) noexcept { std::ranges::copy(text, characters.begin()); }

    /// The text, without the terminating zero.
    [[nodiscard]] constexpr std::string_view view() const noexcept { return { characters.data(), Size - 1 }; }

    std::array<char, Size> characters {}; ///< The literal, its terminating zero included.
};

/// One case of a variant: `std::variant<Case<"segment", Segment>, Case<"clear", Unit>>` is the variant
/// `(variant (case "segment" segment) (case "clear"))`.
template <FixedString Name, typename T>
struct Case
{
    using Payload = T;                                     ///< Its payload's type; Unit for none.
    static constexpr std::string_view Label = Name.view(); ///< Its label.

    T value {}; ///< Its payload.

    /// Two cases are equal when their payloads are.
    bool operator==(Case const&) const = default;
};

/// The rows of TypeOf: specialized for every C++ type that is a component value type.
template <typename T>
struct TypeOf;

/// A record's fields, for the embedder to specialize:
/// `static constexpr auto Fields = std::tuple { field("name", &T::member), … };`, in the type's order.
/// The record type must be default-constructible; reading assigns each field.
template <typename T>
struct Record;

/// An enum's labels, for the embedder to specialize: `static constexpr auto Labels = std::to_array<
/// std::string_view>({ … })`, the enumerators of E being 0, 1, … in the labels' order.
template <typename E>
struct Enum;

/// A flags type's labels, for the embedder to specialize as Enum's; F wraps the bitmask as its
/// `uint32_t bits` member (bit i for label i), and is built as `F { bits }`.
template <typename F>
struct Flags;

/// A variant's labels: given for every std::variant of Case alternatives.
template <typename V>
struct Variant;

/// Whether @p T is a Case.
template <typename T>
inline constexpr bool IsCase = false;

/// A Case is one.
template <FixedString Name, typename T>
inline constexpr bool IsCase<Case<Name, T>> = true;

/// The labels of a variant of Cases: theirs, in order.
template <typename... Cases>
    requires(IsCase<Cases> && ...)
struct Variant<std::variant<Cases...>>
{
    static constexpr auto Labels = std::array<std::string_view, sizeof...(Cases)> { Cases::Label... };
};

/// One field of a Record: its label and the member holding it.
template <typename Class, typename Member>
struct FieldRow
{
    using Value = Member; ///< The field's type.

    std::string_view name;  ///< Its label.
    Member Class::* member; ///< The member.
};

/// A Record field.
/// @param name Its label, kebab-case.
/// @param member The member holding it.
/// @return The field.
template <typename Class, typename Member>
[[nodiscard]] constexpr FieldRow<Class, Member> field(std::string_view name, Member Class::* member) noexcept
{
    return FieldRow<Class, Member> { .name = name, .member = member };
}

template <typename R>
class Borrow;

namespace detail
{
    struct HandleAccess;
} // namespace detail

/// An own of a resource the host holds (R12): move-only, so one C++ object names it. Dropping it
/// (typed::drop()) or passing it to a call (TypedFunction, a host function's result) consumes it, which
/// leaves the object empty; an empty own is never dropped or passed again, so no own is dropped twice.
/// An own destroyed while not empty is forgotten: the store still holds it, and a host resource's
/// destructor does not run for it.
template <typename R>
class Own
{
  public:
    /// An empty own, naming no resource: what a moved-from own is.
    Own() noexcept = default;

    Own(Own const&) = delete;
    Own& operator=(Own const&) = delete;

    /// Takes @p other's resource; @p other is left empty.
    Own(Own&& other) noexcept: _value { std::exchange(other._value, std::nullopt) } {}

    /// Takes @p other's resource, forgetting the one held (see the class); @p other is left empty.
    Own& operator=(Own&& other) noexcept
    {
        if (this != &other)
            _value = std::exchange(other._value, std::nullopt);
        return *this;
    }

    ~Own() = default;

    /// The resource's representation (checked: not empty).
    [[nodiscard]] uint32_t rep() const noexcept
    {
        Require(_value.has_value());
        return _value->rep;
    }

    /// Whether it names no resource: moved from, consumed, or default-constructed.
    [[nodiscard]] bool isEmpty() const noexcept { return !_value.has_value(); }

    /// A borrow of its resource, to lend to a call (checked: not empty).
    [[nodiscard]] Borrow<R> borrow() const noexcept;

  private:
    friend struct detail::HandleAccess;

    explicit Own(canonical::ResourceValue value) noexcept: _value { value } {}

    std::optional<canonical::ResourceValue> _value {};
};

/// A borrow of a resource, lent for one call: copyable, as a borrow is only a name.
template <typename R>
class Borrow
{
  public:
    /// The resource's representation.
    [[nodiscard]] uint32_t rep() const noexcept { return _value.rep; }

    /// The handle value.
    [[nodiscard]] canonical::ResourceValue const& value() const noexcept { return _value; }

  private:
    friend struct detail::HandleAccess;

    explicit Borrow(canonical::ResourceValue value) noexcept: _value { value } {}

    canonical::ResourceValue _value;
};

namespace detail
{
    /// What the bindings reach of Own and Borrow; not for embedders.
    struct HandleAccess
    {
        template <typename R>
        [[nodiscard]] static Own<R> own(canonical::ResourceValue value) noexcept
        {
            return Own<R> { value };
        }

        template <typename R>
        [[nodiscard]] static Borrow<R> borrow(canonical::ResourceValue value) noexcept
        {
            return Borrow<R> { value };
        }

        /// The own's handle value (checked: not empty, as writing an empty own is a precondition
        /// violation).
        template <typename R>
        [[nodiscard]] static canonical::ResourceValue const& value(Own<R> const& own) noexcept
        {
            Require(own._value.has_value());
            return *own._value;
        }

        /// Empties @p own, which was consumed.
        template <typename R>
        static void consume(Own<R>& own) noexcept
        {
            own._value.reset();
        }
    };
} // namespace detail

template <typename R>
Borrow<R> Own<R>::borrow() const noexcept
{
    Require(_value.has_value());
    auto lent = *_value;
    lent.ownership = canonical::Ownership::Borrow;
    return detail::HandleAccess::borrow<R>(lent);
}

namespace detail
{
    /// The index of @p R among @p Resources: the first that is it, or their count for none.
    template <typename R, typename... Resources>
    [[nodiscard]] consteval std::size_t indexOfType() noexcept
    {
        auto const matches = std::array<bool, sizeof...(Resources)> { std::is_same_v<R, Resources>... };
        return static_cast<std::size_t>(std::ranges::find(matches, true) - matches.begin());
    }

    /// Whether every type of @p Resources occurs once.
    template <typename... Resources>
    [[nodiscard]] consteval bool areDistinct() noexcept
    {
        auto const firsts =
            std::array<std::size_t, sizeof...(Resources)> { indexOfType<Resources, Resources...>()... };
        auto position = std::size_t { 0 };
        for (auto const first: firsts)
            if (first != position++)
                return false;
        return true;
    }
} // namespace detail

/// The host resource types the bindings name by C++ type: Own<R> and Borrow<R> for each R of
/// @p Resources are handles of the HostResourceType given for it. Unbound types do not compile.
template <typename... Resources>
class ResourceTypes
{
    static_assert(detail::areDistinct<Resources...>(), "a resource type is bound twice");

  public:
    /// No resource types.
    ResourceTypes()
        requires(sizeof...(Resources) == 0)
    = default;

    /// One resource type per R of Resources, in their order.
    /// @param types The types.
    explicit ResourceTypes(std::conditional_t<true, HostResourceType, Resources>... types)
        requires(sizeof...(Resources) > 0)
        : _types { std::move(types)... }
    {
    }

    /// Whether @p R is bound.
    template <typename R>
    [[nodiscard]] static consteval bool contains() noexcept
    {
        return detail::indexOfType<R, Resources...>() < sizeof...(Resources);
    }

    /// The resource type bound to @p R.
    template <typename R>
    [[nodiscard]] HostResourceType const& get() const noexcept
    {
        constexpr auto Index = detail::indexOfType<R, Resources...>();
        static_assert(Index < sizeof...(Resources), "the resource type is not bound");
        return _types[Index];
    }

  private:
    std::array<HostResourceType, sizeof...(Resources)> _types;
};

/// What TypeOf<T>::type() interns a type with: a linker's arena and the host resource types.
template <typename... Resources>
class TypeContext
{
  public:
    /// @param linker The linker whose arena takes the types; outlives the context.
    /// @param resources The host resource types; outlive the context.
    TypeContext(Linker& linker, ResourceTypes<Resources...> const& resources) noexcept:
        _linker { &linker }, _resources { &resources }
    {
    }

    /// The primitive @p kind.
    [[nodiscard]] types::TypeId primitive(types::ValueKind kind) const noexcept
    {
        return _linker->types().primitive(kind);
    }

    /// @p type, interned.
    /// @return Its id; or TypeLimit, naming the cap.
    /// @throws std::bad_alloc When the type cannot be stored.
    [[nodiscard]] std::expected<types::TypeId, LinkerError> intern(types::DefinedType type)
    {
        return _linker->types().intern(std::move(type)).transform_error([](types::TypeLimitError error) {
            return LinkerError { .code = LinkerErrorCode::TypeLimit,
                                 .path = {},
                                 .detail =
                                     std::string { LimitTable[static_cast<std::size_t>(error.limit)].name } };
        });
    }

    /// `own<R>`, of the resource type bound to @p R.
    template <typename R>
    [[nodiscard]] std::expected<types::TypeId, LinkerError> own()
    {
        return _linker->ownType(_resources->template get<R>());
    }

    /// `borrow<R>`, of the resource type bound to @p R.
    template <typename R>
    [[nodiscard]] std::expected<types::TypeId, LinkerError> borrow()
    {
        return _linker->borrowType(_resources->template get<R>());
    }

  private:
    Linker* _linker;
    ResourceTypes<Resources...> const* _resources;
};

/// What TypeOf<T>::matches() checks a type against by structure alone: the types of @p arena, every
/// handle by its kind.
class StructuralMatch
{
  public:
    /// @param arena The types; outlive the match.
    explicit StructuralMatch(types::TypeArena const& arena) noexcept: _arena { &arena } {}

    /// The type @p id.
    [[nodiscard]] types::DefinedType const& operator[](types::TypeId id) const { return (*_arena)[id]; }

    /// Every handle matches by its kind.
    template <typename R>
    [[nodiscard]] static bool resourceMatches(types::TypeId) noexcept
    {
        return true;
    }

  private:
    types::TypeArena const* _arena;
};

/// What TypedFunction::bind() checks a function's types against: their structure, and for Own<R> and
/// Borrow<R> of a host resource type bound in @p Resources, that the handle names that resource type in the
/// instance's store. A resource the guest defines, which no HostResourceType stands for, matches by kind.
template <typename... Resources>
class BoundMatch
{
  public:
    /// @param arena The function's types.
    /// @param instance The instance it was found in.
    /// @param function The function.
    /// @param resources The host resource types; all four outlive the match.
    BoundMatch(types::TypeArena const& arena,
               ComponentInstance const& instance,
               ComponentFunction const& function,
               ResourceTypes<Resources...> const& resources) noexcept:
        _arena { &arena }, _instance { &instance }, _function { &function }, _resources { &resources }
    {
    }

    /// The type @p id.
    [[nodiscard]] types::DefinedType const& operator[](types::TypeId id) const { return (*_arena)[id]; }

    /// Whether the handle type @p handle names @p R's host resource type, when @p R is one.
    template <typename R>
    [[nodiscard]] bool resourceMatches(types::TypeId handle) const
    {
        if constexpr (!ResourceTypes<Resources...>::template contains<R>())
            return true;
        else
        {
            auto const named = _instance->resourceTypeOf(*_function, handle);
            return named.has_value() && named == _instance->hostResourceTypeOf(_resources->template get<R>());
        }
    }

  private:
    types::TypeArena const* _arena;
    ComponentInstance const* _instance;
    ComponentFunction const* _function;
    ResourceTypes<Resources...> const* _resources;
};

namespace detail
{
    using types::TypeId;
    using types::ValueKind;

    /// A list's or a map's count as begin() takes it: one past what fits is refused by the builder, as
    /// too many children follow.
    [[nodiscard]] inline uint32_t countOf(std::size_t size) noexcept
    {
        return static_cast<uint32_t>(std::min<std::size_t>(size, std::numeric_limits<uint32_t>::max()));
    }

    /// A scalar's bits as ValueBuilder::scalar() takes them.
    template <typename T>
    [[nodiscard]] constexpr uint64_t bitsOf(T value) noexcept
    {
        if constexpr (std::is_same_v<T, bool>)
            return value ? 1 : 0;
        else if constexpr (std::is_same_v<T, float>)
            return std::bit_cast<uint32_t>(value);
        else if constexpr (std::is_same_v<T, double>)
            return std::bit_cast<uint64_t>(value);
        else if constexpr (std::is_same_v<T, char32_t>)
            return value;
        else
            return static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(value));
    }

    /// Whether @p type's labels are @p labels, in order.
    [[nodiscard]] inline bool labelsAre(types::DefinedType const& type,
                                        std::span<std::string_view const> labels)
    {
        return std::ranges::equal(
            type.labels, labels, std::equal_to {}, [](types::SharedName const& name) -> std::string_view {
                return name.str();
            });
    }

    /// The labels as an arena holds them.
    [[nodiscard]] inline std::vector<types::SharedName> sharedLabels(std::span<std::string_view const> labels)
    {
        auto shared = std::vector<types::SharedName> {};
        shared.reserve(labels.size());
        for (auto const label: labels)
            shared.emplace_back(std::string { label });
        return shared;
    }

    /// The type of a payload: none for Unit.
    template <typename T, typename Context>
    [[nodiscard]] std::expected<std::optional<TypeId>, LinkerError> payloadType(Context& context)
    {
        if constexpr (std::is_same_v<T, Unit>)
            return std::optional<TypeId> {};
        else
            return TypeOf<T>::type(context).transform([](TypeId id) { return std::optional { id }; });
    }

    /// Calls @p visit with every own @p value holds, in order: nothing for a type that holds none.
    template <typename T, typename Held, typename F>
    void forEachOwn(Held& value, F& visit)
    {
        if constexpr (TypeOf<T>::HoldsOwn)
            TypeOf<T>::visitOwns(value, visit);
    }

    /// Whether @p value holds an empty own, which no call or result may pass.
    template <typename T>
    [[nodiscard]] bool holdsEmptyOwn(T const& value)
    {
        auto empty = false;
        auto const visit = [&empty](auto const& own) {
            empty = empty || own.isEmpty();
        };
        forEachOwn<T>(value, visit);
        return empty;
    }

    /// The types of @p Ts, as a DefinedType's children; none for a Unit.
    template <typename... Ts, typename Context>
    [[nodiscard]] std::expected<std::vector<std::optional<TypeId>>, LinkerError> childTypes(Context& context)
    {
        auto children = std::vector<std::optional<TypeId>> {};
        auto failure = std::optional<LinkerError> {};
        [[maybe_unused]] auto const add = [&]<typename T>() {
            if (failure)
                return;
            auto child = payloadType<T>(context);
            if (child)
                children.push_back(*child);
            else
                failure = std::move(child.error());
        };
        (add.template operator()<Ts>(), ...);
        if (failure)
            return std::unexpected { std::move(*failure) };
        return children;
    }

    /// @p type, of @p kind with @p Ts's types as its children and @p labels, interned.
    template <typename... Ts, typename Context>
    [[nodiscard]] std::expected<TypeId, LinkerError> compoundType(
        Context& context, ValueKind kind, std::span<std::string_view const> labels = {})
    {
        return childTypes<Ts...>(context).and_then([&](std::vector<std::optional<TypeId>> children) {
            return context.intern(types::DefinedType {
                .kind = kind, .children = std::move(children), .labels = sharedLabels(labels) });
        });
    }

    /// Whether a payload's type of another arena is @p T's: none for Unit.
    template <typename T, typename Match>
    [[nodiscard]] bool payloadMatches(Match const& arena, std::optional<TypeId> type)
    {
        if constexpr (std::is_same_v<T, Unit>)
            return !type.has_value();
        else
            return type.has_value() && TypeOf<T>::matches(arena, *type);
    }

    /// Whether @p type is of @p kind with @p Ts's types as its children.
    template <typename... Ts, typename Match>
    [[nodiscard]] bool compoundMatches(Match const& arena, TypeId type, ValueKind kind)
    {
        auto const& defined = arena[type];
        if (defined.kind != kind || defined.children.size() != sizeof...(Ts))
            return false;
        [[maybe_unused]] auto child = defined.children.begin();
        return (payloadMatches<Ts>(arena, *child++) && ...);
    }

    /// Writes a payload: nothing for Unit.
    template <typename T>
    void writePayload(canonical::ValueBuilder& builder, T const& value)
    {
        if constexpr (!std::is_same_v<T, Unit>)
            TypeOf<T>::write(builder, value);
    }

    /// Reads the payload of a variant's, an option's or a result's case @p view holds.
    template <typename T>
    [[nodiscard]] std::optional<T> readPayload(canonical::ValueView view)
    {
        auto const payload = view.payload();
        if constexpr (std::is_same_v<T, Unit>)
            return payload ? std::nullopt : std::optional { Unit {} };
        else
            return payload ? TypeOf<T>::read(*payload) : std::nullopt;
    }

    /// Reads @p views' values, one per T of @p Ts, in order; stops at the first that is not of its type.
    template <typename... Ts, typename Views>
    [[nodiscard]] std::optional<std::tuple<Ts...>> readSequence(Views views)
    {
        auto values = std::tuple<std::optional<Ts>...> {};
        auto at = views.begin();
        auto valid = true;
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            // Unused for a function of no parameters, whose fold expands to nothing.
            [[maybe_unused]] auto const readOne = [&]<std::size_t I>() {
                if (!valid)
                    return;
                using T = std::tuple_element_t<I, std::tuple<Ts...>>;
                std::get<I>(values) = TypeOf<T>::read(*at);
                valid = std::get<I>(values).has_value();
                ++at;
            };
            (readOne.template operator()<Index>(), ...);
        }(std::index_sequence_for<Ts...> {});
        if (!valid)
            return std::nullopt;
        return std::apply([](auto&... value) { return std::tuple<Ts...> { std::move(*value)... }; }, values);
    }

    /// The roots of a Values, as a forward range of views.
    class RootRange
    {
      public:
        explicit RootRange(canonical::Values const& values) noexcept: _values { &values } {}

        class Iterator
        {
          public:
            Iterator(canonical::Values const& values, std::size_t index) noexcept:
                _values { &values }, _index { index }
            {
            }
            [[nodiscard]] canonical::ValueView operator*() const { return _values->root(_index); }
            Iterator& operator++() noexcept
            {
                ++_index;
                return *this;
            }

          private:
            canonical::Values const* _values;
            std::size_t _index;
        };

        [[nodiscard]] Iterator begin() const noexcept { return Iterator { *_values, 0 }; }

      private:
        canonical::Values const* _values;
    };

    /// The scalar rows: a C++ arithmetic type and its primitive kind.
    template <typename T, ValueKind Kind>
    struct ScalarRow
    {
        static constexpr bool HoldsOwn = false; ///< Whether its values may hold an own.

        template <typename Context>
        [[nodiscard]] static std::expected<TypeId, LinkerError> type(Context& context)
        {
            return context.primitive(Kind);
        }

        static void write(canonical::ValueBuilder& builder, T const& value) { builder.scalar(bitsOf(value)); }

        [[nodiscard]] static std::optional<T> read(canonical::ValueView view)
        {
            if (view.kind() != Kind)
                return std::nullopt;
            return view.template scalar<T>();
        }

        template <typename Match>
        [[nodiscard]] static bool matches(Match const& arena, TypeId type)
        {
            return arena[type].kind == Kind;
        }
    };

    /// Whether @p T is one of the fundamental integer types (signed char … unsigned long long, whatever
    /// the fixed-width aliases and std::size_t name on a platform), not bool or a character type.
    template <typename T>
    concept IsIntegerRow = std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>
                           && !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t>
                           && !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t> && sizeof(T) <= 8;

    /// The value kind of an integer type: by its width and signedness.
    template <IsIntegerRow T>
    [[nodiscard]] consteval ValueKind integerKind() noexcept
    {
        constexpr auto Signed = std::is_signed_v<T>;
        if constexpr (sizeof(T) == 1)
            return Signed ? ValueKind::S8 : ValueKind::U8;
        else if constexpr (sizeof(T) == 2)
            return Signed ? ValueKind::S16 : ValueKind::U16;
        else if constexpr (sizeof(T) == 4)
            return Signed ? ValueKind::S32 : ValueKind::U32;
        else
            return Signed ? ValueKind::S64 : ValueKind::U64;
    }

    /// The handle rows: Own<R> and Borrow<R>.
    template <typename Handle, typename R, ValueKind Kind>
    struct HandleRow
    {
        static constexpr bool HoldsOwn = Kind == ValueKind::Own;

        /// Calls @p visit with the own @p value is.
        template <typename Held, typename F>
        static void visitOwns(Held& value, F& visit)
        {
            visit(value);
        }

        template <typename Context>
        [[nodiscard]] static std::expected<TypeId, LinkerError> type(Context& context)
        {
            if constexpr (Kind == ValueKind::Own)
                return context.template own<R>();
            else
                return context.template borrow<R>();
        }

        static void write(canonical::ValueBuilder& builder, Handle const& value)
        {
            if constexpr (Kind == ValueKind::Own)
                builder.resource(HandleAccess::value(value));
            else
                builder.resource(value.value());
        }

        [[nodiscard]] static std::optional<Handle> read(canonical::ValueView view)
        {
            if (view.kind() != Kind)
                return std::nullopt;
            if constexpr (Kind == ValueKind::Own)
                return HandleAccess::own<R>(view.resource());
            else
                return HandleAccess::borrow<R>(view.resource());
        }

        template <typename Match>
        [[nodiscard]] static bool matches(Match const& arena, TypeId type)
        {
            return arena[type].kind == Kind && arena.template resourceMatches<R>(type);
        }
    };
} // namespace detail

// clang-format off
template <> struct TypeOf<bool>: detail::ScalarRow<bool, types::ValueKind::Bool> {};
template <detail::IsIntegerRow T> struct TypeOf<T>: detail::ScalarRow<T, detail::integerKind<T>()> {};
template <> struct TypeOf<float>: detail::ScalarRow<float, types::ValueKind::F32> {};
template <> struct TypeOf<double>: detail::ScalarRow<double, types::ValueKind::F64> {};
template <> struct TypeOf<char32_t>: detail::ScalarRow<char32_t, types::ValueKind::Char> {};
template <typename R> struct TypeOf<Own<R>>: detail::HandleRow<Own<R>, R, types::ValueKind::Own> {};
template <typename R> struct TypeOf<Borrow<R>>: detail::HandleRow<Borrow<R>, R, types::ValueKind::Borrow> {};
// clang-format on

/// `string`, as UTF-8.
template <>
struct TypeOf<std::string>
{
    static constexpr bool HoldsOwn = false;

    template <typename Context>
    [[nodiscard]] static std::expected<types::TypeId, LinkerError> type(Context& context)
    {
        return context.primitive(types::ValueKind::String);
    }

    static void write(canonical::ValueBuilder& builder, std::string const& value) { builder.string(value); }

    [[nodiscard]] static std::optional<std::string> read(canonical::ValueView view)
    {
        if (view.kind() != types::ValueKind::String)
            return std::nullopt;
        auto text = view.string().utf8();
        if (!text)
            return std::nullopt;
        return std::move(*text);
    }

    template <typename Match>
    [[nodiscard]] static bool matches(Match const& arena, types::TypeId type)
    {
        return arena[type].kind == types::ValueKind::String;
    }
};

/// `list<T>`.
template <typename T>
struct TypeOf<std::vector<T>>
{
    static constexpr bool HoldsOwn = TypeOf<T>::HoldsOwn;

    template <typename Held, typename F>
    static void visitOwns(Held& value, F& visit)
    {
        for (auto& element: value)
            detail::forEachOwn<T>(element, visit);
    }

    template <typename Context>
    [[nodiscard]] static std::expected<types::TypeId, LinkerError> type(Context& context)
    {
        return detail::compoundType<T>(context, types::ValueKind::List);
    }

    static void write(canonical::ValueBuilder& builder, std::vector<T> const& value)
    {
        builder.begin(detail::countOf(value.size()));
        for (auto const& element: value)
            TypeOf<T>::write(builder, element);
        builder.end();
    }

    [[nodiscard]] static std::optional<std::vector<T>> read(canonical::ValueView view)
    {
        if (view.kind() != types::ValueKind::List)
            return std::nullopt;
        auto elements = std::vector<T> {};
        elements.reserve(view.childCount());
        for (auto const element: view.children())
        {
            auto value = TypeOf<T>::read(element);
            if (!value)
                return std::nullopt;
            elements.push_back(std::move(*value));
        }
        return elements;
    }

    template <typename Match>
    [[nodiscard]] static bool matches(Match const& arena, types::TypeId type)
    {
        return detail::compoundMatches<T>(arena, type, types::ValueKind::List);
    }
};

/// `option<T>`.
template <typename T>
struct TypeOf<std::optional<T>>
{
    static constexpr bool HoldsOwn = TypeOf<T>::HoldsOwn;

    template <typename Held, typename F>
    static void visitOwns(Held& value, F& visit)
    {
        if (value)
            detail::forEachOwn<T>(*value, visit);
    }

    template <typename Context>
    [[nodiscard]] static std::expected<types::TypeId, LinkerError> type(Context& context)
    {
        return detail::compoundType<T>(context, types::ValueKind::Option);
    }

    static void write(canonical::ValueBuilder& builder, std::optional<T> const& value)
    {
        builder.begin(value ? 1 : 0);
        if (value)
            TypeOf<T>::write(builder, *value);
        builder.end();
    }

    [[nodiscard]] static std::optional<std::optional<T>> read(canonical::ValueView view)
    {
        if (view.kind() != types::ValueKind::Option)
            return std::nullopt;
        if (view.caseIndex() == 0)
            return std::optional { std::optional<T> {} };
        auto value = detail::readPayload<T>(view);
        if (!value)
            return std::nullopt;
        return std::optional<std::optional<T>> { std::move(value) };
    }

    template <typename Match>
    [[nodiscard]] static bool matches(Match const& arena, types::TypeId type)
    {
        return detail::compoundMatches<T>(arena, type, types::ValueKind::Option);
    }
};

/// `tuple<T…>`.
template <typename... Ts>
struct TypeOf<std::tuple<Ts...>>
{
    static constexpr bool HoldsOwn = (TypeOf<Ts>::HoldsOwn || ...);

    template <typename Held, typename F>
    static void visitOwns(Held& value, F& visit)
    {
        std::apply([&](auto&... element) { (detail::forEachOwn<Ts>(element, visit), ...); }, value);
    }

    template <typename Context>
    [[nodiscard]] static std::expected<types::TypeId, LinkerError> type(Context& context)
    {
        return detail::compoundType<Ts...>(context, types::ValueKind::Tuple);
    }

    static void write(canonical::ValueBuilder& builder, std::tuple<Ts...> const& value)
    {
        builder.begin(sizeof...(Ts));
        std::apply([&](auto const&... element) { (TypeOf<Ts>::write(builder, element), ...); }, value);
        builder.end();
    }

    [[nodiscard]] static std::optional<std::tuple<Ts...>> read(canonical::ValueView view)
    {
        if (view.kind() != types::ValueKind::Tuple || view.childCount() != sizeof...(Ts))
            return std::nullopt;
        return detail::readSequence<Ts...>(view.children());
    }

    template <typename Match>
    [[nodiscard]] static bool matches(Match const& arena, types::TypeId type)
    {
        return detail::compoundMatches<Ts...>(arena, type, types::ValueKind::Tuple);
    }
};

/// `result<T, E>`; Unit for a case without payload.
template <typename T, typename E>
struct TypeOf<std::expected<T, E>>
{
    static constexpr bool HoldsOwn = TypeOf<T>::HoldsOwn || TypeOf<E>::HoldsOwn;

    template <typename Held, typename F>
    static void visitOwns(Held& value, F& visit)
    {
        if (value)
            detail::forEachOwn<T>(*value, visit);
        else
            detail::forEachOwn<E>(value.error(), visit);
    }

    template <typename Context>
    [[nodiscard]] static std::expected<types::TypeId, LinkerError> type(Context& context)
    {
        return detail::compoundType<T, E>(context, types::ValueKind::Result);
    }

    static void write(canonical::ValueBuilder& builder, std::expected<T, E> const& value)
    {
        builder.begin(value ? 0 : 1);
        if (value)
            detail::writePayload(builder, *value);
        else
            detail::writePayload(builder, value.error());
        builder.end();
    }

    [[nodiscard]] static std::optional<std::expected<T, E>> read(canonical::ValueView view)
    {
        if (view.kind() != types::ValueKind::Result)
            return std::nullopt;
        if (view.caseIndex() == 0)
            return detail::readPayload<T>(view).transform(
                [](T value) { return std::expected<T, E> { std::move(value) }; });
        return detail::readPayload<E>(view).transform(
            [](E error) { return std::expected<T, E> { std::unexpect, std::move(error) }; });
    }

    template <typename Match>
    [[nodiscard]] static bool matches(Match const& arena, types::TypeId type)
    {
        return detail::compoundMatches<T, E>(arena, type, types::ValueKind::Result);
    }
};

/// The absent payload's row: no type of its own, only HoldsOwn for the rows that ask.
template <>
struct TypeOf<Unit>
{
    static constexpr bool HoldsOwn = false;
};

/// `map<K, V>`. A read keeps the last value of a key the guest gives twice (Explainer).
template <typename K, typename V>
struct TypeOf<std::map<K, V>>
{
    static constexpr bool HoldsOwn = TypeOf<V>::HoldsOwn;

    template <typename Held, typename F>
    static void visitOwns(Held& value, F& visit)
    {
        for (auto& entry: value)
            detail::forEachOwn<V>(entry.second, visit);
    }

    template <typename Context>
    [[nodiscard]] static std::expected<types::TypeId, LinkerError> type(Context& context)
    {
        return detail::compoundType<K, V>(context, types::ValueKind::Map);
    }

    static void write(canonical::ValueBuilder& builder, std::map<K, V> const& value)
    {
        builder.begin(detail::countOf(value.size()));
        for (auto const& [key, mapped]: value)
        {
            TypeOf<K>::write(builder, key);
            TypeOf<V>::write(builder, mapped);
        }
        builder.end();
    }

    [[nodiscard]] static std::optional<std::map<K, V>> read(canonical::ValueView view)
    {
        if (view.kind() != types::ValueKind::Map)
            return std::nullopt;
        auto entries = std::map<K, V> {};
        auto children = view.children();
        for (auto at = children.begin(); at != children.end(); ++at)
        {
            auto key = TypeOf<K>::read(*at);
            auto mapped = TypeOf<V>::read(*++at);
            if (!key || !mapped)
                return std::nullopt;
            entries.insert_or_assign(std::move(*key), std::move(*mapped));
        }
        return entries;
    }

    template <typename Match>
    [[nodiscard]] static bool matches(Match const& arena, types::TypeId type)
    {
        return detail::compoundMatches<K, V>(arena, type, types::ValueKind::Map);
    }
};

/// Whether @p T is a record: Record<T> is specialized.
template <typename T>
concept IsRecord = requires { Record<T>::Fields; };

/// Whether @p E is an enum with labels.
template <typename E>
concept IsEnum = std::is_enum_v<E> && requires { Enum<E>::Labels; };

/// Whether @p F is a flags type: labels, and a `uint32_t bits` member.
template <typename F>
concept IsFlags = requires(F const& value) {
    Flags<F>::Labels;
    { value.bits } -> std::convertible_to<uint32_t>;
};

namespace detail
{
    template <typename Fields>
    struct FieldTypes;

    /// The types of a Record's fields, as a tuple.
    template <typename Class, typename... Members>
    struct FieldTypes<std::tuple<FieldRow<Class, Members>...>>
    {
        using Tuple = std::tuple<Members...>;

        static constexpr bool HoldsOwn = (TypeOf<Members>::HoldsOwn || ...);

        template <typename Context>
        [[nodiscard]] static std::expected<TypeId, LinkerError> type(Context& context,
                                                                     std::span<std::string_view const> labels)
        {
            return compoundType<Members...>(context, ValueKind::Record, labels);
        }

        template <typename Match>
        [[nodiscard]] static bool matches(Match const& arena, TypeId type)
        {
            return compoundMatches<Members...>(arena, type, ValueKind::Record);
        }

        [[nodiscard]] static std::optional<Tuple> read(canonical::ValueView view)
        {
            return readSequence<Members...>(view.children());
        }
    };

    /// A record's labels, in its fields' order.
    template <typename T>
    inline constexpr auto RecordLabels = std::apply(
        [](auto const&... fields) {
            return std::array<std::string_view, sizeof...(fields)> { fields.name... };
        },
        Record<T>::Fields);
} // namespace detail

/// A record (Record<T>).
template <IsRecord T>
struct TypeOf<T>
{
    using Fields = detail::FieldTypes<std::remove_cvref_t<decltype(Record<T>::Fields)>>;

    static constexpr bool HoldsOwn = Fields::HoldsOwn;

    template <typename Held, typename F>
    static void visitOwns(Held& value, F& visit)
    {
        std::apply(
            [&](auto const&... fields) {
                (detail::forEachOwn<typename std::remove_cvref_t<decltype(fields)>::Value>(
                     value.*fields.member, visit),
                 ...);
            },
            Record<T>::Fields);
    }

    template <typename Context>
    [[nodiscard]] static std::expected<types::TypeId, LinkerError> type(Context& context)
    {
        return Fields::type(context, detail::RecordLabels<T>);
    }

    static void write(canonical::ValueBuilder& builder, T const& value)
    {
        builder.begin(std::tuple_size_v<typename Fields::Tuple>);
        std::apply(
            [&](auto const&... fields) {
                (TypeOf<typename std::remove_cvref_t<decltype(fields)>::Value>::write(builder,
                                                                                      value.*fields.member),
                 ...);
            },
            Record<T>::Fields);
        builder.end();
    }

    [[nodiscard]] static std::optional<T> read(canonical::ValueView view)
    {
        if (view.kind() != types::ValueKind::Record
            || view.childCount() != std::tuple_size_v<typename Fields::Tuple>)
            return std::nullopt;
        auto values = Fields::read(view);
        if (!values)
            return std::nullopt;
        auto record = T {};
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            ((record.*std::get<Index>(Record<T>::Fields).member = std::move(std::get<Index>(*values))), ...);
        }(std::make_index_sequence<std::tuple_size_v<typename Fields::Tuple>> {});
        return record;
    }

    template <typename Match>
    [[nodiscard]] static bool matches(Match const& arena, types::TypeId type)
    {
        return Fields::matches(arena, type) && detail::labelsAre(arena[type], detail::RecordLabels<T>);
    }
};

/// An enum (Enum<E>): its case is its enumerator's value.
template <IsEnum E>
struct TypeOf<E>
{
    static constexpr bool HoldsOwn = false;

    template <typename Context>
    [[nodiscard]] static std::expected<types::TypeId, LinkerError> type(Context& context)
    {
        return detail::compoundType<>(context, types::ValueKind::Enum, Enum<E>::Labels);
    }

    static void write(canonical::ValueBuilder& builder, E value)
    {
        builder.scalar(static_cast<uint64_t>(std::to_underlying(value)));
    }

    [[nodiscard]] static std::optional<E> read(canonical::ValueView view)
    {
        if (view.kind() != types::ValueKind::Enum)
            return std::nullopt;
        return static_cast<E>(view.caseIndex());
    }

    template <typename Match>
    [[nodiscard]] static bool matches(Match const& arena, types::TypeId type)
    {
        return arena[type].kind == types::ValueKind::Enum && detail::labelsAre(arena[type], Enum<E>::Labels);
    }
};

/// A flags type (Flags<F>).
template <IsFlags F>
struct TypeOf<F>
{
    static_assert(Flags<F>::Labels.size() <= canonical::MaxFlagLabels, "a flags type has at most 32 labels");

    static constexpr bool HoldsOwn = false;

    template <typename Context>
    [[nodiscard]] static std::expected<types::TypeId, LinkerError> type(Context& context)
    {
        return detail::compoundType<>(context, types::ValueKind::Flags, Flags<F>::Labels);
    }

    static void write(canonical::ValueBuilder& builder, F const& value) { builder.scalar(value.bits); }

    [[nodiscard]] static std::optional<F> read(canonical::ValueView view)
    {
        if (view.kind() != types::ValueKind::Flags)
            return std::nullopt;
        return F { view.flagBits() };
    }

    template <typename Match>
    [[nodiscard]] static bool matches(Match const& arena, types::TypeId type)
    {
        return arena[type].kind == types::ValueKind::Flags
               && detail::labelsAre(arena[type], Flags<F>::Labels);
    }
};

/// A variant of Cases (Variant<V>).
template <typename... Cases>
    requires(IsCase<Cases> && ...)
struct TypeOf<std::variant<Cases...>>
{
    using Value = std::variant<Cases...>;

    static constexpr bool HoldsOwn = (TypeOf<typename Cases::Payload>::HoldsOwn || ...);

    template <typename Held, typename F>
    static void visitOwns(Held& value, F& visit)
    {
        std::visit(
            [&](auto& alternative) {
                detail::forEachOwn<typename std::remove_cvref_t<decltype(alternative)>::Payload>(
                    alternative.value, visit);
            },
            value);
    }

    template <typename Context>
    [[nodiscard]] static std::expected<types::TypeId, LinkerError> type(Context& context)
    {
        return detail::compoundType<typename Cases::Payload...>(
            context, types::ValueKind::Variant, Variant<Value>::Labels);
    }

    static void write(canonical::ValueBuilder& builder, Value const& value)
    {
        Require(!value.valueless_by_exception());
        builder.begin(static_cast<uint32_t>(value.index()));
        std::visit(
            [&](auto const& alternative) {
                detail::writePayload<typename std::remove_cvref_t<decltype(alternative)>::Payload>(
                    builder, alternative.value);
            },
            value);
        builder.end();
    }

    [[nodiscard]] static std::optional<Value> read(canonical::ValueView view)
    {
        if (view.kind() != types::ValueKind::Variant)
            return std::nullopt;
        auto const index = view.caseIndex();
        auto value = std::optional<Value> {};
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            auto const readCase = [&]<std::size_t I>() {
                if (index != I)
                    return;
                using Alternative = std::variant_alternative_t<I, Value>;
                auto payload = detail::readPayload<typename Alternative::Payload>(view);
                if (payload)
                    value.emplace(std::in_place_index<I>, Alternative { std::move(*payload) });
            };
            (readCase.template operator()<Index>(), ...);
        }(std::index_sequence_for<Cases...> {});
        return value;
    }

    template <typename Match>
    [[nodiscard]] static bool matches(Match const& arena, types::TypeId type)
    {
        return detail::compoundMatches<typename Cases::Payload...>(arena, type, types::ValueKind::Variant)
               && detail::labelsAre(arena[type], Variant<Value>::Labels);
    }
};

/// Whether @p T is a component value type: TypeOf<T> has a row.
template <typename T>
concept IsValueType = !std::is_same_v<T, Unit> && requires { TypeOf<T>::HoldsOwn; };

namespace detail
{
    /// Whether a call operator's type is declared noexcept.
    template <typename Operator>
    inline constexpr bool IsNoexceptOperator = false;

    // clang-format off
    template <typename R, typename C, typename... A> inline constexpr bool IsNoexceptOperator<R (C::*)(A...) noexcept> = true;
    template <typename R, typename C, typename... A> inline constexpr bool IsNoexceptOperator<R (C::*)(A...) const noexcept> = true;
    template <typename R, typename... A> inline constexpr bool IsNoexceptOperator<R (*)(A...) noexcept> = true;
    // clang-format on

    /// Whether calling @p F is noexcept: its one call operator's declaration says so (the arguments' own
    /// construction aside, which may throw where a standard container's move allocates, as MSVC's
    /// std::map does); else, for a generic or overloaded one, the call with @p Args is noexcept.
    template <typename F, typename... Args>
    [[nodiscard]] consteval bool isNoexceptCall() noexcept
    {
        if constexpr (requires { &F::operator(); })
            return IsNoexceptOperator<decltype(&F::operator())>;
        else
            return std::is_nothrow_invocable_v<F&, Args...>;
    }

    template <typename Signature>
    struct SignatureTraits;

    /// A function type `Result(Params...)`; a void or Unit Result is no result.
    template <typename Result, typename... Params>
    struct SignatureTraits<Result(Params...)>
    {
        static_assert((IsValueType<Params> && ...), "every parameter must be a component value type");
        static_assert(std::is_void_v<Result> || std::is_same_v<Result, Unit> || IsValueType<Result>,
                      "the result must be void, Unit or a component value type");

        static constexpr bool HasResult = !std::is_void_v<Result> && !std::is_same_v<Result, Unit>;

        /// Whether @p X is what a host body may return: the result, or it or a HostFailure.
        template <typename X>
        static constexpr bool IsBodyResult =
            std::is_same_v<X, Result> || std::is_same_v<X, std::expected<Result, isoline::api::HostFailure>>;

        /// Whether @p F, invocable with @p Args, is noexcept and returns what a host body may.
        template <typename F, typename... Args>
        [[nodiscard]] static consteval bool returnsResult() noexcept
        {
            if constexpr (std::is_invocable_v<F&, Args...>)
                return isNoexceptCall<F, Args...>() && IsBodyResult<std::invoke_result_t<F&, Args...>>;
            else
                return false;
        }

        /// Whether @p F is a host body: called with the context first, or without it.
        template <typename F>
        static constexpr bool IsBody =
            returnsResult<F, HostComponentContext&, Params...>() || returnsResult<F, Params...>();

        /// Whether a function type of another arena is this one's, labels aside.
        template <typename Match>
        [[nodiscard]] static bool matches(Match const& arena, types::FunctionTypeInfo const& type)
        {
            if (type.params.size() != sizeof...(Params))
                return false;
            [[maybe_unused]] auto param = type.params.begin();
            if (!(TypeOf<Params>::matches(arena, (param++)->second) && ...))
                return false;
            if constexpr (HasResult)
                return type.result.has_value() && TypeOf<Result>::matches(arena, *type.result);
            else
                return !type.result.has_value();
        }

        /// The function type, in @p linker's arena.
        template <typename... Resources>
        [[nodiscard]] static std::expected<FunctionTypeSpec, LinkerError> spec(
            Linker& linker,
            ResourceTypes<Resources...> const& resources,
            std::span<std::string_view const> names)
        {
            Require(names.empty() || names.size() == sizeof...(Params));
            auto context = TypeContext<Resources...> { linker, resources };
            auto params = childTypes<Params...>(context);
            if (!params)
                return std::unexpected { std::move(params.error()) };
            auto spec = FunctionTypeSpec {};
            for (auto const& param: *params)
            {
                auto const index = spec.params.size();
                spec.params.emplace_back(
                    names.empty() ? std::format("p{}", index) : std::string { names[index] }, *param);
            }
            if constexpr (!HasResult)
                return spec;
            else
            {
                auto result = TypeOf<Result>::type(context);
                if (!result)
                    return std::unexpected { std::move(result.error()) };
                spec.result = *result;
                return spec;
            }
        }
    };

    /// A host function of @p Signature running @p F.
    template <typename Signature, typename F>
    class TypedHostFunction;

    template <typename Result, typename... Params, typename F>
    class TypedHostFunction<Result(Params...), F> final: public IHostComponentFunction
    {
      public:
        using Traits = SignatureTraits<Result(Params...)>;

        explicit TypedHostFunction(F body): _body { std::move(body) } {}

        [[nodiscard]] std::expected<void, isoline::api::HostFailure> call(
            HostComponentContext& context,
            canonical::Values const& args,
            canonical::ValueBuilder& results) noexcept override
        {
            if (args.rootCount() != sizeof...(Params))
                return std::unexpected { argumentMismatch() };
            auto values = readSequence<Params...>(RootRange { args });
            if (!values)
                return std::unexpected { argumentMismatch() };
            return std::apply([&](auto&... value) { return run(context, results, std::move(value)...); },
                              *values);
        }

      private:
        [[nodiscard]] static isoline::api::HostFailure argumentMismatch() noexcept
        {
            return isoline::api::HostFailure { .kind = isoline::api::HostFailureKind::ArgumentMismatch,
                                               .code = 0,
                                               .mismatch = std::nullopt };
        }

        /// Calls the body, with the context when it takes it.
        [[nodiscard]] decltype(auto) invoke(HostComponentContext& context, Params&&... values) noexcept
        {
            if constexpr (Traits::template returnsResult<F, HostComponentContext&, Params...>())
                return std::invoke(_body, context, std::move(values)...);
            else
                return std::invoke(_body, std::move(values)...);
        }

        [[nodiscard]] std::expected<void, isoline::api::HostFailure> run(HostComponentContext& context,
                                                                         canonical::ValueBuilder& results,
                                                                         Params&&... values) noexcept
        {
            // Every branch returns, so that no statement after the chain is dead to MSVC (C4702).
            using Outcome = decltype(invoke(context, std::move(values)...));
            if constexpr (std::is_void_v<Outcome>)
            {
                invoke(context, std::move(values)...);
                return {};
            }
            else if constexpr (!std::is_same_v<Outcome, Result>)
            {
                auto outcome = invoke(context, std::move(values)...);
                if (!outcome)
                    return std::unexpected { outcome.error() };
                if constexpr (Traits::HasResult)
                    return writeResult(results, *outcome);
                else
                    return {};
            }
            else if constexpr (Traits::HasResult)
                return writeResult(results, invoke(context, std::move(values)...));
            else
            {
                std::ignore = invoke(context, std::move(values)...);
                return {};
            }
        }

        /// Writes the body's result; one holding an empty own is no value of its type.
        template <typename T>
        [[nodiscard]] static std::expected<void, isoline::api::HostFailure> writeResult(
            canonical::ValueBuilder& results, T const& value)
        {
            if (holdsEmptyOwn<Result>(value))
                return std::unexpected { isoline::api::HostFailure {
                    .kind = isoline::api::HostFailureKind::ResultMismatch,
                    .code = 0,
                    .mismatch = std::nullopt } };
            TypeOf<Result>::write(results, value);
            return {};
        }

        F _body;
    };
} // namespace detail

/// Whether makeTypedHostFunction<Signature>() takes @p F: a callable declared noexcept, called with the
/// parameters (by value, as rvalues), optionally preceded by the HostComponentContext&, and returning the
/// result type, or it or a HostFailure (std::expected<Result, isoline::api::HostFailure>).
template <typename Signature, typename F>
concept canMakeTypedHostFunction = detail::SignatureTraits<Signature>::template IsBody<F>;

/// A host function of @p Signature, `Result(Params...)`, for Linker::defineFunction() with
/// functionType<Signature>(). Each call reads the arguments as Params (a HostFailure{ArgumentMismatch}
/// when one is not of its type), runs @p function, and writes its result, which the call checks (a
/// HostFailure{ResultMismatch} when it is not of the type). The owns among the arguments are the host's;
/// the owns it returns pass to the guest.
/// @param function The body (canMakeTypedHostFunction).
/// @return The host function.
/// @throws std::bad_alloc When it cannot be stored.
template <typename Signature, typename F>
    requires canMakeTypedHostFunction<Signature, F>
[[nodiscard]] std::shared_ptr<IHostComponentFunction> makeTypedHostFunction(F function)
{
    return std::make_shared<detail::TypedHostFunction<Signature, F>>(std::move(function));
}

/// The function type of @p Signature in @p linker's arena, for Linker::defineFunction().
/// @param linker The linker.
/// @param resources The host resource types its Own and Borrow parameters name.
/// @param names The parameters' labels, kebab-case; or none, for `p0`, `p1`, … (checked: one per
///              parameter when given).
/// @return The type; or TypeLimit when the arena refuses one of its types.
/// @throws std::bad_alloc When the types cannot be stored.
template <typename Signature, typename... Resources>
[[nodiscard]] std::expected<FunctionTypeSpec, LinkerError> functionType(
    Linker& linker,
    ResourceTypes<Resources...> const& resources = {},
    std::span<std::string_view const> names = {})
{
    return detail::SignatureTraits<Signature>::spec(linker, resources, names);
}

/// An own of a host resource, which the host then holds: ComponentStore::hostResource().
/// @param store The store.
/// @param resources The resource types, @p R's among them.
/// @param rep The host's representation of the resource.
/// @return The own; or WrongResource when no instantiation of the store imported @p R's type.
/// @throws std::bad_alloc When the trap's message or the held own cannot be stored.
template <typename R, typename... Resources>
[[nodiscard]] std::expected<Own<R>, canonical::Trap> hostOwn(ComponentStore& store,
                                                             ResourceTypes<Resources...> const& resources,
                                                             uint32_t rep)
{
    return store.hostResource(resources.template get<R>(), rep, canonical::Ownership::Own)
        .transform([](canonical::ResourceValue value) { return detail::HandleAccess::own<R>(value); });
}

/// A borrow of a host resource, to lend to one call: ComponentStore::hostResource().
/// @param store The store.
/// @param resources The resource types, @p R's among them.
/// @param rep The host's representation of the resource.
/// @return The borrow; or WrongResource when no instantiation of the store imported @p R's type.
/// @throws std::bad_alloc When the trap's message cannot be stored.
template <typename R, typename... Resources>
[[nodiscard]] std::expected<Borrow<R>, canonical::Trap> hostBorrow(
    ComponentStore& store, ResourceTypes<Resources...> const& resources, uint32_t rep)
{
    return store.hostResource(resources.template get<R>(), rep, canonical::Ownership::Borrow)
        .transform([](canonical::ResourceValue value) { return detail::HandleAccess::borrow<R>(value); });
}

namespace detail
{
    /// The refusal of an own that is empty: moved from, passed on or dropped already.
    [[nodiscard]] inline ComponentCallError emptyOwn()
    {
        return callError(
            ComponentCallFailure::ArgumentMismatch,
            canonical::ValueError { .code = canonical::ValueErrorCode::TypeMismatch,
                                    .path = {},
                                    .detail = "an empty own: moved from, passed on or dropped already" });
    }

    /// Whether @p A passes an argument of the parameter type @p P: anything for a type that holds no own;
    /// else a non-const rvalue of exactly @p P, which the call may consume.
    template <typename P, typename A>
    inline constexpr bool IsArgumentFor =
        !TypeOf<P>::HoldsOwn
        || (std::is_same_v<std::remove_cvref_t<A>, P> && !std::is_reference_v<A> && !std::is_const_v<A>);
} // namespace detail

/// Drops @p own: ComponentStore::drop(). The own is consumed when the store no longer holds it after the
/// drop (it ran, or a guest destructor failed after taking it), and left as it was when the drop was refused.
/// An empty own is refused before the store sees it, so no own is dropped twice.
/// @param store The store.
/// @param own The own.
/// @return As ComponentStore::drop(); or ArgumentMismatch for an empty own.
template <typename R>
[[nodiscard]] std::expected<void, ComponentCallError> drop(ComponentStore& store, Own<R>& own) noexcept
{
    if (own.isEmpty())
        return std::unexpected { detail::emptyOwn() };
    auto const value = detail::HandleAccess::value(own);
    auto const before = store.hostHolds(value);
    auto dropped = store.drop(value);
    if (store.hostHolds(value) < before)
        detail::HandleAccess::consume(own);
    return dropped;
}

/// A function an instance exports, called with C++ values: ComponentInstance::call() through Values.
template <typename Signature>
class TypedFunction;

/// A function of the type `Result(Params...)`.
template <typename Result, typename... Params>
class TypedFunction<Result(Params...)>
{
  public:
    using Traits = detail::SignatureTraits<Result(Params...)>;

    /// The function @p instance exports at @p path, if it is of the signature: one parameter per Params
    /// and the result, each of its TypeOf row's type, labels of parameters aside. An Own<R> or Borrow<R>
    /// whose R is bound in @p resources must name that host resource type in the store; one of an unbound
    /// R (a resource the guest defines) matches any resource, which each call's handle check still holds
    /// to the function's own.
    /// @param instance The instance; its store must outlive the calls.
    /// @param path As ComponentInstance::function().
    /// @param resources The host resource types the signature's handles name.
    /// @return The typed function; none for no such function, another type, or a store that is gone.
    template <typename... Resources>
    [[nodiscard]] static std::optional<TypedFunction> bind(ComponentInstance& instance,
                                                           std::string_view path,
                                                           ResourceTypes<Resources...> const& resources = {})
    {
        auto found = instance.function(path);
        if (!found || !found->has_value())
            return std::nullopt;
        auto const& function = **found;
        auto const* const type = function.type();
        auto const* const arena = function.types();
        Require(type != nullptr && arena != nullptr);
        if (!Traits::matches(BoundMatch<Resources...> { *arena, instance, function, resources }, *type))
            return std::nullopt;
        return TypedFunction { instance, function };
    }

    /// Calls the function with @p args, each converted to its parameter's type. An argument holding an own
    /// is a non-const rvalue of its parameter's type (anything else does not compile). After the call each
    /// own the store no longer holds is consumed, emptied in the caller's object: all of them after a call
    /// that ran, those its lowering took after one that failed, none after a refusal.
    /// @param args One per parameter.
    /// @return The result (none for a void or Unit result); or how the call failed: as
    ///         ComponentInstance::call(), and ArgumentMismatch when an argument holds an empty own or cannot
    ///         be built (a surrogate char, the live-bytes budget).
    template <typename... Args>
        requires(sizeof...(Args) == sizeof...(Params) && (detail::IsArgumentFor<Params, Args> && ...))
    [[nodiscard]] std::expected<Result, ComponentCallError> operator()(Args&&... args)
    {
        auto arguments = std::forward_as_tuple(std::forward<Args>(args)...);
        auto const anyEmpty = [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            return (detail::holdsEmptyOwn<Params>(std::get<Index>(arguments)) || ...);
        }(std::index_sequence_for<Params...> {});
        if (anyEmpty)
            return std::unexpected { detail::emptyOwn() };
        auto builder = _instance.argumentsFor(_function);
        if (!builder)
            return std::unexpected { std::move(builder.error()) };
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            (writeArgument<Params>(*builder, std::get<Index>(arguments)), ...);
        }(std::index_sequence_for<Params...> {});
        auto built = builder->finish();
        if (!built)
            return std::unexpected { callError(ComponentCallFailure::ArgumentMismatch,
                                               std::move(built.error())) };
        auto called = _instance.call(_function, *built);
        [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            [[maybe_unused]] auto held = std::map<std::pair<uint32_t, uint32_t>, uint32_t> {};
            [[maybe_unused]] auto const consumeTaken = [&](auto& own) {
                auto const& value = detail::HandleAccess::value(own);
                auto const key = std::pair { value.type.value, value.rep };
                auto const [entry, isNew] = held.try_emplace(key, 0);
                if (isNew)
                    entry->second = _instance.hostHolds(value);
                if (entry->second > 0)
                    --entry->second;
                else
                    detail::HandleAccess::consume(own);
            };
            (detail::forEachOwn<Params>(std::get<Index>(arguments), consumeTaken), ...);
        }(std::index_sequence_for<Params...> {});
        if (!called)
            return std::unexpected { std::move(called.error()) };
        if constexpr (Traits::HasResult)
        {
            auto result = TypeOf<Result>::read(called->root(0));
            if (!result)
                // uncovered-begin(bound-result-reads)
                return std::unexpected { callError(
                    ComponentCallFailure::ArgumentMismatch,
                    canonical::ValueError { .code = canonical::ValueErrorCode::TypeMismatch,
                                            .path = "[0]",
                                            .detail = "the result does not read as the bound type" }) };
            // uncovered-end(bound-result-reads)
            return std::move(*result);
        }
        else if constexpr (std::is_void_v<Result>)
            return {};
        else
            return Result {};
    }

  private:
    TypedFunction(ComponentInstance instance, ComponentFunction function) noexcept:
        _instance { std::move(instance) }, _function { std::move(function) }
    {
    }

    template <typename T>
    static void writeArgument(canonical::ValueBuilder& builder, T const& value)
    {
        TypeOf<T>::write(builder, value);
    }

    ComponentInstance _instance;
    ComponentFunction _function;
};

/// The host's own table of the objects its resources stand for: each insert gives a representation (a
/// rep) for hostOwn(), and the object is found by it. A rep holds its slot and the slot's generation, so
/// a rep whose object was erased finds nothing, however its slot is reused. A slot whose generations run
/// out is retired, never reused, so no stale rep ever finds a new object.
template <typename T>
class HostResourceTable
{
  public:
    /// An empty table of at most @p capacity objects (checked: 1 to 2^31).
    explicit HostResourceTable(uint32_t capacity):
        _capacity { capacity }, _slotBits { static_cast<uint32_t>(std::bit_width(capacity - 1)) }
    {
        Require(capacity >= 1 && capacity <= uint32_t { 1 } << 31);
    }

    /// Takes @p value in.
    /// @return Its rep; or a Handles trap when @p capacity objects are held, or their slots retired.
    /// @throws std::bad_alloc When the slot cannot be stored.
    [[nodiscard]] std::expected<uint32_t, canonical::Trap> insert(T value)
    {
        auto slot = uint32_t { 0 };
        if (!_free.empty())
        {
            slot = _free.back();
            _free.pop_back();
        }
        else if (_slots.size() < _capacity)
        {
            slot = static_cast<uint32_t>(_slots.size());
            _slots.emplace_back();
        }
        else
            return std::unexpected { canonical::trapOf(canonical::TrapCode::Handles, _capacity) };
        _slots[slot].value.emplace(std::move(value));
        return repOf(slot);
    }

    /// The object of @p rep: valid until it is erased (an insert moves no object).
    /// @return It; or null for a rep the table did not give, or whose object was erased.
    [[nodiscard]] T* find(uint32_t rep) noexcept
    {
        auto const slot = slotOf(rep);
        if (slot >= _slots.size() || !_slots[slot].value || repOf(slot) != rep)
            return nullptr;
        return &*_slots[slot].value;
    }

    /// Erases the object of @p rep, destroying it; nothing for a rep find() finds nothing for.
    void erase(uint32_t rep) noexcept
    {
        if (find(rep) == nullptr)
            return;
        auto const slot = slotOf(rep);
        auto& entry = _slots[slot];
        entry.value.reset();
        if (entry.generation == maxGeneration())
            return;
        ++entry.generation;
        _free.push_back(slot);
    }

    /// The slot @p rep names.
    [[nodiscard]] uint32_t slotOf(uint32_t rep) const noexcept
    {
        return static_cast<uint32_t>(rep & ((uint64_t { 1 } << _slotBits) - 1));
    }

  private:
    struct Slot
    {
        std::optional<T> value {};
        uint32_t generation = 0;
    };

    [[nodiscard]] uint32_t maxGeneration() const noexcept
    {
        return static_cast<uint32_t>((uint64_t { 1 } << (32 - _slotBits)) - 1);
    }

    [[nodiscard]] uint32_t repOf(uint32_t slot) const noexcept
    {
        return static_cast<uint32_t>((uint64_t { _slots[slot].generation } << _slotBits) | slot);
    }

    uint32_t _capacity;
    uint32_t _slotBits;             ///< The rep's low bits that name the slot: enough for capacity slots.
    std::deque<Slot> _slots {};     ///< A deque: growing it moves no object, so find()'s pointers hold.
    std::vector<uint32_t> _free {}; ///< The slots erased and not retired, reused last-erased first.
};

} // namespace isoline::component::api::typed
