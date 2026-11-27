#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Compares a module's surface as two readings of its headers see it: the export lint's and reflection's.

The export lint (check-module-exports.py --list) reads the header set through Clang's AST dump; the reflected
surface (a program of isoline_add_module_surface_reflection(), cmake/IsolineModuleSurface.cmake) is
GCC's `std::meta::members_of` over the row's public namespaces, printed one qualified name per line. The two are
different compilers' readings of the same headers, so a name only one of them sees is a declaration only
one compiler compiles (an `#if` on the compiler around a public declaration), or a kind of declaration one
reading does not report: either is a finding.

Some kinds of declaration `members_of` does not report at all: the rows of --unreflected-kinds, measured
(GCC 16.2.1, 2026-11-27: using-declarations, `UsingDecl`). A name the lint lists only by such kinds is left
out of the comparison, and one the program reflects all the same is a finding too: the measurement no longer
holds, so the row is stale.

The program's umbrella header (--umbrella) is compared too: it must include exactly the header set's headers, as
the lint's umbrella does, so that the two read the same headers. A header only one of them reads is a difference.

Exit status: 0 the two agree, 1 differences, 2 a tool error (the lint or the program failed).

Usage: python3 -I scripts/compare-module-surface.py --clang <clang++> --manifest <surface.json>
           [--work-directory <dir>] [--umbrella <header>] [--unreflected-kinds <kind>...]
           -- <program> [<argument>...]
"""

import argparse
import importlib.util
import pathlib
import re
import subprocess
import sys

EXIT_AGREE = 0
EXIT_DIFFERENCES = 1
EXIT_TOOL_ERROR = 2

LINT = pathlib.Path(__file__).resolve().parent / "check-module-exports.py"

# Why a name only one reading sees differs, by the reading that misses it.
UNLISTED_REASON = "a declaration only one compiler sees (an #if on the compiler), or a kind the lint does not export"
UNREFLECTED_REASON = ("a declaration only one compiler sees (an #if on the compiler), or a kind members_of "
                      "does not report")


class ToolError(Exception):
    """A failure of a reading, not a difference: exit status 2."""


# An include directive of the umbrella: its spelling, angled.
INCLUDE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*<([^>\n]+)>", re.MULTILINE)


def loadLint():
    """check-module-exports.py as a module, so that a header's spelling is the lint's (includeSpelling())."""
    spec = importlib.util.spec_from_file_location("check_module_exports", LINT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def umbrellaDifferences(manifestPath, umbrella):
    """The lines naming each header the umbrella includes that is no header of the set, and each header of the
    set it does not include."""
    lint = loadLint()
    try:
        manifest = lint.readManifest(manifestPath.resolve())
        text = umbrella.read_text(encoding="utf-8")
    except (lint.ToolError, OSError, UnicodeDecodeError) as error:
        raise ToolError(f"the umbrella cannot be compared: {error}") from error
    expected = {lint.includeSpelling(manifest, header) for header in lint.umbrellaHeaders(manifest)}
    included = set(INCLUDE.findall(text))
    lines = [f"umbrella: includes {header}, which is no header of the set: the program reads what the lint does not"
             for header in sorted(included - expected)]
    lines += [f"umbrella: lacks {header}, a header of the set: the program does not read what the lint does"
              for header in sorted(expected - included)]
    return lines


def listedKinds(arguments):
    """The export lint's names: qualified name -> the declaration kinds that declare it."""
    command = [sys.executable, "-I", str(LINT), "--clang", arguments.clang, "--manifest", str(arguments.manifest),
               "--list"]
    if arguments.work_directory is not None:
        command += ["--work-directory", str(arguments.work_directory)]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise ToolError(f"the export lint exited {result.returncode}:\n{result.stdout}{result.stderr}".rstrip())
    kinds = {}
    for line in result.stdout.splitlines():
        name, _, kindList = line.partition(" ")
        kinds[name] = set(kindList.split(","))
    return kinds


def reflectedNames(program):
    """The program's names, one per line."""
    try:
        result = subprocess.run(program, capture_output=True, text=True, check=False)
    except OSError as error:
        raise ToolError(f"the reflected surface's program did not run: {error}") from error
    if result.returncode != 0:
        diagnostics = f":\n{result.stderr.rstrip()}" if result.stderr.strip() else ""
        raise ToolError(f"the reflected surface's program exited {result.returncode}{diagnostics}")
    return {line for line in result.stdout.splitlines() if line}


def differences(listed, reflected, unreflectedKinds):
    """The lines naming each difference, sorted by name, and the count of the names left out (listed only by
    @p unreflectedKinds, and not reflected)."""
    unreflected = set(unreflectedKinds)
    lines = []
    leftOut = 0
    compared = set()
    for name in sorted(set(listed) | reflected):
        kinds = listed.get(name, set())
        isReflected = name in reflected
        isListedOnlyByUnreflected = bool(kinds) and kinds <= unreflected
        if isListedOnlyByUnreflected and not isReflected:
            leftOut += 1
            continue
        compared.add(name)
        if isListedOnlyByUnreflected:
            lines.append(f"{name}: reflected, though the export lint lists it only by {', '.join(sorted(kinds))}, "
                         "a kind members_of was measured not to report: measure again, and drop the kind from "
                         "the row")
        elif not kinds:
            lines.append(f"{name}: reflected, but the export lint does not list it: {UNLISTED_REASON}")
        elif not isReflected:
            lines.append(f"{name}: the export lint lists it ({', '.join(sorted(kinds - unreflected))}), but "
                         f"members_of does not reflect it: {UNREFLECTED_REASON}")
    return lines, len(compared), leftOut


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang", required=True, help="the clang++ the export lint runs")
    parser.add_argument("--manifest", required=True, type=pathlib.Path, help="the module's surface.json")
    parser.add_argument("--work-directory", type=pathlib.Path, help="the export lint's work directory")
    parser.add_argument("--umbrella", type=pathlib.Path, help="the program's umbrella header, to compare too")
    parser.add_argument("--unreflected-kinds", nargs="*", default=[], metavar="KIND",
                        help="the declaration kinds members_of does not report, as measured")
    parser.add_argument("program", nargs="+", help="the program that prints the reflected surface, after --")
    arguments = parser.parse_args()
    try:
        listed = listedKinds(arguments)
        reflected = reflectedNames(arguments.program)
        umbrellaLines = umbrellaDifferences(arguments.manifest, arguments.umbrella) if arguments.umbrella else []
    except ToolError as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_TOOL_ERROR
    lines, compared, leftOut = differences(listed, reflected, arguments.unreflected_kinds)
    lines = umbrellaLines + lines
    for line in lines:
        print(line)
    leftOutNote = (f"({leftOut} left out: listed only by the kinds members_of does not reflect, "
                   f"{', '.join(arguments.unreflected_kinds) or 'none'})")
    if lines:
        plural = "" if len(lines) == 1 else "s"
        print(f"module surface: {len(lines)} difference{plural} among {compared} names {leftOutNote}")
        return EXIT_DIFFERENCES
    print(f"module surface: {compared} names agree {leftOutNote}")
    return EXIT_AGREE


if __name__ == "__main__":
    sys.exit(main())
