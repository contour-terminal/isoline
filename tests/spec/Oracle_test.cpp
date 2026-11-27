// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "Oracle.hpp"
#include "Script.hpp"
#include "WastReader.hpp"
#include <isoline/text/TextLimits.hpp>

using isoline::spectest::checkAlignment;
using isoline::spectest::checkEntries;
using isoline::spectest::Command;
using isoline::spectest::Oracle;
using isoline::spectest::OracleEntry;
using isoline::spectest::parseOracle;

namespace
{

/// The hex-encoded UTF-8 of @p text, as a manifest spells a value.
std::string hexOf(std::string_view text)
{
    auto hex = std::string {};
    for (auto const character: text)
        hex += std::format("{:02x}", static_cast<unsigned char>(character));
    return hex;
}

/// A well-formed parity divergence's digest.
constexpr auto Digest =
    std::string_view { "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" };

/// A well-formed parity divergence at @p offset.
std::string divergenceAt(std::string_view offset)
{
    return std::format("{} {} one type in the reference", offset, Digest);
}

} // namespace

TEST_CASE("Oracle.parses")
{
    auto const oracle = parseOracle("commands 10\n3 4 wasm a.0.wasm\n9 12 skip 6763");
    REQUIRE(oracle.has_value());
    CHECK(oracle->commands == 10);
    REQUIRE(oracle->entries.size() == 2);
    auto three = OracleEntry { 3, 4 };
    three.wasm = "a.0.wasm";
    CHECK(oracle->entries.at(3) == three);
    auto nine = OracleEntry { 9, 12 };
    nine.skip = "gc";
    CHECK(oracle->entries.at(9) == nine);
}

TEST_CASE("Oracle.crLfLineEndingsReadAsLf")
{
    // A checkout that converts line endings (.gitattributes asks for LF) must not change what a manifest
    // says: no CR may stay in a file name, a count or a decoded value.
    constexpr auto Lf = std::string_view { "commands 10\n3 4 wasm a.0.wasm\n9 12 skip 6763\n" };
    constexpr auto CrLf = std::string_view { "commands 10\r\n3 4 wasm a.0.wasm\r\n9 12 skip 6763\r\n" };
    auto const lf = parseOracle(Lf);
    auto const crLf = parseOracle(CrLf);
    REQUIRE(lf.has_value());
    REQUIRE(crLf.has_value());
    CHECK(crLf->commands == lf->commands);
    CHECK(crLf->entries == lf->entries);
    CHECK(crLf->entries.at(3).wasm == "a.0.wasm");
}

TEST_CASE("Oracle.oneCommandMayHaveBothKinds")
{
    // A module the GC cut rejects has its binary and its skip; empty lines are ignored.
    auto const oracle = parseOracle("commands 2\n\n1 5 wasm a.1.wasm\n1 5 skip 6763\n");
    REQUIRE(oracle.has_value());
    REQUIRE(oracle->entries.size() == 1);
    CHECK(oracle->entries.at(1).wasm == "a.1.wasm");
    CHECK(oracle->entries.at(1).skip == "gc");
    CHECK(parseOracle("commands 0").has_value());
    // An expected divergence: Isoline's message, hex-encoded as a reason is.
    auto const divergent = parseOracle("commands 1\n0 3 message 756e6b6e6f776e206c6f63616c");
    REQUIRE(divergent.has_value());
    CHECK(divergent->entries.at(0).message == "unknown local");
    // A parity divergence: the first canonical offset the encodings differ at, the digest of Isoline's
    // encoding, then why.
    auto const parity =
        parseOracle(std::format("commands 1\n0 3 wasm a.0.wasm\n0 3 parity {}", hexOf(divergenceAt("0x1b"))));
    REQUIRE(parity.has_value());
    CHECK(parity->entries.at(0).wasm == "a.0.wasm");
    CHECK(parity->entries.at(0).parity == divergenceAt("0x1b"));
    CHECK(parseOracle(std::format("commands 1\n0 3 parity {}", hexOf(divergenceAt("0x0")))).has_value());
}

TEST_CASE("Oracle.refusesMalformedParityDivergences")
{
    // The offset as the parity check prints it, a SHA-256 digest of 64 lower-case hex digits, a reason.
    auto const digits = Digest.substr(std::string_view { "sha256:" }.size());
    for (auto const& divergence: std::to_array<std::string>({
             "x",
             "0x1",
             std::format("0x {} reason", Digest),
             std::format("0x1g {} reason", Digest),
             std::format("0x1B {} reason", Digest),
             std::format("0x01b {} reason", Digest),
             std::format("1b {} reason", Digest),
             std::format("0x1 {}", Digest),
             std::format("0x1 {} ", Digest),
             "0x1 reason",
             std::format("0x1 sha256:{} reason", digits.substr(1)),
             std::format("0x1 sha256:{}0 reason", digits),
             std::format("0x1 sha256:{}A reason", digits.substr(1)),
             std::format("0x1 sha1:{} reason", digits),
         }))
    {
        INFO(divergence);
        auto const oracle = parseOracle(std::format("commands 1\n0 1 parity {}", hexOf(divergence)));
        REQUIRE_FALSE(oracle.has_value());
        CHECK(oracle.error().line == 2);
        CHECK(
            oracle.error().message
            == std::format("malformed parity divergence '{}': expected '0x<offset> sha256:<digest> <reason>'",
                           divergence));
    }
    auto const skipped =
        parseOracle(std::format("commands 1\n0 1 skip 61\n0 1 parity {}", hexOf(divergenceAt("0x1"))));
    REQUIRE_FALSE(skipped.has_value());
    CHECK(skipped.error().message == "command 0 is both skipped and given a parity divergence");
}

TEST_CASE("Oracle.refusesMalformedManifests")
{
    struct Malformed
    {
        std::string_view manifest;
        std::size_t line;
        std::string_view message;
    };
    for (auto const& malformed: std::to_array<Malformed>({
             { .manifest = "", .line = 0, .message = "missing 'commands <n>'" },
             { .manifest = "\n\n", .line = 2, .message = "missing 'commands <n>'" },
             { .manifest = "0 1 wasm a.wasm",
               .line = 1,
               .message = "expected 'commands <n>', got '0 1 wasm a.wasm'" },
             { .manifest = "commands x", .line = 1, .message = "malformed command count 'x'" },
             { .manifest = "commands ", .line = 1, .message = "malformed command count ''" },
             { .manifest = "commands 1 2", .line = 1, .message = "malformed command count '1 2'" },
             { .manifest = "commands 1\n0 1 wasm",
               .line = 2,
               .message = "expected '<ordinal> <line> wasm|skip|message|parity <value>'" },
             { .manifest = "commands 1\n0  1 wasm a", .line = 2, .message = "expected '<ordinal> <line>" },
             { .manifest = "commands 1\nx 1 wasm a", .line = 2, .message = "malformed ordinal 'x'" },
             { .manifest = "commands 1\n0 -1 wasm a", .line = 2, .message = "malformed line '-1'" },
             { .manifest = "commands 1\n0 1 text a", .line = 2, .message = "unknown entry kind 'text'" },
             { .manifest = "commands 1\n0 1 skip 6", .line = 2, .message = "odd-length hex string '6'" },
             { .manifest = "commands 1\n0 1 skip zz", .line = 2, .message = "malformed hex string 'zz'" },
             { .manifest = "commands 1\n0 1 wasm a\n0 1 wasm b",
               .line = 3,
               .message = "a second wasm line for one command" },
             { .manifest = "commands 1\n0 1 skip 61\n0 1 skip 62",
               .line = 3,
               .message = "a second skip line" },
             { .manifest = "commands 1\n0 1 message ", .line = 2, .message = "an empty message value" },
             { .manifest = "commands 1\n0 1 skip 61\n0 1 message 62",
               .line = 3,
               .message = "command 0 is both skipped and given a message" },
             { .manifest = "commands 1\n0 1 message 62\n0 1 skip 61",
               .line = 3,
               .message = "command 0 is both skipped and given a message" },
             { .manifest = "commands 1\n0 1 message 61\n0 1 message 62",
               .line = 3,
               .message = "a second message line" },
             { .manifest = "commands 1\n0 1 wasm a\n0 2 skip 61",
               .line = 3,
               .message = "command 0 is on line 2 here, but on line 1 before" },
         }))
    {
        INFO(malformed.manifest);
        auto const oracle = parseOracle(malformed.manifest);
        REQUIRE_FALSE(oracle.has_value());
        CHECK(oracle.error().line == malformed.line);
        INFO(oracle.error().message);
        CHECK(oracle.error().message.starts_with(malformed.message));
    }
}

TEST_CASE("Oracle.alignment")
{
    // A reader that splits or merges a command counts other than the oracle, or leaves an entry beyond
    // its last command: the script then fails as a whole, never by checking the wrong module.
    auto oracle = Oracle { .commands = 3, .entries = {} };
    CHECK(checkAlignment(oracle, 3).has_value());
    auto const miscounted = checkAlignment(oracle, 4);
    REQUIRE_FALSE(miscounted.has_value());
    CHECK(miscounted.error() == "the script holds 4 commands, but its oracle counts 3");
    oracle.entries.emplace(2, OracleEntry { 2, 7 });
    CHECK(checkAlignment(oracle, 3).has_value());
    oracle.entries.emplace(3, OracleEntry { 3, 8 });
    auto const beyond = checkAlignment(oracle, 3);
    REQUIRE_FALSE(beyond.has_value());
    CHECK(beyond.error() == "the oracle names command 3, but the script holds 3");
}

namespace
{

/// The commands of a script, which must parse.
std::vector<Command> commandsOf(std::string_view script)
{
    auto commands = isoline::spectest::parseWast(script, isoline::text::TextLimits {});
    REQUIRE(commands.has_value());
    return std::move(*commands);
}

/// The oracle a manifest gives, which must parse.
Oracle oracleOf(std::string_view manifest)
{
    auto oracle = parseOracle(manifest);
    REQUIRE(oracle.has_value());
    return std::move(*oracle);
}

/// A script of one command of each shape an entry may land on.
constexpr auto Script =
    std::string_view { "(module (func))\n"                                                 // 0 text
                       "(module binary \"\\00asm\\01\\00\\00\\00\")\n"                     // 1 binary
                       "(assert_return (invoke \"f\"))\n"                                  // 2 action
                       "(assert_malformed (module quote \"(x)\") \"unknown operator\")\n"  // 3
                       "(assert_invalid (module (func (result i32))) \"type mismatch\")\n" // 4
                       "(assert_malformed (module binary \"\") \"unexpected end\")\n" };   // 5 binary

} // namespace

TEST_CASE("Oracle.entriesFitTheirCommands")
{
    auto const commands = commandsOf(Script);
    REQUIRE(commands.size() == 6);
    // What an importer writes: binaries of the text modules, a divergence of the malformed text, and
    // skips anywhere.
    CHECK(checkEntries(oracleOf(std::format("commands 6\n0 1 wasm a.0.wasm\n2 3 skip 61\n3 4 message "
                                            "756e6b6e6f776e206f70657261746f722078\n"
                                            "4 5 wasm a.1.wasm\n1 2 skip 61\n4 5 parity {}",
                                            hexOf(divergenceAt("0x1")))),
                       commands)
              .has_value());
}

TEST_CASE("Oracle.entriesThatDoNotFitAreRefused")
{
    auto const commands = commandsOf(Script);
    struct Misfit
    {
        std::string manifest;
        std::string_view message;
    };
    auto const parity = hexOf(divergenceAt("0x1"));
    for (auto const& misfit: std::to_array<Misfit>({
             // A binary of a module given as bytes, or of a command without a module.
             { .manifest = "commands 6\n1 2 wasm a.wasm",
               .message =
                   "the oracle's entry for command 1 (line 2) names a binary of a module given as text, but "
                   "the command on line 2 is a module (binary form)" },
             { .manifest = "commands 6\n2 3 wasm a.wasm",
               .message =
                   "the oracle's entry for command 2 (line 3) names a binary of a module given as text, but "
                   "the command on line 3 is an assert_return" },
             // A divergence anywhere but on malformed text, or one that is the script's own message.
             { .manifest = "commands 6\n4 5 message 78",
               .message =
                   "the oracle's entry for command 4 (line 5) names a message of malformed text, but the "
                   "command on line 5 is an assert_invalid (text form)" },
             { .manifest = "commands 6\n0 1 message 78", .message = "names a message of malformed text" },
             { .manifest = "commands 6\n2 3 message 78", .message = "names a message of malformed text" },
             { .manifest = "commands 6\n5 6 message 78",
               .message =
                   "names a message of malformed text, but the command on line 6 is an assert_malformed "
                   "(binary form)" },
             // A parity divergence of a command the oracle gives no binary of.
             { .manifest = std::format("commands 6\n0 1 parity {}", parity),
               .message = "the oracle's entry for command 0 (line 1) records a parity divergence without "
                          "the oracle's binary, but the command on line 1 is a module (text form)" },
             { .manifest = std::format("commands 6\n3 4 parity {}", parity),
               .message = "records a parity divergence without the oracle's binary" },
             { .manifest = "commands 6\n3 4 message 756e6b6e6f776e206f70657261746f72",
               .message =
                   "the oracle's message for command 3 (line 4) is the script's own, so no divergence" },
         }))
    {
        INFO(misfit.manifest);
        auto const checked = checkEntries(oracleOf(misfit.manifest), commands);
        REQUIRE_FALSE(checked.has_value());
        INFO(checked.error());
        CHECK(checked.error().contains(misfit.message));
    }
}

TEST_CASE("Oracle.shiftedEntriesAreRefused")
{
    // The entries of a script, each moved onto the command after its own: the first one that lands on a
    // command it does not fit fails the whole oracle, rather than being dropped or checked against the
    // wrong module.
    auto const commands = commandsOf(Script);
    auto const fitting =
        std::string_view { "commands 6\n0 1 wasm a.0.wasm\n3 4 message 78\n4 5 wasm a.1.wasm\n" };
    REQUIRE(checkEntries(oracleOf(fitting), commands).has_value());
    auto const shifted = checkEntries(oracleOf("commands 6\n1 1 wasm a.0.wasm\n4 4 message 78\n"), commands);
    REQUIRE_FALSE(shifted.has_value());
    CHECK(shifted.error().starts_with("the oracle's entry for command 1 (line 1) names a binary"));
}
