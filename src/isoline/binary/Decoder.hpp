// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <isoline/Features.hpp>
#include <isoline/binary/ByteReader.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Module.hpp>

namespace isoline::binary
{

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

/// Reads one function type (`0x60`, its parameters, its results) where another binary embeds the core
/// grammar: a component's core type definitions and module type declarations.
///
/// The type is read as the type section reads it, with the same errors at the same absolute offsets.
/// @param reader Over the embedding binary, at the type's form byte; advanced past the type when it
///               decodes, and left where it was when it does not.
/// @param features The proposals whose encodings are accepted, closed over their prerequisites as
///                 decodeModule() requires.
/// @param limits The implementation limits to enforce: maxParams and maxResults.
/// @return The type, or the first error, at its absolute offset.
[[nodiscard]] std::expected<FuncType, DecodeError> decodeEmbeddedFuncType(ByteReader& reader,
                                                                          FeatureSet const& features,
                                                                          DecodeLimits const& limits = {});

/// Reads one extern type (`externtype`: a kind byte, then the function type index, table, memory,
/// global or tag type it names) where another binary embeds the core grammar: the imports and exports
/// a component's core module type declares.
///
/// The type is read as the import section reads one after the names.
/// @param reader Over the embedding binary, at the kind byte; advanced past the type when it decodes,
///               and left where it was when it does not.
/// @param features The proposals whose encodings are accepted, closed over their prerequisites as
///                 decodeModule() requires.
/// @param limits The implementation limits to enforce.
/// @return The type as an import with empty names, or the first error, at its absolute offset.
[[nodiscard]] std::expected<Import, DecodeError> decodeEmbeddedExternType(ByteReader& reader,
                                                                          FeatureSet const& features,
                                                                          DecodeLimits const& limits = {});

} // namespace isoline::binary
