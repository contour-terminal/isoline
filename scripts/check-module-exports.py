#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""The export lint of a C++ module: generates its interface unit (--write) and checks it (--check).

A module's surface is data: its row (`isoline_add_module()` in cmake/IsolineModules.cmake) writes a
manifest, `<name>.surface.json`, naming the library's header set, the public and internal namespaces,
the modules it re-exports, the foreign names it re-exports, and what compiling the headers takes. The
headers are the single source of truth; the interface unit (`isoline.cppm`) declares nothing. Its global
module fragment includes the headers that declare into a public namespace, and its purview holds one
`export namespace N { using N::leaf; ... }` block per public namespace.

The lint compiles one umbrella translation unit of the header set with Clang's JSON AST dump, filtered by
the common root of the public namespaces, and walks the direct children of every public namespace. What a
child is decides what the lint does with it (DECLARATION_KINDS): a name to export, something to ignore, or,
for a kind the table does not know, a finding: the lint fails closed. A namespace under a public one is
public, internal, or a finding (`unclassified namespace`).

No namespace the walk reaches, internal ones included, may hold a declaration of internal linkage
([basic.link]/3): an anonymous namespace, a `static` function or variable, or a variable of non-volatile
const-qualified type (`constexpr` included) that is neither inline, nor declared `extern`, nor a template,
nor a redeclaration of one of external linkage. A linkage specification's declarations are its namespace's:
in braces they keep the linkage they spell (`static` stays internal), without them they are declared as if
`extern` were written ([dcl.link]). A public header that carries one has a distinct entity in every
translation unit (the ODR), and a module must not expose one to its importers. Clang 22 diagnoses an export
only where it names an internal-linkage entity directly, not an exported alias, variable or function that
exposes one; refusing every TU-local declaration in the header set closes that gap by construction, with no
exposure analysis. Clang's dump names no linkage, so the lint derives it from the storage class and the
`inline` and `constexpr` flags, and where only the type can decide, asks the compiler: a static_assert per
such variable, compiled beside the umbrella translation unit (judgeVariables()). A variable the compiler
cannot judge is a finding, so the lint fails closed. A namespace-scope anonymous union, which must be
`static`, is refused too, though Clang declares its variable implicitly; so is a structured binding that is
`static` or `const`, and one of any other declaration is a finding, its variable being nameless. An internal
namespace's child of a kind the declaration-kind table does not know is a finding, as in a public one.
Declarations at global scope, outside the root the dump is filtered by, are not seen; where a public
namespace re-exports one, the precompile names it.

A manifest that lists a namespace as public that is internal by its own rules (one it lists as internal, a
leaf `testing` or `detail`, or one under such a namespace) contradicts itself: the lint refuses it with a
tool error rather than let either list win.

A declaration is attributed to the header of the set it is written in. One written in an X-macro row file
(a `.def`, which no interface unit includes) is attributed to the header that includes the row file: one
level deep only, since Clang's AST names a location's includer but not the includer's. A row file that
another row file includes is attributed to itself, so a header whose public names all come from such a
file declares nothing as far as the lint can tell: --write leaves it out of the global module fragment,
and --check's precompile reports the using-declaration it cannot resolve.

--write regenerates the interface unit from the surface. --check reports, one per line, what differs
(RULE_TIERS), then how to fix it. Besides the names, it reads the interface unit by a strict grammar
(GRAMMAR: `module;`, `#include <...>` in the global module fragment, `export module`, `export import` and
`export namespace N { using N::leaf; }` blocks, and comments; nothing else), compares its `export import`
lines with the row's imports and its names outside the module's namespaces with the row's foreign names,
scans the header set for macros a header leaves defined (macros do not cross a module; a row file is read
where its header includes it, as the preprocessor reads it) and for conditional directives that read a
compiler's macro, or definitions of a macro that names one (COMPILER_MACROS: a declaration only one compiler
sees is one neither the lint nor another compiler's module build can tell from a missing name), and finally
precompiles the unit as an importer's build would, its imported modules first, so that what Clang refuses
to export (an entity of internal linkage) is a finding too.

--list prints the surface's public names, sorted, one per line, each followed by the declaration kinds that
declare it (LISTED_KINDS; a template's own kind, an unscoped enumeration's enumerator as EnumConstantDecl),
comma-separated; --kinds lists only the names, and kinds, of the kinds it names. An undetermined surface (the
findings --write refuses on) lists nothing. It is what another reading of the headers is compared with: the
reflected module surface (src/isoline/meta/reflection/ModuleSurface.cpp, docs/language-features.md).

Exit status: 0 clean, 1 findings, 2 a tool error (no Clang, an unreadable manifest, headers Clang cannot
parse, a dump over the ceiling, an imported module without its interface unit, imports that form a cycle,
and any failure the lint did not foresee).

Usage: python3 -I scripts/check-module-exports.py --clang <clang++> --manifest <surface.json>
                                                  (--check | --write | --list [--kinds <kind>...])
                                                  [--work-directory <dir>]
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from typing import Optional

EXIT_CLEAN = 0
EXIT_FINDINGS = 1
EXIT_TOOL_ERROR = 2

# The ceiling of the filtered AST dump. Above it the lint stops with a tool error naming the size, so
# that growth of an internal namespace (which the root filter dumps too) is noticed rather than becoming
# slow. Measured for module isoline (Clang 22.1.8, 2026-11-27): 94.1 MB in 40 roots.
DUMP_SIZE_LIMIT_BYTES = 512 * 1024 * 1024

# The last components that make a namespace internal wherever it is.
ALWAYS_INTERNAL_LEAVES = ("testing", "detail")

# What a namespace's child is to the lint.
EXPORT = "export"  # its name is public
EXPORT_ENUMERATION = "export enumeration"  # its name, and an unscoped enumeration's enumerators
TEMPLATED = "templated"  # what its templated declaration is (its first declaration child that is no parameter)
IGNORE = "ignore"  # it names nothing an importer could spell through a using-declaration
REFUSE = "refuse"  # it names something an interface unit of using-declarations cannot export: a finding
TRANSPARENT = "transparent"  # its children are the namespace's own (a linkage specification)

# The declaration-kind table: (Clang AST kind, action, why). A kind not listed is a finding. Implicit nodes
# are ignored before the table is read, so it has no row for what Clang declares only implicitly: the
# UsingShadowDecl beside each UsingDecl, and implicit deduction guides. Nor for FriendDecl: a friend is a
# child of its class, never of a namespace, so the hidden friends of a public class are not seen at all.
DECLARATION_KIND_ROWS = (
    ("CXXRecordDecl", EXPORT, "a class, struct or union"),
    ("EnumDecl", EXPORT_ENUMERATION, "an enumeration"),
    ("VarDecl", EXPORT, "a variable"),
    ("FunctionDecl", EXPORT, "a function; one name covers its overload set"),
    ("TypeAliasDecl", EXPORT, "an alias"),
    ("TypedefDecl", EXPORT, "a typedef"),
    ("ConceptDecl", EXPORT, "a concept"),
    ("UsingDecl", EXPORT, "a re-export; its name is qualified, the leaf is the name"),
    ("FunctionTemplateDecl", TEMPLATED, "a function template, a deduction guide or an out-of-line member"),
    ("ClassTemplateDecl", TEMPLATED, "a class template"),
    ("VarTemplateDecl", TEMPLATED, "a variable template"),
    ("TypeAliasTemplateDecl", TEMPLATED, "an alias template"),
    ("ClassTemplateSpecializationDecl", IGNORE, "an explicit specialization; the class template is the name"),
    ("ClassTemplatePartialSpecializationDecl", IGNORE, "a partial specialization; the class template is the name"),
    ("VarTemplateSpecializationDecl", IGNORE, "an explicit specialization; the variable template is the name"),
    ("VarTemplatePartialSpecializationDecl", IGNORE,
     "a partial specialization; the variable template is the name"),
    ("CXXMethodDecl", IGNORE, "an out-of-line member definition; the class is the name"),
    ("CXXDeductionGuideDecl", IGNORE, "a deduction guide; the class template is the name"),
    ("StaticAssertDecl", IGNORE, "declares nothing"),
    ("UsingDirectiveDecl", IGNORE, "declares nothing"),
    ("DecompositionDecl", IGNORE, "a structured binding's variable: refused as TU-local or unjudged before this"),
    ("BindingDecl", IGNORE, "a structured binding's name: its DecompositionDecl is what the lint judges"),
    ("LinkageSpecDecl", TRANSPARENT, "a linkage specification; its declarations are the namespace's"),
    ("FullComment", IGNORE, "a namespace's documentation comment"),
    ("EmptyDecl", IGNORE, "a stray `;`, declares nothing"),
    ("UsingEnumDecl", REFUSE,
     "a using-enum-declaration makes an enumeration's enumerators the namespace's, which no using-declaration "
     "of the interface unit names; spell them through the enumeration, or move it into an internal namespace"),
    ("NamespaceAliasDecl", REFUSE,
     "a using-declaration cannot name a namespace, so only a declaration in the purview could export the "
     "alias, and the interface unit declares nothing; alias it in an internal namespace, or name the namespace"),
)
DECLARATION_KINDS = {kind: action for kind, action, _ in DECLARATION_KIND_ROWS}
DECLARATION_REASONS = {kind: why for kind, _, why in DECLARATION_KIND_ROWS}

# The kind of an unscoped enumeration's enumerator, a name of the enumeration's namespace.
ENUMERATOR_KIND = "EnumConstantDecl"
# The kinds --list names a name by: a row's that exports, a template's (the template, not its templated
# declaration), and an enumerator's.
LISTED_KINDS = tuple(sorted([kind for kind, action, _ in DECLARATION_KIND_ROWS
                             if action in (EXPORT, EXPORT_ENUMERATION, TEMPLATED)] + [ENUMERATOR_KIND]))

# The kinds of a template's children that are its parameters, not its templated declaration.
TEMPLATE_PARAMETER_KINDS = ("TemplateTypeParmDecl", "NonTypeTemplateParmDecl", "TemplateTemplateParmDecl")

# How the lint shows a child that has no name.
UNNAMED = "<unnamed>"

# A comment, or a literal that may hold what looks like one, for stripping the comments of a source text.
COMMENT_OR_LITERAL = re.compile(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.DOTALL)

# The states of an interface unit as --check reads it: (state, region a line in it belongs to, how a unit
# that ends in it is incomplete, or None where a unit may end).
PREAMBLE = "preamble"
FRAGMENT = "fragment"
PURVIEW = "purview"
BLOCK_OPENING = "block opening"
BLOCK = "block"
GRAMMAR_STATES = (
    (PREAMBLE, "preamble", "before `module;`"),
    (FRAGMENT, "global module fragment", "before `export module`"),
    (PURVIEW, "purview", None),
    (BLOCK_OPENING, "purview", "inside an export namespace block"),
    (BLOCK, "purview", "inside an export namespace block"),
)
REGIONS = {state: region for state, region, _ in GRAMMAR_STATES}
ENDINGS = {state: ending for state, _, ending in GRAMMAR_STATES}

NAMESPACE_NAME = r"[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*"

# The symbolic operators an operator function's name spells, longest first. Conversion functions are
# members only, so no conversion operator is a namespace's name.
OPERATOR_SYMBOLS = ("<=>", "->*", "<<=", ">>=", "()", "[]", "->", "<<", ">>", "&&", "||", "++", "--", "==", "!=",
                    "<=", ">=", "+=", "-=", "*=", "/=", "%=", "^=", "&=", "|=", "+", "-", "*", "/", "%", "^", "&",
                    "|", "~", "!", "=", "<", ">", ",")
# The name of an operator function, as Clang spells it and --write writes it: a symbolic operator, a
# user-defined literal (`operator""_kib`), `operator new`/`delete` (and their array forms) or `operator co_await`.
OPERATOR_NAME = (r"operator(?:\s*(?:" + "|".join(re.escape(symbol) for symbol in OPERATOR_SYMBOLS)
                 + r')|\s*""\s*[A-Za-z_]\w*|\s+(?:new|delete)(?:\s*\[\s*\])?|\s+co_await)')
QUALIFIED_NAME = rf"(?:::)?(?:[A-Za-z_]\w*::)*(?:{OPERATOR_NAME}|[A-Za-z_]\w*)"

# The strict grammar of an interface unit, one row per kind of line: (state, pattern the whole line
# matches once its comments are stripped, next state, what the line records). A line no row of its state
# matches is a finding; so is a unit that ends where ENDINGS says it may not.
GRAMMAR = (
    (PREAMBLE, re.compile(r"module\s*;"), FRAGMENT, None),
    (FRAGMENT, re.compile(r"#\s*include\s*<([^>]+)>"), FRAGMENT, "include"),
    (FRAGMENT, re.compile(r"export\s+module\s+([\w.]+)\s*;"), PURVIEW, None),
    (PURVIEW, re.compile(r"export\s+import\s+([\w.]+)\s*;"), PURVIEW, "import"),
    (PURVIEW, re.compile(rf"export\s+namespace\s+({NAMESPACE_NAME})"), BLOCK_OPENING, "block"),
    (BLOCK_OPENING, re.compile(r"\{"), BLOCK, None),
    (BLOCK, re.compile(rf"using\s+({QUALIFIED_NAME})\s*;"), BLOCK, "using"),
    (BLOCK, re.compile(r"\}"), PURVIEW, None),
)

# A preprocessor directive that defines or undefines a macro, as the macro scan of the header set reads it.
MACRO_DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(define|undef)[ \t]+([A-Za-z_]\w*)", re.MULTILINE)
# A conditional directive and its condition, or a macro definition and its body, continuation lines joined before
# it is read: a macro whose body names a compiler's macro hides a compiler's check from the conditional that reads it.
CONDITIONAL_DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|elifdef|elifndef|define)\b[ \t]*(.*)$",
                                   re.MULTILINE)
# The macros that name a compiler (its identity or its version), which no conditional directive of the header set
# may read: a public header reads the same on every compiler, and a platform or compiler difference lives in a
# CMake-selected file or a data row instead. A declaration behind one is seen by one compiler only, which neither
# the export lint (Clang) nor another compiler's module build can tell from a missing name. Adding a compiler is a row.
COMPILER_MACROS = (
    "__GNUC__", "__GNUC_MINOR__", "__GNUC_PATCHLEVEL__", "__GNUG__",
    "__clang__", "__clang_major__", "__clang_minor__", "__clang_patchlevel__",
    "_MSC_VER", "_MSC_FULL_VER",
)
COMPILER_MACRO = re.compile(r"(?<![\w])(" + "|".join(re.escape(macro) for macro in COMPILER_MACROS) + r")(?![\w])")
# An include directive, as the macro scan splices row files: its spelling, quoted or angled.
INCLUDE_DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"\n]+)[>\"]", re.MULTILINE)

# How the precompile step compiles an interface unit, beyond the manifest's include directories and
# definitions: as a module interface, to a BMI, Clang's default warnings made errors. Only Clang's defaults:
# the header build already compiles every header with the project's flags, and the module target and the
# installed consumer, under -Werror, are the authority on the unit. Warnings of the system include
# directories are Clang's to suppress, as in the build.
PRECOMPILE_OPTIONS = ("-x", "c++-module", "--precompile", "-Werror")

# The first line of a compiler's output that is a diagnostic of its own, not an "In file included from".
COMPILER_ERROR = re.compile(r"\b(?:fatal )?error:")


class ToolError(Exception):
    """A failure of the lint itself, not a finding: exit status 2."""


@dataclass
class Manifest:
    """A module's row, as its surface manifest records it. Relative paths are relative to the manifest."""

    path: pathlib.Path
    name: str
    source: pathlib.Path
    headerSet: list
    headerBaseDirectories: list
    namespaces: list
    internalNamespaces: list
    imports: list
    foreignNames: list
    includeDirectories: list
    systemIncludeDirectories: list
    compileDefinitions: list
    cxxStandard: str
    # The macros a header of the set leaves defined on purpose, for the set's own headers.
    definedMacros: list = field(default_factory=list)
    # Header (resolved) -> the file read in its place, by Clang (through vfsOverlay) and the text rules
    # alike: a native-contracts build's isoline/Contracts.hpp, which a compiler without contracts cannot
    # read, mapped onto its fallback copy, which declares the same public names.
    substitutes: dict = field(default_factory=dict)
    # The Clang VFS overlay that maps them, written beside the manifest, or None where there are none.
    vfsOverlay: Optional[pathlib.Path] = None


@dataclass
class Surface:
    """What the headers declare into the public namespaces, in document order."""

    # Qualified public name -> the file of its definition, or of its first declaration (_addName()).
    names: dict = field(default_factory=dict)
    # Qualified public name -> the declaration kinds (LISTED_KINDS) that declare it, in --list's output.
    kinds: dict = field(default_factory=dict)
    # Header -> the public namespaces it declares a name into, directly or through a row file it includes.
    declaringFiles: dict = field(default_factory=dict)
    # Qualified names of namespaces under a public one that are neither public nor internal.
    unclassifiedNamespaces: list = field(default_factory=list)
    # (kind, qualified name, file) of the children whose kind DECLARATION_KINDS does not list.
    unknownKinds: list = field(default_factory=list)
    # (kind, qualified name, file, why) of the children whose kind DECLARATION_KINDS refuses.
    unexportable: list = field(default_factory=list)
    # (what, qualified name or "in <namespace>", file) of the TU-local declarations at namespace scope.
    tuLocal: list = field(default_factory=list)
    # (kind, qualified name, file, why) of the variables whose linkage neither the dump nor the compiler tells.
    unjudged: list = field(default_factory=list)


@dataclass
class InterfaceUnit:
    """What --check reads from an interface unit."""

    text: str
    includes: list
    imports: list
    # (block namespace, the qualified name a using-declaration of the block names), in document order.
    usings: list
    # Qualified name -> how often a block of its namespace exports it.
    exportCounts: dict
    # (region, line number) of the first of each run of lines GRAMMAR does not allow.
    disallowedLines: list
    # How the unit is incomplete (ENDINGS), or None.
    incompleteEnding: Optional[str]


@dataclass
class Context:
    """Everything a rule looks at."""

    manifest: Manifest
    surface: Surface
    unit: InterfaceUnit
    generatedText: str
    # The Clang and the directory the precompile step uses.
    clang: str = ""
    workDirectory: Optional[pathlib.Path] = None


def resolvePath(base, value):
    path = pathlib.Path(value)
    return path if path.is_absolute() else (base / path)


def writeOverlay(path, substitutes):
    """Writes beside the manifest at @p path the Clang VFS overlay that maps each header of @p substitutes
    onto its substitute, keeping the header's own path in what Clang reports; returns its path, or None
    where there is nothing to map."""
    if not substitutes:
        return None
    roots = [{"type": "file", "name": header.as_posix(), "external-contents": substitute.resolve().as_posix()}
             for header, substitute in substitutes.items()]
    overlay = path.with_name(path.stem + ".overlay.json")
    try:
        overlay.write_text(json.dumps({"version": 0, "use-external-names": False, "roots": roots}, indent=2) + "\n",
                           encoding="utf-8")
    except OSError as error:
        raise ToolError(f"cannot write the VFS overlay {overlay}: {error.strerror}") from error
    return overlay


def readManifest(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ToolError(f"unreadable manifest {path}: {error}") from error
    base = path.parent
    try:
        substitutes = {resolvePath(base, row["header"]).resolve(): resolvePath(base, row["substitute"])
                       for row in data.get("headerSubstitutes", [])}
        return Manifest(
            path=path,
            name=data["name"],
            source=resolvePath(base, data["source"]),
            headerSet=[resolvePath(base, header) for header in data["headerSet"]],
            headerBaseDirectories=[resolvePath(base, directory) for directory in data["headerBaseDirectories"]],
            namespaces=list(data["namespaces"]),
            internalNamespaces=list(data["internalNamespaces"]),
            imports=[resolvePath(base, imported) for imported in data["imports"]],
            foreignNames=list(data["foreignNames"]),
            includeDirectories=[resolvePath(base, directory) for directory in data["includeDirectories"]],
            systemIncludeDirectories=[resolvePath(base, directory)
                                      for directory in data["systemIncludeDirectories"]],
            compileDefinitions=list(data["compileDefinitions"]),
            cxxStandard=str(data["cxxStandard"]),
            definedMacros=list(data.get("definedMacros", [])),
            substitutes=substitutes,
            vfsOverlay=writeOverlay(path, substitutes),
        )
    except (KeyError, TypeError) as error:
        raise ToolError(f"unreadable manifest {path}: missing or malformed field {error}") from error


def importedModuleNames(manifest):
    """The names of the modules the row re-exports, read from their manifests."""
    return [readManifest(imported).name for imported in manifest.imports]


def umbrellaHeaders(manifest):
    """The headers of the set the umbrella includes: the `.hpp` entries. A `.def` is an X-macro row file its
    own header includes, never a header of its own."""
    return [header for header in manifest.headerSet if header.suffix == ".hpp"]


def includeSpelling(manifest, header):
    """How the interface unit includes @p header: relative to the first base directory holding it."""
    resolved = header.resolve()
    for directory in manifest.headerBaseDirectories:
        try:
            return resolved.relative_to(directory.resolve()).as_posix()
        except ValueError:
            continue
    raise ToolError(f"{header} is under none of the header base directories of {manifest.path}")


def headersOutsideTheSet(manifest):
    """The spellings of the `.hpp` files beside a header of the set, in the same directory, that the set does
    not hold: a public header, by where it lives, that the module would not cover. Sorted."""
    inSet = {header.resolve() for header in manifest.headerSet}
    # Sorted as `/`-separated text, so that the order is every operating system's (Windows' paths compare
    # case-insensitively).
    directories = sorted({header.resolve().parent for header in umbrellaHeaders(manifest)},
                         key=lambda path: path.as_posix())
    return [includeSpelling(manifest, candidate)
            for directory in directories
            for candidate in sorted(directory.glob("*.hpp"), key=lambda path: path.as_posix())
            if candidate.resolve() not in inSet]


def shownFile(manifest, file):
    """A file as a finding names it: relative to the interface unit's directory where it is under it,
    else as the interface unit includes it."""
    resolved = pathlib.Path(file).resolve()
    try:
        return resolved.relative_to(manifest.source.parent.resolve()).as_posix()
    except ValueError:
        pass
    try:
        return includeSpelling(manifest, resolved)
    except ToolError:
        return resolved.as_posix()


def commonRoot(namespaces):
    """The longest common leading components of @p namespaces, as a qualified name."""
    split = [namespace.split("::") for namespace in namespaces]
    common = []
    for components in zip(*split):
        if any(component != components[0] for component in components):
            break
        common.append(components[0])
    return "::".join(common)


def compileArguments(manifest):
    """What compiling the module's headers takes, from its manifest: the C++ standard, the include
    directories (the system ones as -isystem), the VFS overlay of substituted headers and the definitions."""
    systemDirectories = [directory.resolve() for directory in manifest.systemIncludeDirectories]
    arguments = [f"-std=c++{manifest.cxxStandard}"]
    for directory in manifest.includeDirectories:
        if directory.resolve() not in systemDirectories:
            arguments.append(f"-I{directory.resolve().as_posix()}")
    for directory in systemDirectories:
        arguments += ["-isystem", directory.as_posix()]
    if manifest.vfsOverlay is not None:
        arguments += ["-ivfsoverlay", manifest.vfsOverlay.as_posix()]
    return arguments + [f"-D{definition}" for definition in manifest.compileDefinitions]


def dumpAst(manifest, clang, workDirectory, dumpSizeLimitBytes):
    """Compiles the umbrella translation unit with the filtered JSON AST dump; returns the dump's path."""
    root = commonRoot(manifest.namespaces)
    if not root:
        raise ToolError(f"the namespaces of {manifest.path} share no root: {', '.join(manifest.namespaces)}")
    umbrella = workDirectory / f"{manifest.name}.umbrella.cpp"
    umbrella.write_text("".join(f'#include "{header.resolve().as_posix()}"\n'
                                for header in umbrellaHeaders(manifest)), encoding="utf-8")
    arguments = [clang, *compileArguments(manifest), "-fsyntax-only", "-x", "c++"]
    arguments += ["-Xclang", "-ast-dump=json", "-Xclang", "-ast-dump-filter", "-Xclang", root, str(umbrella)]
    dump = workDirectory / f"{manifest.name}.ast.json"
    try:
        with dump.open("wb") as output:
            result = subprocess.run(arguments, stdout=output, stderr=subprocess.PIPE, check=False)
    except OSError as error:
        raise ToolError(f"cannot run Clang {clang}: {error.strerror}") from error
    if result.returncode != 0:
        diagnostics = result.stderr.decode("utf-8", "replace").splitlines()
        errors = [line for line in diagnostics if "error:" in line] or diagnostics
        raise ToolError(f"Clang failed on the header set of {manifest.path} (exit {result.returncode}): "
                        + " | ".join(errors[:3]))
    size = dump.stat().st_size
    if size > dumpSizeLimitBytes:
        raise ToolError(f"the AST dump is {size} bytes, over the ceiling of {dumpSizeLimitBytes} bytes "
                        "(DUMP_SIZE_LIMIT_BYTES): an internal namespace has grown; raise the ceiling with "
                        "the measurement")
    return dump, root


def readRoots(dump):
    """The dump's top-level JSON values, one at a time: Clang writes one per declaration the filter
    matches, concatenated."""
    try:
        text = dump.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise ToolError(f"unreadable AST dump: {error}") from error
    decoder = json.JSONDecoder()
    whitespace = re.compile(r"\s*")
    position = whitespace.match(text, 0).end()
    while position < len(text):
        try:
            value, position = decoder.raw_decode(text, position)
        except ValueError as error:
            raise ToolError(f"unreadable AST dump: {error}") from error
        yield value
        position = whitespace.match(text, position).end()


def lastFileIn(value, skipInner=False):
    """The file of the last source location in @p value, in document order, or None. Clang writes a
    location's file only when it differs from the previous location's, so this is the file in force
    after @p value. A location's `includedFrom` is not a location of its own."""
    if isinstance(value, list):
        for item in reversed(value):
            found = lastFileIn(item)
            if found is not None:
                return found
        return None
    if not isinstance(value, dict):
        return None
    for key in reversed(list(value.keys())):
        if key == "includedFrom" or (skipInner and key == "inner"):
            continue
        if key == "file":
            return value["file"]
        found = lastFileIn(value[key])
        if found is not None:
            return found
    return None


def fileOfLocation(location, currentFile):
    """The file a declaration's `loc` names, given the file in force before it, and the file that includes
    it (None for the main file): the expansion's, where the location is in a macro expansion."""
    parts = [location[key] for key in ("spellingLoc", "expansionLoc") if key in location] or [location]
    for part in parts:
        if "file" in part:
            currentFile = part["file"]
    return currentFile, parts[-1].get("includedFrom", {}).get("file")


# The kinds whose declaration is TU-local at namespace scope where its storage class is `static`.
STATIC_STORAGE_KINDS = ("VarDecl", "FunctionDecl", "DecompositionDecl")

# What tuLocalDeclaration() makes of a structured binding whose linkage it cannot tell: its variable has no
# name, so the compiler cannot judge it (judgeVariables()), and the lint fails closed (surface.unjudged).
UNJUDGED_BINDING = "unjudged structured binding"

# What tuLocalDeclaration() makes of a variable whose linkage depends on its type's const-ness: the dump
# cannot say for certain (a type is printed as spelled: `CI[3]` for an array of an alias of `const int`),
# so the compiler judges it (judgeVariables()).
TYPE_DEPENDENT = "type-dependent"

# The label of a variable the compiler judged const: internal linkage.
CONST_VARIABLE = "non-inline const VarDecl"

# How judgeVariables() asks the compiler: one static_assert per variable, on a line of its own, whose
# message marks the ones that are internal by their type: non-volatile const-qualified, which an array is
# where its elements are ([basic.type.qualifier]). Any other error on a variable's line leaves it unjudged,
# which is a finding: the lint fails closed.
JUDGEMENT_MARKER = "isoline-lint: internal linkage by its const type"
JUDGEMENT_ROW = ("static_assert(!(std::is_const_v<decltype({name})> && "
                 "!std::is_volatile_v<decltype({name})>), \"{marker}\");\n")


def isTypeDependentVariable(node, isExternSpecified):
    """Whether @p node, a namespace's child, is a variable whose linkage only its type decides: neither
    inline, nor `constexpr` (which makes it const, so internal: tuLocalDeclaration() decides that), nor
    declared `extern`, explicitly or by an unbraced linkage specification (@p isExternSpecified). A template
    is not: [basic.link]/3 names non-template variables, so a variable template's own VarDecl is never
    looked at."""
    return (node.get("kind") == "VarDecl" and not node.get("inline") and not isExternSpecified
            and node.get("storageClass") not in ("extern", "static"))


def tuLocalDeclaration(node, isExternSpecified=False):
    """What makes @p node, a namespace's child, a TU-local declaration, TYPE_DEPENDENT where only the
    compiler can tell, or None: an anonymous namespace, a `static` function or variable (templates
    included), a non-inline `constexpr` variable, or one that is const by its type. A public header holds
    none: each would be a distinct entity in every translation unit (the ODR), and no module may expose one
    to an importer. A redeclaration is judged by its first declaration, which the walker knows
    (SurfaceWalker._tuLocalById)."""
    if node.get("kind") == "NamespaceDecl":
        return None if "name" in node else "anonymous namespace"
    declaration = node
    if DECLARATION_KINDS.get(node.get("kind")) == TEMPLATED:
        declaration = next((inner for inner in node.get("inner", [])
                            if inner.get("kind") in STATIC_STORAGE_KINDS), {})
    if declaration.get("kind") in STATIC_STORAGE_KINDS and declaration.get("storageClass") == "static":
        return "static anonymous union" if node.get("isImplicit") else f"static {node.get('kind')}"
    if isTypeDependentVariable(node, isExternSpecified):
        return "non-inline constexpr VarDecl" if node.get("constexpr") else TYPE_DEPENDENT
    if node.get("kind") == "DecompositionDecl":
        # Its type is deduced from `cv auto`, so the cv the dump prints first is the declaration's own; an
        # array of const elements deduced from its initializer may print through an alias, so nothing else
        # is certain.
        words = node.get("type", {}).get("qualType", "").split()
        if words[:1] == ["const"] and "volatile" not in words:
            return "non-inline const DecompositionDecl"
        return UNJUDGED_BINDING
    return None


def judgeVariables(manifest, clang, workDirectory, surface):
    """Has the compiler judge the variables the walk left TYPE_DEPENDENT, beside the umbrella translation
    unit: each becomes CONST_VARIABLE where its type is const, is dropped where it is not, and is moved to
    surface.unjudged, with the compiler's error, where the compiler could not tell. A variable that is not
    kept leaves the public names too."""
    pending = [entry for entry in surface.tuLocal if entry[0] == TYPE_DEPENDENT]
    if not pending:
        return
    probe = workDirectory / f"{manifest.name}.linkage.cpp"
    umbrella = workDirectory / f"{manifest.name}.umbrella.cpp"
    lines = ["#include <type_traits>\n", f'#include "{umbrella.as_posix()}"\n']
    lineOf = {}
    for _, name, _ in pending:
        lineOf[len(lines) + 1] = name
        lines.append(JUDGEMENT_ROW.format(name=f"::{name}", marker=JUDGEMENT_MARKER))
    probe.write_text("".join(lines), encoding="utf-8")
    arguments = [clang, *compileArguments(manifest), "-fsyntax-only", "-ferror-limit=0", "-x", "c++", str(probe)]
    try:
        result = subprocess.run(arguments, capture_output=True, text=True, check=False)
    except OSError as error:
        raise ToolError(f"cannot run Clang {clang}: {error.strerror}") from error
    location = re.compile(rf"^{re.escape(probe.as_posix())}:(\d+):\d+: (?:fatal )?error: (.*)$")
    internal = set()
    unjudged = {}
    for line in result.stderr.splitlines():
        match = location.match(line)
        if match is None or int(match.group(1)) not in lineOf:
            if COMPILER_ERROR.search(line):
                raise ToolError(f"Clang failed on the linkage judgement of {manifest.path}: {line}")
            continue
        name = lineOf[int(match.group(1))]
        if JUDGEMENT_MARKER in match.group(2):
            internal.add(name)
        else:
            unjudged.setdefault(name, match.group(2))
    if result.returncode != 0 and not internal and not unjudged:
        raise ToolError(f"Clang failed on the linkage judgement of {manifest.path} (exit {result.returncode})")
    kept = []
    for what, name, file in surface.tuLocal:
        if what != TYPE_DEPENDENT:
            kept.append((what, name, file))
            continue
        if name in unjudged:
            surface.unjudged.append(("VarDecl", name, file, unjudged[name]))
        elif name in internal:
            kept.append((CONST_VARIABLE, name, file))
        else:
            continue
        surface.names.pop(name, None)
    surface.tuLocal = kept


def internalReason(namespace, internalNamespaces):
    """Why @p namespace is internal by a manifest's rules, or None: it, or a namespace it is under, is
    listed in @p internalNamespaces, or ends in a leaf of ALWAYS_INTERNAL_LEAVES."""
    components = namespace.split("::")
    for depth in range(len(components), 0, -1):
        enclosing = "::".join(components[:depth])
        if enclosing in internalNamespaces:
            return f"the manifest lists {enclosing} as internal"
        if components[depth - 1] in ALWAYS_INTERNAL_LEAVES:
            return f"{enclosing} ends in `{components[depth - 1]}`, internal wherever it is"
    return None


def refuseContradictions(manifest):
    """Raises a ToolError where @p manifest lists a namespace as public that is internal by its own rules
    (internalReason()): neither list may silently win over the other."""
    internal = set(manifest.internalNamespaces)
    contradictions = [f"{namespace} ({reason})" for namespace in manifest.namespaces
                      if (reason := internalReason(namespace, internal)) is not None]
    if contradictions:
        raise ToolError(f"manifest {manifest.path} lists namespaces as public that are internal: "
                        + ", ".join(contradictions))


class SurfaceWalker:
    """Walks the dump's roots and collects the Surface."""

    def __init__(self, manifest, root):
        self._headers = {str(header.resolve()) for header in umbrellaHeaders(manifest)}
        self._public = set(manifest.namespaces)
        self._internal = set(manifest.internalNamespaces)
        self._rootName = root.split("::")[-1]
        self._root = root
        self._currentFile = None
        self._defined = set()
        # What tuLocalDeclaration() made of each declaration, by id, so that a redeclaration has the linkage
        # of its first declaration: `extern const int x;` makes a later `const int x = 1;` external.
        self._tuLocalById = {}
        self.surface = Surface()

    def walkRoot(self, node):
        if node.get("kind") == "NamespaceDecl" and node.get("name") == self._rootName:
            self._walkNamespace(node, self._root)
        else:
            self._advancePast(node)

    def _advancePast(self, node, skipInner=False):
        found = lastFileIn(node, skipInner)
        if found is not None:
            self._currentFile = found

    def _walkNamespace(self, node, qualifiedName, isInternal=False):
        """Walks a namespace's children. Those of a public namespace are classified; those of every other
        are only looked at for TU-local declarations, which no namespace of the header set may hold."""
        self._advancePast(node, skipInner=True)
        isPublic = not isInternal and qualifiedName in self._public
        self._walkChildren(node.get("inner", []), qualifiedName, isPublic, isInternal, isExternSpecified=False)

    def _walkChildren(self, children, qualifiedName, isPublic, isInternal, isExternSpecified):
        """Walks @p children of the namespace @p qualifiedName, declared as if `extern` were written where
        @p isExternSpecified (the one declaration of an unbraced linkage specification)."""
        for child in children:
            if child.get("kind") == "NamespaceDecl":
                self._walkChildNamespace(child, qualifiedName, isPublic, isInternal)
            elif DECLARATION_KINDS.get(child.get("kind")) == TRANSPARENT:
                self._advancePast(child, skipInner=True)
                self._walkChildren(child.get("inner", []), qualifiedName, isPublic, isInternal,
                                   isExternSpecified=not child.get("hasBraces", False))
            elif self._refuseTuLocal(child, qualifiedName, isExternSpecified):
                continue
            elif isPublic:
                self._classify(child, qualifiedName)
            else:
                self._refuseUnknownKind(child, qualifiedName)

    def _refuseUnknownKind(self, child, namespace):
        """Records @p child, a child of the internal namespace @p namespace, where the declaration-kind table
        does not know its kind: what the lint cannot classify, it cannot judge for internal linkage either,
        so it fails closed, as in a public namespace."""
        file, _ = fileOfLocation(child.get("loc", {}), self._currentFile)
        self._advancePast(child)
        if not child.get("isImplicit") and child.get("kind") not in DECLARATION_KINDS:
            self.surface.unknownKinds.append((child.get("kind"), f"{namespace}::{child.get('name', UNNAMED)}", file))

    def _walkChildNamespace(self, child, parent, parentIsPublic, parentIsInternal):
        if self._refuseTuLocal(child, parent):
            return
        qualifiedName = f"{parent}::{child['name']}"
        # Internal before public: a manifest that lists an internal namespace as public is refused before
        # the walk (refuseContradictions()), so the order decides nothing a manifest can ask for.
        if parentIsInternal or internalReason(qualifiedName, self._internal) is not None:
            self._walkNamespace(child, qualifiedName, isInternal=True)
        elif qualifiedName in self._public:
            self._walkNamespace(child, qualifiedName)
        elif parentIsPublic:
            if qualifiedName not in self.surface.unclassifiedNamespaces:
                self.surface.unclassifiedNamespaces.append(qualifiedName)
            self._advancePast(child)
        else:
            # Neither public nor under a public namespace, as the root may be: it can hold a public one.
            self._walkNamespace(child, qualifiedName)

    def _refuseTuLocal(self, child, namespace, isExternSpecified=False):
        """Records @p child, a child of @p namespace, where it is a TU-local declaration, and returns whether
        it is; the walk does not look into it."""
        # Implicit nodes declare nothing a header spells, but for the variable of an anonymous union.
        if child.get("isImplicit") and not (child.get("kind") == "VarDecl" and "name" not in child):
            what = None
        elif child.get("previousDecl") in self._tuLocalById:
            what = self._tuLocalById[child["previousDecl"]]
        else:
            what = tuLocalDeclaration(child, isExternSpecified)
        if "id" in child:
            self._tuLocalById[child["id"]] = what
        if what is None:
            return False
        file, _ = fileOfLocation(child.get("loc", {}), self._currentFile)
        self._advancePast(child)
        name = f"{namespace}::{child['name']}" if "name" in child else f"in {namespace}"
        if child.get("kind") == "DecompositionDecl":
            bindings = ", ".join(inner["name"] for inner in child.get("inner", [])
                                 if inner.get("kind") == "BindingDecl")
            name = f"{namespace}::[{bindings}]"
        if what == UNJUDGED_BINDING:
            self.surface.unjudged.append((child.get("kind"), name, file,
                                          "a structured binding's variable has no name the compiler could judge"))
            return True
        if not any(entry[:2] == (what, name) for entry in self.surface.tuLocal):  # once for redeclarations
            self.surface.tuLocal.append((what, name, file))
        # A variable the compiler judges later is classified meanwhile: judgeVariables() withdraws its name.
        return what != TYPE_DEPENDENT

    def _classify(self, child, namespace):
        file, includer = fileOfLocation(child.get("loc", {}), self._currentFile)
        header = self._declaringHeader(file, includer)
        self._advancePast(child)
        if child.get("isImplicit"):
            return
        kind = child.get("kind")
        listedKind = kind  # a template's own kind, not its templated declaration's
        action = DECLARATION_KINDS.get(kind)
        declaration = child
        if action == TEMPLATED:
            # The first declaration that is no parameter: a requires-clause is an expression child before it.
            declaration = next((inner for inner in child.get("inner", [])
                                if inner.get("kind", "").endswith("Decl")
                                and inner.get("kind") not in TEMPLATE_PARAMETER_KINDS), {})
            kind = f"{kind} of {declaration.get('kind')}"
            action = DECLARATION_KINDS.get(declaration.get("kind"))
            if action == TEMPLATED:  # no declaration is a template of a template
                action = None
        if action is None:
            self.surface.unknownKinds.append((kind, f"{namespace}::{child.get('name', UNNAMED)}", file))
            return
        if action == REFUSE:
            self.surface.unexportable.append((kind, f"{namespace}::{child.get('name', UNNAMED)}", file,
                                              DECLARATION_REASONS[kind]))
            return
        # A declaration whose scope is another one (an out-of-line definition of a member, or of a
        # nested namespace's function) is exported where it is declared.
        if action == IGNORE or "parentDeclContextId" in child:
            return
        # Clang marks a class's definition; an enumeration's is the one with its enumerators.
        isDefinition = bool(declaration.get("completeDefinition")) or (
            action == EXPORT_ENUMERATION
            and any(inner.get("kind") == "EnumConstantDecl" for inner in child.get("inner", [])))
        if "name" in child:
            self._addName(namespace, child["name"].split("::")[-1], listedKind, file, header, isDefinition)
        # An unscoped enumeration's enumerators are names of the namespace, the enumeration named or not.
        if action == EXPORT_ENUMERATION and "scopedEnumTag" not in child:
            for enumerator in child.get("inner", []):
                if enumerator.get("kind") == "EnumConstantDecl":
                    self._addName(namespace, enumerator["name"], ENUMERATOR_KIND, file, header, isDefinition)

    def _declaringHeader(self, file, includer):
        """The header of the set a declaration in @p file comes from: @p file itself, or the header that
        includes it, where it is a row file (an X-macro `.def`) that no interface unit can include.

        One level deep only: Clang's location names @p includer, not the file that includes @p includer.
        A row file that a row file includes is therefore its own declaring file, and a header that declares
        only through one is not seen to declare at all (no `header not included`, no include from --write);
        --check's precompile then fails on the name (the case def-included-two-levels)."""
        if file is not None and str(pathlib.Path(file).resolve()) in self._headers:
            return file
        if includer is not None and str(pathlib.Path(includer).resolve()) in self._headers:
            return includer
        return file

    def _addName(self, namespace, leaf, kind, file, header, isDefinition):
        """Records a public name, declared by a declaration of @p kind. Its file is that of its definition where
        a class or enumeration has one in the headers, else that of its first declaration: a forward declaration
        does not say where it lives."""
        name = f"{namespace}::{leaf}"
        self.surface.kinds.setdefault(name, set()).add(kind)
        if name not in self.surface.names or (isDefinition and name not in self._defined):
            self.surface.names[name] = file
        if isDefinition:
            self._defined.add(name)
        if header is not None:
            self.surface.declaringFiles.setdefault(str(pathlib.Path(header).resolve()), set()).add(namespace)


def collectSurface(manifest, clang, workDirectory, dumpSizeLimitBytes):
    dump, root = dumpAst(manifest, clang, workDirectory, dumpSizeLimitBytes)
    walker = SurfaceWalker(manifest, root)
    for node in readRoots(dump):
        walker.walkRoot(node)
    dump.unlink()
    judgeVariables(manifest, clang, workDirectory, walker.surface)
    return walker.surface


def declaringHeaders(manifest, surface):
    """The headers of the set that declare a name into a public namespace: (spelling, namespaces)."""
    headers = []
    for header in umbrellaHeaders(manifest):
        namespaces = surface.declaringFiles.get(str(header.resolve()))
        if namespaces:
            headers.append((includeSpelling(manifest, header), sorted(namespaces)))
    return sorted(headers)


def splitQualified(name):
    namespace, _, leaf = name.rpartition("::")
    return namespace, leaf


def usingOrder(leaf):
    """The order of a block's using-declarations: clang-format's (SortUsingDeclarations), case-insensitive
    first, so that the generated unit is formatted as it is written."""
    return (leaf.lower(), leaf)


def generate(manifest, surface):
    """The interface unit's text, as --write writes it."""
    lines = [
        "// SPDX-License-Identifier: Apache-2.0",
        "//",
        f"// The interface unit of module {manifest.name}, generated by scripts/check-module-exports.py --write",
        "// from the module's surface manifest. Do not edit it: the headers are the source of truth, and the",
        "// target isoline_write_module_exports regenerates it.",
        "module;",
        "",
    ]
    lines += [f"#include <{spelling}>" for spelling, _ in declaringHeaders(manifest, surface)]
    lines += ["", f"export module {manifest.name};", ""]
    imports = importedModuleNames(manifest)
    if imports:
        lines += [f"export import {name};" for name in imports] + [""]
    blocks = {}
    for name in list(surface.names) + manifest.foreignNames:
        namespace, leaf = splitQualified(name)
        blocks.setdefault(namespace, set()).add(leaf)
    publicBlocks = [namespace for namespace in sorted(manifest.namespaces) if namespace in blocks]
    foreignBlocks = sorted(namespace for namespace in blocks if namespace not in manifest.namespaces)
    for namespace in publicBlocks + foreignBlocks:
        lines += [f"export namespace {namespace}", "{"]
        lines += [f"using {namespace}::{leaf};" for leaf in sorted(blocks[namespace], key=usingOrder)]
        lines += [f"}} // namespace {namespace}", ""]
    return "\n".join(lines).rstrip("\n") + "\n"


def readInterfaceUnit(path):
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise ToolError(f"unreadable interface unit {path}: {error.strerror}") from error
    except UnicodeDecodeError as error:
        raise ToolError(f"unreadable interface unit {path}: {error}") from error
    unit = InterfaceUnit(text=text, includes=[], imports=[], usings=[], exportCounts={}, disallowedLines=[],
                         incompleteEnding=None)
    state = PREAMBLE
    blockNamespace = None
    previousDisallowed = False
    for number, line in enumerate(withoutComments(text).splitlines(), start=1):
        line = line.strip()
        if not line:
            # A blank line, or one that was only a comment, ends a run of disallowed lines.
            previousDisallowed = False
            continue
        row = next(((match, nextState, record) for rowState, pattern, nextState, record in GRAMMAR
                    if rowState == state and (match := pattern.fullmatch(line))), None)
        if row is None:
            # One finding for a run of disallowed lines: a declaration over several lines is one.
            if not previousDisallowed:
                unit.disallowedLines.append((REGIONS[state], number))
            previousDisallowed = True
            continue
        previousDisallowed = False
        match, state, record = row
        if record == "include":
            unit.includes.append(match.group(1))
        elif record == "import":
            unit.imports.append(match.group(1))
        elif record == "block":
            blockNamespace = match.group(1)
        elif record == "using":
            qualified = match.group(1).removeprefix("::")
            unit.usings.append((blockNamespace, qualified))
            name = f"{blockNamespace}::{splitQualified(qualified)[1]}"
            unit.exportCounts[name] = unit.exportCounts.get(name, 0) + 1
    unit.incompleteEnding = ENDINGS[state]
    return unit


def withoutComments(text):
    """@p text with each comment replaced by one space, as C++ replaces it, and its line breaks, so that
    line numbers stay."""
    def blank(match):
        token = match.group(0)
        return " " + "\n" * token.count("\n") if token.startswith("/") else token
    return COMMENT_OR_LITERAL.sub(blank, text)


def rowFiles(manifest):
    """The row files of the set (every entry that is no `.hpp`), by each way a header may include one: as the
    interface unit would spell it, and by its resolved path for an include relative to the includer."""
    files = {}
    for rowFile in manifest.headerSet:
        if rowFile.suffix != ".hpp":
            files[includeSpelling(manifest, rowFile)] = rowFile
            files[str(rowFile.resolve())] = rowFile
    return files


def readSource(path):
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise ToolError(f"unreadable header {path}: {error}") from error


def splicedText(path, files, splicing=()):
    """@p path's text, comments stripped, with every row file of @p files it includes spliced in where it
    is included, row files within row files too, as the preprocessor reads it. @p splicing is the chain of
    row files being spliced, so that a row file that includes itself is spliced once."""
    def splice(match):
        spelling = match.group(1)
        rowFile = files.get(spelling) or files.get(str((path.parent / spelling).resolve()))
        if rowFile is None or rowFile in splicing:
            return match.group(0)
        return splicedText(rowFile, files, (*splicing, rowFile))
    return INCLUDE_DIRECTIVE.sub(splice, withoutComments(readSource(path)))


def readHeaders(manifest, header):
    """The files the text rules read for @p header: the header itself, and its substitute too, where the
    manifest's overlay names one. Clang reads the substitute, but the build reads the header, so a rule
    holds both to it (a native isoline/Contracts.hpp and its fallback copy alike)."""
    substitute = manifest.substitutes.get(header.resolve())
    return [header] if substitute is None else [header, substitute]


def leftDefinedMacros(manifest):
    """(macro, header) for each macro a header of the set leaves defined: whose last #define no #undef
    follows. A row file is read where its header includes it, so that its directives count for that header,
    in order, and a macro it leaves defined is that header's."""
    files = rowFiles(manifest)
    leftDefined = []
    for header in umbrellaHeaders(manifest):
        for read in readHeaders(manifest, header):
            lastDirective = {}
            for directive, macro in MACRO_DIRECTIVE.findall(splicedText(read, files)):
                lastDirective[macro] = directive
            leftDefined += [(macro, header) for macro, directive in lastDirective.items()
                            if directive == "define" and (macro, header) not in leftDefined]
    return leftDefined


def compilerConditionals(manifest):
    """(directive, macros, header) for each conditional directive of the set that reads a compiler's macro
    (COMPILER_MACROS), and each macro definition whose body names one, comments and literals left out. A row file is read where its header includes it, as
    leftDefinedMacros() reads it, so that its directives are that header's."""
    files = rowFiles(manifest)
    found = []
    for header in umbrellaHeaders(manifest):
        for read in readHeaders(manifest, header):
            text = splicedText(read, files).replace("\\\n", " ")
            for keyword, condition in CONDITIONAL_DIRECTIVE.findall(text):
                macros = list(dict.fromkeys(COMPILER_MACRO.findall(condition)))
                directive = " ".join(f"#{keyword} {condition}".split())
                if macros and (directive, macros, header) not in found:
                    found.append((directive, macros, header))
    return found


def precompile(manifest, clang, workDirectory, built, importing=()):
    """Precompiles @p manifest's interface unit to a BMI in @p workDirectory, each module it imports first
    (with their own manifests' arguments), as an importer's build would. @p built maps the module names
    precompiled so far to their BMIs; @p importing is the chain of modules that import this one.
    Returns the first compiler diagnostic of the first unit that fails, or None."""
    if manifest.name in importing:
        raise ToolError(f"the imports of module {importing[0]} form a cycle: "
                        + " -> ".join([*importing, manifest.name]))
    for imported in manifest.imports:
        importedManifest = readManifest(imported)
        if importedManifest.name not in built:
            diagnostic = precompile(importedManifest, clang, workDirectory, built, (*importing, manifest.name))
            if diagnostic is not None:
                return diagnostic
    if not manifest.source.is_file():
        raise ToolError(f"unreadable interface unit {manifest.source.resolve()}: no such file (module {manifest.name})")
    moduleFile = workDirectory / f"{manifest.name}.pcm"
    arguments = [clang, *compileArguments(manifest), *PRECOMPILE_OPTIONS]
    # Every module precompiled so far: an imported module's own imports are among them.
    arguments += [f"-fmodule-file={name}={path.as_posix()}" for name, path in built.items()]
    arguments += ["-o", moduleFile.as_posix(), manifest.source.resolve().as_posix()]
    try:
        result = subprocess.run(arguments, capture_output=True, text=True, errors="replace", check=False)
    except OSError as error:
        raise ToolError(f"cannot run Clang {clang}: {error.strerror}") from error
    if result.returncode != 0:
        diagnostics = result.stderr.splitlines()
        return next((line for line in diagnostics if COMPILER_ERROR.search(line)),
                    diagnostics[0] if diagnostics else f"exit status {result.returncode}")
    built[manifest.name] = moduleFile
    return None


def expectedNames(context):
    return list(context.surface.names) + context.manifest.foreignNames


def checkUnclassifiedNamespaces(context):
    return [{"namespace": namespace} for namespace in context.surface.unclassifiedNamespaces]


def checkUnknownKinds(context):
    return [{"kind": kind, "name": name, "file": shownFile(context.manifest, file) if file else "?"}
            for kind, name, file in context.surface.unknownKinds]


def checkUnexportable(context):
    return [{"kind": kind, "name": name, "file": shownFile(context.manifest, file) if file else "?", "why": why}
            for kind, name, file, why in context.surface.unexportable]


def checkUnjudged(context):
    return [{"kind": kind, "name": name, "file": shownFile(context.manifest, file) if file else "?",
             "diagnostic": diagnostic}
            for kind, name, file, diagnostic in context.surface.unjudged]


def checkTuLocal(context):
    return [{"what": what, "name": name, "file": shownFile(context.manifest, file) if file else "?"}
            for what, name, file in context.surface.tuLocal]


def checkMissing(context):
    findings = []
    for name in expectedNames(context):
        if name in context.unit.exportCounts:
            continue
        file = context.surface.names.get(name)
        where = f"declared in {shownFile(context.manifest, file)}" if file else "a foreign name of the row"
        findings.append({"name": name, "where": where})
    return findings


def isForeign(context, name):
    """Whether @p name is outside the module's namespaces: under none of them, nor under their root."""
    root = commonRoot(context.manifest.namespaces)
    return not (name.startswith(f"{root}::") or splitQualified(name)[0] in context.manifest.namespaces)


def checkStale(context):
    expected = set(expectedNames(context))
    return [{"name": name} for name in context.unit.exportCounts
            if name not in expected and not isForeign(context, name)]


def checkForeignNames(context):
    listed = set(context.manifest.foreignNames)
    return [{"name": name} for name in context.unit.exportCounts
            if isForeign(context, name) and name not in listed]


def checkGrammar(context):
    return [{"region": region, "line": line} for region, line in context.unit.disallowedLines]


def checkComplete(context):
    ending = context.unit.incompleteEnding
    return [] if ending is None else [{"ending": ending}]


def checkNamespaceMismatch(context):
    return [{"block": block, "name": qualified} for block, qualified in context.unit.usings
            if splitQualified(qualified)[0] != block]


def checkImports(context):
    rowImports = importedModuleNames(context.manifest)
    if sorted(context.unit.imports) == sorted(rowImports):
        return []
    return [{"unit": ", ".join(context.unit.imports), "row": ", ".join(rowImports)}]


def checkMacros(context):
    return [{"macro": macro, "header": shownFile(context.manifest, header)}
            for macro, header in leftDefinedMacros(context.manifest) if macro not in context.manifest.definedMacros]


def checkStaleDefinedMacros(context):
    """A macro the row lists as left defined that no header leaves defined: a rename went unnoticed, or its
    header is gone."""
    defined = {macro for macro, _ in leftDefinedMacros(context.manifest)}
    return [{"macro": macro} for macro in context.manifest.definedMacros if macro not in defined]


def checkCompilerConditionals(context):
    return [{"directive": directive, "macros": ", ".join(macros), "header": shownFile(context.manifest, header)}
            for directive, macros, header in compilerConditionals(context.manifest)]


def checkPrecompile(context):
    diagnostic = precompile(context.manifest, context.clang, context.workDirectory, {})
    return [] if diagnostic is None else [{"diagnostic": diagnostic}]


def checkDuplicates(context):
    return [{"name": name} for name, count in context.unit.exportCounts.items() if count > 1]


def checkHeadersIncluded(context):
    included = set(context.unit.includes)
    return [{"header": spelling, "namespaces": ", ".join(namespaces)}
            for spelling, namespaces in declaringHeaders(context.manifest, context.surface)
            if spelling not in included]


def checkHeadersInSet(context):
    return [{"header": header} for header in headersOutsideTheSet(context.manifest)]


def checkGeneratedForm(context):
    return [] if context.unit.text == context.generatedText else [{}]


# The rules --check applies: (rule, check, message). A check returns the findings' fields.
RULES = (
    ("grammar", checkGrammar, "not allowed in the {region}: line {line}"),
    ("complete", checkComplete, "incomplete interface unit: it ends {ending}"),
    ("unclassifiedNamespace", checkUnclassifiedNamespaces, "unclassified namespace: {namespace}"),
    ("unknownKind", checkUnknownKinds, "unknown declaration kind: {kind} {name} ({file})"),
    ("unexportable", checkUnexportable, "not exportable: {kind} {name} ({file}): {why}"),
    ("tuLocal", checkTuLocal,
     "TU-local declaration: {what} {name} ({file}): a public header declares nothing of internal linkage"),
    ("unjudged", checkUnjudged,
     "cannot judge the linkage of {kind} {name} ({file}): {diagnostic}: declare it so that its linkage is certain"),
    ("missing", checkMissing, "missing: {name} ({where})"),
    ("stale", checkStale, "stale: {name}"),
    ("duplicate", checkDuplicates, "duplicate: {name}"),
    ("namespaceMismatch", checkNamespaceMismatch, "export namespace mismatch: block {block} uses {name}"),
    ("imports", checkImports, "imports differ: .cppm has [{unit}], row has [{row}]"),
    ("foreignName", checkForeignNames, "foreign name not listed: {name}"),
    ("headerNotIncluded", checkHeadersIncluded, "header not included: {header} declares into {namespaces}"),
    ("headerOutsideSet", checkHeadersInSet,
     "header outside the header set: {header}, beside the set's own: add it to the library's FILE_SET HEADERS, "
     "or move it out of the public header directories"),
    ("macro", checkMacros, "macro left defined: {macro} in {header}"),
    ("staleDefinedMacro", checkStaleDefinedMacros,
     "stale DEFINED_MACROS entry: {macro}: no header of the set leaves it defined"),
    ("compilerConditional", checkCompilerConditionals,
     "compiler conditional: `{directive}` in {header} names {macros}: an installed header reads the same on every "
     "compiler"),
)
# The tiers of rules --check applies, in order; a tier runs only where every tier before it found nothing.
# The generated form says the unit is not what --write writes, which every finding of the first tier
# implies. The precompile is worth its compile only for a unit --write would write: what it then finds,
# the names and the headers cannot show (an entity of internal linkage).
RULE_TIERS = (
    RULES,
    (("generatedForm", checkGeneratedForm, "not in generated form: run --write"),),
    (("precompile", checkPrecompile, "precompile failed: {diagnostic}"),),
)

# The rules whose findings leave the surface undetermined, so that --write refuses to write.
SURFACE_RULES = ("unclassifiedNamespace", "unknownKind", "unexportable", "tuLocal", "unjudged")


def applyRules(context, rules):
    return [message.format(**fields) for _, check, message in rules for fields in check(context)]


def runCheck(manifest, surface, clang, workDirectory):
    unit = readInterfaceUnit(manifest.source)
    context = Context(manifest=manifest, surface=surface, unit=unit, generatedText=generate(manifest, surface),
                      clang=clang, workDirectory=workDirectory)
    findings = []
    for tier in RULE_TIERS:
        findings = applyRules(context, tier)
        if findings:
            break
    for finding in findings:
        print(finding)
    if findings:
        print(f"hint: regenerate {manifest.source} with --write (the target isoline_write_module_exports), "
              "once what it cannot fix above is fixed in the headers or in the row")
        return EXIT_FINDINGS
    return EXIT_CLEAN


def runWrite(manifest, surface):
    context = Context(manifest=manifest, surface=surface, unit=None, generatedText="")
    findings = applyRules(context, [rule for rule in RULES if rule[0] in SURFACE_RULES])
    if findings:
        for finding in findings:
            print(finding)
        print(f"error: {manifest.source} not written: the surface is undetermined", file=sys.stderr)
        return EXIT_FINDINGS
    manifest.source.write_text(generate(manifest, surface), encoding="utf-8")
    return EXIT_CLEAN


def listedNames(surface, kinds):
    """The lines of --list: each public name declared by a kind of @p kinds, with those of its kinds, sorted."""
    lines = []
    for name in sorted(surface.names):
        nameKinds = sorted(surface.kinds.get(name, set()) & set(kinds))
        if nameKinds:
            lines.append(f"{name} {','.join(nameKinds)}")
    return lines


def runList(manifest, surface, kinds):
    """Prints the public names (listedNames()), unless the surface is undetermined, as --write refuses to write."""
    context = Context(manifest=manifest, surface=surface, unit=None, generatedText="")
    findings = applyRules(context, [rule for rule in RULES if rule[0] in SURFACE_RULES])
    if findings:
        for finding in findings:
            print(finding)
        print("error: no names listed: the surface is undetermined", file=sys.stderr)
        return EXIT_FINDINGS
    for line in listedNames(surface, kinds):
        print(line)
    return EXIT_CLEAN


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang", required=True, help="the clang++ that dumps the AST (the pinned LLVM)")
    parser.add_argument("--manifest", required=True, type=pathlib.Path, help="the module's surface.json")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="report what the interface unit lacks")
    mode.add_argument("--write", action="store_true", help="regenerate the interface unit")
    mode.add_argument("--list", action="store_true",
                      help="print the public names, one per line, each with the kinds that declare it")
    parser.add_argument("--kinds", nargs="+", metavar="KIND",
                        help=f"--list only the names a declaration of these kinds declares (of {', '.join(LISTED_KINDS)})")
    parser.add_argument("--work-directory", type=pathlib.Path,
                        help="where the umbrella, the dump and the BMIs go (default: a temporary directory)")
    parser.add_argument("--dump-size-limit-bytes", type=int, default=DUMP_SIZE_LIMIT_BYTES,
                        help="the ceiling of the AST dump")
    arguments = parser.parse_args()
    kinds = arguments.kinds if arguments.kinds is not None else LISTED_KINDS
    try:
        unlistedKinds = [kind for kind in kinds if kind not in LISTED_KINDS]
        if unlistedKinds:
            raise ToolError(f"--kinds {' '.join(unlistedKinds)}: no kind the lint lists a name by "
                            f"({', '.join(LISTED_KINDS)})")
        if arguments.kinds is not None and not arguments.list:
            raise ToolError("--kinds selects what --list prints; it takes --list")
        manifest = readManifest(arguments.manifest.resolve())
        refuseContradictions(manifest)
        if arguments.work_directory is not None:
            arguments.work_directory.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=arguments.work_directory) as workDirectory:
            surface = collectSurface(manifest, arguments.clang, pathlib.Path(workDirectory),
                                     arguments.dump_size_limit_bytes)
            if arguments.write:
                return runWrite(manifest, surface)
            if arguments.list:
                return runList(manifest, surface, kinds)
            return runCheck(manifest, surface, arguments.clang, pathlib.Path(workDirectory))
    except ToolError as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_TOOL_ERROR
    except Exception as error:
        # A defect of the lint, or input it did not foresee (a manifest field of another type): exit 2, so
        # that it never reads as exit 1's findings.
        print(f"error: unexpected {type(error).__name__}: {error}", file=sys.stderr)
        return EXIT_TOOL_ERROR


if __name__ == "__main__":
    sys.exit(main())
