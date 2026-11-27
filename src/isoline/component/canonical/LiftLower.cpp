// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/canonical/LiftLower.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Types.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/canonical/GuestMemory.hpp>
#include <isoline/component/canonical/Layout.hpp>
#include <isoline/component/canonical/LiftSteps.hpp>
#include <isoline/component/canonical/Strings.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::canonical
{

namespace
{
    using binary::CoreValueKind;
    using types::DefinedType;
    using types::TypeId;
    using types::ValueKind;

    /// The bits below @p bytes bytes.
    [[nodiscard]] constexpr uint64_t widthMask(std::size_t bytes) noexcept
    {
        return bytes >= 8 ? ~uint64_t { 0 } : (uint64_t { 1 } << (8U * bytes)) - 1;
    }

    /// One row of CoreKindTable: a core value type, its width, and its NaN.
    struct CoreKindRow
    {
        CoreValueKind kind;        ///< The type; equal to the row's index.
        uint8_t bytes;             ///< Its width.
        uint64_t exponentMask = 0; ///< For a float, its exponent's bits; else 0.
        uint64_t canonicalNan = 0; ///< For a float, definitions.py's CANONICAL_FLOAT32/64_NAN; else 0.
    };

    /// The four core value types, in CoreValueKind's order.
    constexpr auto CoreKindTable = std::to_array<CoreKindRow>({
        { .kind = CoreValueKind::I32, .bytes = 4 },
        { .kind = CoreValueKind::I64, .bytes = 8 },
        { .kind = CoreValueKind::F32, .bytes = 4, .exponentMask = 0x7f80'0000, .canonicalNan = 0x7fc0'0000 },
        { .kind = CoreValueKind::F64,
          .bytes = 8,
          .exponentMask = 0x7ff0'0000'0000'0000,
          .canonicalNan = 0x7ff8'0000'0000'0000 },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: CoreKindTable)
            {
                if (static_cast<std::size_t>(row.kind) != index)
                    return false;
                ++index;
            }
            return true;
        }(),
        "CoreKindTable must hold one row per CoreValueKind, at the index of its enumerator");

    [[nodiscard]] constexpr CoreKindRow const& coreKindRow(CoreValueKind kind) noexcept
    {
        return CoreKindTable[static_cast<std::size_t>(kind)];
    }

    /// One row of JoinTable: a narrower core type a joined variant slot of a wider one carries
    /// (definitions.py's join, read backwards by lift_flat_variant's CoerceValueIter and forwards by
    /// lower_flat_variant).
    struct JoinRow
    {
        CoreValueKind narrow; ///< What a case holds.
        CoreValueKind wide;   ///< The joined slot it travels in, its bits zero-extended.
    };

    /// Every pair of distinct core types a joined slot relates: lifting takes the narrow type's low bits
    /// of the wide value; lowering zero-extends.
    constexpr auto JoinTable = std::to_array<JoinRow>({
        { .narrow = CoreValueKind::F32, .wide = CoreValueKind::I32 },
        { .narrow = CoreValueKind::I32, .wide = CoreValueKind::I64 },
        { .narrow = CoreValueKind::F32, .wide = CoreValueKind::I64 },
        { .narrow = CoreValueKind::F64, .wide = CoreValueKind::I64 },
    });

    /// Whether a value of @p narrow may travel in a slot of @p wide.
    [[nodiscard]] constexpr bool travelsIn(CoreValueKind narrow, CoreValueKind wide) noexcept
    {
        return narrow == wide || std::ranges::any_of(JoinTable, [&](JoinRow const& row) {
                   return row.narrow == narrow && row.wide == wide;
               });
    }

    /// definitions.py's canonicalize_nan32/64, and maybe_scramble_nan32/64 in the DETERMINISTIC_PROFILE
    /// (Global Constraints): a NaN's bits become the canonical NaN's.
    /// @param bits A float's bits.
    /// @param kind F32 or F64.
    [[nodiscard]] constexpr uint64_t canonicalFloat(uint64_t bits, CoreValueKind kind) noexcept
    {
        auto const& row = coreKindRow(kind);
        auto const fraction = widthMask(row.bytes) >> 1U & ~row.exponentMask;
        auto const isNan = (bits & row.exponentMask) == row.exponentMask && (bits & fraction) != 0;
        return isNan ? row.canonicalNan : bits;
    }

    /// The kind of a core value.
    [[nodiscard]] CoreValueKind coreKindOf(isoline::api::Value const& value)
    {
        if (std::holds_alternative<isoline::api::I32>(value))
            return CoreValueKind::I32;
        if (std::holds_alternative<isoline::api::I64>(value))
            return CoreValueKind::I64;
        if (std::holds_alternative<isoline::api::F32>(value))
            return CoreValueKind::F32;
        Require(std::holds_alternative<isoline::api::F64>(value));
        return CoreValueKind::F64;
    }

    /// The bits of a core value of one of the four number types (checked).
    [[nodiscard]] uint64_t bitsOf(isoline::api::Value const& value)
    {
        return std::visit(
            [](auto const& number) -> uint64_t {
                if constexpr (requires { number.bits; })
                    return number.bits;
                else
                // uncovered-begin(unreachable)
                {
                    Require(false);
                    return 0;
                }
                // uncovered-end(unreachable)
            },
            value);
    }

    /// The core value of @p kind whose bits are the low ones of @p bits.
    [[nodiscard]] isoline::api::Value valueOf(CoreValueKind kind, uint64_t bits)
    {
        switch (kind)
        {
            case CoreValueKind::I32: return isoline::api::I32 { static_cast<uint32_t>(bits) };
            case CoreValueKind::I64: return isoline::api::I64 { bits };
            case CoreValueKind::F32: return isoline::api::F32 { static_cast<uint32_t>(bits) };
            case CoreValueKind::F64: return isoline::api::F64 { bits };
        }
        Require(false);
        return {}; // uncovered(exhaustive-switch)
    }

    /// definitions.py's align_to, for a power-of-two @p alignment.
    [[nodiscard]] constexpr uint64_t alignTo(uint64_t offset, uint64_t alignment) noexcept
    {
        return (offset + alignment - 1) / alignment * alignment;
    }

    /// The number of cases of a variant, enum, option or result type.
    [[nodiscard]] std::size_t caseCount(DefinedType const& type) noexcept
    {
        auto const& row = valueShapeRow(type.kind);
        return row.firstPayloadCase != 0 || type.kind == ValueKind::Result ? 2 : type.labels.size();
    }

    /// The payload type of case @p index of a variant, option or result type; empty for none.
    [[nodiscard]] std::optional<TypeId> casePayload(DefinedType const& type, std::size_t index)
    {
        auto const first = std::size_t { valueShapeRow(type.kind).firstPayloadCase };
        if (index < first)
            return std::nullopt;
        Require(index - first < type.children.size());
        return type.children[index - first];
    }

    /// A tuple of value types laid out one after another (definitions.py's TupleType of a function's
    /// parameters or results).
    struct TupleFootprint
    {
        uint64_t size = 0;                ///< Its size, a multiple of its alignment.
        uint32_t alignment = 1;           ///< Its alignment.
        std::vector<uint64_t> offsets {}; ///< Each type's offset.
    };

    /// The footprint of a tuple of @p types (definitions.py's elem_size_record and alignment_record).
    [[nodiscard]] TupleFootprint footprintOf(LiftContext const& context, std::span<TypeId const> types)
    {
        auto footprint = TupleFootprint {};
        footprint.offsets.reserve(types.size());
        for (auto const type: types)
        {
            Require(type.value < context.layouts.size());
            auto const& layout = context.layouts[type.value];
            footprint.size = alignTo(footprint.size, layout.alignment);
            footprint.offsets.push_back(footprint.size);
            footprint.size += layout.size;
            footprint.alignment = std::max(footprint.alignment, layout.alignment);
        }
        footprint.size = alignTo(footprint.size, footprint.alignment);
        return footprint;
    }

    /// The length of the flattening of @p types, or MaxFlatParams + 1 once it is longer than that.
    [[nodiscard]] std::size_t flatLength(LiftContext const& context, std::span<TypeId const> types)
    {
        auto length = std::size_t { 0 };
        for (auto const type: types)
        {
            Require(type.value < context.layouts.size());
            auto const& layout = context.layouts[type.value];
            if (layout.flatShape == FlatShape::Overflows)
                return MaxFlatParams + 1;
            length = std::min(length + layout.flat.size(), std::size_t { MaxFlatParams } + 1);
        }
        return length;
    }

    // -------------------------------------------------------------------------------------------------
    // Sources and sinks: where a lifted value's bits come from, and where a lowered value's go. A lift or
    // a lower is one loop over the types; only these differ between core values and memory.

    /// Where a value's bits are read from or written to.
    enum class Placement : uint8_t
    {
        Primary, ///< The lift's source or the lower's sink: core values, or memory.
        Memory,  ///< Guest memory: a list's elements, wherever the list's pointer pair is.
    };

    /// Reads a flat lift's core values in order (definitions.py's CoreValueIter). Inside a variant, a
    /// slot holds the joined type of every case, wider than what the case reads from it
    /// (CoerceValueIter): the slot's type is the outermost variant's joined type at that position.
    class FlatSource
    {
      public:
        explicit FlatSource(std::span<isoline::api::Value const> flat) noexcept: _flat { flat } {}

        /// The next core value's bits, read as @p want: the low bits of the joined slot it travels in.
        [[nodiscard]] std::expected<uint64_t, Trap> bits(uint32_t /*address*/,
                                                         std::size_t /*bytes*/,
                                                         CoreValueKind want)
        {
            Require(_next < _flat.size());
            auto const& value = _flat[_next];
            auto const slot = _windowDepth == 0 ? want : _window[_next - _windowStart];
            Require(coreKindOf(value) == slot && travelsIn(want, slot));
            ++_next;
            return bitsOf(value) & widthMask(coreKindRow(want).bytes);
        }

        /// Enters a variant whose discriminant is the next core value.
        /// @return The position of its discriminant, for leaveCases().
        [[nodiscard]] std::size_t enterCases(Layout const& layout) noexcept
        {
            if (_windowDepth++ == 0)
            {
                _window = layout.flat;
                _windowStart = _next;
            }
            return _next;
        }

        /// Leaves the variant entered at @p start: the slots its case did not read are skipped.
        void leaveCases(std::size_t start, Layout const& layout) noexcept
        {
            Require(_next <= start + layout.flat.size());
            _next = start + layout.flat.size();
            --_windowDepth;
        }

      private:
        std::span<isoline::api::Value const> _flat;
        std::size_t _next = 0;
        std::span<CoreValueKind const> _window {}; ///< The outermost variant's flat types.
        std::size_t _windowStart = 0;              ///< Its discriminant's position.
        std::size_t _windowDepth = 0;              ///< The variants entered and not left.
    };

    /// The little-endian value of @p bytes.
    [[nodiscard]] uint64_t littleEndianBits(std::span<std::byte const> bytes) noexcept
    {
        auto value = uint64_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, bytes.size()))
            value |= uint64_t { std::to_integer<uint8_t>(bytes[index]) } << (8U * index);
        return value;
    }

    /// The @p bytes low bytes of @p bits, little-endian.
    [[nodiscard]] std::array<std::byte, 8> littleEndianBytes(uint64_t bits) noexcept
    {
        auto buffer = std::array<std::byte, 8> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, buffer.size()))
            buffer[index] = static_cast<std::byte>((bits >> (8U * index)) & 0xFFU);
        return buffer;
    }

    /// Reads a lift's scalars from guest memory, each by one copy, little-endian.
    class MemorySource
    {
      public:
        explicit MemorySource(LiftContext& context) noexcept: _context { &context } {}

        /// The @p bytes bytes at @p address.
        [[nodiscard]] std::expected<uint64_t, Trap> bits(uint32_t address,
                                                         std::size_t bytes,
                                                         CoreValueKind /*want*/)
        {
            auto buffer = std::array<std::byte, 8> {};
            auto const read = std::span { buffer }.first(bytes);
            if (auto copied = _context->memory->read(address, read); !copied)
                return std::unexpected { std::move(copied.error()) };
            auto const value = littleEndianBits(read);
            return _context->polls.copied(*_context->memory, bytes).transform([value] { return value; });
        }

        [[nodiscard]] static std::size_t enterCases(Layout const& /*layout*/) noexcept { return 0; }
        static void leaveCases(std::size_t /*start*/, Layout const& /*layout*/) noexcept {}

      private:
        LiftContext* _context;
    };

    /// Appends a flat lower's core values. A variant's payload is lowered in its own types, then widened
    /// into the variant's joined slots and padded with zeros (definitions.py's lower_flat_variant).
    class FlatSink
    {
      public:
        explicit FlatSink(std::vector<isoline::api::Value>& out) noexcept: _out { &out } {}

        [[nodiscard]] std::expected<void, Trap> put(uint32_t /*address*/,
                                                    std::size_t /*bytes*/,
                                                    CoreValueKind flat,
                                                    uint64_t bits)
        {
            _out->push_back(valueOf(flat, bits & widthMask(coreKindRow(flat).bytes)));
            return {};
        }

        [[nodiscard]] std::size_t enterCases(Layout const& /*layout*/) const noexcept { return _out->size(); }

        /// Widens the payload of the variant whose discriminant is at @p start into its joined slots, and
        /// pads the slots its case left empty.
        void leaveCases(std::size_t start, Layout const& layout)
        {
            auto& out = *_out;
            Require(out.size() <= start + layout.flat.size());
            for (auto const position: std::views::iota(start + 1, out.size()))
            {
                auto const joined = layout.flat[position - start];
                Require(travelsIn(coreKindOf(out[position]), joined));
                out[position] = valueOf(joined, bitsOf(out[position]));
            }
            for (auto const joined: std::span { layout.flat }.subspan(out.size() - start))
                out.push_back(valueOf(joined, 0));
        }

      private:
        std::vector<isoline::api::Value>* _out;
    };

    /// Writes a lower's scalars into guest memory, each by one copy, little-endian.
    class MemorySink
    {
      public:
        explicit MemorySink(LiftContext& context) noexcept: _context { &context } {}

        [[nodiscard]] std::expected<void, Trap> put(uint32_t address,
                                                    std::size_t bytes,
                                                    CoreValueKind /*flat*/,
                                                    uint64_t bits)
        {
            auto const buffer = littleEndianBytes(bits);
            if (auto written = _context->memory->write(address, std::span { buffer }.first(bytes)); !written)
                return written;
            return _context->polls.copied(*_context->memory, bytes);
        }

        [[nodiscard]] static std::size_t enterCases(Layout const& /*layout*/) noexcept { return 0; }
        static void leaveCases(std::size_t /*start*/, Layout const& /*layout*/) noexcept {}

      private:
        LiftContext* _context;
    };

    // -------------------------------------------------------------------------------------------------
    // Walking a type

    /// Where a compound's next child lies: its type, and in memory its address.
    struct Child
    {
        TypeId type {};       ///< The child's type.
        uint32_t address = 0; ///< In memory, its address; else 0.
    };

    /// A compound being walked, one child at a time: the walk's stack holds one per nesting level, so
    /// its depth is the type's, bounded by maxTypeDepth, whatever the number of children.
    struct Compound
    {
        TypeId type {};                   ///< Its type.
        uint32_t address = 0;             ///< In memory, its address (a list's, its block's); else 0.
        uint32_t count = 0;               ///< Its children to walk.
        uint32_t next = 0;                ///< The next of them.
        std::size_t cases = 0;            ///< For a variant, the source's or sink's enterCases() token.
        std::optional<TypeId> payload {}; ///< For a variant, the case's payload type.
        Placement placement = Placement::Primary; ///< Where its children are; a variant's, where it is.
        uint32_t stride = 0;                      ///< For a list or a map, an element's or an entry's size.
        uint32_t valueOffset = 0;                 ///< For a map, an entry's value's offset.
    };

    /// How a list's elements or a map's entries lie in memory (definitions.py's elem_size and alignment
    /// of the element type; a map's is its despecialized tuple's).
    struct ElementFootprint
    {
        uint32_t size = 0;        ///< One element's bytes.
        uint32_t alignment = 1;   ///< Its alignment.
        uint32_t valueOffset = 0; ///< For a map, an entry's value's offset; else 0.
    };

    /// The footprint of the elements of the list or map type @p type.
    [[nodiscard]] ElementFootprint elementFootprint(LiftContext const& context, DefinedType const& type)
    {
        if (type.kind == ValueKind::Map)
        {
            auto const entry = footprintOf(context, std::array { *type.children[0], *type.children[1] });
            return ElementFootprint { .size = static_cast<uint32_t>(entry.size),
                                      .alignment = entry.alignment,
                                      .valueOffset = static_cast<uint32_t>(entry.offsets[1]) };
        }
        auto const& layout = context.layouts[type.children[0]->value];
        return ElementFootprint { .size = layout.size, .alignment = layout.alignment };
    }

    /// The element type of a list whose elements are scalars, which lift and lower as one copy.
    [[nodiscard]] std::optional<TypeId> scalarElement(LiftContext const& context, DefinedType const& type)
    {
        if (type.kind != ValueKind::List)
            return std::nullopt;
        auto const element = *type.children[0];
        if (valueShapeRow(context.arena[element].kind).shape != ValueShape::Scalar)
            return std::nullopt;
        return element;
    }

    /// The @p index-th child of @p compound.
    [[nodiscard]] Child childOf(LiftContext const& context, Compound const& compound, uint32_t index)
    {
        auto const& type = context.arena[compound.type];
        auto const& layout = context.layouts[compound.type.value];
        switch (valueShapeRow(type.kind).shape)
        {
            case ValueShape::Fields:
                return Child { .type = *type.children[index],
                               .address = compound.address + layout.fieldOffsets[index] };
            case ValueShape::FixedList:
                return Child { .type = *type.children.front(),
                               .address = compound.address
                                          + (index * context.layouts[type.children.front()->value].size) };
            case ValueShape::Cases:
                return Child { .type = *compound.payload,
                               .address = compound.address + layout.payloadOffset };
            case ValueShape::List:
                // The block was checked to lie in memory: no element's address wraps.
                return Child { .type = *type.children.front(),
                               .address = static_cast<uint32_t>(compound.address
                                                                + (uint64_t { index } * compound.stride)) };
            case ValueShape::Map:
                // A key at its entry's start, a value at the entry's value offset.
                return Child { .type = *type.children[index % 2],
                               .address = static_cast<uint32_t>(
                                   compound.address + (uint64_t { index / 2 } * compound.stride)
                                   + (uint64_t { index % 2 } * compound.valueOffset)) };
            default: break; // uncovered(exhaustive-switch)
        }
        Require(false);
        return {}; // uncovered(exhaustive-switch)
    }

    /// The bytes a leaf takes in memory: a scalar's, a flags value's or a handle's size, or an enum's or a
    /// variant's discriminant's.
    [[nodiscard]] std::size_t leafBytes(Layout const& layout, ValueShape shape) noexcept
    {
        return shape == ValueShape::Cases || shape == ValueShape::Enum ? layout.discriminantBytes
                                                                       : layout.size;
    }

    /// A scalar's value from its bits (lift_flat_unsigned/signed, convert_int_to_bool,
    /// canonicalize_nan32/64, convert_i32_to_char).
    [[nodiscard]] std::expected<uint64_t, Trap> liftedScalar(ScalarRule rule,
                                                             uint64_t bits,
                                                             CoreValueKind flat)
    {
        switch (rule)
        {
            case ScalarRule::Bool: return bits != 0 ? 1 : 0;
            case ScalarRule::Float: return canonicalFloat(bits, flat);
            case ScalarRule::Char:
                if (bits >= 0x11'0000 || (bits >= 0xd800 && bits <= 0xdfff))
                    return std::unexpected { trapOf(TrapCode::InvalidChar, std::format("{:#x}", bits)) };
                return bits;
            case ScalarRule::Unsigned:
            case ScalarRule::Signed: return bits; // at its width, as the builder takes a two's complement
            case ScalarRule::None: break;         // uncovered(exhaustive-switch)
        }
        Require(false);
        return 0; // uncovered(exhaustive-switch)
    }

    /// Whether a scalar of @p rule is lowered into memory as its low bytes, unchanged: loweredScalar()
    /// changes only a bool's and a float's bits, and a signed integer's only above its width.
    [[nodiscard]] constexpr bool isStoredAsLowered(ScalarRule rule) noexcept
    {
        return rule == ScalarRule::Unsigned || rule == ScalarRule::Signed || rule == ScalarRule::Char;
    }

    /// A scalar's bits as they are written (lower_flat_signed, maybe_scramble_nan32/64 in the
    /// DETERMINISTIC_PROFILE, a bool as 0 or 1): a signed integer sign-extended to 64 bits, which its
    /// core value or its bytes then take the low bits of.
    [[nodiscard]] uint64_t loweredScalar(ValueShapeRow const& row, uint64_t bits, CoreValueKind flat) noexcept
    {
        switch (row.rule)
        {
            case ScalarRule::Bool: return bits != 0 ? 1 : 0;
            case ScalarRule::Float: return canonicalFloat(bits, flat);
            case ScalarRule::Signed:
                return static_cast<uint64_t>(static_cast<int64_t>(bits << (64U - (8U * row.bytes)))
                                             >> (64U - (8U * row.bytes)));
            case ScalarRule::Char:
            case ScalarRule::Unsigned:
            case ScalarRule::None: break;
        }
        return bits;
    }

    /// Writes @p bits, the @p bytes.size() low bytes of them, into @p bytes in host byte order: how a
    /// Values packs a list's scalars.
    void storeHostOrder(std::span<std::byte> bytes, uint64_t bits) noexcept
    {
        // The low bytes of a 64-bit integer are its first on a little-endian host, its last on a big one.
        auto const raw = std::bit_cast<std::array<std::byte, sizeof(uint64_t)>>(bits);
        auto const first = std::endian::native == std::endian::little ? 0 : raw.size() - bytes.size();
        std::ranges::copy(std::span { raw }.subspan(first, bytes.size()), bytes.begin());
    }

    /// Lifts in place the @p elements a list of scalars of @p row copied from guest memory: each read
    /// little-endian, lifted by its rule, stored in host byte order. An integer list on a little-endian
    /// host is already what it must be.
    [[nodiscard]] std::expected<void, Trap> liftInPlace(ValueShapeRow const& row,
                                                        CoreValueKind flat,
                                                        std::span<std::byte> elements)
    {
        auto const integer = row.rule == ScalarRule::Unsigned || row.rule == ScalarRule::Signed;
        if (integer && std::endian::native == std::endian::little)
            return {};
        for (auto const offset: std::views::iota(std::size_t { 0 }, elements.size() / row.bytes))
        {
            auto const element = elements.subspan(offset * row.bytes, row.bytes);
            auto const value = liftedScalar(row.rule, littleEndianBits(element), flat);
            if (!value)
                return std::unexpected { value.error() };
            storeHostOrder(element, *value);
        }
        return {};
    }

    /// definitions.py's load_list_from_range's checks, in order and in 64-bit arithmetic: the list's
    /// bytes at most MaxListByteLength, its block aligned and in memory.
    [[nodiscard]] std::expected<void, Trap> checkList(LiftContext& context,
                                                      uint32_t address,
                                                      uint32_t length,
                                                      ElementFootprint const& element)
    {
        auto const bytes = uint64_t { length } * element.size;
        if (bytes > MaxListByteLength)
            return std::unexpected { trapOf(
                TrapCode::OutOfBounds,
                std::format("a list of {} elements of {} bytes, more than the {} a "
                            "list spans",
                            length,
                            element.size,
                            MaxListByteLength)) };
        return checkRange(address, bytes, element.alignment, context.memory->size());
    }

    /// The values a lift builds as it walks the types: a scalar per leaf, begin() and end() per compound.
    /// A list's elements are read from memory, wherever its pointer pair is read from.
    template <typename Source>
    class Lifter
    {
      public:
        Lifter(LiftContext& context, Source& source, ValueBuilder& builder) noexcept:
            _context { &context }, _source { &source }, _memory { context }, _builder { &builder }
        {
        }

        /// Lifts one value of @p type, at @p address in memory.
        [[nodiscard]] std::expected<void, Trap> lift(TypeId type, uint32_t address)
        {
            if (auto entered = enter(type, address, Placement::Primary); !entered)
                return entered;
            while (!_stack.empty())
            {
                auto& top = _stack.back();
                if (top.next == top.count)
                {
                    auto const done = top;
                    _stack.pop_back();
                    // end() allocates nothing, so the budget cannot refuse it: no step to check.
                    _builder->end();
                    if (valueShapeRow(context().arena[done.type].kind).shape == ValueShape::Cases)
                        onSource(done.placement, [&](auto& source) {
                            source.leaveCases(done.cases, context().layouts[done.type.value]);
                        });
                    continue;
                }
                auto const placement = top.placement;
                auto const child = childOf(context(), top, top.next++);
                if (auto entered = enter(child.type, child.address, placement); !entered)
                    return entered;
            }
            return {};
        }

      private:
        [[nodiscard]] LiftContext& context() const noexcept { return *_context; }

        /// Calls @p body with the source of @p placement.
        template <typename F>
        decltype(auto) onSource(Placement placement, F const& body)
        {
            if (placement == Placement::Memory)
            {
                Require(context().memory != nullptr);
                return body(_memory);
            }
            return body(*_source);
        }

        [[nodiscard]] std::expected<uint64_t, Trap> readBits(Placement placement,
                                                             uint32_t address,
                                                             std::size_t bytes,
                                                             CoreValueKind want)
        {
            return onSource(placement, [&](auto& source) { return source.bits(address, bytes, want); });
        }

        /// The builder's refusal as a trap.
        [[nodiscard]] std::expected<void, Trap> checked() const
        {
            if (auto const& error = _builder->error(); error)
                return std::unexpected { builderTrap(context(), *error) };
            return {};
        }

        /// Builds a leaf of @p type, or opens a compound.
        [[nodiscard]] std::expected<void, Trap> enter(TypeId id, uint32_t address, Placement placement)
        {
            Require(id.value < context().layouts.size());
            auto const& type = context().arena[id];
            auto const& layout = context().layouts[id.value];
            auto const& row = valueShapeRow(type.kind);
            auto const bytes = leafBytes(layout, row.shape);
            switch (row.shape)
            {
                case ValueShape::Scalar: {
                    auto bits = readBits(placement, address, bytes, layout.flat.front());
                    if (!bits)
                        return std::unexpected { std::move(bits.error()) };
                    // A bool is any nonzero core value (convert_int_to_bool), not only its low byte; every
                    // other scalar wraps to its width (lift_flat_unsigned/signed).
                    auto const width =
                        row.rule == ScalarRule::Bool ? std::size_t { 8 } : std::size_t { row.bytes };
                    auto const value = liftedScalar(row.rule, *bits & widthMask(width), layout.flat.front());
                    if (!value)
                        return std::unexpected { value.error() };
                    _builder->scalar(*value);
                    break;
                }
                case ValueShape::Enum:
                case ValueShape::Flags: {
                    auto const bits = readBits(placement, address, bytes, CoreValueKind::I32);
                    if (!bits)
                        return std::unexpected { bits.error() };
                    if (row.shape == ValueShape::Flags)
                        // Unknown flag bits are ignored (definitions.py's unpack_flags_from_int).
                        _builder->scalar(*bits & ((uint64_t { 1 } << type.labels.size()) - 1));
                    else if (*bits >= type.labels.size())
                        return std::unexpected { trapOf(
                            TrapCode::InvalidDiscriminant, *bits, type.labels.size()) };
                    else
                        _builder->scalar(*bits);
                    break;
                }
                case ValueShape::Handle: {
                    auto const index = readBits(placement, address, bytes, CoreValueKind::I32);
                    if (!index)
                        return std::unexpected { index.error() };
                    Require(type.resource.has_value());
                    auto const resource =
                        type.kind == ValueKind::Own
                            ? context().handles.liftOwn(*type.resource, static_cast<uint32_t>(*index))
                            : context().handles.liftBorrow(*type.resource, static_cast<uint32_t>(*index));
                    if (!resource)
                        return std::unexpected { resource.error() };
                    _builder->resource(*resource);
                    break;
                }
                case ValueShape::Fields:
                case ValueShape::FixedList: {
                    auto const count = row.shape == ValueShape::Fields
                                           ? static_cast<uint32_t>(type.children.size())
                                           : type.length;
                    _builder->begin(count);
                    _stack.push_back(
                        Compound { .type = id, .address = address, .count = count, .placement = placement });
                    break;
                }
                case ValueShape::Cases: {
                    auto const start =
                        onSource(placement, [&](auto& source) { return source.enterCases(layout); });
                    auto const discriminant = readBits(placement, address, bytes, CoreValueKind::I32);
                    if (!discriminant)
                        return std::unexpected { discriminant.error() };
                    auto const cases = caseCount(type);
                    if (*discriminant >= cases)
                        return std::unexpected { trapOf(
                            TrapCode::InvalidDiscriminant, *discriminant, cases) };
                    auto const payload = casePayload(type, static_cast<std::size_t>(*discriminant));
                    _builder->begin(static_cast<uint32_t>(*discriminant));
                    _stack.push_back(Compound { .type = id,
                                                .address = address,
                                                .count = payload ? 1U : 0U,
                                                .cases = start,
                                                .payload = payload,
                                                .placement = placement });
                    break;
                }
                case ValueShape::String: {
                    auto const pair = pointerPair(placement, address);
                    if (!pair)
                        return std::unexpected { pair.error() };
                    if (auto lifted = liftString(context(), *_builder, pair->first, pair->second); !lifted)
                        return lifted;
                    break;
                }
                case ValueShape::List:
                case ValueShape::Map: return enterList(id, type, address, placement);
                case ValueShape::Unsupported: {
                    // Validation refuses streams, futures and error contexts until P12, so no table holds
                    // one: any index a guest passes names none (definitions.py's handles.get traps).
                    auto const index = readBits(placement, address, bytes, CoreValueKind::I32);
                    if (!index)
                        return std::unexpected { index.error() };
                    return std::unexpected { trapOf(TrapCode::UnknownHandle, *index) };
                }
            }
            return checked();
        }

        /// A string's or a list's pointer and length (definitions.py's lift_flat_string and
        /// lift_flat_list; load_string and load_list).
        [[nodiscard]] std::expected<std::pair<uint32_t, uint32_t>, Trap> pointerPair(Placement placement,
                                                                                     uint32_t address)
        {
            auto const pointer = readBits(placement, address, 4, CoreValueKind::I32);
            if (!pointer)
                return std::unexpected { pointer.error() };
            return readBits(placement, address + 4, 4, CoreValueKind::I32).transform([&](uint64_t length) {
                return std::pair { static_cast<uint32_t>(*pointer), static_cast<uint32_t>(length) };
            });
        }

        /// Lifts a list or a map (load_list_from_range): checked whole before the builder charges for its
        /// elements; a list of scalars copied at once, any other walked element by element.
        [[nodiscard]] std::expected<void, Trap> enterList(TypeId id,
                                                          DefinedType const& type,
                                                          uint32_t address,
                                                          Placement placement)
        {
            auto const pair = pointerPair(placement, address);
            if (!pair)
                return std::unexpected { pair.error() };
            auto const [block, length] = *pair;
            auto const element = elementFootprint(context(), type);
            if (auto inList = checkList(context(), block, length, element); !inList)
                return inList;
            _builder->begin(length);
            if (auto begun = checked(); !begun)
                return begun;
            if (auto const scalar = scalarElement(context(), type); scalar)
            {
                if (auto copied = liftPacked(*scalar, block); !copied)
                    return copied;
                _builder->end();
                return checked();
            }
            auto const children = type.kind == ValueKind::Map ? 2 * length : length;
            _stack.push_back(Compound { .type = id,
                                        .address = block,
                                        .count = children,
                                        .placement = Placement::Memory,
                                        .stride = element.size,
                                        .valueOffset = element.valueOffset });
            return {};
        }

        /// Copies the elements of the list of scalars begin() opened from @p block, in api::BulkPollBytes
        /// granules straight into the values, polling after each.
        [[nodiscard]] std::expected<void, Trap> liftPacked(TypeId element, uint32_t block)
        {
            auto const& row = valueShapeRow(context().arena[element].kind);
            auto const flat = context().layouts[element.value].flat.front();
            // begin() charged for the elements: the builder refuses nothing here, and an error would be
            // the end()'s to report.
            auto const elements = LiftSteps::packedElements(*_builder);
            auto& memory = *context().memory;
            auto copied = std::size_t { 0 };
            while (copied < elements.size())
            {
                // A granule is a multiple of every scalar's width: no element straddles two.
                auto const count =
                    std::min<std::size_t>(elements.size() - copied, isoline::api::BulkPollBytes);
                auto const granule = elements.subspan(copied, count);
                if (auto read = memory.read(static_cast<uint32_t>(block + copied), granule); !read)
                    return read;
                if (auto lifted = liftInPlace(row, flat, granule); !lifted)
                    return lifted;
                if (auto polled = context().polls.copied(memory, count); !polled)
                    return polled;
                copied += count;
            }
            return {};
        }

        LiftContext* _context;
        Source* _source;
        MemorySource _memory;
        ValueBuilder* _builder;
        std::vector<Compound> _stack {};
    };

    /// A pointer pair a lower writes once the block it points at is filled: definitions.py's store_list
    /// and lower_flat_list write it after store_list_into_range, so a `realloc` that hands out
    /// overlapping blocks sees the reference's order of writes.
    struct PendingPair
    {
        Placement placement = Placement::Primary; ///< Where it goes.
        uint32_t address = 0;                     ///< In memory, its address; else 0.
        uint32_t pointer = 0;                     ///< The block's address.
        uint32_t length = 0;                      ///< Its elements, or a string's tagged code units.
    };

    /// Writes values as it walks the destination types and the source values side by side (D9): the
    /// source's types may be another arena's, and are never looked up; its structure must be the
    /// destination's (checked). A list's elements go to memory, wherever its pointer pair goes.
    template <typename Sink>
    class Lowerer
    {
      public:
        Lowerer(LiftContext& context, Sink& sink) noexcept:
            _context { &context }, _sink { &sink }, _memory { context }
        {
        }

        /// Lowers @p value as @p type, at @p address in memory.
        [[nodiscard]] std::expected<void, Trap> lower(TypeId type, ValueView value, uint32_t address)
        {
            if (auto entered = enter(type, value, address, Placement::Primary); !entered)
                return entered;
            while (!_stack.empty())
            {
                auto& top = _stack.back();
                if (top.compound.next == top.compound.count)
                {
                    auto const done = top;
                    _stack.pop_back();
                    if (valueShapeRow(context().arena[done.compound.type].kind).shape == ValueShape::Cases)
                        onSink(done.compound.placement, [&](auto& sink) {
                            sink.leaveCases(done.compound.cases, context().layouts[done.compound.type.value]);
                        });
                    if (done.pair)
                    {
                        if (auto put = putPair(*done.pair); !put)
                            return put;
                    }
                    continue;
                }
                auto const placement = top.compound.placement;
                auto const child = childOf(context(), top.compound, top.compound.next++);
                Require(top.source != top.end);
                auto const source = *top.source;
                ++top.source;
                if (auto entered = enter(child.type, source, child.address, placement); !entered)
                    return entered;
            }
            return {};
        }

      private:
        /// A compound being lowered: the destination's walk, the source's children beside it, and for a
        /// list its pointer pair.
        struct Frame
        {
            Frame(Compound walk, ChildRange children, std::optional<PendingPair> pending = {}) noexcept:
                compound { walk }, source { children.begin() }, end { children.end() }, pair { pending }
            {
            }

            Compound compound;               ///< The destination.
            ChildIterator source;            ///< The source's next child.
            ChildIterator end;               ///< Past its last.
            std::optional<PendingPair> pair; ///< For a list or a map, its pointer pair.
        };

        [[nodiscard]] LiftContext& context() const noexcept { return *_context; }

        /// Calls @p body with the sink of @p placement.
        template <typename F>
        decltype(auto) onSink(Placement placement, F const& body)
        {
            if (placement == Placement::Memory)
            {
                Require(context().memory != nullptr);
                return body(_memory);
            }
            return body(*_sink);
        }

        [[nodiscard]] std::expected<void, Trap> write(
            Placement placement, uint32_t address, std::size_t bytes, CoreValueKind flat, uint64_t bits)
        {
            return onSink(placement, [&](auto& sink) { return sink.put(address, bytes, flat, bits); });
        }

        [[nodiscard]] std::expected<void, Trap> putPair(PendingPair const& pair)
        {
            return write(pair.placement, pair.address, 4, CoreValueKind::I32, pair.pointer).and_then([&] {
                return write(pair.placement, pair.address + 4, 4, CoreValueKind::I32, pair.length);
            });
        }

        /// Writes a leaf of @p type, or opens a compound.
        [[nodiscard]] std::expected<void, Trap> enter(TypeId id,
                                                      ValueView value,
                                                      uint32_t address,
                                                      Placement placement)
        {
            Require(id.value < context().layouts.size());
            auto const& type = context().arena[id];
            auto const& layout = context().layouts[id.value];
            auto const& row = valueShapeRow(type.kind);
            Require(value.kind() == type.kind);
            auto const bytes = leafBytes(layout, row.shape);
            switch (row.shape)
            {
                case ValueShape::Scalar:
                    return write(placement,
                                 address,
                                 bytes,
                                 layout.flat.front(),
                                 loweredScalar(row, value.scalarBits(), layout.flat.front()));
                case ValueShape::Enum:
                    Require(value.caseIndex() < type.labels.size());
                    return write(placement, address, bytes, CoreValueKind::I32, value.caseIndex());
                case ValueShape::Flags:
                    // definitions.py's pack_flags_into_int: a bit per label of the type, which the source
                    // value's labels must match.
                    Require(uint64_t { value.flagBits() } >> type.labels.size() == 0);
                    return write(placement, address, bytes, CoreValueKind::I32, value.flagBits());
                case ValueShape::Handle: {
                    Require(type.resource.has_value());
                    auto const index = type.kind == ValueKind::Own
                                           ? context().handles.lowerOwn(*type.resource, value.resource())
                                           : context().handles.lowerBorrow(*type.resource, value.resource());
                    if (!index)
                        return std::unexpected { index.error() };
                    return write(placement, address, bytes, CoreValueKind::I32, *index);
                }
                case ValueShape::Fields:
                case ValueShape::FixedList: {
                    auto const count = row.shape == ValueShape::Fields
                                           ? static_cast<uint32_t>(type.children.size())
                                           : type.length;
                    Require(value.childCount() == count);
                    _stack.emplace_back(
                        Compound { .type = id, .address = address, .count = count, .placement = placement },
                        value.children());
                    return {};
                }
                case ValueShape::Cases: {
                    auto const caseIndex = value.caseIndex();
                    Require(caseIndex < caseCount(type));
                    auto const payload = casePayload(type, caseIndex);
                    Require(payload.has_value() == value.payload().has_value());
                    auto const start = onSink(placement, [&](auto& sink) { return sink.enterCases(layout); });
                    if (auto put = write(placement, address, bytes, CoreValueKind::I32, caseIndex); !put)
                        return put;
                    _stack.emplace_back(Compound { .type = id,
                                                   .address = address,
                                                   .count = payload ? 1U : 0U,
                                                   .cases = start,
                                                   .payload = payload,
                                                   .placement = placement },
                                        value.children());
                    return {};
                }
                case ValueShape::String: {
                    Require(context().memory != nullptr);
                    auto const stored = storeString(context(), value.string());
                    if (!stored)
                        return std::unexpected { stored.error() };
                    return putPair(PendingPair { .placement = placement,
                                                 .address = address,
                                                 .pointer = stored->first,
                                                 .length = stored->second });
                }
                case ValueShape::List:
                case ValueShape::Map: return enterList(id, type, value, address, placement);
                // No Values holds a stream, a future or an error context (ValueBuilder refuses them), so
                // the kind check above refused this value already.
                case ValueShape::Unsupported: // uncovered(unsupported-value)
                    break;                    // uncovered(unsupported-value)
            }
            Require(false);
            return {}; // uncovered(unsupported-value)
        }

        /// Lowers a list or a map (store_list_into_range): its block allocated and checked, a list of
        /// scalars written at once, any other's elements walked into it, then its pointer pair written.
        [[nodiscard]] std::expected<void, Trap> enterList(
            TypeId id, DefinedType const& type, ValueView value, uint32_t address, Placement placement)
        {
            Require(context().memory != nullptr);
            auto const element = elementFootprint(context(), type);
            // In 64-bit arithmetic: a map's 2^31 entries or more have more children than 32 bits count.
            auto const children = uint64_t { value.childCount() };
            auto const length = type.kind == ValueKind::Map ? children / 2 : children;
            auto const bytes = length * element.size;
            // Each element is below MaxValueByteSize, but a host list may hold more than any 32-bit memory.
            if (bytes > std::numeric_limits<uint32_t>::max())
                return std::unexpected { trapOf(
                    TrapCode::OutOfBounds,
                    std::format("a list of {} bytes, more than any 32-bit memory holds", bytes)) };
            auto const block =
                reallocate(*context().memory, 0, 0, element.alignment, static_cast<uint32_t>(bytes));
            if (!block)
                return std::unexpected { block.error() };
            // Past the guard both counts fit 32 bits: an element is a byte at least, a map entry two.
            auto const pair = PendingPair { .placement = placement,
                                            .address = address,
                                            .pointer = *block,
                                            .length = static_cast<uint32_t>(length) };
            if (auto const scalar = scalarElement(context(), type); scalar)
            {
                if (auto written = lowerPacked(*scalar, value, *block, static_cast<uint32_t>(bytes));
                    !written)
                    return written;
                return putPair(pair);
            }
            _stack.emplace_back(Compound { .type = id,
                                           .address = *block,
                                           .count = static_cast<uint32_t>(children),
                                           .placement = Placement::Memory,
                                           .stride = element.size,
                                           .valueOffset = element.valueOffset },
                                value.children(),
                                pair);
            return {};
        }

        /// Writes the elements of a list of scalars into @p block, through one BlockWriter: one write per
        /// BlockWriter::BufferBytes, not one per element.
        [[nodiscard]] std::expected<void, Trap> lowerPacked(TypeId element,
                                                            ValueView value,
                                                            uint32_t block,
                                                            uint32_t size)
        {
            auto const& row = valueShapeRow(context().arena[element].kind);
            auto const flat = context().layouts[element.value].flat.front();
            auto writer = BlockWriter { context(), block, size };
            // Integers and chars are lowered as they are stored, so on a little-endian host the packed
            // elements are their own lowering: one copy, polled per granule as any other.
            if (auto const packed = value.packedElementBytes();
                std::endian::native == std::endian::little && !packed.empty() && isStoredAsLowered(row.rule))
            {
                Require(packed.size() == size); // the block realloc made for exactly these elements
                if (auto put = writer.put(packed); !put)
                    return put;
                return writer.flush();
            }
            for (auto const item: value.children())
            {
                Require(item.kind() == context().arena[element].kind);
                auto const bytes = littleEndianBytes(loweredScalar(row, item.scalarBits(), flat));
                if (auto put = writer.put(std::span { bytes }.first(row.bytes)); !put)
                    return put;
            }
            return writer.flush();
        }

        LiftContext* _context;
        Sink* _sink;
        MemorySink _memory;
        std::vector<Frame> _stack {};
    };

    /// Lifts one value per type through @p source, at @p addresses in memory (zeros when flat).
    template <typename Source>
    [[nodiscard]] std::expected<Values, Trap> liftAll(LiftContext& context,
                                                      std::span<TypeId const> types,
                                                      Source& source,
                                                      std::span<uint64_t const> addresses)
    {
        auto builder = ValueBuilder { context.arena, types, context.budget };
        auto lifter = Lifter<Source> { context, source, builder };
        for (auto const index: std::views::iota(std::size_t { 0 }, types.size()))
        {
            auto const address = addresses.empty() ? 0 : static_cast<uint32_t>(addresses[index]);
            if (auto lifted = lifter.lift(types[index], address); !lifted)
                return std::unexpected { std::move(lifted.error()) };
        }
        // Every refusal of a step ended the lift as it happened (Lifter::checked()); the constructor's
        // charge for the roots, at the first step.
        auto values = builder.finish();
        Guarantee(values.has_value());
        return std::move(*values);
    }

    /// Lowers one value per type into guest memory, as a tuple at @p address that checkRange() admitted.
    [[nodiscard]] std::expected<void, Trap> storeAll(LiftContext& context,
                                                     std::span<TypeId const> types,
                                                     std::span<ValueView const> values,
                                                     TupleFootprint const& footprint,
                                                     uint32_t address)
    {
        auto sink = MemorySink { context };
        auto lowerer = Lowerer<MemorySink> { context, sink };
        for (auto const index: std::views::iota(std::size_t { 0 }, types.size()))
        {
            auto const at = static_cast<uint32_t>(address + footprint.offsets[index]);
            if (auto stored = lowerer.lower(types[index], values[index], at); !stored)
                return stored;
        }
        return {};
    }
} // namespace

// -------------------------------------------------------------------------------------------------
// BulkPolls, BlockWriter, builderTrap

std::expected<void, Trap> BulkPolls::copied(IGuestMemoryAccess& memory, uint64_t bytes)
{
    _sincePoll += bytes;
    while (_sincePoll >= isoline::api::BulkPollBytes)
    {
        _sincePoll -= isoline::api::BulkPollBytes;
        if (auto polled = memory.poll(); !polled)
            return polled;
    }
    return {};
}

Trap builderTrap(LiftContext const& context, ValueError const& error)
{
    Guarantee(error.code == ValueErrorCode::Budget);
    return trapOf(TrapCode::LiveValueBytes, context.budget->limit());
}

BlockWriter::BlockWriter(LiftContext& context, uint32_t block, uint32_t size) noexcept:
    _context { &context }, _block { block }, _size { size }
{
    Require(context.memory != nullptr);
}

std::expected<void, Trap> BlockWriter::put(std::span<std::byte const> bytes)
{
    Require(bytes.size() <= _size - offset());
    while (!bytes.empty())
    {
        auto const count = std::min(bytes.size(), BufferBytes - _filled);
        std::ranges::copy(bytes.first(count), _buffer.begin() + static_cast<std::ptrdiff_t>(_filled));
        _filled += count;
        bytes = bytes.subspan(count);
        if (_filled == BufferBytes)
        {
            if (auto flushed = flush(); !flushed)
                return flushed;
        }
    }
    return {};
}

std::expected<void, Trap> BlockWriter::flush()
{
    if (_filled == 0)
        return {};
    auto& memory = *_context->memory;
    auto const count = std::exchange(_filled, 0);
    auto const address = static_cast<uint32_t>(_block + _flushed);
    _flushed += count;
    if (auto written = memory.write(address, std::span { _buffer }.first(count)); !written)
        return written;
    return _context->polls.copied(memory, count);
}

void BlockWriter::moveTo(uint32_t block, uint32_t size) noexcept
{
    Require(_filled == 0 && _flushed <= size);
    _block = block;
    _size = size;
}

void BlockWriter::seek(uint64_t offset) noexcept
{
    Require(_filled == 0 && offset <= _size);
    _flushed = offset;
}

// -------------------------------------------------------------------------------------------------
// liftFlat, load, lowerFlat, store

std::expected<Values, Trap> liftFlat(LiftContext& context,
                                     std::span<types::TypeId const> types,
                                     std::span<isoline::api::Value const> flat,
                                     uint32_t maxFlat)
{
    Require(maxFlat <= MaxFlatParams);
    if (flatLength(context, types) > maxFlat)
    {
        // definitions.py's lift_flat_values: the one core value points at the tuple.
        Require(flat.size() == 1 && coreKindOf(flat.front()) == CoreValueKind::I32);
        return load(context, types, static_cast<uint32_t>(bitsOf(flat.front())));
    }
    Require(flat.size() == flatLength(context, types));
    auto source = FlatSource { flat };
    return liftAll(context, types, source, {});
}

std::expected<Values, Trap> load(LiftContext& context, std::span<types::TypeId const> types, uint32_t address)
{
    Require(context.memory != nullptr);
    auto const footprint = footprintOf(context, types);
    if (auto inRange = checkRange(address, footprint.size, footprint.alignment, context.memory->size());
        !inRange)
        return std::unexpected { std::move(inRange.error()) };
    auto addresses = footprint.offsets;
    for (auto& offset: addresses)
        offset += address;
    auto source = MemorySource { context };
    return liftAll(context, types, source, addresses);
}

std::expected<std::vector<isoline::api::Value>, Trap> lowerFlat(LiftContext& context,
                                                                std::span<types::TypeId const> types,
                                                                std::span<ValueView const> values,
                                                                uint32_t maxFlat,
                                                                std::optional<uint32_t> resultPointer)
{
    Require(maxFlat <= MaxFlatParams && types.size() == values.size());
    auto out = std::vector<isoline::api::Value> {};
    if (flatLength(context, types) <= maxFlat)
    {
        out.reserve(flatLength(context, types));
        auto sink = FlatSink { out };
        auto lowerer = Lowerer<FlatSink> { context, sink };
        for (auto const index: std::views::iota(std::size_t { 0 }, types.size()))
        {
            if (auto lowered = lowerer.lower(types[index], values[index], 0); !lowered)
                return std::unexpected { std::move(lowered.error()) };
        }
        return out;
    }
    // definitions.py's lower_flat_values: the tuple goes to memory, at the result pointer given, or in a
    // block realloc allocates, whose address is then the one core value.
    Require(context.memory != nullptr);
    auto const footprint = footprintOf(context, types);
    auto address = resultPointer;
    if (!address)
    {
        // Each type is below MaxValueByteSize, but many of them together may pass what any 32-bit memory
        // holds: no realloc could return such a block.
        if (footprint.size > std::numeric_limits<uint32_t>::max())
            return std::unexpected { trapOf(
                TrapCode::OutOfBounds,
                std::format("a tuple of {} bytes, more than any 32-bit memory holds", footprint.size)) };
        auto allocated =
            reallocate(*context.memory, 0, 0, footprint.alignment, static_cast<uint32_t>(footprint.size));
        if (!allocated)
            return std::unexpected { std::move(allocated.error()) };
        address = *allocated;
        out.emplace_back(isoline::api::I32 { *allocated });
    }
    else if (auto inRange = checkRange(*address, footprint.size, footprint.alignment, context.memory->size());
             !inRange)
        return std::unexpected { std::move(inRange.error()) };
    if (auto stored = storeAll(context, types, values, footprint, *address); !stored)
        return std::unexpected { std::move(stored.error()) };
    return out;
}

std::expected<void, Trap> store(LiftContext& context, types::TypeId type, ValueView value, uint32_t address)
{
    Require(context.memory != nullptr);
    auto const types = std::array { type };
    auto const footprint = footprintOf(context, types);
    if (auto inRange = checkRange(address, footprint.size, footprint.alignment, context.memory->size());
        !inRange)
        return inRange;
    return storeAll(context, types, std::span { &value, 1 }, footprint, address);
}

} // namespace isoline::component::canonical
