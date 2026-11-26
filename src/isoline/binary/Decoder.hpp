// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Module.hpp>

namespace isoline::binary
{

/// The implementation limits the decoder enforces (spec §9, Robustness), published so embedders
/// know them; exceeding one is ImplementationLimitExceeded, or TooManyLocals for maxLocals.
///
/// The defaults are those the JavaScript embedding fixes, which every browser engine shares.
struct DecodeLimits
{
    uint32_t maxTypes = 1'000'000;            ///< Entries of the type section.
    uint32_t maxFunctions = 1'000'000;        ///< Entries of the function section.
    uint32_t maxImports = 100'000;            ///< Entries of the import section.
    uint32_t maxExports = 100'000;            ///< Entries of the export section.
    uint32_t maxGlobals = 1'000'000;          ///< Entries of the global section.
    uint32_t maxTables = 100'000;             ///< Tables, imported and defined alike, as JS allows.
    uint32_t maxMemories = 100;               ///< Memories, imported and defined alike.
    uint32_t maxTags = 1'000'000;             ///< Entries of the tag section.
    uint32_t maxDataSegments = 100'000;       ///< Entries of the data section.
    uint32_t maxElementSegments = 10'000'000; ///< Entries of the element section.
    uint32_t maxElementItems = 10'000'000;    ///< Items of one element segment.
    uint32_t maxLocals = 50'000;              ///< Declared locals of one function, over all its runs.
    uint32_t maxParams = 1'000;               ///< Parameters of one function type.
    uint32_t maxResults = 1'000;              ///< Results of one function type.
    uint32_t maxFunctionBodySize = 7'654'321; ///< Bytes of one function body, its locals included.
    uint32_t maxBrTableSize = 65'520;         ///< Labels of one br_table.
    /// Catch clauses of one try_table: as many as br_table may name labels, as each clause names one.
    uint32_t maxCatchClauses = 65'520;
};

/// Decodes a binary module.
///
/// The decoder reports what the spec reference interpreter reports for the same bytes, so a module
/// is read as one stream: a section that declares fewer bytes than its contents take runs on into
/// what follows and fails with SectionSizeMismatch, and every length and vector count is bounded by
/// the bytes left in the module (ByteReader::readLength) before anything is reserved for it.
/// Proposals beyond the MVP decode only when @p features enables them.
/// @param bytes The module's bytes, from the magic number on.
/// @param features The proposals whose encodings are accepted. A set that enables a feature without
///                 one of its prerequisites (FeatureSet::checkPrerequisites) is refused with
///                 MissingFeaturePrerequisite at offset 0 before a byte is read: this is the one
///                 point a FeatureSet enters the engine, and the validator and runtime take the set
///                 decoding accepted.
/// @param limits The implementation limits to enforce.
/// @return The module, or the first error, at its absolute offset.
[[nodiscard]] std::expected<Module, DecodeError> decodeModule(std::span<std::byte const> bytes,
                                                              FeatureSet const& features,
                                                              DecodeLimits const& limits = {});

} // namespace isoline::binary
