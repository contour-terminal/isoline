#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Checks the enum tables in source files, which the reflection checks cannot reach.

The reflection checks (src/isoline/meta/reflection/EnumTablesCheck.cpp) see every table at namespace
scope of a header. A table in a source file, in its anonymous namespace or a function, they cannot
see. This check scans every non-test `.cpp` under src/isoline/ for table declarations,
`… auto Name = std::to_array<Row>(…)` and `std::array<Row, N> Name …`. A table whose Row is a struct
whose first member is a scoped enum (an enum-first table) must be a row of src/isoline/meta/EnumTables.def:

    ISOLINE_SOURCE_LOCAL_TABLE("<file under src/isoline/>", Table, "reason")

The reason says why the table stays in the source file. Every such row must name a table that its file
declares, so a stale row fails. The block between `<!-- source-local-tables:begin -->` and
`<!-- source-local-tables:end -->` of docs/language-features.md lists the rows, one line each,
`` - `<file>`: `Table`, <reason> ``, in the registry's order. The check fails, printing a diff, when
the block differs from the rows; --update rewrites it.

Tests, test helpers (`testing/`) and the reflection checks' own sources (`meta/reflection/`) are not
scanned. The scan reads declarations textually. An enum is a scoped enum declared anywhere under src/isoline/,
and a row struct is looked up in the source file first, then in any file.

Usage: python3 -I scripts/check-enum-tables.py [--root <repository>] [--update]
"""

import argparse
import difflib
import pathlib
import re
import sys

SOURCES = pathlib.PurePosixPath("src/isoline")
REGISTRY = SOURCES / "meta" / "EnumTables.def"
DOCUMENT = pathlib.PurePosixPath("docs/language-features.md")
BLOCK = "source-local-tables"

SCOPED_ENUM = re.compile(r"\benum\s+class\s+(\w+)")
STRUCT = re.compile(r"\bstruct\s+(\w+)\s*(?:final\s*)?\{")
# A data member: an optional const, a type (possibly qualified), a name, an optional initializer.
MEMBER = re.compile(r"^\s*(?:const\s+)?([\w:]+)(<[^;]*>)?\s+\w+\s*(?:=[^;]*|\{[^;]*\})?;")
TO_ARRAY = re.compile(r"\bconstexpr\s+auto\s+(\w+)\s*=\s*std::to_array<\s*([\w:]+)\s*>")
STD_ARRAY = re.compile(r"\bstd::array<\s*([\w:]+)\s*,[^;>]*>\s+(\w+)\s*(?:=|\{)")
SOURCE_LOCAL_ROW = re.compile(
    r'ISOLINE_SOURCE_LOCAL_TABLE\(\s*"([^"]+)"\s*,\s*(\w+)\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)')
LINE_COMMENT = re.compile(r"//[^\n]*")


def begin():
    return f"<!-- {BLOCK}:begin -->"


def end():
    return f"<!-- {BLOCK}:end -->"


def withoutComments(text):
    """@p text without its line comments, so that a declaration a comment quotes is not one."""
    return LINE_COMMENT.sub("", text)


def firstMemberType(body):
    """The last component of the type of the first data member in @p body, a struct's text after its
    opening brace, or None when its first member is a template or none is found."""
    for line in body.splitlines():
        if not line.strip():
            continue
        match = MEMBER.match(line)
        if match is None:
            return None
        if match.group(2):
            return None
        return match.group(1).split("::")[-1]
    return None


def rowTypes(text):
    """The first-member type of each struct @p text defines, by name."""
    rows = {}
    for match in STRUCT.finditer(text):
        rows.setdefault(match.group(1), firstMemberType(text[match.end():]))
    return rows


# Source files the scan skips: tests and their helpers, and the reflection checks' own, which are not
# the library's.
SKIPPED = (re.compile(r"_test\.cpp$"), re.compile(r"(^|/)testing/"), re.compile(r"^meta/reflection/"))


def isSkipped(relative):
    return any(pattern.search(relative) for pattern in SKIPPED)


def scan(root):
    """The (file, table) pairs of every enum-first table in a source file, and every table each source
    file declares, by file."""
    sources = root / SOURCES
    texts = {path.relative_to(sources).as_posix(): withoutComments(path.read_text(encoding="utf-8"))
             for path in sorted(sources.rglob("*")) if path.suffix in (".hpp", ".cpp")}
    enums = {name for text in texts.values() for name in SCOPED_ENUM.findall(text)}
    globalRows = {}
    for text in texts.values():
        for name, member in rowTypes(text).items():
            globalRows.setdefault(name, member)
    enumFirst = []
    declared = {}
    for relative, text in texts.items():
        if not relative.endswith(".cpp") or isSkipped(relative):
            continue
        localRows = rowTypes(text)
        tables = [(m.group(1), m.group(2)) for m in TO_ARRAY.finditer(text)]
        tables += [(m.group(2), m.group(1)) for m in STD_ARRAY.finditer(text)]
        declared[relative] = {name for name, _ in tables}
        for name, row in tables:
            rowName = row.split("::")[-1]
            member = localRows[rowName] if rowName in localRows else globalRows.get(rowName)
            if member in enums:
                enumFirst.append((relative, name))
    return enumFirst, declared


def registryRows(root):
    """The ISOLINE_SOURCE_LOCAL_TABLE rows of the registry, in order: (file, table, reason)."""
    return SOURCE_LOCAL_ROW.findall(withoutComments((root / REGISTRY).read_text(encoding="utf-8")))


def documentBlock(rows):
    return [f"- `{file}`: `{table}`, {reason}" for file, table, reason in rows]


def splitDocument(text):
    """The document's text before, inside and after the block, or None without one."""
    head, sep, rest = text.partition(begin() + "\n")
    inside, sep2, tail = rest.partition(end())
    if not sep or not sep2:
        return None
    return head + sep, inside, sep2 + tail


def check(root, update):
    """The problems, one message each; with @p update, the document's block is rewritten first."""
    problems = []
    enumFirst, declared = scan(root)
    rows = registryRows(root)
    registered = {(file, table) for file, table, _ in rows}
    for file, table in enumFirst:
        if (file, table) not in registered:
            problems.append(
                f"src/isoline/{file}: {table} is an enum-first table in a source file, which reflection cannot "
                f"reach: move it into a header (and give it its row), or add "
                f'ISOLINE_SOURCE_LOCAL_TABLE("{file}", {table}, "<why it stays>") to {REGISTRY}')
    for file, table, _ in rows:
        if table not in declared.get(file, set()):
            problems.append(f"{REGISTRY}: ISOLINE_SOURCE_LOCAL_TABLE(\"{file}\", {table}, …) is stale: "
                            f"src/isoline/{file} declares no table {table}")

    document = root / DOCUMENT
    parts = splitDocument(document.read_text(encoding="utf-8"))
    if parts is None:
        problems.append(f"{DOCUMENT}: no block {begin()} … {end()}")
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
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent)
    parser.add_argument("--update", action="store_true", help="rewrite the document's block")
    arguments = parser.parse_args()
    problems = check(arguments.root, arguments.update)
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"source-local enum tables: every one is registered, and {DOCUMENT} lists them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
