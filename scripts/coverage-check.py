#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Checks Isoline's coverage against the ratchet in coverage-baseline.txt.

Reads an `llvm-cov export -format=text` JSON and fails when a source file under src/isoline/ covers
fewer lines or branches than its baseline entry says, or has no entry at all, and when a file of
the baseline is missing from the report (no longer linked into a test, or filtered out). With
--update it rewrites the baseline instead: entries rise to the coverage reached and files new to
the report are added, but no entry is ever lowered or dropped, so a file that lost coverage, or
left the report, still fails. --prune, with --update, drops the entries of files that no longer
exist.

The baseline holds one `<path> <line%> <branch%>` line per file, the path relative to the
repository root and `-` for the branch coverage of a file without branches. Lines starting with `#`
are comments, kept by --update: the place to name, as `# path:line reason`, each line that no test
can reach.

Usage: python3 -I scripts/coverage-check.py <coverage.json> <coverage-baseline.txt> [--update [--prune]]
"""

import argparse
import json
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# The files the ratchet covers: Isoline's own sources, not its tests or their helpers.
SOURCE_PREFIX = "src/isoline/"
EXCLUDED_SUFFIXES = ("_test.cpp",)
EXCLUDED_DIRECTORIES = ("/testing/",)

# What the baseline writes for the branch coverage of a file without branches.
NO_BRANCHES = "-"


def floorPercent(percent):
    """Rounds a percentage down to the two decimals the baseline keeps, so a check never rounds up."""
    return math.floor(percent * 100 + 1e-9) / 100


def formatPercent(percent):
    return NO_BRANCHES if percent is None else f"{percent:.2f}"


def parsePercent(token, where):
    if token == NO_BRANCHES:
        return None
    try:
        return float(token)
    except ValueError:
        raise SystemExit(f"{where}: '{token}' is not a percentage") from None


def relativeSource(filename, root):
    """The repository-relative path of a covered file, or None when the ratchet does not cover it."""
    text = filename.replace("\\", "/")
    prefix = root.rstrip("/") + "/"
    if text.startswith(prefix):
        text = text[len(prefix):]
    if not text.startswith(SOURCE_PREFIX) or text.endswith(EXCLUDED_SUFFIXES):
        return None
    if any(directory in "/" + text for directory in EXCLUDED_DIRECTORIES):
        return None
    return text


def readCoverage(path, root):
    """Maps each covered file to its (line%, branch%) from an llvm-cov JSON export."""
    export = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    coverage = {}
    for data in export["data"]:
        for entry in data["files"]:
            source = relativeSource(entry["filename"], root)
            if source is None:
                continue
            lines = entry["summary"]["lines"]
            branches = entry["summary"]["branches"]
            coverage[source] = (
                floorPercent(lines["percent"]) if lines["count"] else 100.0,
                floorPercent(branches["percent"]) if branches["count"] else None,
            )
    return coverage


def readBaseline(path):
    """Returns the baseline's comment lines and its entries, path to (line%, branch%)."""
    comments = []
    entries = {}
    for number, line in enumerate(pathlib.Path(path).read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip() or line.startswith("#"):
            comments.append(line)
            continue
        fields = line.split()
        where = f"{path}:{number}"
        if len(fields) != 3:
            raise SystemExit(f"{where}: expected '<path> <line%> <branch%>', got '{line}'")
        lines = parsePercent(fields[1], where)
        if lines is None:
            raise SystemExit(f"{where}: line coverage cannot be '{NO_BRANCHES}'")
        entries[fields[0]] = (lines, parsePercent(fields[2], where))
    return comments, entries


def regressions(coverage, entries):
    """Describes every file missing from the baseline or the report, or covering less than its entry."""
    found = [f"{source}: in the baseline, not in the report; is it still linked into a test?"
             for source in sorted(entries) if source not in coverage]
    for source, (lines, branches) in sorted(coverage.items()):
        if source not in entries:
            found.append(f"{source}: not in the baseline (lines {formatPercent(lines)} %, "
                         f"branches {formatPercent(branches)} %)")
            continue
        baseLines, baseBranches = entries[source]
        if lines < baseLines:
            found.append(f"{source}: lines {formatPercent(lines)} % < baseline {formatPercent(baseLines)} %")
        if baseBranches is not None and (branches is None or branches < baseBranches):
            found.append(f"{source}: branches {formatPercent(branches)} % < baseline "
                         f"{formatPercent(baseBranches)} %")
    return found


def ratcheted(coverage, entries, exists):
    """The new entries: each file's baseline raised to its coverage, never lowered.

    An entry the report lacks is kept, unless exists() says its file is gone."""
    updated = {source: entry for source, entry in entries.items() if source not in coverage and exists(source)}
    for source, (lines, branches) in coverage.items():
        baseLines, baseBranches = entries.get(source, (lines, branches))
        if branches is None or baseBranches is None:
            raisedBranches = branches if baseBranches is None else baseBranches
        else:
            raisedBranches = max(branches, baseBranches)
        updated[source] = (max(lines, baseLines), raisedBranches)
    return updated


def writeBaseline(path, comments, entries):
    width = max((len(source) for source in entries), default=0)
    lines = list(comments)
    while lines and not lines[-1].strip():
        lines.pop()
    if lines:
        lines.append("")
    lines += [f"{source:<{width}} {formatPercent(line)} {formatPercent(branch)}"
              for source, (line, branch) in sorted(entries.items())]
    pathlib.Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("coverage", type=pathlib.Path, help="the llvm-cov export JSON")
    parser.add_argument("baseline", type=pathlib.Path, help="the coverage-baseline.txt to check against")
    parser.add_argument("--update", action="store_true", help="raise the baseline to the coverage reached")
    parser.add_argument("--prune", action="store_true",
                        help="with --update: drop the entries of files that no longer exist under --root")
    parser.add_argument("--root", default=ROOT.as_posix(),
                        help="the repository root the report's absolute paths are relative to")
    arguments = parser.parse_args(arguments)
    if arguments.prune and not arguments.update:
        parser.error("--prune needs --update")

    coverage = readCoverage(arguments.coverage, arguments.root.replace("\\", "/"))
    if not coverage:
        print(f"{arguments.coverage}: covers no file under {SOURCE_PREFIX}", file=sys.stderr)
        return 1
    comments, entries = readBaseline(arguments.baseline) if arguments.baseline.exists() else ([], {})

    if arguments.update:
        root = pathlib.Path(arguments.root)
        entries = ratcheted(coverage, entries,
                            lambda source: not arguments.prune or (root / source).exists())
        writeBaseline(arguments.baseline, comments, entries)
        print(f"{arguments.baseline}: {len(coverage)} files")

    # After an update too: it never lowers an entry, so a file that lost coverage still fails.
    found = regressions(coverage, entries)
    for message in found:
        print(message, file=sys.stderr)
    if found:
        print(f"coverage fell below {arguments.baseline}; add tests, or justify the lines no test "
              "can reach in it", file=sys.stderr)
        return 1
    print(f"coverage meets {arguments.baseline}: {len(coverage)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
