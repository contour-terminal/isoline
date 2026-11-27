// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/canonical/LiftLower.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
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
                {
                    Require(false);
                    return 0; // uncovered(unreachable)
                }
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

    /// Counts the bytes copied to or from guest memory, and polls the store for every
    /// api::BulkPollBytes of them (Global Constraints, interrupt latency).
    class BulkPolls
    {
      public:
        explicit BulkPolls(IGuestMemoryAccess& memory) noexcept: _memory { &memory } {}

        /// Counts @p bytes copied, polling for each whole granule they complete.
        [[nodiscard]] std::expected<void, Trap> copied(uint64_t bytes)
        {
            _sincePoll += bytes;
            while (_sincePoll >= isoline::api::BulkPollBytes)
            {
                _sincePoll -= isoline::api::BulkPollBytes;
                if (auto polled = _memory->poll(); !polled)
                    return polled;
            }
            return {};
        }

      private:
        IGuestMemoryAccess* _memory;
        uint64_t _sincePoll = 0;
    };

    // -------------------------------------------------------------------------------------------------
    // Sources and sinks: where a lifted value's bits come from, and where a lowered value's go. A lift or
    // a lower is one loop over the types; only these differ between core values and memory.

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

    /// Reads a lift's scalars from guest memory, each by one copy, little-endian.
    class MemorySource
    {
      public:
        explicit MemorySource(IGuestMemoryAccess& memory) noexcept: _memory { &memory }, _polls { memory } {}

        /// The @p bytes bytes at @p address.
        [[nodiscard]] std::expected<uint64_t, Trap> bits(uint32_t address,
                                                         std::size_t bytes,
                                                         CoreValueKind /*want*/)
        {
            auto buffer = std::array<std::byte, 8> {};
            if (auto read = _memory->read(address, std::span { buffer }.first(bytes)); !read)
                return std::unexpected { std::move(read.error()) };
            auto value = uint64_t { 0 };
            for (auto const index: std::views::iota(std::size_t { 0 }, bytes))
                value |= uint64_t { std::to_integer<uint8_t>(buffer[index]) } << (8U * index);
            return _polls.copied(bytes).transform([value] { return value; });
        }

        [[nodiscard]] static std::size_t enterCases(Layout const& /*layout*/) noexcept { return 0; }
        static void leaveCases(std::size_t /*start*/, Layout const& /*layout*/) noexcept {}

      private:
        IGuestMemoryAccess* _memory;
        BulkPolls _polls;
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
        explicit MemorySink(IGuestMemoryAccess& memory) noexcept: _memory { &memory }, _polls { memory } {}

        [[nodiscard]] std::expected<void, Trap> put(uint32_t address,
                                                    std::size_t bytes,
                                                    CoreValueKind /*flat*/,
                                                    uint64_t bits)
        {
            auto buffer = std::array<std::byte, 8> {};
            for (auto const index: std::views::iota(std::size_t { 0 }, bytes))
                buffer[index] = static_cast<std::byte>((bits >> (8U * index)) & 0xFFU);
            if (auto written = _memory->write(address, std::span { buffer }.first(bytes)); !written)
                return written;
            return _polls.copied(bytes);
        }

        [[nodiscard]] static std::size_t enterCases(Layout const& /*layout*/) noexcept { return 0; }
        static void leaveCases(std::size_t /*start*/, Layout const& /*layout*/) noexcept {}

      private:
        IGuestMemoryAccess* _memory;
        BulkPolls _polls;
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
        uint32_t address = 0;             ///< In memory, its address; else 0.
        uint32_t count = 0;               ///< Its children to walk.
        uint32_t next = 0;                ///< The next of them.
        std::size_t cases = 0;            ///< For a variant, the source's or sink's enterCases() token.
        std::optional<TypeId> payload {}; ///< For a variant, the case's payload type.
    };

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
            case ValueShape::FixedList: {
                auto const element = *type.children.front();
                return Child { .type = element,
                               .address = compound.address + (index * context.layouts[element.value].size) };
            }
            case ValueShape::Cases:
                return Child { .type = *compound.payload,
                               .address = compound.address + layout.payloadOffset };
            default: break;
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

    /// The values a lift builds as it walks the types: a scalar per leaf, begin() and end() per compound.
    template <typename Source>
    class Lifter
    {
      public:
        Lifter(LiftContext& context, Source& source, ValueBuilder& builder) noexcept:
            _context { &context }, _source { &source }, _builder { &builder }
        {
        }

        /// Lifts one value of @p type, at @p address in memory.
        [[nodiscard]] std::expected<void, Trap> lift(TypeId type, uint32_t address)
        {
            if (auto entered = enter(type, address); !entered)
                return entered;
            while (!_stack.empty())
            {
                auto& top = _stack.back();
                if (top.next == top.count)
                {
                    auto const done = top;
                    _stack.pop_back();
                    _builder->end();
                    if (valueShapeRow(context().arena[done.type].kind).shape == ValueShape::Cases)
                        _source->leaveCases(done.cases, context().layouts[done.type.value]);
                    if (auto built = checked(); !built)
                        return built;
                    continue;
                }
                auto const child = childOf(context(), top, top.next++);
                if (auto entered = enter(child.type, child.address); !entered)
                    return entered;
            }
            return {};
        }

      private:
        [[nodiscard]] LiftContext& context() const noexcept { return *_context; }

        /// The builder's refusal as a trap: the budget's is LiveValueBytes; any other means the walk
        /// built a value its type does not admit.
        [[nodiscard]] std::expected<void, Trap> checked() const
        {
            auto const& error = _builder->error();
            if (!error)
                return {};
            Guarantee(error->code == ValueErrorCode::Budget);
            return std::unexpected { trapOf(TrapCode::LiveValueBytes, context().budget->limit()) };
        }

        /// Builds a leaf of @p type, or opens a compound.
        [[nodiscard]] std::expected<void, Trap> enter(TypeId id, uint32_t address)
        {
            Require(id.value < context().layouts.size());
            auto const& type = context().arena[id];
            auto const& layout = context().layouts[id.value];
            auto const& row = valueShapeRow(type.kind);
            auto const bytes = leafBytes(layout, row.shape);
            switch (row.shape)
            {
                case ValueShape::Scalar: {
                    auto bits = _source->bits(address, bytes, layout.flat.front());
                    if (!bits)
                        return std::unexpected { std::move(bits.error()) };
                    // A bool is any nonzero core value (convert_int_to_bool), not only its low byte; every
                    // other scalar wraps to its width (lift_flat_unsigned/signed).
                    auto const width =
                        row.rule == ScalarRule::Bool ? std::size_t { 8 } : std::size_t { row.bytes };
                    auto const value = scalar(row.rule, *bits & widthMask(width), layout.flat.front());
                    if (!value)
                        return std::unexpected { value.error() };
                    _builder->scalar(*value);
                    break;
                }
                case ValueShape::Enum:
                case ValueShape::Flags: {
                    auto const bits = _source->bits(address, bytes, CoreValueKind::I32);
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
                    auto const index = _source->bits(address, bytes, CoreValueKind::I32);
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
                    _stack.push_back(Compound { .type = id, .address = address, .count = count });
                    break;
                }
                case ValueShape::Cases: {
                    auto const start = _source->enterCases(layout);
                    auto const discriminant = _source->bits(address, bytes, CoreValueKind::I32);
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
                                                .payload = payload });
                    break;
                }
                case ValueShape::String:
                case ValueShape::List:
                case ValueShape::Map:
                    // Task 12 lifts strings, lists and maps.
                case ValueShape::Unsupported:
                    // Validation refuses streams, futures and error contexts until P12.
                    Require(false);
                    break; // uncovered(task-12)
            }
            return checked();
        }

        /// A scalar's value from its bits (lift_flat_unsigned/signed, convert_int_to_bool,
        /// canonicalize_nan32/64, convert_i32_to_char).
        [[nodiscard]] static std::expected<uint64_t, Trap> scalar(ScalarRule rule,
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
                case ScalarRule::None: break;
            }
            Require(false);
            return 0; // uncovered(exhaustive-switch)
        }

        LiftContext* _context;
        Source* _source;
        ValueBuilder* _builder;
        std::vector<Compound> _stack {};
    };

    /// Writes values as it walks the destination types and the source values side by side (D9): the
    /// source's types may be another arena's, and are never looked up; its structure must be the
    /// destination's (checked).
    template <typename Sink>
    class Lowerer
    {
      public:
        Lowerer(LiftContext& context, Sink& sink) noexcept: _context { &context }, _sink { &sink } {}

        /// Lowers @p value as @p type, at @p address in memory.
        [[nodiscard]] std::expected<void, Trap> lower(TypeId type, ValueView value, uint32_t address)
        {
            if (auto entered = enter(type, value, address); !entered)
                return entered;
            while (!_stack.empty())
            {
                auto& top = _stack.back();
                if (top.compound.next == top.compound.count)
                {
                    auto const done = top.compound;
                    _stack.pop_back();
                    if (valueShapeRow(context().arena[done.type].kind).shape == ValueShape::Cases)
                        _sink->leaveCases(done.cases, context().layouts[done.type.value]);
                    continue;
                }
                auto const child = childOf(context(), top.compound, top.compound.next++);
                Require(top.source != top.end);
                auto const source = *top.source;
                ++top.source;
                if (auto entered = enter(child.type, source, child.address); !entered)
                    return entered;
            }
            return {};
        }

      private:
        /// A compound being lowered: the destination's walk, and the source's children beside it.
        struct Frame
        {
            Frame(Compound walk, ChildRange children) noexcept:
                compound { walk }, source { children.begin() }, end { children.end() }
            {
            }

            Compound compound;    ///< The destination.
            ChildIterator source; ///< The source's next child.
            ChildIterator end;    ///< Past its last.
        };

        [[nodiscard]] LiftContext& context() const noexcept { return *_context; }

        /// Writes a leaf of @p type, or opens a compound.
        [[nodiscard]] std::expected<void, Trap> enter(TypeId id, ValueView value, uint32_t address)
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
                    return _sink->put(address,
                                      bytes,
                                      layout.flat.front(),
                                      scalar(row, value.scalarBits(), layout.flat.front()));
                case ValueShape::Enum:
                    Require(value.caseIndex() < type.labels.size());
                    return _sink->put(address, bytes, CoreValueKind::I32, value.caseIndex());
                case ValueShape::Flags:
                    // definitions.py's pack_flags_into_int: a bit per label of the type, which the source
                    // value's labels must match.
                    Require(uint64_t { value.flagBits() } >> type.labels.size() == 0);
                    return _sink->put(address, bytes, CoreValueKind::I32, value.flagBits());
                case ValueShape::Handle: {
                    Require(type.resource.has_value());
                    auto const index = type.kind == ValueKind::Own
                                           ? context().handles.lowerOwn(*type.resource, value.resource())
                                           : context().handles.lowerBorrow(*type.resource, value.resource());
                    if (!index)
                        return std::unexpected { index.error() };
                    return _sink->put(address, bytes, CoreValueKind::I32, *index);
                }
                case ValueShape::Fields:
                case ValueShape::FixedList: {
                    auto const count = row.shape == ValueShape::Fields
                                           ? static_cast<uint32_t>(type.children.size())
                                           : type.length;
                    Require(value.childCount() == count);
                    _stack.emplace_back(Compound { .type = id, .address = address, .count = count },
                                        value.children());
                    return {};
                }
                case ValueShape::Cases: {
                    auto const caseIndex = value.caseIndex();
                    Require(caseIndex < caseCount(type));
                    auto const payload = casePayload(type, caseIndex);
                    Require(payload.has_value() == value.payload().has_value());
                    auto const start = _sink->enterCases(layout);
                    if (auto put = _sink->put(address, bytes, CoreValueKind::I32, caseIndex); !put)
                        return put;
                    _stack.emplace_back(Compound { .type = id,
                                                   .address = address,
                                                   .count = payload ? 1U : 0U,
                                                   .cases = start,
                                                   .payload = payload },
                                        value.children());
                    return {};
                }
                case ValueShape::String:
                case ValueShape::List:
                case ValueShape::Map:
                    // Task 12 lowers strings, lists and maps.
                case ValueShape::Unsupported:
                    // Validation refuses streams, futures and error contexts until P12.
                    break;
            }
            Require(false);
            return {}; // uncovered(task-12)
        }

        /// A scalar's bits as they are written (lower_flat_signed, maybe_scramble_nan32/64 in the
        /// DETERMINISTIC_PROFILE, a bool as 0 or 1): a signed integer sign-extended to 64 bits, which its
        /// core value or its bytes then take the low bits of.
        [[nodiscard]] static uint64_t scalar(ValueShapeRow const& row,
                                             uint64_t bits,
                                             CoreValueKind flat) noexcept
        {
            switch (row.rule)
            {
                case ScalarRule::Bool: return bits != 0 ? 1 : 0;
                case ScalarRule::Float: return canonicalFloat(bits, flat);
                case ScalarRule::Signed: {
                    auto const shift = 64U - (8U * row.bytes);
                    return static_cast<uint64_t>(static_cast<int64_t>(bits << shift) >> shift);
                }
                case ScalarRule::Char:
                case ScalarRule::Unsigned:
                case ScalarRule::None: break;
            }
            return bits;
        }

        LiftContext* _context;
        Sink* _sink;
        std::vector<Frame> _stack {};
    };

    /// The values a ValueBuilder made, or the trap the walk or the builder ended in.
    [[nodiscard]] std::expected<Values, Trap> finished(LiftContext const& context, ValueBuilder& builder)
    {
        auto values = builder.finish();
        if (values)
            return std::move(*values);
        Guarantee(values.error().code == ValueErrorCode::Budget);
        return std::unexpected { trapOf(TrapCode::LiveValueBytes, context.budget->limit()) };
    }

    /// Lifts one value per type through @p source, at @p addresses in memory (zeros when flat).
    template <typename Source>
    [[nodiscard]] std::expected<Values, Trap> liftAll(LiftContext& context,
                                                      std::span<TypeId const> types,
                                                      Source& source,
                                                      std::span<uint64_t const> addresses)
    {
        auto builder = ValueBuilder { context.arena, types, context.budget };
        if (builder.error())
            return finished(context, builder);
        auto lifter = Lifter<Source> { context, source, builder };
        for (auto const index: std::views::iota(std::size_t { 0 }, types.size()))
        {
            auto const address = addresses.empty() ? 0 : static_cast<uint32_t>(addresses[index]);
            if (auto lifted = lifter.lift(types[index], address); !lifted)
                return std::unexpected { std::move(lifted.error()) };
        }
        return finished(context, builder);
    }

    /// Lowers one value per type into guest memory, as a tuple at @p address that checkRange() admitted.
    [[nodiscard]] std::expected<void, Trap> storeAll(LiftContext& context,
                                                     std::span<TypeId const> types,
                                                     std::span<ValueView const> values,
                                                     TupleFootprint const& footprint,
                                                     uint32_t address)
    {
        auto sink = MemorySink { *context.memory };
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
    auto source = MemorySource { *context.memory };
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
            context.memory->reallocate(0, 0, footprint.alignment, static_cast<uint32_t>(footprint.size));
        if (!allocated)
            return std::unexpected { std::move(allocated.error()) };
        address = *allocated;
        out.emplace_back(isoline::api::I32 { *allocated });
    }
    // The memory's size is asked after the realloc, which may have grown it.
    if (auto inRange = checkRange(*address, footprint.size, footprint.alignment, context.memory->size());
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
