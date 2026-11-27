// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/canonical/Layout.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace isoline::component::canonical
{

namespace
{
    using binary::CoreValueKind;
    using types::Despecialized;
    using types::ValueKind;

    /// How the canonical ABI lays out a value kind once despecialized (definitions.py's despecialize).
    enum class LayoutRule : uint8_t
    {
        Scalar,      ///< Of LayoutRuleRow::bytes bytes, so aligned, flat one LayoutRuleRow::flat.
        PointerPair, ///< A pointer and a length: string, list, and map as `list<tuple<k, v>>`.
        FixedList,   ///< The element, length times.
        Record,      ///< The fields in turn: record, and tuple as a record.
        Variant,     ///< A discriminant, then the largest case: variant, and enum, option, result as one.
        Flags,       ///< 1, 2 or 4 bytes by label count, flat one i32.
    };

    /// One row of LayoutRuleTable: a value kind and how it is laid out.
    struct LayoutRuleRow
    {
        ValueKind kind;                          ///< The kind; equal to the row's index.
        LayoutRule rule;                         ///< How it is laid out.
        uint8_t bytes = 0;                       ///< For Scalar: its size and alignment.
        CoreValueKind flat = CoreValueKind::I32; ///< For Scalar: its one flat type.
        uint8_t cases = 0; ///< For Variant: its fixed case count, or 0 for one case per label.
    };

    /// Every value kind's layout rule (definitions.py's alignment, elem_size and flatten_type at the
    /// pin), one row per ValueKind enumerator, in enumerator order.
    constexpr auto LayoutRuleTable = std::to_array<LayoutRuleRow>({
        { .kind = ValueKind::Bool, .rule = LayoutRule::Scalar, .bytes = 1 },
        { .kind = ValueKind::S8, .rule = LayoutRule::Scalar, .bytes = 1 },
        { .kind = ValueKind::U8, .rule = LayoutRule::Scalar, .bytes = 1 },
        { .kind = ValueKind::S16, .rule = LayoutRule::Scalar, .bytes = 2 },
        { .kind = ValueKind::U16, .rule = LayoutRule::Scalar, .bytes = 2 },
        { .kind = ValueKind::S32, .rule = LayoutRule::Scalar, .bytes = 4 },
        { .kind = ValueKind::U32, .rule = LayoutRule::Scalar, .bytes = 4 },
        { .kind = ValueKind::S64, .rule = LayoutRule::Scalar, .bytes = 8, .flat = CoreValueKind::I64 },
        { .kind = ValueKind::U64, .rule = LayoutRule::Scalar, .bytes = 8, .flat = CoreValueKind::I64 },
        { .kind = ValueKind::F32, .rule = LayoutRule::Scalar, .bytes = 4, .flat = CoreValueKind::F32 },
        { .kind = ValueKind::F64, .rule = LayoutRule::Scalar, .bytes = 8, .flat = CoreValueKind::F64 },
        { .kind = ValueKind::Char, .rule = LayoutRule::Scalar, .bytes = 4 },
        { .kind = ValueKind::String, .rule = LayoutRule::PointerPair },
        { .kind = ValueKind::ErrorContext, .rule = LayoutRule::Scalar, .bytes = 4 },
        { .kind = ValueKind::List, .rule = LayoutRule::PointerPair },
        { .kind = ValueKind::FixedList, .rule = LayoutRule::FixedList },
        { .kind = ValueKind::Record, .rule = LayoutRule::Record },
        { .kind = ValueKind::Tuple, .rule = LayoutRule::Record },
        { .kind = ValueKind::Variant, .rule = LayoutRule::Variant },
        { .kind = ValueKind::Enum, .rule = LayoutRule::Variant },
        { .kind = ValueKind::Option, .rule = LayoutRule::Variant, .cases = 2 },
        { .kind = ValueKind::Result, .rule = LayoutRule::Variant, .cases = 2 },
        { .kind = ValueKind::Flags, .rule = LayoutRule::Flags },
        { .kind = ValueKind::Map, .rule = LayoutRule::PointerPair },
        { .kind = ValueKind::Own, .rule = LayoutRule::Scalar, .bytes = 4 },
        { .kind = ValueKind::Borrow, .rule = LayoutRule::Scalar, .bytes = 4 },
        { .kind = ValueKind::Stream, .rule = LayoutRule::Scalar, .bytes = 4 },
        { .kind = ValueKind::Future, .rule = LayoutRule::Scalar, .bytes = 4 },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: LayoutRuleTable)
            {
                if (static_cast<std::size_t>(row.kind) != index)
                    return false;
                // A specialized kind takes the rule of what it despecializes to.
                auto const despecialized = types::valueKindRow(row.kind).despecialized;
                if (despecialized == Despecialized::Record && row.rule != LayoutRule::Record)
                    return false;
                if (despecialized == Despecialized::Variant && row.rule != LayoutRule::Variant)
                    return false;
                ++index;
            }
            return index == types::ValueKindCount;
        }(),
        "LayoutRuleTable must hold one row per ValueKind, in its order, despecialized as the arena says");

    /// The two pointer widths a layout is computed for: 32-bit addresses for the ABI (D14), 64-bit
    /// ones for the size limit only.
    constexpr auto PointerBytes = std::to_array<uint64_t>({ 4, 8 });

    /// A size and an alignment at one pointer width, wide enough that no sum of them wraps: every
    /// child is below MaxValueByteSize, a type has fewer than 2^32 children, so a sum stays below
    /// 2^60, and a fixed list's product of a 32-bit length and a child's size too.
    struct Footprint
    {
        uint64_t size = 0;
        uint64_t alignment = 1;
    };

    /// The footprints of one type, at each width of PointerBytes.
    using Footprints = std::array<Footprint, PointerBytes.size()>;

    /// definitions.py's align_to, for a power-of-two @p alignment.
    [[nodiscard]] constexpr uint64_t alignTo(uint64_t offset, uint64_t alignment) noexcept
    {
        return (offset + alignment - 1) / alignment * alignment;
    }

    /// The footprints a laid-out child has.
    [[nodiscard]] Footprints footprintsOf(Layout const& layout) noexcept
    {
        return Footprints { Footprint { .size = layout.size, .alignment = layout.alignment },
                            Footprint { .size = layout.size64, .alignment = layout.alignment64 } };
    }

    /// The bytes of a variant's discriminant (definitions.py's discriminant_type): u8 up to 256 cases,
    /// u16 up to 65'536, else u32.
    [[nodiscard]] constexpr uint8_t discriminantBytesFor(std::size_t cases) noexcept
    {
        if (cases <= std::size_t { 1 } << 8U)
            return 1;
        if (cases <= std::size_t { 1 } << 16U)
            return 2;
        return 4;
    }

    /// The bytes of a flags type (definitions.py's elem_size_flags), whose label count is in range.
    [[nodiscard]] constexpr uint8_t flagsBytesFor(std::size_t labels) noexcept
    {
        if (labels <= 8)
            return 1;
        if (labels <= 16)
            return 2;
        return 4;
    }

    /// definitions.py's join: the one flat type two cases share at a position.
    [[nodiscard]] constexpr CoreValueKind joined(CoreValueKind first, CoreValueKind second) noexcept
    {
        if (first == second)
            return first;
        auto const isWord = [](CoreValueKind kind) {
            return kind == CoreValueKind::I32 || kind == CoreValueKind::F32;
        };
        return isWord(first) && isWord(second) ? CoreValueKind::I32 : CoreValueKind::I64;
    }

    /// The most flat entries a layout keeps: one past what fits, so that Overflows is known.
    constexpr auto MaxFlatKept = std::size_t { MaxFlatParams } + 1;

    /// Lays out one type from its children's layouts, counting its work.
    class LayoutBuilder
    {
      public:
        LayoutBuilder(types::TypeId id, std::span<Layout const> children, uint64_t& steps) noexcept:
            _id { id }, _done { children }, _steps { steps }
        {
            ++_steps;
        }

        [[nodiscard]] std::expected<Layout, LayoutError> build(types::DefinedType const& type) &&
        {
            auto const& row = LayoutRuleTable[static_cast<std::size_t>(type.kind)];
            switch (row.rule)
            {
                case LayoutRule::Scalar: return scalar(row.bytes, row.flat);
                case LayoutRule::PointerPair: return pointerPair();
                case LayoutRule::Flags: return flags(type.labels.size());
                case LayoutRule::Record: return record(type.children);
                case LayoutRule::Variant:
                    return variant(row.cases != 0 ? row.cases : type.labels.size(), type.children);
                case LayoutRule::FixedList: return fixedList(type.children, type.length);
            }
            Require(false);
            return std::move(_layout); // uncovered(exhaustive-switch)
        }

      private:
        [[nodiscard]] Layout const& child(std::optional<types::TypeId> const& id)
        {
            // The arena interns a type after its children, so each is laid out already.
            Require(id.has_value() && id->value < _id.value && id->value < _done.size());
            ++_steps;
            auto const& layout = _done[id->value];
            if (layout.pointers == PointerContent::Held)
                _layout.pointers = PointerContent::Held;
            return layout;
        }

        /// Appends @p flat to the type's flat list, which stops after MaxFlatKept entries: the one
        /// place a flattening is truncated.
        void appendFlat(std::span<CoreValueKind const> flat)
        {
            for (auto const kind: flat.first(std::min(flat.size(), MaxFlatKept - _layout.flat.size())))
            {
                ++_steps;
                _layout.flat.push_back(kind);
            }
        }

        void pushFlat(CoreValueKind kind) { appendFlat(std::span { &kind, 1 }); }

        [[nodiscard]] std::expected<Layout, LayoutError> finish(Footprints const& footprints)
        {
            auto const& [narrow, wide] = footprints;
            if (wide.size >= MaxValueByteSize)
                return std::unexpected { LayoutError {
                    .code = LayoutErrorCode::ExceedsMaximumByteSize, .type = _id, .value = wide.size } };
            // Every narrow size is at most its wide one: pointers and their alignments only shrink.
            _layout.size = static_cast<uint32_t>(narrow.size);
            _layout.alignment = static_cast<uint32_t>(narrow.alignment);
            _layout.size64 = static_cast<uint32_t>(wide.size);
            _layout.alignment64 = static_cast<uint32_t>(wide.alignment);
            _layout.flatShape = _layout.flat.size() > MaxFlatParams ? FlatShape::Overflows : FlatShape::Fits;
            return std::move(_layout);
        }

        [[nodiscard]] static Footprints uniform(uint64_t bytes) noexcept
        {
            auto footprints = Footprints {};
            footprints.fill(Footprint { .size = bytes, .alignment = bytes });
            return footprints;
        }

        [[nodiscard]] std::expected<Layout, LayoutError> scalar(uint8_t bytes, CoreValueKind flat)
        {
            pushFlat(flat);
            return finish(uniform(bytes));
        }

        [[nodiscard]] std::expected<Layout, LayoutError> pointerPair()
        {
            _layout.pointers = PointerContent::Held;
            pushFlat(CoreValueKind::I32);
            pushFlat(CoreValueKind::I32);
            auto footprints = Footprints {};
            for (auto const width: std::views::iota(std::size_t { 0 }, PointerBytes.size()))
                footprints[width] =
                    Footprint { .size = 2 * PointerBytes[width], .alignment = PointerBytes[width] };
            return finish(footprints);
        }

        [[nodiscard]] std::expected<Layout, LayoutError> flags(std::size_t labels)
        {
            if (labels == 0 || labels > MaxFlagLabels)
                return std::unexpected { LayoutError {
                    .code = LayoutErrorCode::FlagsLabelCount, .type = _id, .value = labels } };
            pushFlat(CoreValueKind::I32);
            return finish(uniform(flagsBytesFor(labels)));
        }

        [[nodiscard]] std::expected<Layout, LayoutError> record(
            std::vector<std::optional<types::TypeId>> const& fields)
        {
            // definitions.py's elem_size_record and alignment_record, and flatten_record.
            auto footprints = Footprints {};
            _layout.fieldOffsets.reserve(fields.size());
            for (auto const& field: fields)
            {
                auto const& layout = child(field);
                auto const childFootprints = footprintsOf(layout);
                for (auto const width: std::views::iota(std::size_t { 0 }, PointerBytes.size()))
                {
                    auto& footprint = footprints[width];
                    footprint.size = alignTo(footprint.size, childFootprints[width].alignment)
                                     + childFootprints[width].size;
                    footprint.alignment = std::max(footprint.alignment, childFootprints[width].alignment);
                }
                // Refused as soon as it is too large, so that an offset always fits 32 bits.
                if (footprints.back().size >= MaxValueByteSize)
                    return finish(footprints);
                _layout.fieldOffsets.push_back(
                    static_cast<uint32_t>(footprints.front().size - childFootprints.front().size));
                appendFlat(layout.flat);
            }
            for (auto& footprint: footprints)
                footprint.size = alignTo(footprint.size, footprint.alignment);
            return finish(footprints);
        }

        [[nodiscard]] std::expected<Layout, LayoutError> variant(
            std::size_t cases, std::vector<std::optional<types::TypeId>> const& payloads)
        {
            // definitions.py's elem_size_variant, alignment_variant and flatten_variant: the payloads
            // of a variant's cases, an option's `some` and a result's `ok` and `error`; an enum has
            // none. Only the first MaxFlatParams joined positions are kept after the discriminant.
            _layout.discriminantBytes = discriminantBytesFor(cases);
            pushFlat(CoreValueKind::I32);
            auto payloadFootprints = Footprints {};
            for (auto const& payload: payloads)
            {
                if (!payload)
                    continue;
                auto const& layout = child(payload);
                auto const caseFootprints = footprintsOf(layout);
                for (auto const width: std::views::iota(std::size_t { 0 }, PointerBytes.size()))
                {
                    auto& footprint = payloadFootprints[width];
                    footprint.size = std::max(footprint.size, caseFootprints[width].size);
                    footprint.alignment = std::max(footprint.alignment, caseFootprints[width].alignment);
                }
                auto const shared = std::min(layout.flat.size(), _layout.flat.size() - 1);
                for (auto const position: std::views::iota(std::size_t { 0 }, shared))
                {
                    ++_steps;
                    _layout.flat[1 + position] = joined(_layout.flat[1 + position], layout.flat[position]);
                }
                appendFlat(std::span { layout.flat }.subspan(shared));
            }
            auto footprints = Footprints {};
            for (auto const width: std::views::iota(std::size_t { 0 }, PointerBytes.size()))
            {
                auto& footprint = footprints[width];
                auto const& payloadFootprint = payloadFootprints[width];
                footprint.alignment =
                    std::max(uint64_t { _layout.discriminantBytes }, payloadFootprint.alignment);
                auto const offset = alignTo(_layout.discriminantBytes, payloadFootprint.alignment);
                footprint.size = alignTo(offset + payloadFootprint.size, footprint.alignment);
            }
            _layout.payloadOffset = static_cast<uint32_t>(
                alignTo(_layout.discriminantBytes, payloadFootprints.front().alignment));
            return finish(footprints);
        }

        [[nodiscard]] std::expected<Layout, LayoutError> fixedList(
            std::vector<std::optional<types::TypeId>> const& element, uint32_t length)
        {
            // definitions.py's elem_size_list and alignment_list with a length, and flatten_list: the
            // element's flat types, length times, until MaxFlatKept.
            Require(element.size() == 1);
            auto const& layout = child(element.front());
            auto footprints = footprintsOf(layout);
            for (auto& footprint: footprints)
                footprint.size *= length;
            if (!layout.flat.empty())
            {
                for ([[maybe_unused]] auto const copy: std::views::iota(uint32_t { 0 }, length))
                {
                    if (_layout.flat.size() == MaxFlatKept)
                        break;
                    appendFlat(layout.flat);
                }
            }
            return finish(footprints);
        }

        types::TypeId _id;
        std::span<Layout const> _done;
        uint64_t& _steps;
        Layout _layout {};
    };
} // namespace

std::expected<void, LayoutError> LayoutTable::extend(types::TypeArena const& arena)
{
    Require(arena.id() == _arena);
    for (auto const index: std::views::iota(_layouts.size(), arena.valueTypeCount()))
    {
        auto const id = types::TypeId { static_cast<uint32_t>(index) };
        auto layout = LayoutBuilder { id, _layouts, _steps }.build(arena[id]);
        if (!layout)
            return std::unexpected { layout.error() };
        _layouts.push_back(std::move(*layout));
    }
    return {};
}

Layout const& LayoutTable::operator[](types::TypeId id) const
{
    Require(id.value < _layouts.size());
    return _layouts[id.value];
}

std::expected<LayoutTable, LayoutError> computeLayouts(types::TypeArena const& arena)
{
    auto table = LayoutTable { arena.id() };
    return table.extend(arena).transform([&] { return std::move(table); });
}

FlatShape flatParamsShape(LayoutTable const& layouts, types::FunctionTypeInfo const& function)
{
    auto count = std::size_t { 0 };
    for (auto const& [label, param]: function.params)
    {
        count += layouts[param].flat.size();
        if (count > MaxFlatParams)
            return FlatShape::Overflows;
    }
    return FlatShape::Fits;
}

CoreSignature flattenFunction(LayoutTable const& layouts,
                              types::FunctionTypeInfo const& function,
                              FlattenContext context)
{
    // definitions.py's flatten_functype, synchronous, with 32-bit addresses.
    Require(function.asyncness == binary::Asyncness::Sync);
    auto signature = CoreSignature {};
    if (flatParamsShape(layouts, function) == FlatShape::Overflows)
        signature.params = { CoreValueKind::I32 };
    else
    {
        for (auto const& [label, param]: function.params)
        {
            auto const& flat = layouts[param].flat;
            signature.params.insert(signature.params.end(), flat.begin(), flat.end());
        }
    }
    if (!function.result)
        return signature;
    auto const& flat = layouts[*function.result].flat;
    if (flat.size() <= MaxFlatResults)
        signature.results = flat;
    else if (context == FlattenContext::Lift)
        signature.results = { CoreValueKind::I32 };
    else
        signature.params.push_back(CoreValueKind::I32);
    return signature;
}

} // namespace isoline::component::canonical
