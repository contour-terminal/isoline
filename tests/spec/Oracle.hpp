// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "Script.hpp"

namespace isoline::spectest
{

/// What the oracle says of one command of a `.wast` script, keyed by the command's ordinal.
struct OracleEntry
{
    /// Builds an entry that says nothing yet; the ordinal and the line are required, so one that
    /// forgets which command it is about does not compile.
    /// @param ordinal The command's index in the script, counted from 0.
    /// @param line The line wasm-tools gives the command, for diagnostics only.
    OracleEntry(std::size_t ordinal, std::size_t line): ordinal { ordinal }, line { line } {}

    std::size_t ordinal; ///< The command's index in the script, counted from 0.
    /// The line wasm-tools gives the command: an assertion's that of its inner `(module` or
    /// `(invoke`, so not always the command's own. For diagnostics only.
    std::size_t line;
    std::optional<std::string> wasm; ///< The oracle's binary of the command's module, beside the manifest.
    std::optional<std::string> skip; ///< Why the command is skipped: it is never compiled nor run.
    /// An expected divergence: the message Isoline reports for an assert_malformed's or an
    /// assert_malformed_custom's module text where it reports another of the text's errors first than
    /// the reference interpreter does. Isoline's message must then equal it exactly, at the stage the
    /// assertion expects, so the command still runs and the divergence stays on record (docs/conformance.md
    /// lists every one) rather than becoming a skip.
    std::optional<std::string> message;
    /// A parity divergence: where the oracle's binary of the command's module is known to be wrong, so
    /// that Isoline's canonical encoding differs from it. It is `0x<offset> sha256:<digest> <reason>`: the
    /// first canonical offset the two differ at, the SHA-256 of Isoline's canonical encoding, and why the
    /// oracle is wrong there. The parity check must then fail at exactly that offset with exactly that
    /// digest, so a divergence that goes away or moves fails as stale, and so does any later change to
    /// Isoline's encoding, before or behind the offset. A second difference already behind the offset
    /// when the row is recorded is not seen: whoever records it checks there is none. The command itself
    /// still runs. docs/conformance.md lists every one.
    std::optional<std::string> parity;

    bool operator==(OracleEntry const&) const = default;
};

/// A `.wast` script's oracle manifest: how many commands wasm-tools counts in it, and what it says of
/// some of them.
struct Oracle
{
    std::size_t commands = 0;                      ///< How many commands the script holds.
    std::map<std::size_t, OracleEntry> entries {}; ///< The entries, by ordinal.
};

/// Parses an `.oracle` manifest.
///
/// Its first line is `commands <n>`; every other line is `<ordinal> <line> wasm <file>`,
/// `<ordinal> <line> skip <hex reason>`, `<ordinal> <line> message <hex message>` or
/// `<ordinal> <line> parity <hex divergence>`, tokens separated by one space, a reason, message or
/// divergence hex-encoded UTF-8, and a divergence `0x<offset> sha256:<digest> <reason>`. One ordinal may have
/// one line of each kind, which must give the same line, except that a skipped command has no message and no
/// parity divergence. Empty lines are ignored.
/// @param manifest The manifest's text.
/// @return The oracle, or the first line that does not parse.
[[nodiscard]] std::expected<Oracle, ParseError> parseOracle(std::string_view manifest);

/// Checks that an oracle describes a script of @p commands commands: that both count the same, and that
/// every entry's ordinal is one of them. A script reader that splits or merges a command fails here,
/// rather than checking one command against another's binary.
/// @param oracle The oracle.
/// @param commands How many commands the script was read as.
/// @return Nothing, or why the two do not line up.
[[nodiscard]] std::expected<void, std::string> checkAlignment(Oracle const& oracle, std::size_t commands);

/// Checks that every entry of an oracle fits the command it lands on: a `wasm` entry a command whose
/// module is given as text (`quote` included), a `message` entry an assert_malformed or
/// assert_malformed_custom of such a module, with a message other than the script's own, and a `parity`
/// entry a command the oracle gives a binary of. A `skip` fits any command. Entries shifted onto the wrong
/// commands fail here rather than being ignored.
/// @param oracle The oracle, which checkAlignment() found to describe @p commands.
/// @param commands The script's commands.
/// @return Nothing, or the first entry that does not fit, and why.
[[nodiscard]] std::expected<void, std::string> checkEntries(Oracle const& oracle,
                                                            std::span<Command const> commands);

} // namespace isoline::spectest
