// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#include <isoline/binary/Module.hpp>
#include <isoline/text/TextLimits.hpp>

// The text format's writer, the inverse of ModuleReader: binary::Module → `.wat`. It prints the flat
// form, one instruction per line, so that neither writing nor reading the result back has a depth
// limit; compileText of what it writes encodes to the module it was given, canonically
// (binary::canonicalize), names and annotations included where they can be re-synthesized.
namespace isoline::text
{

/// How a ModuleWriter lays out its text, and the most text it may produce.
struct WriterOptions
{
    /// The spaces of one indentation level.
    uint32_t indentWidth = 2;

    /// The deepest indentation level printed: a line nested deeper is indented as deep as this, so that
    /// a module of deeply nested blocks costs at most indentWidth × maxIndentLevel bytes of indentation
    /// per line, not bytes quadratic in its nesting.
    uint32_t maxIndentLevel = 32;

    /// The most text a module may be written as, in bytes. Text is linear in the module but where the
    /// text format has no shorthand for what a few bytes of binary say, each a few bytes of binary asking
    /// for far more text:
    /// - a compact import group's module name, which every import of the group repeats (as long as the
    ///   name per one-byte item);
    /// - a run of locals, which declares each local by its type (about four bytes per local, up to
    ///   DecodeLimits::maxLocals of them per run of two bytes);
    /// - a named parameter, which forces the whole signature inline beside `(type N)`, as only an inline
    ///   parameter can carry a name (every parameter of the type, per function or import or tag);
    /// - deep nesting, each line indented up to indentWidth × maxIndentLevel (about 47 bytes of text per
    ///   byte of `block` and `end` with the defaults).
    /// The writer refuses a module as its text reaches this limit. 1 GiB: the reader accepts no larger
    /// source (TextLimits::maxSourceBytes).
    std::size_t maxTextBytes = TextLimits {}.maxSourceBytes;
};

/// Why a ModuleWriter refused to write a module.
enum class WriteError : uint8_t
{
    TextTooLarge,        ///< Its text would exceed WriterOptions::maxTextBytes.
    PageSizeNotWritable, ///< A memory's page size is 2^32 or more, which the text format cannot say.
};

/// The last WriteError enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastWriteError = WriteError::PageSizeNotWritable;

/// Writes core modules in the text format.
///
/// The text is the flat form: one field per line, one instruction per line, `block`/`end` indented.
/// Every immediate is spelled by its ImmediateKind; every index, branch target included, is printed
/// numerically, so that no name can be shadowed or name nothing; every type use is an explicit
/// `(type N)`, so that reading it back never re-derives an implicit type. Floats are exact hex floats
/// (`0x1.8p+1`), NaNs `nan:0x…` with their payload, and `v128.const` four `i32x4` lanes in hex. Locals
/// are printed as their runs, imports one field each, and a data count section not at all, as reading
/// the text back derives it.
///
/// The name and branch hint sections are printed as identifiers and annotations only where reading the
/// text back re-synthesizes the identical section at the identical place: the section decodes, every
/// entry names an entity the module has, and its place is the one binary::SynthesizedSections gives
/// it. A name that is no identifier is written `$"…"`, and one an identifier cannot hold (the empty
/// name, a later duplicate in one scope) `(@name "…")`. Every other custom section, and those two when
/// they cannot be re-synthesized, is written `(@custom "name" (after X) "…")`.
///
/// Writing is one pass over the module, without recursion, linear in the module and its text.
class ModuleWriter
{
  public:
    /// A writer of the given layout and output limit.
    /// @param options How to lay out the text, and the most it may be.
    explicit ModuleWriter(WriterOptions options) noexcept: _options { options } {}

    /// Writes a module: any one binary::decodeModule produced, valid or not.
    /// @param module The module.
    /// @return Its text, a `(module …)` form, or the WriteError that refuses it.
    [[nodiscard]] std::expected<std::string, WriteError> write(binary::Module const& module) const;

    /// Writes a module as write(module) does, its form starting at @p indentLevel: P11 nests core
    /// modules in components.
    /// @param module The module.
    /// @param indentLevel The indentation level of the `(module` line; its fields are one deeper.
    /// @return Its text, or the WriteError that refuses it.
    [[nodiscard]] std::expected<std::string, WriteError> write(binary::Module const& module,
                                                               uint32_t indentLevel) const;

  private:
    WriterOptions _options; ///< The layout and the output limit.
};

} // namespace isoline::text
