// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>

#include <isoline/Features.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>

// The binary format's encodings, as tables: what the decoder reads and the encoder writes, so that one
// is the other's inverse by construction. Adding an encoding is adding a row here.

namespace isoline::binary
{

/// The magic number every module starts with, "\0asm", read as a little-endian word.
inline constexpr auto ModuleMagic = uint32_t { 0x6D73'6100 };

/// The only binary format version there is.
inline constexpr auto BinaryVersion = uint32_t { 1 };

/// The id of a custom section, which may appear anywhere.
inline constexpr auto CustomSectionId = uint8_t { 0 };

/// The byte that introduces a function type in the type section.
inline constexpr auto FuncTypeForm = uint8_t { 0x60 };

/// Whether a type byte is a whole type, or the start of a reference whose heap type follows.
enum class TypeForm : uint8_t
{
    Complete,    ///< The byte is the type.
    HeapFollows, ///< A heap type follows; the row's type gives the reference's nullability.
};

/// One encoding of a type byte: the byte, the type it stands for, and the proposal it needs.
struct TypeEncoding
{
    uint8_t byte;                       ///< The byte, an s7 in the binary grammar.
    ValueType type;                     ///< The type it encodes; its heap read on for HeapFollows.
    std::optional<Feature> required;    ///< The proposal that introduced it, unless it is MVP.
    TypeForm form = TypeForm::Complete; ///< Whether a heap type follows.
};

/// The reference types WebAssembly 3.0 adds beyond funcref and externref, wherever a reference
/// type may stand: exception handling's shorthands, and typed function references' (ref ht) and
/// (ref null ht), whose heap type follows. The GC proposal's shorthands are no type here.
inline constexpr auto Wasm3ReferenceEncodings = std::to_array<TypeEncoding>({
    { .byte = 0x69, .type = ExnRef, .required = Feature::ExceptionHandling },
    { .byte = 0x74, .type = NullExnRef, .required = Feature::ExceptionHandling },
    { .byte = 0x64,
      .type = ValueType { .kind = ValueKind::Ref, .nullable = Nullable::No },
      .required = Feature::TypedFunctionReferences,
      .form = TypeForm::HeapFollows },
    { .byte = 0x63,
      .type = ValueType { .kind = ValueKind::Ref, .nullable = Nullable::Yes },
      .required = Feature::TypedFunctionReferences,
      .form = TypeForm::HeapFollows },
});

/// @p head followed by Wasm3ReferenceEncodings; for the tables below, at compile time only.
template <std::size_t N>
[[nodiscard]] consteval auto withWasm3References(std::array<TypeEncoding, N> const& head) noexcept
{
    auto all = std::array<TypeEncoding, N + Wasm3ReferenceEncodings.size()> {};
    auto out = all.begin();
    for (auto const& row: head)
        *out++ = row;
    for (auto const& row: Wasm3ReferenceEncodings)
        *out++ = row;
    return all;
}

/// Every value type decoded.
inline constexpr auto ValueTypeEncodings = withWasm3References(std::to_array<TypeEncoding>({
    { .byte = 0x7F, .type = I32, .required = std::nullopt },
    { .byte = 0x7E, .type = I64, .required = std::nullopt },
    { .byte = 0x7D, .type = F32, .required = std::nullopt },
    { .byte = 0x7C, .type = F64, .required = std::nullopt },
    { .byte = 0x7B, .type = V128, .required = Feature::Simd },
    { .byte = 0x70, .type = FuncRef, .required = Feature::ReferenceTypes },
    { .byte = 0x6F, .type = ExternRef, .required = Feature::ReferenceTypes },
}));

/// Every reference type decoded where only a reference type may stand: funcref was a table's
/// element type in the MVP already.
inline constexpr auto RefTypeEncodings = withWasm3References(std::to_array<TypeEncoding>({
    { .byte = 0x70, .type = FuncRef, .required = std::nullopt },
    { .byte = 0x6F, .type = ExternRef, .required = Feature::ReferenceTypes },
}));

/// One abstract heap type's code, as ref.null and a (ref ht) or (ref null ht) give it.
struct HeapEncoding
{
    uint8_t byte;                    ///< The code, an s7: a negative s33.
    AbstractHeapType kind;           ///< The abstract heap type it names.
    std::optional<Feature> required; ///< The proposal that introduced it, unless it is MVP.
};

/// Every abstract heap type decoded: func and extern, and exception handling's exn and noexn. The
/// GC heaps (any, eq, i31, struct, array, none, nofunc, noextern) are malformed without GC, as
/// the pinned wasm-tools has them.
inline constexpr auto AbstractHeapEncodings = std::to_array<HeapEncoding>({
    { .byte = 0x70, .kind = AbstractHeapType::Func, .required = std::nullopt },
    { .byte = 0x6F, .kind = AbstractHeapType::Extern, .required = Feature::ReferenceTypes },
    { .byte = 0x69, .kind = AbstractHeapType::Exn, .required = Feature::ExceptionHandling },
    { .byte = 0x74, .kind = AbstractHeapType::NoExn, .required = Feature::ExceptionHandling },
});

/// The byte that announces a table with an initializer (typed function references), followed
/// by a zero byte, the table type and the initializer.
inline constexpr auto TableWithInitializer = uint8_t { 0x40 };

/// What an import or an export names with one kind byte, and the proposal that introduced it.
struct ExternKindRow
{
    ExternKind kind;                 ///< The kind of entity; its enumerator is the byte.
    std::optional<Feature> required; ///< The proposal that introduced it, unless it is MVP.
};

/// What an import, or an export, names, by its kind byte.
inline constexpr auto ExternKinds = std::to_array<ExternKindRow>({
    { .kind = ExternKind::Func, .required = std::nullopt },
    { .kind = ExternKind::Table, .required = std::nullopt },
    { .kind = ExternKind::Memory, .required = std::nullopt },
    { .kind = ExternKind::Global, .required = std::nullopt },
    { .kind = ExternKind::Tag, .required = Feature::ExceptionHandling },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ExternKinds)
            if (static_cast<std::size_t>(row.kind) != index++)
                return false;
        return true;
    }(),
    "ExternKinds holds one row per kind byte, at the index of the ExternKind it decodes to");

/// What a try_table's catch clause is, by its kind byte.
inline constexpr auto CatchKinds = std::to_array<CatchKind>({
    CatchKind::Catch,
    CatchKind::CatchRef,
    CatchKind::CatchAll,
    CatchKind::CatchAllRef,
});

/// Whether a segment names its table or memory, or uses index 0 implicitly.
enum class IndexEncoding : uint8_t
{
    Implicit, ///< No index is encoded; the segment initializes table or memory 0.
    Explicit, ///< A u32 index precedes the offset expression.
};

/// How an element segment encodes the type of its items.
enum class ElementTypeEncoding : uint8_t
{
    Implicit, ///< Not encoded: (ref func) for function indices, funcref for expressions.
    ElemKind, ///< An element kind byte, of which 0x00 (ref func) is the only one.
    RefType,  ///< A reference type.
};

/// How an element segment encodes its items.
enum class ItemEncoding : uint8_t
{
    FunctionIndices, ///< Function indices, each standing for (ref.func index).
    Expressions,     ///< Constant expressions.
};

/// How an element segment's flags shape it (WebAssembly 2.0, §5.5.12).
struct ElementForm
{
    SegmentMode mode;                ///< How the segment is used.
    IndexEncoding table;             ///< Whether a table index precedes the offset.
    ElementTypeEncoding type;        ///< How the item type is encoded.
    ItemEncoding items;              ///< How the items are encoded.
    std::optional<Feature> required; ///< The proposal that introduced the form.
};

/// Every element segment form, indexed by its flags.
inline constexpr auto ElementForms = std::to_array<ElementForm>({
    { SegmentMode::Active,
      IndexEncoding::Implicit,
      ElementTypeEncoding::Implicit,
      ItemEncoding::FunctionIndices,
      std::nullopt },
    { SegmentMode::Passive,
      IndexEncoding::Implicit,
      ElementTypeEncoding::ElemKind,
      ItemEncoding::FunctionIndices,
      Feature::BulkMemory },
    { SegmentMode::Active,
      IndexEncoding::Explicit,
      ElementTypeEncoding::ElemKind,
      ItemEncoding::FunctionIndices,
      Feature::BulkMemory },
    { SegmentMode::Declarative,
      IndexEncoding::Implicit,
      ElementTypeEncoding::ElemKind,
      ItemEncoding::FunctionIndices,
      Feature::ReferenceTypes },
    { SegmentMode::Active,
      IndexEncoding::Implicit,
      ElementTypeEncoding::Implicit,
      ItemEncoding::Expressions,
      Feature::BulkMemory },
    { SegmentMode::Passive,
      IndexEncoding::Implicit,
      ElementTypeEncoding::RefType,
      ItemEncoding::Expressions,
      Feature::BulkMemory },
    { SegmentMode::Active,
      IndexEncoding::Explicit,
      ElementTypeEncoding::RefType,
      ItemEncoding::Expressions,
      Feature::BulkMemory },
    { SegmentMode::Declarative,
      IndexEncoding::Implicit,
      ElementTypeEncoding::RefType,
      ItemEncoding::Expressions,
      Feature::ReferenceTypes },
});

/// How a data segment's flags shape it.
struct DataForm
{
    SegmentMode mode;                ///< How the segment is used.
    IndexEncoding memory;            ///< Whether a memory index precedes the offset.
    std::optional<Feature> required; ///< The proposal that introduced the form.
};

/// Every data segment form, indexed by its flags. Flags 2 was the MVP's memory index 2, which
/// no MVP module could validly use.
inline constexpr auto DataForms = std::to_array<DataForm>({
    { SegmentMode::Active, IndexEncoding::Implicit, std::nullopt },
    { SegmentMode::Passive, IndexEncoding::Implicit, Feature::BulkMemory },
    { SegmentMode::Active, IndexEncoding::Explicit, Feature::BulkMemory },
});

/// Which bounds a limits flag announces.
enum class LimitsBound : uint8_t
{
    Minimum,           ///< A minimum only.
    MinimumAndMaximum, ///< A minimum, then a maximum.
};

/// Whether a page size exponent follows a limits flag's bounds (custom-page-sizes).
enum class PageSizeField : uint8_t
{
    Default,  ///< None: the page size is 64 KiB.
    Exponent, ///< A u32 exponent, the page size's base-2 logarithm; memories only.
};

/// One limits flag byte WebAssembly 3.0 admits: what follows it, and the proposals it needs.
struct LimitsFlagRow
{
    uint8_t flags;           ///< The flag byte.
    LimitsBound bound;       ///< The bounds that follow.
    AddressType addressType; ///< The address type of the memory or table.
    PageSizeField pageSize;  ///< Whether a page size follows the bounds.
    FeatureSet required;     ///< The proposals that admit it; none for the MVP's flags.
};

namespace detail
{
    inline constexpr auto NeedsMemory64 = FeatureSet::none().with(Feature::Memory64);
    inline constexpr auto NeedsPageSizes = FeatureSet::none().with(Feature::CustomPageSizes);
    inline constexpr auto NeedsMemory64AndPageSizes = NeedsMemory64.with(Feature::CustomPageSizes);
} // namespace detail

/// The limits flags of tables and memories: bit 0 a maximum, bit 2 i64 addresses, bit 3 a custom
/// page size (memories only). Any other byte is MalformedLimitsFlags; in WebAssembly 2.0's grammar
/// only the first two rows exist.
inline constexpr auto LimitsFlagTable = std::to_array<LimitsFlagRow>({
    { .flags = 0x00,
      .bound = LimitsBound::Minimum,
      .addressType = AddressType::I32,
      .pageSize = PageSizeField::Default,
      .required = FeatureSet::none() },
    { .flags = 0x01,
      .bound = LimitsBound::MinimumAndMaximum,
      .addressType = AddressType::I32,
      .pageSize = PageSizeField::Default,
      .required = FeatureSet::none() },
    { .flags = 0x04,
      .bound = LimitsBound::Minimum,
      .addressType = AddressType::I64,
      .pageSize = PageSizeField::Default,
      .required = detail::NeedsMemory64 },
    { .flags = 0x05,
      .bound = LimitsBound::MinimumAndMaximum,
      .addressType = AddressType::I64,
      .pageSize = PageSizeField::Default,
      .required = detail::NeedsMemory64 },
    { .flags = 0x08,
      .bound = LimitsBound::Minimum,
      .addressType = AddressType::I32,
      .pageSize = PageSizeField::Exponent,
      .required = detail::NeedsPageSizes },
    { .flags = 0x09,
      .bound = LimitsBound::MinimumAndMaximum,
      .addressType = AddressType::I32,
      .pageSize = PageSizeField::Exponent,
      .required = detail::NeedsPageSizes },
    { .flags = 0x0C,
      .bound = LimitsBound::Minimum,
      .addressType = AddressType::I64,
      .pageSize = PageSizeField::Exponent,
      .required = detail::NeedsMemory64AndPageSizes },
    { .flags = 0x0D,
      .bound = LimitsBound::MinimumAndMaximum,
      .addressType = AddressType::I64,
      .pageSize = PageSizeField::Exponent,
      .required = detail::NeedsMemory64AndPageSizes },
});

static_assert(
    [] {
        auto index = uint8_t { 0 };
        for (auto const& row: LimitsFlagTable | std::views::take(2))
            if (row.flags != index++ || row.required != FeatureSet::none()
                || row.pageSize != PageSizeField::Default)
                return false;
        return true;
    }(),
    "LimitsFlagTable's first two rows are WebAssembly 2.0's flags 0x00 and 0x01, indexed by them");

/// The block type byte of a block without a result.
inline constexpr auto EmptyBlockType = uint8_t { 0x40 };

/// The byte that marks a global immutable.
inline constexpr auto ImmutableGlobal = uint8_t { 0x00 };

/// The byte that marks a global mutable.
inline constexpr auto MutableGlobal = uint8_t { 0x01 };

/// The attribute byte of a tag type: an exception, the only attribute there is.
inline constexpr auto ExceptionAttribute = uint8_t { 0x00 };

/// The element kind byte of function indices, (ref func): the only element kind there is.
inline constexpr auto FuncElementKind = uint8_t { 0x00 };

/// The reserved zero byte: a memory index before multi-memory, a table index in call_indirect before
/// reference types, and the byte after TableWithInitializer.
inline constexpr auto ReservedZeroByte = uint8_t { 0x00 };

/// The memarg flag that says a u32 memory index follows the alignment (multi-memory): bit 6.
inline constexpr auto MemArgMemoryIndexFlag = uint32_t { 0b0100'0000 };

/// One non-custom section: its id, the anchor a custom section after it records, and the proposal
/// that added it.
struct SectionEncoding
{
    uint8_t id;                      ///< The section id.
    SectionAnchor anchor;            ///< The section, as CustomSection::after names it.
    std::optional<Feature> required; ///< The proposal that added it, unless it is MVP.
};

/// Every non-custom section, in the order a module must list them; each appears at most once.
inline constexpr auto SectionEncodings = std::to_array<SectionEncoding>({
    { .id = 1, .anchor = SectionAnchor::Type, .required = std::nullopt },
    { .id = 2, .anchor = SectionAnchor::Import, .required = std::nullopt },
    { .id = 3, .anchor = SectionAnchor::Function, .required = std::nullopt },
    { .id = 4, .anchor = SectionAnchor::Table, .required = std::nullopt },
    { .id = 5, .anchor = SectionAnchor::Memory, .required = std::nullopt },
    { .id = 13, .anchor = SectionAnchor::Tag, .required = Feature::ExceptionHandling },
    { .id = 6, .anchor = SectionAnchor::Global, .required = std::nullopt },
    { .id = 7, .anchor = SectionAnchor::Export, .required = std::nullopt },
    { .id = 8, .anchor = SectionAnchor::Start, .required = std::nullopt },
    { .id = 9, .anchor = SectionAnchor::Element, .required = std::nullopt },
    { .id = 12, .anchor = SectionAnchor::DataCount, .required = Feature::BulkMemory },
    { .id = 10, .anchor = SectionAnchor::Code, .required = std::nullopt },
    { .id = 11, .anchor = SectionAnchor::Data, .required = std::nullopt },
});

static_assert(
    [] {
        auto anchor = std::to_underlying(SectionAnchor::BeforeFirst);
        for (auto const& row: SectionEncodings)
            if (std::to_underlying(row.anchor) != ++anchor)
                return false;
        return anchor == std::to_underlying(SectionAnchor::Data);
    }(),
    "SectionEncodings lists one row per SectionAnchor after BeforeFirst, in the anchors' order");

} // namespace isoline::binary
