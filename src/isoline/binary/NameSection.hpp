// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/DecodeError.hpp>
#include <isoline/meta/EnumRows.hpp>

// The name section (WebAssembly 3.0, appendix "Name Section"): debug names for a module's entities.
// It is advisory: a module whose name section is malformed is still a valid module, so the decoder
// keeps the section verbatim and nothing but the code that wants names reads it, through
// decodeNameSection.

namespace isoline::binary
{

/// The name of the custom section that holds a module's names.
inline constexpr auto NameSectionName = std::string_view { "name" };

/// What a name subsection names; the index of its NameSubsections row.
enum class NameKind : uint8_t
{
    Module,       ///< The module itself.
    Function,     ///< Functions, imported ones included.
    Local,        ///< Each function's locals, its parameters included.
    Label,        ///< Each function's block, loop, if and try_table labels.
    Type,         ///< Types.
    Table,        ///< Tables.
    Memory,       ///< Memories.
    Global,       ///< Globals.
    Element,      ///< Element segments.
    Data,         ///< Data segments.
    Tag,          ///< Tags.
    Parameter,    ///< Each function type's parameters, as wasm-tools names them (subsection 12).
    TagParameter, ///< Each tag's parameters, as wasm-tools names them (subsection 13).
};

/// The last NameKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastNameKind = NameKind::TagParameter;

/// The number of NameKind enumerators: the size of NameSection's per-kind arrays.
inline constexpr auto NameKindCount = std::size_t { std::to_underlying(LastNameKind) + 1U };

/// How a name subsection lays out its names.
enum class NameShape : uint8_t
{
    Single,   ///< One name.
    Direct,   ///< A name map: an index and a name per entry.
    Indirect, ///< An indirect name map: an index and a name map per entry.
};

/// The last NameShape enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastNameShape = NameShape::Indirect;

/// One subsection of the name section: what it names, its id, and its layout.
struct NameSubsection
{
    NameKind kind;   ///< What it names; equal to the row's index.
    uint8_t id;      ///< Its subsection id.
    NameShape shape; ///< How its contents are laid out.
};

/// Every name subsection Isoline reads, one row per NameKind, in id order: the spec's 0-2, the
/// extended name section's 3-9 and 11, and the parameter names 12 and 13 the pinned wasm-tools
/// (1.263.0) writes. Id 10, the GC proposal's field names, is not read: it is skipped as an unknown
/// id is.
inline constexpr auto NameSubsections = std::to_array<NameSubsection>({
    { .kind = NameKind::Module, .id = 0, .shape = NameShape::Single },
    { .kind = NameKind::Function, .id = 1, .shape = NameShape::Direct },
    { .kind = NameKind::Local, .id = 2, .shape = NameShape::Indirect },
    { .kind = NameKind::Label, .id = 3, .shape = NameShape::Indirect },
    { .kind = NameKind::Type, .id = 4, .shape = NameShape::Direct },
    { .kind = NameKind::Table, .id = 5, .shape = NameShape::Direct },
    { .kind = NameKind::Memory, .id = 6, .shape = NameShape::Direct },
    { .kind = NameKind::Global, .id = 7, .shape = NameShape::Direct },
    { .kind = NameKind::Element, .id = 8, .shape = NameShape::Direct },
    { .kind = NameKind::Data, .id = 9, .shape = NameShape::Direct },
    { .kind = NameKind::Tag, .id = 11, .shape = NameShape::Direct },
    { .kind = NameKind::Parameter, .id = 12, .shape = NameShape::Indirect },
    { .kind = NameKind::TagParameter, .id = 13, .shape = NameShape::Indirect },
});

static_assert(
    meta::firstMisplacedRow<LastNameKind>(NameSubsections, &NameSubsection::kind) == meta::NoMisplacedRow
        && std::ranges::adjacent_find(NameSubsections, std::ranges::greater_equal {}, &NameSubsection::id)
               == NameSubsections.end(),
    "NameSubsections must hold one row per NameKind, at the index of its enumerator, in id order");

/// One entry of a name map: an entity's index and its name.
struct NameAssoc
{
    uint32_t index = 0; ///< The entity's index in its index space.
    std::string name;   ///< Its name, valid UTF-8.

    bool operator==(NameAssoc const&) const = default;
};

/// One entry of an indirect name map: an outer entity's index (a function's) and the names within it.
struct IndirectNameMap
{
    uint32_t index = 0;           ///< The outer entity's index.
    std::vector<NameAssoc> names; ///< The names of its inner entities; decoded ones by ascending index.

    bool operator==(IndirectNameMap const&) const = default;
};

/// A decoded name section: every name it gives, by NameKind.
struct NameSection
{
    std::optional<std::string> module; ///< The module's name, if the section gives one.
    /// The name maps of the Direct subsections, indexed by NameKind; the other rows are unused, and
    /// must stay empty.
    std::array<std::vector<NameAssoc>, NameKindCount> direct;
    /// The indirect name maps of the Indirect subsections (locals, labels and parameters), indexed by
    /// NameKind; the other rows are unused, and must stay empty.
    std::array<std::vector<IndirectNameMap>, NameKindCount> indirect;

    bool operator==(NameSection const&) const = default;
};

/// Decodes a name section's payload.
///
/// A subsection whose id has no NameSubsections row is skipped, as custom sections tolerate what they
/// do not know, and so is one whose id is not above every id before it (out of order, or repeated).
/// A skipped subsection's contents are not read. Every other malformation makes the whole section
/// invalid: a map whose indices do not increase strictly, a name that is not UTF-8, or contents that
/// do not fill their subsection exactly. Memory is linear in the payload: every count is bounded by
/// the bytes left (ByteReader::readLength) and nothing is reserved ahead of what is read.
/// @param payload The section's bytes after its name.
/// @return The names, or the first error, at its offset within @p payload: NameIndexOutOfOrder,
///         MalformedUtf8Encoding, NameSubsectionSizeMismatch, or UnexpectedEnd and the other errors of
///         ByteReader.
[[nodiscard]] std::expected<NameSection, DecodeError> decodeNameSection(std::span<std::byte const> payload);

/// Encodes a name section's payload: the inverse of decodeNameSection on what it accepts.
///
/// Subsections are written in id order, those with nothing to name omitted, and every map by
/// ascending index, whatever order it is held in; every LEB128 takes its shortest form.
///
/// Preconditions (checked): no map holds one index twice; every name is valid UTF-8; the rows of
/// NameSection::direct and NameSection::indirect whose kind the subsection table gives another shape
/// are empty.
/// @param names The names to encode.
/// @return The payload, to follow the section's name; linear in the names' size.
[[nodiscard]] std::vector<std::byte> encodeNameSection(NameSection const& names);

} // namespace isoline::binary
