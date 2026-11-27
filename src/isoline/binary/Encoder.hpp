// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>

namespace isoline::binary
{

/// Where a custom section that is built rather than kept verbatim goes.
enum class SynthesizedPlacement : uint8_t
{
    BeforeCode, ///< Immediately before the code section, after every custom section of that slot.
    Last,       ///< At the module's end, after every other section, custom ones included.
};

/// One custom section the encoder, the text reader or the writer builds from a Module's fields.
struct SynthesizedSection
{
    std::string_view name;          ///< The section's name.
    SynthesizedPlacement placement; ///< Where it goes.
};

/// The custom sections built from a Module's fields, and where each goes; the one placement rule the
/// encoder, the text reader and the writer share.
///
/// The branch hint section is built from Module::branchHints, before the code whose offsets it names.
/// The name section, built from text identifiers, goes after every custom section, as wasm-tools
/// places it.
inline constexpr auto SynthesizedSections = std::to_array<SynthesizedSection>({
    { .name = BranchHintSectionName, .placement = SynthesizedPlacement::BeforeCode },
    { .name = NameSectionName, .placement = SynthesizedPlacement::Last },
});

/// Encodes a module in the binary format: the inverse of decodeModule.
///
/// Every instruction is written from its OpcodeTable row: its prefix and code, then its immediates
/// by the row's ImmediateKind. Every LEB128 takes its shortest form. Each section is written when it
/// has entries (the start and data count sections when set), in section order, and each custom
/// section right after its anchor's slot (CustomSection::after). Imports are written one plain entry
/// per Import, whatever compact grouping they were read from; a memarg sets its memory index flag
/// only for a memory other than 0; an element or data segment takes the smallest flag form that
/// expresses it.
///
/// Unless Module::customs holds a branch hint section already, Module::branchHints are written as
/// one, at the place SynthesizedSections gives it, with the offset of every hinted instruction,
/// whatever its opcode: a hint on an instruction that is no if or br_if encodes, and the decoder then
/// refuses the section.
///
/// Preconditions (checked): every br_table, typed select and try_table of a function body has its
/// run within its function's side table; every v128.const and i8x16.shuffle its literal within
/// Module::v128Literals; every import its module name within Module::importModules; every branch
/// hint a defined function and an instruction of its body; every type, limits and segment an
/// encoding the decoder reads (Encodings.hpp): no GC heap type, an element segment of function
/// indices typed (ref func), no declarative data segment. Constant expressions have no side tables:
/// a br_table, typed select or try_table in one is written with an empty run.
/// @param module The module to encode.
/// @return Its bytes, from the magic number on. They are linear in the module's size but for the
///         import section, which repeats an import's module name for every item of a compact group
///         (Module::importModules holds it once): bound that with the overload taking a byte limit.
[[nodiscard]] std::vector<std::byte> encodeModule(Module const& module);

/// Why encodeModule refused to encode a module.
enum class EncodeError : uint8_t
{
    ModuleTooLarge, ///< Its bytes would exceed the limit given.
};

/// Encodes a module as encodeModule(module) does, refusing it once its bytes would exceed @p maxBytes.
///
/// The limit is checked as the bytes are written, per import entry, function body and section, so a
/// module whose encoding would be far larger than itself is refused having held about @p maxBytes,
/// never its whole encoding.
/// @param module The module to encode.
/// @param maxBytes The most bytes the encoding may take.
/// @return Its bytes, or ModuleTooLarge.
[[nodiscard]] std::expected<std::vector<std::byte>, EncodeError> encodeModule(Module const& module,
                                                                              std::size_t maxBytes);

/// The payload of the branch hint section encodeModule synthesizes from Module::branchHints: every
/// hint at the offset encodeModule gives its instruction, relative to its function's locals
/// declaration.
///
/// Only the hinted functions are encoded, one at a time, so it holds no more than the largest of
/// their bodies and the payload, never the module's encoding.
///
/// Preconditions (checked): as encodeModule's, for the hinted functions and the hints.
/// @param module The module, its branch hints sorted or not.
/// @return The payload; empty without hints.
[[nodiscard]] std::vector<std::byte> encodeBranchHints(Module const& module);

/// The anchor a custom section anchored at @p anchor is written after: the last non-custom section,
/// at or before @p anchor in section order, that encodeModule writes for @p module, as the decoder of
/// the encoding records it.
/// @param module The module.
/// @param anchor The anchor asked for.
/// @return @p anchor when encodeModule writes that section; otherwise the last section before it that
///         it writes, or BeforeFirst when it writes none.
[[nodiscard]] SectionAnchor writtenAnchor(Module const& module, SectionAnchor anchor) noexcept;

/// @p module with the encoding choices that do not change its meaning made canonical, so that two
/// modules that differ only in how they were encoded compare equal:
/// - a data count section that is redundant (its count equals the data segments' while no function
///   body holds a memory.init or data.drop) is dropped;
/// - local runs of no locals are dropped, and adjacent runs of one type merged;
/// - a custom section anchored to a section encodeModule does not write is anchored to the last
///   section before it that it does write, where encodeModule places it anyway;
/// - custom sections are ordered as encodeModule writes them: by anchor, in module order within one;
/// - a branch hint section the decoder read whole is dropped, as Module::branchHints hold what it says,
///   and encodeModule writes it again at the offsets the encoding gives the hinted instructions, at its
///   SynthesizedSections place (before the code section, after that slot's customs) wherever the raw
///   one sat; one the decoder refused stays verbatim, as hints are advisory and a refused one has
///   nothing to re-derive. Its offsets still name the original encoding's bytes, so once the code is
///   re-encoded they may name other instructions, valid ones included: such a section can change
///   meaning, and canonicalize is then not idempotent, as the next pass may read it whole and rewrite it.
/// - a name section's payload is replaced by encodeNameSection(decodeNameSection(payload)) when it
///   decodes, which drops what the codec skips (field names, unknown, out-of-order and repeated
///   subsections) and empty subsections, and writes every map by ascending index in shortest
///   LEB128s; one that does not decode stays verbatim, as names are advisory.
/// @param module The module, as decoded or as read from text.
/// @return The canonical module.
[[nodiscard]] Module normalize(Module module);

/// The canonical encoding of a binary module: encodeModule(normalize(decodeModule(bytes))). Two
/// modules have the same canonical encoding exactly when they differ only in encoding choices: LEB128
/// widths, segment flag forms, compact import grouping, a redundant data count section and how locals
/// are grouped into runs. In a name section that decodes, the differences canonicalized away are the
/// subsections decodeNameSection skips (the GC proposal's field names, id 10; ids without a
/// NameSubsections row, 14 and above; one whose id is not above every id before it, out of order or
/// repeated), empty subsections, and LEB128 widths. A name section that does not decode is compared
/// byte for byte.
/// @param bytes The module's bytes.
/// @param features The proposals whose encodings are accepted.
/// @return The canonical bytes, or the error that decoding @p bytes reports.
[[nodiscard]] std::expected<std::vector<std::byte>, DecodeError> canonicalize(
    std::span<std::byte const> bytes, FeatureSet const& features);

} // namespace isoline::binary
