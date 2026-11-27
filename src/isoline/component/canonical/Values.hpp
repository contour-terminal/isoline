// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <isoline/component/Limits.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::canonical
{

/// A resource type at run time: one per execution of a resource definition (D5), so two instances of
/// one component have two. Task 13's ResourceTypeTable mints them; a value only carries one.
struct ResourceTypeIndex
{
    uint32_t value = 0; ///< The index in the store's resource type table.

    /// Indices compare by their value.
    auto operator<=>(ResourceTypeIndex const&) const = default;
};

/// A charge LiveBytesBudget refused: what was asked, and what the budget held when it refused.
struct BudgetExceeded
{
    uint64_t requested = 0; ///< The bytes asked for.
    uint64_t live = 0;      ///< The bytes the budget held.
    uint64_t limit = 0;     ///< The most it holds.

    /// The refusal's text, LimitTable's LiveValueBytes row.
    /// @return "live value bytes exceed the limit of <limit>".
    [[nodiscard]] std::string message() const;

    /// Two refusals are equal when every member is.
    bool operator==(BudgetExceeded const&) const = default;
};

/// The host bytes every live Values of one ComponentStore holds together (C3, spec §7), capped at
/// ComponentLimits::maxLiveValueBytes: a Values charges its storage before allocating it and releases
/// it when freed, so no guest value, however deep or wide, makes the host hold more.
///
/// Store-wide and shared, since a Values may outlive the call that made it. Charges and releases are
/// atomic, so a Values may be destroyed on another thread than the one its store runs on.
class LiveBytesBudget
{
  public:
    /// A budget holding nothing.
    /// @param limit The most bytes it holds at once.
    explicit LiveBytesBudget(uint64_t limit) noexcept: _limit { limit } {}

    LiveBytesBudget(LiveBytesBudget const&) = delete;
    LiveBytesBudget& operator=(LiveBytesBudget const&) = delete;
    LiveBytesBudget(LiveBytesBudget&&) = delete;
    LiveBytesBudget& operator=(LiveBytesBudget&&) = delete;
    ~LiveBytesBudget() = default;

    /// Charges @p bytes, whole or not at all.
    /// @param bytes The bytes about to be allocated.
    /// @return Nothing, or the refusal when the charge would take the budget past its limit; a
    ///         refused charge changes nothing.
    [[nodiscard]] std::expected<void, BudgetExceeded> charge(uint64_t bytes) noexcept;

    /// Charges as many whole units as the budget has left, up to @p most bytes, and at least @p least:
    /// one atomic decision, so a growth sized by it never fails on headroom another thread took
    /// meanwhile, and never holds a partial unit's bytes.
    /// @param least The fewest bytes that will do; more than @p most, nothing will.
    /// @param most The bytes wanted.
    /// @param unit The granule of the charge, an element's size; @p least and @p most are multiples of
    ///        it (checked).
    /// @return The bytes charged, a multiple of @p unit from @p least to @p most; or the refusal when
    ///         fewer than @p least are left, which changes nothing.
    [[nodiscard]] std::expected<uint64_t, BudgetExceeded> chargeWithin(uint64_t least,
                                                                       uint64_t most,
                                                                       uint64_t unit) noexcept;

    /// Releases bytes charged before.
    /// @param bytes At most what is live (checked).
    void release(uint64_t bytes) noexcept;

    /// The bytes charged and not released.
    /// @return The live bytes.
    [[nodiscard]] uint64_t live() const noexcept { return _live.load(std::memory_order_relaxed); }

    /// The most bytes the budget holds.
    /// @return The limit it was made with.
    [[nodiscard]] uint64_t limit() const noexcept { return _limit; }

    /// The bytes a charge may take now: a hint for sizing a charge, which charge() decides.
    /// @return limit() minus live().
    [[nodiscard]] uint64_t available() const noexcept { return _limit - live(); }

  private:
    uint64_t _limit;
    std::atomic<uint64_t> _live { 0 };
};

/// How a growth for the caller to fill in place starts out.
enum class GrowthFill : uint8_t
{
    Zeroed,       ///< Zero bytes: what a host gets, so a byte it leaves unwritten is never stale memory.
    ForOverwrite, ///< Indeterminate (poisoned in debug builds): only for the runtime's own copies, which
                  ///< overwrite every byte.
};

/// Whether this build poisons a GrowthFill::ForOverwrite growth with 0xFF bytes, so that a byte left
/// unwritten shows (0xFF is never UTF-8): in debug builds, where NDEBUG is not defined.
enum class OverwritePoison : uint8_t
{
    Off, ///< A release build: the bytes are left as the allocation found them.
    On,  ///< A debug build: every byte is 0xFF until overwritten.
};

#ifdef NDEBUG
inline constexpr auto ThisBuildPoisons = OverwritePoison::Off; ///< This build's choice.
#else
inline constexpr auto ThisBuildPoisons = OverwritePoison::On; ///< This build's choice.
#endif

/// The allocator of ChargedVector's storage: std::allocator, except that a growth that gives no value
/// (resize()) default-initializes, so the bytes of a trivially constructible element are left for their
/// caller to overwrite rather than zeroed first: the resize-for-overwrite std::vector lacks.
template <typename T>
class OverwriteAllocator: public std::allocator<T>
{
  public:
    OverwriteAllocator() noexcept = default;

    /// The allocator of another element type, as std::allocator_traits rebinds it.
    template <typename U>
    explicit OverwriteAllocator(OverwriteAllocator<U> const& /*other*/) noexcept
    {
    }

    /// Constructs a @p U at @p place: default-initialized without arguments, from them otherwise.
    template <typename U, typename... Args>
    void construct(U* place, Args&&... args)
    {
        if constexpr (sizeof...(Args) == 0)
            ::new (static_cast<void*>(place)) U;
        else
            std::construct_at(place, std::forward<Args>(args)...);
    }
};

/// A vector whose capacity is charged to a LiveBytesBudget before it is allocated, and released when
/// it is freed: what Values hold their storage in, so their charge is exact on every path, a failed
/// build and an unwinding exception included.
///
/// It grows geometrically, so filling it costs amortized O(1) per element, but never by more than the
/// budget has left: a growth the budget cannot afford in full is made as large as the headroom allows,
/// at least as large as needed, and refused when even that does not fit. One atomic charge decides it
/// (LiveBytesBudget::chargeWithin), whatever other threads charge meanwhile. While it grows, the old and
/// the new storage are both charged, as both are allocated.
template <typename T>
class ChargedVector
{
  public:
    /// An empty vector, charging @p budget.
    /// @param budget The budget its storage is charged to.
    explicit ChargedVector(std::shared_ptr<LiveBytesBudget> budget) noexcept: _budget { std::move(budget) } {}

    ChargedVector(ChargedVector const&) = delete;
    ChargedVector& operator=(ChargedVector const&) = delete;

    /// Takes over @p other's storage and charge; @p other is left empty, charging nothing.
    ChargedVector(ChargedVector&& other) noexcept:
        _budget { std::move(other._budget) },
        _items { std::move(other._items) },
        _capacity { std::exchange(other._capacity, 0) }
    {
    }

    /// Releases the storage held, then takes over @p other's storage and charge.
    ChargedVector& operator=(ChargedVector&& other) noexcept
    {
        if (this != &other)
        {
            releaseCharge();
            _budget = std::move(other._budget);
            _items = std::move(other._items);
            _capacity = std::exchange(other._capacity, 0);
        }
        return *this;
    }

    /// Frees the storage and releases its charge.
    ~ChargedVector() { releaseCharge(); }

    /// Makes room for @p extra more elements, charging the budget first.
    /// @param extra The elements about to be appended.
    /// @return Nothing, or the budget's refusal, which leaves the vector and the budget unchanged.
    /// @throws std::bad_alloc, std::length_error When the host cannot allocate what the budget allowed;
    ///         the charge is released before the exception leaves.
    [[nodiscard]] std::expected<void, BudgetExceeded> reserveFor(std::size_t extra)
    {
        constexpr auto Unit = uint64_t { sizeof(T) };
        constexpr auto MostElements = std::numeric_limits<uint64_t>::max() / Unit;
        auto const size = uint64_t { _items.size() };
        if (extra <= _capacity - size)
            return {};
        if (extra > MostElements - size)
            return std::unexpected { BudgetExceeded { .requested = std::numeric_limits<uint64_t>::max(),
                                                      .live = _budget->live(),
                                                      .limit = _budget->limit() } };
        auto const needed = size + extra;
        auto const doubled = _capacity > MostElements / 2 ? MostElements : _capacity * 2;
        // One charge decides the growth against the headroom it sees: as much of the doubled capacity as
        // the budget has left, never less than needed. Whatever another thread charges, the growth fits.
        auto const granted = _budget->chargeWithin(needed * Unit, std::max(needed, doubled) * Unit, Unit);
        if (!granted)
            return std::unexpected { granted.error() };
        auto const bytes = *granted;
        auto const capacity = bytes / Unit;
        try
        {
            _items.reserve(static_cast<std::size_t>(capacity));
        }
        catch (...)
        {
            _budget->release(bytes);
            throw;
        }
        _budget->release(_capacity * Unit);
        _capacity = capacity;
        return {};
    }

    /// Appends an element into room reserveFor() made (checked), so that it never allocates.
    /// @param item The element.
    void append(T const& item)
    {
        Require(_items.size() < _capacity);
        _items.push_back(item);
    }

    /// Appends elements into room reserveFor() made (checked).
    /// @param items The elements.
    void append(std::span<T const> items)
    {
        Require(items.size() <= _capacity - _items.size());
        _items.insert(_items.end(), items.begin(), items.end());
    }

    /// Appends @p count elements into room reserveFor() made (checked), for the caller to fill in place.
    /// @param count The elements.
    /// @param fill Zeroed (value-initialized), or ForOverwrite: default-initialized, so a trivially
    ///             constructible element's bytes are indeterminate (0xFF in a debug build) until the
    ///             caller overwrites them, and no pass zeroes what a copy overwrites.
    /// @return They, valid until the next growth.
    [[nodiscard]] std::span<T> grow(std::size_t count, GrowthFill fill)
    {
        Require(count <= _capacity - _items.size());
        if (fill == GrowthFill::Zeroed)
            _items.resize(_items.size() + count, T {});
        else
            _items.resize(_items.size() + count);
        auto const grown = std::span { _items }.last(count);
        if (fill == GrowthFill::ForOverwrite && ThisBuildPoisons == OverwritePoison::On)
            std::ranges::fill(std::as_writable_bytes(grown), std::byte { 0xFF });
        return grown;
    }

    /// The number of elements.
    /// @return The size.
    [[nodiscard]] std::size_t size() const noexcept { return _items.size(); }

    /// An element.
    /// @param index Below size() (checked).
    /// @return It.
    [[nodiscard]] T& operator[](std::size_t index)
    {
        Require(index < _items.size());
        return _items[index];
    }

    /// An element.
    /// @param index Below size() (checked).
    /// @return It.
    [[nodiscard]] T const& operator[](std::size_t index) const
    {
        Require(index < _items.size());
        return _items[index];
    }

    /// Every element.
    /// @return They, valid until the next growth.
    [[nodiscard]] std::span<T const> items() const noexcept { return _items; }

    /// The bytes charged for the storage.
    /// @return The charged capacity times the element size.
    [[nodiscard]] uint64_t chargedBytes() const noexcept { return _capacity * uint64_t { sizeof(T) }; }

  private:
    void releaseCharge() noexcept
    {
        if (_budget)
            _budget->release(chargedBytes());
        _capacity = 0;
    }

    std::shared_ptr<LiveBytesBudget> _budget;
    std::vector<T, OverwriteAllocator<T>> _items {};
    uint64_t _capacity = 0; ///< The capacity charged, which reserveFor() reserved.
};

/// The encoding a string's bytes are in (D4): its source's, kept until it is lowered.
enum class StringEncoding : uint8_t
{
    Utf8,        ///< UTF-8.
    Utf16,       ///< UTF-16, little-endian.
    Latin1Utf16, ///< Latin-1, or UTF-16 when taggedCodeUnits carries Utf16Tag.
};

/// The bit of a latin1+utf16 string's tagged code units that marks it UTF-16 (definitions.py's
/// UTF16_TAG).
inline constexpr auto Utf16Tag = uint32_t { 1 } << 31U;

/// The encoding a string's bytes are in once a latin1+utf16 string's tag is read (definitions.py's
/// src_simple_encoding).
enum class SimpleEncoding : uint8_t
{
    Utf8,   ///< UTF-8.
    Utf16,  ///< UTF-16, little-endian.
    Latin1, ///< Latin-1: one byte per code point, U+0000 to U+00FF.
};

/// One row of SimpleEncodingTable: an encoding, its code unit, and its name.
struct SimpleEncodingRow
{
    SimpleEncoding encoding; ///< The encoding; equal to the row's index.
    uint8_t codeUnitBytes;   ///< The bytes of one code unit.
    std::string_view name;   ///< Its name, as traps spell it.
};

/// Every simple encoding, one row per SimpleEncoding enumerator, in enumerator order.
inline constexpr auto SimpleEncodingTable = std::to_array<SimpleEncodingRow>({
    { .encoding = SimpleEncoding::Utf8, .codeUnitBytes = 1, .name = "utf-8" },
    { .encoding = SimpleEncoding::Utf16, .codeUnitBytes = 2, .name = "utf-16" },
    { .encoding = SimpleEncoding::Latin1, .codeUnitBytes = 1, .name = "latin-1" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: SimpleEncodingTable)
        {
            if (static_cast<std::size_t>(row.encoding) != index)
                return false;
            ++index;
        }
        return true;
    }(),
    "SimpleEncodingTable must hold one row per SimpleEncoding, at the index of its enumerator");

/// The row of @p encoding.
/// @param encoding A simple encoding.
/// @return Its SimpleEncodingTable row.
[[nodiscard]] constexpr SimpleEncodingRow const& simpleEncodingRow(SimpleEncoding encoding) noexcept
{
    return SimpleEncodingTable[static_cast<std::size_t>(encoding)];
}

/// The simple encoding of a string in @p encoding whose tagged code units are @p taggedCodeUnits.
/// @param encoding The string's encoding.
/// @param taggedCodeUnits Its tagged code units: Utf16Tag selects UTF-16 for latin1+utf16.
/// @return Its simple encoding.
[[nodiscard]] constexpr SimpleEncoding simpleEncodingOf(StringEncoding encoding,
                                                        uint32_t taggedCodeUnits) noexcept
{
    switch (encoding)
    {
        case StringEncoding::Utf8: return SimpleEncoding::Utf8;
        case StringEncoding::Utf16: return SimpleEncoding::Utf16;
        case StringEncoding::Latin1Utf16: break;
    }
    return (taggedCodeUnits & Utf16Tag) != 0 ? SimpleEncoding::Utf16 : SimpleEncoding::Latin1;
}

/// The code units of a string in @p encoding whose tagged code units are @p taggedCodeUnits: without
/// the tag, for latin1+utf16.
/// @param encoding The string's encoding.
/// @param taggedCodeUnits Its tagged code units.
/// @return Its code units.
[[nodiscard]] constexpr uint32_t codeUnitsOf(StringEncoding encoding, uint32_t taggedCodeUnits) noexcept
{
    return encoding == StringEncoding::Latin1Utf16 ? taggedCodeUnits & ~Utf16Tag : taggedCodeUnits;
}

/// The bytes of a string in @p encoding whose tagged code units are @p taggedCodeUnits (definitions.py's
/// byte_length in load_string_from_range), in 64-bit arithmetic: up to 2^33 - 2.
/// @param encoding The string's encoding.
/// @param taggedCodeUnits Its tagged code units.
/// @return Its byte length.
[[nodiscard]] constexpr uint64_t stringByteLength(StringEncoding encoding, uint32_t taggedCodeUnits) noexcept
{
    auto const& row = simpleEncodingRow(simpleEncodingOf(encoding, taggedCodeUnits));
    return uint64_t { codeUnitsOf(encoding, taggedCodeUnits) } * row.codeUnitBytes;
}

/// Why a string's bytes are not text in their encoding.
enum class StringError : uint8_t
{
    InvalidUtf8,  ///< Not well-formed UTF-8 (RFC 3629).
    InvalidUtf16, ///< An odd byte count, or a lone surrogate.
};

/// A string value as definitions.py holds it: its bytes in their encoding, and its tagged code units.
struct StringValue
{
    std::span<std::byte const> bytes;               ///< The bytes, valid while their Values lives.
    StringEncoding encoding = StringEncoding::Utf8; ///< Their encoding.
    /// The code units in that encoding (bytes for UTF-8 and Latin-1, 16-bit units for UTF-16), with
    /// Utf16Tag set for a latin1+utf16 string in UTF-16.
    uint32_t taggedCodeUnits = 0;

    /// The string in UTF-8.
    /// @return Its text, or why the bytes are not text in their encoding.
    /// @throws std::bad_alloc When the text cannot be stored.
    [[nodiscard]] std::expected<std::string, StringError> utf8() const;

    /// The encoding its bytes are in, the tag read.
    /// @return Its simple encoding.
    [[nodiscard]] SimpleEncoding simpleEncoding() const noexcept
    {
        return simpleEncodingOf(encoding, taggedCodeUnits);
    }
};

/// Whether a handle value owns its resource or borrows it.
enum class Ownership : uint8_t
{
    Own,    ///< An `own`.
    Borrow, ///< A `borrow`.
};

/// A handle value: a resource of a runtime resource type, by its representation.
struct ResourceValue
{
    ResourceTypeIndex type {};            ///< The resource's runtime type.
    uint32_t rep = 0;                     ///< Its representation.
    Ownership ownership = Ownership::Own; ///< Whether the value owns it.

    /// Two handle values are equal when every member is.
    bool operator==(ResourceValue const&) const = default;
};

/// How a value of a kind is built and stored.
enum class ValueShape : uint8_t
{
    Scalar,      ///< A number, bool or char: its bits, in the node or packed in the pool.
    String,      ///< Bytes in the pool.
    Fields,      ///< A record or a tuple: one child per field.
    List,        ///< Any number of elements: nodes, or packed in the pool when they are scalars.
    FixedList,   ///< A list of the type's length.
    Cases,       ///< A variant, option or result: a case and at most one payload child.
    Enum,        ///< A case without payload.
    Flags,       ///< A bitmask of its labels.
    Map,         ///< Entries: a key child, then a value child, per entry.
    Handle,      ///< An own or a borrow.
    Unsupported, ///< A stream, a future or an error context: no value before P12.
};

/// How a scalar's bits are read.
enum class ScalarRule : uint8_t
{
    None,     ///< Not a scalar.
    Unsigned, ///< An unsigned integer of its width.
    Signed,   ///< A two's-complement integer of its width.
    Float,    ///< An IEEE 754 binary float of its width.
    Bool,     ///< 0 or 1.
    Char,     ///< A Unicode scalar value: below 0x110000 and not a surrogate.
};

/// One row of ValueShapeTable: a kind, how its values are built and stored, and its wast form.
struct ValueShapeRow
{
    types::ValueKind kind; ///< The kind; equal to the row's index.
    ValueShape shape;      ///< How its values are built and stored.
    ScalarRule rule;       ///< For a scalar, how its bits read; else None.
    uint8_t bytes;         ///< For a scalar, its width in bytes (packed in a list); else 0.
    /// For Cases, the first case with a payload: an option's `none` has none, so its case c holds
    /// child c - 1; a variant's and a result's case c holds child c.
    uint8_t firstPayloadCase;
    std::string_view form; ///< The wast value form's keyword; for an option or a result, see CaseForms.
};

/// Every value kind's shape, one row per ValueKind enumerator, in enumerator order.
inline constexpr auto ValueShapeTable = std::to_array<ValueShapeRow>({
    // clang-format off
    { .kind = types::ValueKind::Bool, .shape = ValueShape::Scalar, .rule = ScalarRule::Bool, .bytes = 1, .firstPayloadCase = 0, .form = "bool.const" },
    { .kind = types::ValueKind::S8, .shape = ValueShape::Scalar, .rule = ScalarRule::Signed, .bytes = 1, .firstPayloadCase = 0, .form = "s8.const" },
    { .kind = types::ValueKind::U8, .shape = ValueShape::Scalar, .rule = ScalarRule::Unsigned, .bytes = 1, .firstPayloadCase = 0, .form = "u8.const" },
    { .kind = types::ValueKind::S16, .shape = ValueShape::Scalar, .rule = ScalarRule::Signed, .bytes = 2, .firstPayloadCase = 0, .form = "s16.const" },
    { .kind = types::ValueKind::U16, .shape = ValueShape::Scalar, .rule = ScalarRule::Unsigned, .bytes = 2, .firstPayloadCase = 0, .form = "u16.const" },
    { .kind = types::ValueKind::S32, .shape = ValueShape::Scalar, .rule = ScalarRule::Signed, .bytes = 4, .firstPayloadCase = 0, .form = "s32.const" },
    { .kind = types::ValueKind::U32, .shape = ValueShape::Scalar, .rule = ScalarRule::Unsigned, .bytes = 4, .firstPayloadCase = 0, .form = "u32.const" },
    { .kind = types::ValueKind::S64, .shape = ValueShape::Scalar, .rule = ScalarRule::Signed, .bytes = 8, .firstPayloadCase = 0, .form = "s64.const" },
    { .kind = types::ValueKind::U64, .shape = ValueShape::Scalar, .rule = ScalarRule::Unsigned, .bytes = 8, .firstPayloadCase = 0, .form = "u64.const" },
    { .kind = types::ValueKind::F32, .shape = ValueShape::Scalar, .rule = ScalarRule::Float, .bytes = 4, .firstPayloadCase = 0, .form = "f32.const" },
    { .kind = types::ValueKind::F64, .shape = ValueShape::Scalar, .rule = ScalarRule::Float, .bytes = 8, .firstPayloadCase = 0, .form = "f64.const" },
    { .kind = types::ValueKind::Char, .shape = ValueShape::Scalar, .rule = ScalarRule::Char, .bytes = 4, .firstPayloadCase = 0, .form = "char.const" },
    { .kind = types::ValueKind::String, .shape = ValueShape::String, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "str.const" },
    { .kind = types::ValueKind::ErrorContext, .shape = ValueShape::Unsupported, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = {} },
    { .kind = types::ValueKind::List, .shape = ValueShape::List, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "list.const" },
    { .kind = types::ValueKind::FixedList, .shape = ValueShape::FixedList, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "list.const" },
    { .kind = types::ValueKind::Record, .shape = ValueShape::Fields, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "record.const" },
    { .kind = types::ValueKind::Tuple, .shape = ValueShape::Fields, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "tuple.const" },
    { .kind = types::ValueKind::Variant, .shape = ValueShape::Cases, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "variant.const" },
    { .kind = types::ValueKind::Enum, .shape = ValueShape::Enum, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "enum.const" },
    { .kind = types::ValueKind::Option, .shape = ValueShape::Cases, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 1, .form = {} },
    { .kind = types::ValueKind::Result, .shape = ValueShape::Cases, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = {} },
    { .kind = types::ValueKind::Flags, .shape = ValueShape::Flags, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "flags.const" },
    { .kind = types::ValueKind::Map, .shape = ValueShape::Map, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "map.const" },
    { .kind = types::ValueKind::Own, .shape = ValueShape::Handle, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "own" },
    { .kind = types::ValueKind::Borrow, .shape = ValueShape::Handle, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = "borrow" },
    { .kind = types::ValueKind::Stream, .shape = ValueShape::Unsupported, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = {} },
    { .kind = types::ValueKind::Future, .shape = ValueShape::Unsupported, .rule = ScalarRule::None, .bytes = 0, .firstPayloadCase = 0, .form = {} },
    // clang-format on
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ValueShapeTable)
        {
            if (static_cast<std::size_t>(row.kind) != index)
                return false;
            ++index;
        }
        return index == types::ValueKindCount;
    }(),
    "ValueShapeTable must hold one row per ValueKind, at the index of its enumerator");

/// The shape row of @p kind.
/// @param kind A value kind.
/// @return Its row in ValueShapeTable.
[[nodiscard]] constexpr ValueShapeRow const& valueShapeRow(types::ValueKind kind) noexcept
{
    return ValueShapeTable[static_cast<std::size_t>(kind)];
}

/// One row of CaseForms: the wast form of one case of an option or a result.
struct CaseFormRow
{
    types::ValueKind kind; ///< Option or Result.
    uint32_t caseIndex;    ///< The case.
    std::string_view form; ///< Its keyword.
};

/// The wast forms of the option and result cases (wast 263.0.0, src/component/wast.rs).
inline constexpr auto CaseForms = std::to_array<CaseFormRow>({
    { .kind = types::ValueKind::Option, .caseIndex = 0, .form = "option.none" },
    { .kind = types::ValueKind::Option, .caseIndex = 1, .form = "option.some" },
    { .kind = types::ValueKind::Result, .caseIndex = 0, .form = "result.ok" },
    { .kind = types::ValueKind::Result, .caseIndex = 1, .form = "result.err" },
});

/// One value of a Values, in pre-order: a node and its descendants are subtreeSize consecutive nodes.
///
/// What payload holds, by ValueShape: a scalar's bits at its width, zero-extended (a float's bit
/// pattern); a string's or a list's ValueRange index; a record's or a tuple's field count; a case
/// index; a flags bitmask; a map's entry count; a handle's ResourceTypeIndex in the high and its
/// representation in the low 32 bits.
struct ValueNode
{
    types::TypeId type {};                          ///< Its type, in its Values' arena.
    types::ValueKind kind = types::ValueKind::Bool; ///< Its type's kind.
    uint64_t payload = 0;                           ///< What its shape holds (see above).
    uint32_t subtreeSize = 1;                       ///< It and its descendants.

    /// Two nodes are equal when every member is.
    bool operator==(ValueNode const&) const = default;
};

/// Where a list keeps its elements.
enum class ElementStorage : uint8_t
{
    Nodes, ///< As child nodes.
    Pool,  ///< Packed in the pool at their width: a list of scalars, one pool range however long.
};

/// A range of a Values' pool, or a list's element count: what a string or a list node's payload names.
struct ValueRange
{
    uint64_t offset = 0;                                   ///< Its first pool byte.
    uint32_t count = 0;                                    ///< A string's bytes; a list's elements.
    uint32_t taggedCodeUnits = 0;                          ///< A string's tagged code units.
    types::TypeId element {};                              ///< A list's element type.
    types::ValueKind elementKind = types::ValueKind::Bool; ///< Its kind.
    StringEncoding encoding = StringEncoding::Utf8;        ///< A string's encoding.
    ElementStorage storage = ElementStorage::Nodes;        ///< Where a list keeps its elements.
};

/// Why a ValueBuilder refused a step.
enum class ValueErrorCode : uint8_t
{
    TypeMismatch, ///< The step builds a value of another type than the one expected there.
    InvalidValue, ///< The value is outside its type: a bool of 2, a surrogate char, a case past the last.
    Unbalanced,   ///< begin() and end() do not pair, or a compound or the roots have too many or too few
                  ///< values.
    Budget,       ///< The live-bytes budget refused the storage.
    Unsupported,  ///< A stream, future or error-context value, which P11 does not build.
    TooLarge,     ///< More than 2^32 - 1 nodes, or a string of 2^32 bytes or more.
    TooManyFlags, ///< A flags value of a type of more than MaxFlagLabels labels, which no 32-bit
                  ///< flags value holds: validation refuses such a type, but an embedder may intern one.
};

/// One row of ValueErrorMessages: an error and its text, a format of its path and its detail.
struct ValueErrorRow
{
    ValueErrorCode code;     ///< The error; equal to the row's index.
    std::string_view format; ///< Its message, a std::format string of the path and the detail.
};

/// Every value error, one row per ValueErrorCode enumerator, in enumerator order.
inline constexpr auto ValueErrorMessages = std::to_array<ValueErrorRow>({
    { .code = ValueErrorCode::TypeMismatch, .format = "type mismatch at {}: {}" },
    { .code = ValueErrorCode::InvalidValue, .format = "invalid value at {}: {}" },
    { .code = ValueErrorCode::Unbalanced, .format = "unbalanced value at {}: {}" },
    { .code = ValueErrorCode::Budget, .format = "value at {} refused: {}" },
    { .code = ValueErrorCode::Unsupported, .format = "unsupported value at {}: {}" },
    { .code = ValueErrorCode::TooLarge, .format = "value too large at {}: {}" },
    { .code = ValueErrorCode::TooManyFlags, .format = "too many flags at {}: {}" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ValueErrorMessages)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return true;
    }(),
    "ValueErrorMessages must hold one row per ValueErrorCode, at the index of its enumerator");

/// A step a ValueBuilder refused, and where.
struct ValueError
{
    ValueErrorCode code = ValueErrorCode::TypeMismatch; ///< Why.
    /// Where: the root's index, then one index per enclosing compound, each in brackets ("[0][2]"): a
    /// field's, an element's or a map's child position, or a variant's, option's or result's case.
    std::string path;
    std::string detail; ///< What was wrong there.

    /// The error's text.
    /// @return Its ValueErrorMessages row, formatted with the path and the detail.
    [[nodiscard]] std::string message() const;

    /// Two errors are equal when every member is.
    bool operator==(ValueError const&) const = default;
};

class Values;
class ChildIterator;
class ChildRange;
class LiftSteps;

/// Whether a ValueView names a node or an element packed in a list's pool range.
enum class ViewForm : uint8_t
{
    Node,    ///< A node.
    Element, ///< An element of a list whose elements are packed (ElementStorage::Pool).
};

/// One value of a Values, by position: cheap to copy, valid while its Values lives and is not moved.
class ValueView
{
  public:
    /// The value's kind.
    /// @return Its type's kind.
    [[nodiscard]] types::ValueKind kind() const noexcept;

    /// The value's type.
    /// @return Its TypeId, in arena().
    [[nodiscard]] types::TypeId type() const noexcept;

    /// The arena its type belongs to.
    /// @return Its Values' arena.
    [[nodiscard]] types::TypeArenaId arena() const noexcept;

    /// A scalar's value as @p T: bool for bool, int8_t … uint64_t for the integers of their width and
    /// signedness, float and double for f32 and f64, char32_t for char (checked).
    /// @return Its value.
    template <typename T>
    [[nodiscard]] T scalar() const;

    /// A scalar's bits, at its width and zero-extended; a float's bit pattern.
    /// @return Its bits (checked: a scalar).
    [[nodiscard]] uint64_t scalarBits() const;

    /// A string's bytes and encoding.
    /// @return It (checked: a string).
    [[nodiscard]] StringValue string() const;

    /// The number of children: a record's or tuple's fields, a list's elements, a map's keys and values
    /// (two per entry, key first), and one for a variant, option or result with a payload.
    /// @return The count; 0 for every other value.
    [[nodiscard]] std::size_t childCount() const noexcept;

    /// The children, in order: O(1) per step, so walking every value of a Values is O(n).
    /// @return A forward range of them.
    [[nodiscard]] ChildRange children() const;

    /// One child. O(1) for a list of scalars, O(index) otherwise, as the children before it are
    /// skipped; children() is the walk to use.
    /// @param index Below childCount() (checked).
    /// @return It.
    [[nodiscard]] ValueView child(std::size_t index) const;

    /// A variant's, an option's, a result's or an enum's case.
    /// @return The case index (checked).
    [[nodiscard]] uint32_t caseIndex() const;

    /// A variant's, an option's or a result's payload.
    /// @return It, or empty for a case without one (checked: a variant, option or result).
    [[nodiscard]] std::optional<ValueView> payload() const;

    /// A flags value's bitmask, bit i for label i.
    /// @return It (checked).
    [[nodiscard]] uint32_t flagBits() const;

    /// A handle.
    /// @return It (checked: an own or a borrow).
    [[nodiscard]] ResourceValue resource() const;

  private:
    friend class Values;
    friend class ChildIterator;

    ValueView(Values const& values, uint32_t node, uint32_t element, ViewForm form) noexcept:
        _values { &values }, _node { node }, _element { element }, _form { form }
    {
    }

    [[nodiscard]] ValueNode const& node() const;
    [[nodiscard]] ValueRange const& range() const;
    [[nodiscard]] ValueShapeRow const& shapeRow() const noexcept { return valueShapeRow(kind()); }

    Values const* _values;
    uint32_t _node;    ///< The node, or the list holding the element.
    uint32_t _element; ///< For ViewForm::Element, the element's index; else 0.
    ViewForm _form;
};

/// A forward iterator over a value's children.
class ChildIterator
{
  public:
    /// The child it is at.
    /// @return Its view.
    [[nodiscard]] ValueView operator*() const noexcept;

    /// Steps to the next child, past every descendant of this one: O(1).
    /// @return The iterator.
    ChildIterator& operator++() noexcept;

    /// Iterators are equal at the same child of the same value.
    bool operator==(ChildIterator const&) const = default;

  private:
    friend class ValueView;

    ChildIterator(Values const& values, uint32_t node, uint32_t element, ViewForm form) noexcept:
        _values { &values }, _node { node }, _element { element }, _form { form }
    {
    }

    Values const* _values;
    uint32_t _node;    ///< The child node; for packed elements, the list.
    uint32_t _element; ///< For packed elements, the element's index.
    ViewForm _form;
};

/// A value's children, as a forward range for a range-based for.
class ChildRange
{
  public:
    /// The first child.
    /// @return An iterator at it.
    [[nodiscard]] ChildIterator begin() const noexcept { return _begin; }

    /// Past the last child.
    /// @return The end iterator.
    [[nodiscard]] ChildIterator end() const noexcept { return _end; }

  private:
    friend class ValueView;

    ChildRange(ChildIterator first, ChildIterator last) noexcept: _begin { first }, _end { last } {}

    ChildIterator _begin;
    ChildIterator _end;
};

/// Component values as one flat arena (D9): the nodes of every root in pre-order, the bytes of every
/// string and of every list of scalars in one pool, tagged with the TypeArena their types belong to.
///
/// Its storage is charged to its store's LiveBytesBudget before it is allocated, and released when the
/// Values is destroyed: four vectors, freed in O(1) frames however deep the values are. Movable, not
/// copyable. A ValueBuilder makes one.
class Values
{
  public:
    Values(Values const&) = delete;
    Values& operator=(Values const&) = delete;
    /// Takes over @p other's values and charge; @p other is left empty.
    Values(Values&& other) noexcept = default;
    /// Releases the values held, then takes over @p other's.
    Values& operator=(Values&& other) noexcept = default;
    ~Values() = default;

    /// The arena the values' types belong to.
    /// @return Its identity.
    [[nodiscard]] types::TypeArenaId arena() const noexcept { return _arena; }

    /// The number of values: one per root type the builder was given.
    /// @return The count.
    [[nodiscard]] std::size_t rootCount() const noexcept { return _roots.size(); }

    /// One value.
    /// @param index Below rootCount() (checked).
    /// @return Its view.
    [[nodiscard]] ValueView root(std::size_t index) const;

    /// The host bytes the values hold, all of them charged to the budget.
    /// @return Their storage's charged bytes.
    [[nodiscard]] uint64_t hostBytes() const noexcept
    {
        return _nodes.chargedBytes() + _pool.chargedBytes() + _ranges.chargedBytes() + _roots.chargedBytes();
    }

    /// The number of nodes: one per value that is not an element of a list of scalars.
    /// @return The count.
    [[nodiscard]] std::size_t nodeCount() const noexcept { return _nodes.size(); }

  private:
    friend class ValueBuilder;
    friend class ValueView;
    friend class ChildIterator;

    Values(types::TypeArenaId arena,
           ChargedVector<ValueNode> nodes,
           ChargedVector<std::byte> pool,
           ChargedVector<ValueRange> ranges,
           ChargedVector<uint32_t> roots) noexcept:
        _arena { arena },
        _nodes { std::move(nodes) },
        _pool { std::move(pool) },
        _ranges { std::move(ranges) },
        _roots { std::move(roots) }
    {
    }

    types::TypeArenaId _arena;
    ChargedVector<ValueNode> _nodes;
    ChargedVector<std::byte> _pool;
    ChargedVector<ValueRange> _ranges;
    ChargedVector<uint32_t> _roots; ///< The node of each root.
};

/// What one ValueBuilder step builds.
enum class BuildStep : uint8_t
{
    Scalar,   ///< scalar(): a scalar, an enum or a flags value.
    String,   ///< string().
    Begin,    ///< begin(): a compound.
    Resource, ///< resource(): a handle.
};

/// Builds Values, one step per value in pre-order, checked against the root types as it goes (R9):
/// the type of each step is the one its position expects, so steps name no type.
///
/// A scalar, an enum and a flags value are scalar(); a string is string(), or encodedString() written in
/// place; a handle is resource(); a record, tuple, list, fixed list, variant, option, result or map is
/// begin(), its children (a list of scalars' all at once through packedElements()), then end(). Every storage
/// growth is charged to the budget first, and begin() of a list or a map charges for its count before
/// anything grows, so a count that lies is refused before the host allocates.
///
/// The first refused step is the builder's error, readable through error(): later steps do nothing,
/// and finish() returns it. A refused or abandoned build releases its whole charge when the builder is
/// destroyed, so the budget is as it was before the build.
class ValueBuilder
{
  public:
    /// A builder of one value per root type.
    /// @param arena The arena the root types belong to; it must outlive the builder.
    /// @param roots The types of the values to build, in order (checked: of @p arena).
    /// @param budget The budget the values' storage is charged to.
    /// @throws std::bad_alloc When the host cannot allocate what the budget allowed.
    ValueBuilder(types::TypeArena const& arena,
                 std::span<types::TypeId const> roots,
                 std::shared_ptr<LiveBytesBudget> budget);

    // Neither copied nor moved: a moved-from builder's storage holds no budget, which its next step
    // would dereference. It lives where its steps are taken, and finish() hands the values on.
    ValueBuilder(ValueBuilder const&) = delete;
    ValueBuilder& operator=(ValueBuilder const&) = delete;
    ValueBuilder(ValueBuilder&&) = delete;
    ValueBuilder& operator=(ValueBuilder&&) = delete;
    ~ValueBuilder() = default;

    /// A scalar, an enum's case index or a flags bitmask.
    /// @param bits A scalar's bits at its width (a float's bit pattern, a signed integer's two's
    ///             complement), zero-extended; an enum's case; a flags' bits.
    /// @return The builder.
    /// @throws std::bad_alloc When the host cannot allocate what the budget allowed.
    ValueBuilder& scalar(uint64_t bits);

    /// A string, from UTF-8.
    /// @param utf8 Its text, copied.
    /// @return The builder.
    /// @throws std::bad_alloc When the host cannot allocate what the budget allowed.
    ValueBuilder& string(std::string_view utf8);

    /// Opens a compound: its children follow, then end().
    /// @param caseIndexOrCount A variant's, option's or result's case; a list's or a fixed list's
    ///                         element count; a map's entry count; a record's or tuple's field count.
    /// @return The builder.
    /// @throws std::bad_alloc When the host cannot allocate what the budget allowed.
    ValueBuilder& begin(uint32_t caseIndexOrCount);

    /// A string whose bytes the caller writes in place, in their encoding (D4), charged before they are
    /// allocated.
    ///
    /// The caller writes text in @p encoding into the bytes returned before the builder's next step; the
    /// builder does not read them, and a string that is not text traps InvalidEncoding when it is lowered.
    /// A byte left unwritten stays zero. (Lifting copies guest memory in through LiftSteps instead, whose
    /// growth it overwrites whole and so is not zeroed first.)
    /// @param encoding The string's encoding.
    /// @param taggedCodeUnits Its tagged code units, which give its byte length (stringByteLength()).
    /// @return Its bytes, zero-filled; empty when the step is refused (error() then says why) or the string
    ///         is empty.
    /// @throws std::bad_alloc When the host cannot allocate what the budget allowed.
    [[nodiscard]] std::span<std::byte> encodedString(StringEncoding encoding, uint32_t taggedCodeUnits);

    /// Gives the list of scalars begin() opened last all its elements at once, for the caller to write in
    /// place rather than one scalar() each.
    ///
    /// The caller writes each element's bits, at its width and in host byte order, a value of the element
    /// type (a bool 0 or 1, a char a Unicode scalar value), before the builder's next step; an element left
    /// unwritten stays zero. (Lifting copies guest memory in through LiftSteps, unzeroed.)
    /// @return The elements' bytes, zero-filled, count times the element's width (begin() charged for
    ///         them); empty when the step is refused (error() then says why: the open compound is not a
    ///         list of scalars, or has elements given) or the list is empty.
    [[nodiscard]] std::span<std::byte> packedElements();

    /// Closes the compound begin() opened last, once all its children are given.
    /// @return The builder.
    ValueBuilder& end();

    /// A handle.
    /// @param value Its resource; its ownership must be the type's.
    /// @return The builder.
    /// @throws std::bad_alloc When the host cannot allocate what the budget allowed.
    ValueBuilder& resource(ResourceValue value);

    /// The first step refused.
    /// @return It, or empty while every step succeeded.
    [[nodiscard]] std::optional<ValueError> const& error() const noexcept { return _error; }

    /// The values, once every root is built; the builder is spent afterwards (checked).
    /// @return Them, or error(), or Unbalanced when a compound is open or a root is missing.
    [[nodiscard]] std::expected<Values, ValueError> finish();

  private:
    friend class LiftSteps;

    [[nodiscard]] std::span<std::byte> stringSteps(StringEncoding encoding,
                                                   uint32_t taggedCodeUnits,
                                                   GrowthFill fill);
    [[nodiscard]] std::span<std::byte> packedSteps(GrowthFill fill);

    /// A compound begin() opened.
    struct Frame
    {
        uint32_t node = 0;                              ///< Its node.
        types::TypeId type {};                          ///< Its type.
        types::ValueKind kind = types::ValueKind::Bool; ///< Its type's kind.
        uint64_t expected = 0;                          ///< The children it takes.
        uint64_t given = 0;                             ///< The children given.
        uint32_t caseIndex = 0;                         ///< For Cases, the case.
        ElementStorage storage = ElementStorage::Nodes; ///< Where its children go.
    };

    /// Where the next step's value goes.
    struct Slot
    {
        types::TypeId type {};                          ///< Its type.
        types::ValueKind kind = types::ValueKind::Bool; ///< Its kind.
        ElementStorage storage = ElementStorage::Nodes; ///< A node, or packed in the pool.
    };

    /// Whether finish() has been called.
    enum class BuildState : uint8_t
    {
        Building,
        Finished,
    };

    template <typename F>
    ValueBuilder& step(F const& body);
    [[nodiscard]] std::expected<Slot, ValueError> slotFor(BuildStep step) const;
    [[nodiscard]] std::expected<void, ValueError> buildScalar(uint64_t bits);
    [[nodiscard]] std::expected<void, ValueError> buildString(std::string_view utf8);
    [[nodiscard]] std::expected<std::span<std::byte>, ValueError> appendString(Slot const& slot,
                                                                               StringEncoding encoding,
                                                                               uint32_t taggedCodeUnits,
                                                                               GrowthFill fill);
    [[nodiscard]] std::expected<std::span<std::byte>, ValueError> buildPackedElements(GrowthFill fill);
    [[nodiscard]] std::expected<void, ValueError> buildBegin(uint32_t caseIndexOrCount);
    [[nodiscard]] std::expected<void, ValueError> buildEnd();
    [[nodiscard]] std::expected<void, ValueError> buildResource(ResourceValue value);
    [[nodiscard]] std::expected<void, ValueError> pushNode(ValueNode node, uint64_t reserveAfter);
    [[nodiscard]] std::expected<void, ValueError> unbalancedAtFinish() const;
    template <typename T>
    [[nodiscard]] std::expected<void, ValueError> reserve(ChargedVector<T>& vector, std::size_t extra) const;
    [[nodiscard]] ValueError errorHere(ValueErrorCode code, std::string detail) const;
    void completed() noexcept;

    types::TypeArena const* _arena;
    std::vector<types::TypeId> _rootTypes; ///< The type of each value to build: root i is built as
                                           ///< _rootTypes[i], and _nextRoot is the one built next.
    std::vector<Frame> _frames {};         ///< At most the root types' depth: maxTypeDepth.
    std::size_t _nextRoot = 0;
    ChargedVector<ValueNode> _nodes;
    ChargedVector<std::byte> _pool;
    ChargedVector<ValueRange> _ranges;
    ChargedVector<uint32_t> _roots;
    std::optional<ValueError> _error {};
    BuildState _state = BuildState::Building;
};

/// Whether two values are equal: the same kinds, cases, flags, handles and numbers (every NaN equal
/// to every NaN, otherwise floats by their bits), strings of the same text whatever their encodings,
/// and children equal in order. Structural: their types may come from different arenas, and labels
/// are not compared. Iterative, whatever the depth.
/// @param lhs A value.
/// @param rhs Another value.
/// @return true when they are equal.
[[nodiscard]] bool equal(ValueView lhs, ValueView rhs);

/// A value in the wast value syntax (wast 263.0.0's component values: `(u32.const 7)`,
/// `(record.const (field "a" (u32.const 7)))`, `(map.const (entry … …))`, …), NaN printed as `nan`.
/// A handle, which has no wast form, prints as `(own <type> <rep>)` or `(borrow <type> <rep>)`.
/// Iterative, whatever the depth.
/// @param arena The arena of @p value's types, whose labels it prints (checked).
/// @param value A value.
/// @return Its text.
/// @throws std::bad_alloc When the text cannot be stored.
[[nodiscard]] std::string toText(types::TypeArena const& arena, ValueView value);

/// The ScalarRule a C++ type reads a scalar as, for ValueView::scalar.
template <typename T>
[[nodiscard]] consteval ScalarRule scalarRuleOf() noexcept
{
    if constexpr (std::is_same_v<T, bool>)
        return ScalarRule::Bool;
    else if constexpr (std::is_same_v<T, char32_t>)
        return ScalarRule::Char;
    else if constexpr (std::is_floating_point_v<T>)
        return ScalarRule::Float;
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
        return ScalarRule::Signed;
    else if constexpr (std::is_integral_v<T>)
        return ScalarRule::Unsigned;
    else
        return ScalarRule::None;
}

template <typename T>
T ValueView::scalar() const
{
    static_assert(scalarRuleOf<T>() != ScalarRule::None, "scalar() reads arithmetic types only");
    auto const& row = shapeRow();
    Require(row.rule == scalarRuleOf<T>() && row.bytes == sizeof(T));
    auto const bits = scalarBits();
    if constexpr (std::is_same_v<T, bool>)
        return bits != 0;
    else if constexpr (std::is_same_v<T, float>)
        return std::bit_cast<float>(static_cast<uint32_t>(bits));
    else if constexpr (std::is_same_v<T, double>)
        return std::bit_cast<double>(bits);
    else
        return static_cast<T>(static_cast<std::make_unsigned_t<T>>(bits));
}

} // namespace isoline::component::canonical
