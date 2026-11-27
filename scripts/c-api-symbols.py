#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""The symbols of Isoline's C API: what its headers declare, what src/isoline/c/exported-symbols.txt
lists, and what the shared library exports.

declared     prints, sorted, every function the given header declares for export: wasm.h's
             (WASM_API_EXTERN) and Isoline's own (ISOLINE_API). The header is preprocessed with both
             macros defined to a marker, so the declarations a macro expands to (WASM_DECLARE_VEC and
             its siblings) are found as the compiler sees them, and a declaration is the marker's
             identifier before its `(`.
check-list   checks the list file against the header: sorted, one name per line, no repeats, and equal
             to what the header declares. With --implemented the list may be a subset, the functions
             implemented so far; the C API's last task drops the flag and the list is total.
exports      checks what a shared library exports against the list file, exactly: a symbol the list
             lacks is a leak through the export rules (version script, exported-symbols list,
             dllexport), and a listed symbol that is missing is an implementation lost.

Usage:
  c-api-symbols.py declared --compiler <cc> [-I <dir>]... <header>
  c-api-symbols.py check-list --compiler <cc> [-I <dir>]... --list <file> [--implemented] <header>
  c-api-symbols.py exports --format elf|pe|macho --tool <nm or llvm-readobj> --list <file> <library>
"""

import argparse
import pathlib
import re
import subprocess
import sys

MARKER = "__isoline_export"

# The macros a declaration for export starts with; each is defined to MARKER while scanning.
EXPORT_MACROS = ("WASM_API_EXTERN", "ISOLINE_API")

DECLARATION = re.compile(MARKER + r"\b[^;{}()]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


# How each object format's exports are listed: the tool's arguments, the pattern of a line naming an
# exported symbol (its name is the group `name`), what of a matching line is no symbol at all, and the
# patterns of the listing's framing, which names nothing. Any other line is refused: a listing this
# cannot read is a failure, never an empty list that passes.
EXPORT_FORMATS = {
    # nm -D lists the version node of a version script as an absolute symbol of its own name (GNU nm;
    # llvm-nm leaves it out).
    "elf": {"arguments": ["-D", "--defined-only"],
            "line": re.compile(r"^(?:[0-9a-fA-F]+\s+)?(?P<type>[A-Za-z])\s+(?P<name>[^\s@]+)(?:@@?\S+)?$"),
            "ignored": lambda match: match["type"] == "A" and match["name"].startswith("ISOLINE_C_"),
            "framing": ()},
    "pe": {"arguments": ["--coff-exports"],
           "line": re.compile(r"^Name:\s+(?P<name>\S+)$"),
           "ignored": lambda match: False,
           "framing": tuple(re.compile(pattern) for pattern in (
               r"^File: .+$", r"^Format: \S+$", r"^Arch: \S+$", r"^AddressSize: \S+$", r"^Export \{$",
               r"^Ordinal: \d+$", r"^RVA: 0x[0-9A-Fa-f]+$", r"^\}$"))},
    # Mach-O prefixes every C symbol with an underscore.
    "macho": {"arguments": ["-gU"],
              "line": re.compile(r"^(?:[0-9a-fA-F]+\s+)?[A-Za-z]\s+_(?P<name>\S+)$"),
              "ignored": lambda match: False,
              "framing": ()},
}


def preprocess(compiler, includes, header):
    """The header as the C preprocessor expands it, with every export macro defined to the marker."""
    command = [compiler, "-E", "-P", "-x", "c"]
    command += [f"-D{macro}={MARKER}" for macro in EXPORT_MACROS]
    for include in includes:
        command += ["-I", str(include)]
    command.append(str(header))
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit(f"c-api-symbols: preprocessing {header} failed:\n{result.stderr}")
    return result.stdout


def declaredNames(preprocessed):
    """The names of the functions the preprocessed text declares for export, sorted."""
    return sorted(set(DECLARATION.findall(preprocessed)))


def readList(path):
    """The names of a list file in their order, and the problems of its form."""
    names = []
    problems = []
    for number, line in enumerate(pathlib.Path(path).read_text(encoding="utf-8").splitlines(), start=1):
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", line):
            problems.append(f"{path}:{number}: '{line}' is not one name")
            continue
        names.append(line)
    if names != sorted(names):
        problems.append(f"{path}: the names are not sorted")
    if len(names) != len(set(names)):
        problems.append(f"{path}: a name appears twice")
    return names, problems


def listProblems(listed, declared, implemented):
    """What is wrong with a list of names, given what the header declares."""
    declaredSet = set(declared)
    problems = [f"{name}: listed, but the header declares no such function" for name in listed
                if name not in declaredSet]
    if not implemented:
        listedSet = set(listed)
        problems += [f"{name}: declared, but not listed" for name in declared if name not in listedSet]
    return problems


def exportedNames(output, exportFormat):
    """The names a library's export listing (the tool's output) names, sorted, and the lines it could
    not read."""
    row = EXPORT_FORMATS[exportFormat]
    names = set()
    problems = []
    for line in output.splitlines():
        text = line.strip()
        if not text or any(pattern.match(text) for pattern in row["framing"]):
            continue
        match = row["line"].match(text)
        if match is None:
            problems.append(f"unreadable line of the {exportFormat} export listing: '{text}'")
        elif not row["ignored"](match):
            names.add(match["name"])
    return sorted(names), problems


def exportProblems(exported, listed):
    """How a library's exports differ from the list."""
    exportedSet = set(exported)
    listedSet = set(listed)
    return ([f"{name}: exported, but not listed" for name in exported if name not in listedSet]
            + [f"{name}: listed, but not exported" for name in listed if name not in exportedSet])


def report(problems, success):
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(success)
    return 0


def main(arguments):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("declared", "check-list"):
        command = commands.add_parser(name)
        command.add_argument("--compiler", required=True)
        command.add_argument("-I", dest="includes", action="append", default=[])
        command.add_argument("header")
        if name == "check-list":
            command.add_argument("--list", required=True)
            command.add_argument("--implemented", action="store_true")
    exports = commands.add_parser("exports")
    exports.add_argument("--format", required=True, choices=sorted(EXPORT_FORMATS))
    exports.add_argument("--tool", required=True)
    exports.add_argument("--list", required=True)
    exports.add_argument("library")
    options = parser.parse_args(arguments)

    if options.command == "exports":
        command = [options.tool, *EXPORT_FORMATS[options.format]["arguments"], options.library]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise SystemExit(f"c-api-symbols: {' '.join(command)} failed:\n{result.stderr}")
        listed, problems = readList(options.list)
        exported, unreadable = exportedNames(result.stdout, options.format)
        return report(problems + unreadable + exportProblems(exported, listed),
                      f"{options.library} exports exactly the {len(listed)} listed symbol(s)")

    declared = declaredNames(preprocess(options.compiler, options.includes, options.header))
    if options.command == "declared":
        print("\n".join(declared))
        return 0
    listed, problems = readList(options.list)
    return report(problems + listProblems(listed, declared, options.implemented),
                  f"{options.list}: {len(listed)} of {len(declared)} declared function(s) listed")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
