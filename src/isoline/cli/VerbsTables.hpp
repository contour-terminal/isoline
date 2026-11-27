// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <string_view>

#include <isoline/cli/Verbs.hpp>
#include <isoline/meta/EnumRows.hpp>
#include <isoline/text/ModuleWriter.hpp>

// The tables the verbs read (cli/Verbs.cpp): private, never installed, and a header of their own so
// that the reflection checks reach them (meta/EnumTables.def).

namespace isoline::cli
{

/// One row of WriteRefusals: the refusal a WriteError is, and its message, which may place the
/// text limit with `{}`.
struct WriteRefusal
{
    text::WriteError error;   ///< The writer's error, at its enumerator's index.
    Refusal refusal;          ///< What it refuses.
    std::string_view message; ///< The diagnostic after `error: `.
};

/// Every WriteError, as the refusal it is.
inline constexpr auto WriteRefusals = std::to_array<WriteRefusal>({
    { .error = text::WriteError::TextTooLarge,
      .refusal = Refusal::TextTooLarge,
      .message = "its text would be larger than the limit of {} bytes" },
    { .error = text::WriteError::PageSizeNotWritable,
      .refusal = Refusal::PageSizeNotWritable,
      .message = "memory page size 2^32 or more cannot be written as text" },
});

static_assert(meta::firstMisplacedRow<text::LastWriteError>(WriteRefusals, &WriteRefusal::error)
                  == meta::NoMisplacedRow,
              "WriteRefusals must hold one row per WriteError, at the index of its enumerator");

} // namespace isoline::cli
