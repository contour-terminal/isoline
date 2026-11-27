#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Checks the enum tables reflection cannot reach, from Clang's AST.

The reflection checks (src/isoline/meta/reflection/EnumTablesCheck.cpp) see every table at namespace
scope of a header. This lint sees the rest: it compiles every first-party source file of a Clang build
tree (its compile_commands.json) with Clang's JSON AST dump, filtered to Isoline's namespaces, and finds
every variable whose type is `std::array<Row, N>` after sugar, however it is spelled: `std::to_array`
with or without its template argument, class template argument deduction, east or west const, a macro
or a helper that builds it. A variable counts when it is declared in a source file (at namespace scope,
an anonymous namespace's included, in a function or a class), or in a header anywhere but at namespace
scope. It is an enum-first table when Row is a class whose first field's type, read from the class's
declaration (so neither const nor its spelling matters), is a scoped enum.

Every enum-first table must be a row of src/isoline/meta/EnumTables.def:

    ISOLINE_SOURCE_LOCAL_TABLE("<file under src/isoline/>", Table, "reason")

and every such row must name a `std::array` variable that its file declares, so a stale row fails. The
block between `<!-- source-local-tables:begin -->` and `<!-- source-local-tables:end -->` of
docs/language-features.md lists the rows, one line each, `` - `<file>`: `Table`, <reason> ``, in the
registry's order; the lint fails, printing a diff, when it differs, and --update rewrites it.

Tests (`*_test.cpp`), test helpers (any `testing/` directory) and the reflection checks' own sources
(`meta/reflection/`) are not linted, and the dump holds declarations under a namespace named isoline
only. So that nothing escapes it, a text pass over the same source files fails on every `std::array` or
`std::to_array` spelt outside `namespace isoline`: in main(), a global anonymous namespace or any other
namespace. It reads the code alone (comments, literals and preprocessor directives left out), so a
file-scope macro that builds a table inside `namespace isoline` is not flagged. A field type written through a type alias or a using-declaration comes desugared; a class or
enum named directly comes as written, and is resolved from the class's scope outwards through its
enclosing scopes, and failing that by its name alone. One that resolves to more than one kind of type is a tool error, so the lint fails
closed. The dumps run in parallel (--jobs), and each translation unit's result
is cached (--cache), keyed by its compile command and the content of the file, of every header under
src/isoline/ and the generated headers, and of this script: a time stamp alone invalidates nothing.

Exit status: 0 clean, 1 findings, 2 a tool error (Clang failed, a row type the lint cannot find, an
ambiguous field type).

Usage: python3 -I scripts/check-enum-tables.py --compile-commands <compile_commands.json>
           [--clang <clang++>] [--root <repository>] [--cache <directory>] [--jobs <n>] [--update]
"""

import argparse
import concurrent.futures
import difflib
import hashlib
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys

SOURCES = pathlib.PurePosixPath("src/isoline")
REGISTRY = SOURCES / "meta" / "EnumTables.def"
DOCUMENT = pathlib.PurePosixPath("docs/language-features.md")
BLOCK = "source-local-tables"

# The dump holds the declarations whose qualified name contains this: Isoline's namespaces.
DUMP_FILTER = "isoline"

# Bumped when what a cached result means changes.
CACHE_VERSION = "2"

# Files under src/isoline/ the lint skips: tests, their helpers, and the reflection checks' own sources.
SKIPPED = (re.compile(r"_test\.cpp$"), re.compile(r"(^|/)testing/"), re.compile(r"^meta/reflection/"))

SOURCE_LOCAL_ROW = re.compile(
    r'ISOLINE_SOURCE_LOCAL_TABLE\(\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)')
LINE_COMMENT = re.compile(r"//[^\n]*")
ANONYMOUS = "(anonymous namespace)"
CV_QUALIFIERS = re.compile(r"\b(const|volatile)\b")
ELABORATION = re.compile(r"^(enum|struct|class|union)\s+")

# The kinds whose children repeat a declaration made elsewhere: a using-declaration's shadow dumps its
# target again.
SHADOWS = ("UsingShadowDecl", "ConstructorUsingShadowDecl", "UsingEnumDecl")

# What a resolved type is.
SCOPED_ENUM = "scoped enum"
OTHER = "other"


class ToolError(Exception):
    """A failure of the lint itself, not a finding."""


# What the text pass sees of a source file's code: a namespace opening (its name, or none for an
# anonymous one), any other brace, and a spelling of std::array.
CODE_TOKEN = re.compile(r"\bnamespace\b\s*(?P<namespace>[\w:\s]*?)\s*\{|(?P<array>\bstd\s*::\s*(?:to_)?array\b)"
                        r"|(?P<open>\{)|(?P<close>\})")
RAW_STRING = re.compile(r'(?<!\w)(?:u8|u|U|L)?R"([^()\\\s]{0,16})\(')
WORD_BEFORE = re.compile(r"[\w']*$")
NAMESPACE = "isoline"


def withoutComments(text):
    return LINE_COMMENT.sub("", text)


def codeOnly(text):
    """@p text, a source file, with its comments, string and character literals and preprocessor
    directives blanked out, every newline kept so that lines keep their numbers."""
    output = []
    index = 0
    lineStart = True
    while index < len(text):
        character = text[index]
        if character == "\n":
            output.append(character)
            index += 1
            lineStart = True
            continue
        if lineStart and character == "#":
            end = index
            while end < len(text) and text[end] != "\n":
                end += 2 if text.startswith("\\\n", end) else 1
            output.append("".join("\n" if c == "\n" else " " for c in text[index:end]))
            index = end
            continue
        if not character.isspace():
            lineStart = False
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = len(text) if end < 0 else end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = len(text) if end < 0 else end + 2
        elif raw := RAW_STRING.match(text, index):
            closing = text.find(")" + raw.group(1) + '"', raw.end())
            end = len(text) if closing < 0 else closing + len(raw.group(1)) + 2
        elif character == '"' or (character == "'" and not isDigitSeparator(text, index)):
            end = index + 1
            while end < len(text) and text[end] not in (character, "\n"):
                end += 2 if text[end] == "\\" else 1
            end += 1
        else:
            output.append(character)
            index += 1
            continue
        output.append("".join("\n" if c == "\n" else " " for c in text[index:end]))
        index = end
    return "".join(output)


def isDigitSeparator(text, index):
    """Whether the quote at @p index of @p text separates the digits of a number (1'000), rather than
    opening a character literal (u8'a')."""
    before = WORD_BEFORE.search(text, max(0, index - 64), index).group()
    return before[:1].isdigit()


def arraysOutsideIsoline(text):
    """The lines of @p text, a source file, that spell std::array or std::to_array outside namespace
    isoline, each with its spelling: there the AST dump does not reach."""
    code = codeOnly(text)
    stack = []  # per open brace: the namespace's first component ("" when anonymous), or None
    found = []
    for match in CODE_TOKEN.finditer(code):
        if match.group("namespace") is not None:
            components = splitScopes(match.group("namespace").replace("inline", " ").replace(" ", ""))
            stack.append(components[0] if components else "")
        elif match.group("open"):
            stack.append(None)
        elif match.group("close"):
            if stack:
                stack.pop()
        elif not stack or stack[0] != NAMESPACE:
            found.append((code.count("\n", 0, match.start()) + 1, " ".join(match.group("array").split())))
    return found


def isSkipped(relative):
    return any(pattern.search(relative) for pattern in SKIPPED)


def splitTopLevel(text, separator=","):
    """@p text split at each @p separator outside angle brackets and parentheses."""
    parts = []
    depth = 0
    current = ""
    for character in text:
        if character in "<(":
            depth += 1
        elif character in ">)":
            depth -= 1
        if character == separator and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += character
    parts.append(current.strip())
    return parts


def splitScopes(name):
    """The components of a qualified name, `::` inside template arguments left alone."""
    components = []
    depth = 0
    current = ""
    index = 0
    while index < len(name):
        character = name[index]
        if character in "<(":
            depth += 1
        elif character in ">)":
            depth -= 1
        if depth == 0 and name.startswith("::", index):
            components.append(current)
            current = ""
            index += 2
            continue
        current += character
        index += 1
    components.append(current)
    return [component for component in components if component]


def cleanType(spelled):
    """@p spelled without cv-qualifiers, an elaborating keyword or surrounding blanks."""
    return ELABORATION.sub("", CV_QUALIFIERS.sub("", spelled).strip()).strip()


# A standard library's inline namespace, which its types print with: libc++'s `std::__1::array`.
STANDARD_INLINE_NAMESPACE = re.compile(r"\bstd::__\w+::")


def arrayRow(typeName):
    """The Row of `std::array<Row, N>` (cv-qualified or not), or None for any other type."""
    name = STANDARD_INLINE_NAMESPACE.sub("std::", cleanType(typeName))
    if not name.startswith("std::array<") or not name.endswith(">"):
        return None
    arguments = splitTopLevel(name[len("std::array<"):-1])
    return arguments[0] if len(arguments) == 2 else None


class Index:
    """What one translation unit's dump declares: the types, the records' first fields, and every
    std::array variable, each with its file and scope."""

    def __init__(self, sources, directory):
        self.sources = sources
        self.directory = directory
        self.types = {}      # qualified name -> kind: SCOPED_ENUM, "enum" or "record"
        self.byLeaf = {}     # leaf name -> qualified names
        self.records = {}    # qualified name -> (first field's type, record scope) or (None, scope)
        self.arrays = []     # (file, name, row as written, scope, scope kinds)
        self.currentFile = None

    def addType(self, scope, name, kind):
        qualified = "::".join(scope + [name])
        self.types.setdefault(qualified, kind)
        self.byLeaf.setdefault(name, set()).add(qualified)

    def track(self, value):
        """Follows the file in force through the locations in @p value, in document order."""
        if isinstance(value, dict):
            for key, item in value.items():
                if key == "includedFrom":
                    continue
                if key == "file":
                    self.currentFile = item
                elif key != "inner":
                    self.track(item)
        elif isinstance(value, list):
            for item in value:
                self.track(item)

    def walk(self, root):
        """Indexes @p root, a top-level declaration of the dump, in document order."""
        # A stack of (node, scope, scope kinds) to visit, and markers that end nothing: the scope travels
        # with each node, so leaving one needs no bookkeeping.
        stack = [(root, [], [])]
        while stack:
            node, scope, kinds = stack.pop()
            if not isinstance(node, dict):
                continue
            kind = node.get("kind")
            self.track(node.get("loc", {}))
            file = self.currentFile
            for key, value in node.items():
                if key not in ("loc", "inner"):
                    self.track(value)
            childScope, childKinds = scope, kinds
            name = node.get("name")
            if kind == "NamespaceDecl":
                childScope, childKinds = scope + [name or ANONYMOUS], kinds + ["namespace"]
            elif kind == "EnumDecl" and name:
                self.addType(scope, name, SCOPED_ENUM if node.get("scopedEnumTag") else "enum")
            elif kind in ("CXXRecordDecl", "ClassTemplateSpecializationDecl") and name:
                childScope, childKinds = self.record(node, scope, kinds, name)
            elif kind in ("FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl",
                          "CXXConversionDecl", "LambdaExpr"):
                childScope, childKinds = scope + [f"{name or 'lambda'}()"], kinds + ["function"]
            elif kind == "VarDecl" and name:
                typeName = node.get("type", {})
                row = arrayRow(typeName.get("desugaredQualType", typeName.get("qualType", "")))
                if row is not None:
                    self.arrays.append((file, name, row, list(scope), list(kinds)))
            if kind in SHADOWS:
                continue  # it repeats the declaration it brings in, which is indexed where it is declared
            for child in reversed(node.get("inner", [])):
                stack.append((child, childScope, childKinds))

    def record(self, node, scope, kinds, name):
        """Indexes a class definition: its name and its first field. The class a definition implicitly
        declares inside itself is no definition."""
        if node.get("isImplicit") or not node.get("completeDefinition"):
            return scope, kinds
        inner = node.get("inner", [])
        if node.get("kind") == "ClassTemplateSpecializationDecl":
            arguments = [child.get("type", {}).get("qualType", "?") for child in inner
                         if child.get("kind") == "TemplateArgument"]
            name = f"{name}<{', '.join(arguments)}>"
        self.addType(scope, name, "record")
        fields = [child for child in inner if child.get("kind") == "FieldDecl"]
        first = None
        if fields:
            fieldType = fields[0].get("type", {})
            first = fieldType.get("desugaredQualType", fieldType.get("qualType"))
        self.records.setdefault("::".join(scope + [name]), (first, list(scope)))
        return scope + [name], kinds + ["record"]

    def lookup(self, spelled, scope):
        """The qualified name @p spelled denotes from @p scope, or the candidates when no enclosing
        scope declares it."""
        components = splitScopes(spelled)
        if spelled.startswith("::"):
            return "::".join(components), None
        for depth in range(len(scope), -1, -1):
            base = scope[:depth]
            qualified = "::".join(base + components)
            if qualified in self.types:
                return qualified, None
        leaf = components[-1]
        candidates = sorted(name for name in self.byLeaf.get(leaf, ())
                            if splitScopes(name)[-len(components):] == components)
        return None, candidates

    def kindOf(self, spelled, scope):
        """What @p spelled, a type as written in @p scope, is: SCOPED_ENUM or OTHER. A type alias needs no
        resolving here, nor one a using-declaration brings in: Clang gives a type written through either its
        desugared, fully qualified spelling. A class or enum named directly is printed as written."""
        name = cleanType(spelled)
        if not name or any(mark in name for mark in "*&[(") or name.endswith(">"):
            return OTHER
        qualified, candidates = self.lookup(name, scope)
        if qualified is None:
            kinds = {self.kindOfDeclared(candidate) for candidate in candidates}
            if not kinds:
                return OTHER
            if len(kinds) > 1:
                raise ToolError(f"the type {spelled} in {'::'.join(scope)} may be any of {', '.join(candidates)}")
            return kinds.pop()
        return self.kindOfDeclared(qualified)

    def kindOfDeclared(self, qualified):
        return SCOPED_ENUM if self.types.get(qualified) == SCOPED_ENUM else OTHER

    def rowRecord(self, row, scope):
        """The record @p row names from @p scope: its key in self.records, or None for a row that is no
        class of Isoline's (a standard type, an enum, a pointer)."""
        name = cleanType(row)
        if name in self.records:
            return name
        for depth in range(len(scope), -1, -1):
            qualified = "::".join(scope[:depth] + splitScopes(name))
            if qualified in self.records:
                return qualified
        if name.startswith("std::") or self.kindOf(name, scope) == SCOPED_ENUM or name in ("int", "char"):
            return None
        if not any(mark in name for mark in "*&(") and (name.startswith(DUMP_FILTER + "::") or "::" not in name):
            qualified, candidates = self.lookup(name, scope)
            if qualified is None and not candidates and "<" not in name:
                return None
            raise ToolError(f"no class definition found for the row type {row} (in {'::'.join(scope)})")
        return None

    def result(self):
        """Every std::array variable the lint is responsible for: (file, name, whether enum-first)."""
        tables = {}
        for file, name, row, scope, kinds in self.arrays:
            if file is None:
                continue
            path = (self.directory / file).resolve()
            try:
                relative = path.relative_to(self.sources).as_posix()
            except ValueError:
                continue
            if isSkipped(relative):
                continue
            if relative.endswith((".hpp", ".h")) and all(kind == "namespace" for kind in kinds) \
                    and ANONYMOUS not in scope:
                continue  # a header's namespace scope: the reflection checks' domain
            record = self.rowRecord(row, scope)
            enumFirst = False
            if record is not None:
                first, recordScope = self.records[record]
                enumFirst = first is not None and self.kindOf(first, recordScope + [splitScopes(record)[-1]]) \
                    == SCOPED_ENUM
            key = (relative, name)
            tables[key] = tables.get(key, False) or enumFirst
        return [[file, name, enumFirst] for (file, name), enumFirst in sorted(tables.items())]


def readRoots(text):
    """The dump's top-level JSON values: Clang writes one per declaration the filter matches."""
    decoder = json.JSONDecoder()
    whitespace = re.compile(r"\s*")
    position = whitespace.match(text, 0).end()
    while position < len(text):
        value, position = decoder.raw_decode(text, position)
        yield value
        position = whitespace.match(text, position).end()


def dumpCommand(entry, clang):
    """The compile command of @p entry, a compile_commands.json row, as a filtered JSON AST dump."""
    arguments = entry["arguments"] if "arguments" in entry else shlex.split(entry["command"])
    dump = [clang or arguments[0]]
    skipNext = False
    for argument in arguments[1:]:
        if skipNext:
            skipNext = False
        elif argument in ("-o", "-MF", "-MT", "-MQ"):
            skipNext = True
        elif argument in ("-c", "-MD", "-MMD") or argument.startswith("-o"):
            continue
        else:
            dump.append(argument)
    # -w: the lint judges declarations, not warnings, and a tree's -Werror must not fail it.
    return dump + ["-w", "-fsyntax-only", "-Xclang", "-ast-dump=json", "-Xclang", "-ast-dump-filter",
                   "-Xclang", DUMP_FILTER]


def lintUnit(relative, arguments, directory, sources):
    """Dumps one translation unit and indexes it; returns its result, or raises ToolError."""
    process = subprocess.run(arguments, cwd=directory, capture_output=True, check=False)
    if process.returncode != 0:
        errors = [line for line in process.stderr.decode("utf-8", "replace").splitlines() if "error" in line]
        raise ToolError(f"Clang failed (exit {process.returncode}) on src/isoline/{relative}: "
                        + " | ".join(errors[:3]))
    index = Index(pathlib.Path(sources), pathlib.Path(directory))
    for root in readRoots(process.stdout.decode("utf-8", "replace")):
        index.walk(root)
    return index.result()


def unitsOf(compileCommands, sources):
    """The compile_commands.json rows of the first-party source files the lint covers."""
    entries = json.loads(compileCommands.read_text(encoding="utf-8"))
    units = {}
    for entry in entries:
        path = pathlib.Path(entry["directory"], entry["file"]).resolve()
        try:
            relative = path.relative_to(sources).as_posix()
        except ValueError:
            continue
        if relative.endswith(".cpp") and not isSkipped(relative):
            units.setdefault(relative, entry)
    return units


def treeStamp(root, compileCommands):
    """What every cached result depends on besides its own file: the content of every header under
    src/isoline/ and under the build tree's generated include directory, and of this script."""
    digest = hashlib.sha256(pathlib.Path(__file__).read_bytes())
    for directory in (root / SOURCES, compileCommands.parent / "include"):
        if directory.is_dir():
            for path in sorted(directory.rglob("*")):
                if path.suffix in (".hpp", ".h", ".def", ".inc") and path.is_file():
                    digest.update(str(path).encode() + b"\0" + hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def scan(root, compileCommands, clang, cache, jobs):
    """Every std::array variable the lint covers, over every translation unit: (file, name, enum-first)."""
    sources = (root / SOURCES).resolve()
    units = unitsOf(compileCommands, sources)
    if not units:
        raise ToolError(f"{compileCommands} holds no source file under {sources}")
    stamp = treeStamp(root, compileCommands)
    results = {}
    pending = {}
    for relative, entry in sorted(units.items()):
        arguments = dumpCommand(entry, clang)
        source = sources / relative
        key = hashlib.sha256("\0".join([CACHE_VERSION, stamp, hashlib.sha256(source.read_bytes()).hexdigest(),
                                        *arguments]).encode()).hexdigest()
        cached = cache / f"{hashlib.sha256(relative.encode()).hexdigest()}.json" if cache else None
        if cached is not None and cached.is_file():
            stored = json.loads(cached.read_text(encoding="utf-8"))
            if stored.get("key") == key:
                results[relative] = stored["tables"]
                continue
        pending[relative] = (arguments, entry["directory"], key, cached)
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as executor:
        futures = {executor.submit(lintUnit, relative, arguments, directory, str(sources)): relative
                   for relative, (arguments, directory, _, _) in pending.items()}
        for future in concurrent.futures.as_completed(futures):
            relative = futures[future]
            tables = future.result()
            results[relative] = tables
            _, _, key, cached = pending[relative]
            if cached is not None:
                cached.parent.mkdir(parents=True, exist_ok=True)
                cached.write_text(json.dumps({"key": key, "tables": tables}), encoding="utf-8")
    merged = {}
    for tables in results.values():
        for file, name, enumFirst in tables:
            merged[(file, name)] = merged.get((file, name), False) or enumFirst
    return merged


def escapedArrays(root, compileCommands):
    """The findings of the text pass: every std::array spelt outside namespace isoline in a source file
    the lint covers, one message each."""
    sources = (root / SOURCES).resolve()
    problems = []
    for relative in sorted(unitsOf(compileCommands, sources)):
        for line, spelling in arraysOutsideIsoline((sources / relative).read_text(encoding="utf-8")):
            problems.append(f"src/isoline/{relative}:{line}: {spelling} outside namespace isoline, where the "
                            f"AST lint does not look: move it into namespace isoline")
    return problems


def registryRows(root):
    """The ISOLINE_SOURCE_LOCAL_TABLE rows of the registry, in order: (file, table, reason)."""
    return SOURCE_LOCAL_ROW.findall(withoutComments((root / REGISTRY).read_text(encoding="utf-8")))


def documentBlock(rows):
    return [f"- `{file}`: `{table}`, {reason}" for file, table, reason in rows]


def splitDocument(text):
    head, sep, rest = text.partition(f"<!-- {BLOCK}:begin -->\n")
    inside, sep2, tail = rest.partition(f"<!-- {BLOCK}:end -->")
    if not sep or not sep2:
        return None
    return head + sep, inside, sep2 + tail


def check(root, tables, update):
    """The findings, one message each; with @p update, the document's block is rewritten first."""
    problems = []
    rows = registryRows(root)
    registered = {(file, table) for file, table, _ in rows}
    for (file, table), enumFirst in sorted(tables.items()):
        if enumFirst and (file, table) not in registered:
            problems.append(
                f"src/isoline/{file}: {table} is an enum-first table reflection cannot reach: move it to a "
                f"header's namespace scope (and give it its row), or add "
                f'ISOLINE_SOURCE_LOCAL_TABLE("{file}", {table}, "<why it stays>") to {REGISTRY}')
    for file, table, _ in rows:
        if (file, table) not in tables:
            problems.append(f'{REGISTRY}: ISOLINE_SOURCE_LOCAL_TABLE("{file}", {table}, …) is stale: '
                            f"src/isoline/{file} declares no std::array {table} the lint covers")
    document = root / DOCUMENT
    parts = splitDocument(document.read_text(encoding="utf-8"))
    if parts is None:
        problems.append(f"{DOCUMENT}: no block <!-- {BLOCK}:begin --> … <!-- {BLOCK}:end -->")
        return problems
    head, inside, tail = parts
    wanted = documentBlock(rows)
    actual = inside.splitlines()
    if actual != wanted:
        if update:
            document.write_text(head + "\n".join(wanted) + "\n" + tail, encoding="utf-8")
        else:
            diff = "\n".join(difflib.unified_diff(actual, wanted, "document", "registry", lineterm=""))
            problems.append(f"{DOCUMENT}: the {BLOCK} block differs from the registry's rows "
                            f"(run with --update):\n{diff}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compile-commands", type=pathlib.Path, required=True)
    parser.add_argument("--clang", help="the Clang to dump with (default: each command's compiler)")
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent)
    parser.add_argument("--cache", type=pathlib.Path, help="where to keep each translation unit's result")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--update", action="store_true", help="rewrite the document's block")
    arguments = parser.parse_args()
    try:
        tables = scan(arguments.root.resolve(), arguments.compile_commands.resolve(), arguments.clang,
                      arguments.cache, arguments.jobs)
    except ToolError as error:
        print(f"check-enum-tables: {error}", file=sys.stderr)
        return 2
    problems = escapedArrays(arguments.root.resolve(), arguments.compile_commands.resolve()) \
        + check(arguments.root, tables, arguments.update)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    count = sum(1 for enumFirst in tables.values() if enumFirst)
    print(f"source-local enum tables: {count} found, each registered; {DOCUMENT} lists them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
