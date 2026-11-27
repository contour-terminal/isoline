// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

#include <isoline/api/Engine.hpp>

namespace isoline::text
{

/// The caps on what reading text may grow to, given to every reader of the text format when it is
/// constructed and fixed thereafter.
///
/// Every resource reading text holds grows with the source, linearly; these caps bound the ones that
/// would otherwise be bounded by the source alone, so that an embedder decides how much a source may
/// cost.
struct TextLimits
{
    /// The largest source accepted, in bytes. Required below 2^32: tokens and nodes keep 32-bit offsets.
    std::size_t maxSourceBytes = std::size_t { 1 } << 30;

    /// The deepest nesting of parenthesized lists accepted.
    uint32_t maxNesting = 10'000;

    /// The longest numeric token accepted, in bytes; it bounds the exact arithmetic of a literal.
    uint32_t maxNumberLength = 4'096;

    /// The largest binary module compiling may produce, in bytes. Output is linear in the source but for
    /// a compact import group, whose module name the binary repeats for every item, so a small source
    /// can ask for gigabytes; the encoder refuses it as it reaches this limit. By default the most an
    /// api::Engine loads (api::LoadLimits::maxModuleBytes, 64 MiB): a larger module is of no use.
    std::size_t maxModuleBytes = static_cast<std::size_t>(api::LoadLimits {}.maxModuleBytes);
};

} // namespace isoline::text
