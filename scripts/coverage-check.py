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
are comments, kept by --update.

The lines no test can reach are exclusions, one `exclude <path> <marker> <reason…>` line each. The
marker names the lines in the source: `// uncovered(<marker>)` marks the line it ends, and
`// uncovered-begin(<marker>)` … `// uncovered-end(<marker>)` the lines from one to the other. The
uncovered lines of a file must be exactly its marked ones, as `llvm-cov show` counts lines: a line
that is uncovered and unmarked fails, and so does a marked line a test now covers, a marker that
marks no code, a marker the baseline does not list, and an exclusion whose marker is gone. Being a
name rather than a line number, a marker moves with its code.

Usage: python3 -I scripts/coverage-check.py <coverage.json> <coverage-baseline.txt> [--update [--prune]]
"""

import argparse
import json
import math
import pathlib
import re
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


# The fields of a segment of an llvm-cov export, in order.
SEGMENT_LINE = 0
SEGMENT_COLUMN = 1
SEGMENT_COUNT = 2
SEGMENT_HAS_COUNT = 3
SEGMENT_IS_REGION_ENTRY = 4
SEGMENT_IS_GAP = 5


def lineCounts(segments):
    """Maps each line the segments map to its execution count, as `llvm-cov show` counts them
    (llvm's LineCoverageIterator): a line is mapped unless it starts a skipped region, when a
    region with a count starts on it or one wraps onto it, and counts the most of those."""
    counts = {}
    wrapped = None
    index = 0
    line = segments[0][SEGMENT_LINE] if segments else 1
    last = segments[-1][SEGMENT_LINE] if segments else 0
    while line <= last:
        onLine = []
        while index < len(segments) and segments[index][SEGMENT_LINE] == line:
            onLine.append(segments[index])
            index += 1
        starts = [segment for segment in onLine if segment[SEGMENT_HAS_COUNT] and segment[SEGMENT_IS_REGION_ENTRY]
                  and not segment[SEGMENT_IS_GAP]]
        skipped = bool(onLine) and not onLine[0][SEGMENT_HAS_COUNT] and onLine[0][SEGMENT_IS_REGION_ENTRY]
        mapped = not skipped and (bool(starts) or (wrapped is not None and wrapped[SEGMENT_HAS_COUNT]))
        mapped = mapped or any(segment[SEGMENT_IS_REGION_ENTRY] and segment[SEGMENT_HAS_COUNT] for segment in onLine)
        if mapped:
            wrappedCount = wrapped[SEGMENT_COUNT] if wrapped is not None else 0
            counts[line] = max([wrappedCount, *(segment[SEGMENT_COUNT] for segment in starts)])
        if onLine:
            wrapped = onLine[-1]
        line += 1
    return counts


def readCoverage(path, root):
    """From an llvm-cov JSON export, maps each covered file to its (line%, branch%), and each to the
    execution count of every line it maps."""
    export = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    coverage = {}
    counts = {}
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
            if "segments" not in entry:
                raise SystemExit(f"{path}: {source} has no segments; export without -summary-only")
            counts[source] = lineCounts(entry["segments"])
    return coverage, counts


# A marker comment: `// uncovered(<marker>)`, or the -begin and -end of a range.
MARKER = re.compile(r"//\s*uncovered(-begin|-end)?\(([A-Za-z0-9_.-]+)\)")


class Marked:
    """The lines a marker marks, and of them the ones that must be uncovered: all but the comment
    lines a range begins and ends with, which take a count from the code around them."""

    def __init__(self):
        self.lines = set()
        self.checked = set()


def readMarkers(text, where):
    """Maps each marker of a source text to the lines it marks."""
    markers = {}
    begins = {}
    for number, line in enumerate(text.splitlines(), start=1):
        for match in MARKER.finditer(line):
            kind, marker = match.groups()
            marked = markers.setdefault(marker, Marked())
            if kind is None:
                marked.lines.add(number)
                marked.checked.add(number)
                continue
            if not line.lstrip().startswith("//"):
                raise SystemExit(f"{where}:{number}: uncovered{kind}({marker}) must be a line of its own")
            if kind == "-begin":
                if marker in begins:
                    raise SystemExit(f"{where}:{number}: uncovered-begin({marker}) twice")
                begins[marker] = number
                continue
            if marker not in begins:
                raise SystemExit(f"{where}:{number}: uncovered-end({marker}) without a begin")
            begin = begins.pop(marker)
            marked.lines.update(range(begin, number + 1))
            marked.checked.update(range(begin + 1, number))
    if begins:
        marker, number = next(iter(begins.items()))
        raise SystemExit(f"{where}:{number}: uncovered-begin({marker}) without an end")
    return markers


def exclusionFindings(counts, exclusions, root):
    """Describes every difference between each file's uncovered lines and its marked exclusions."""
    found = [f"{source}: exclusion '{marker}' names a file not in the report"
             for source, marker in sorted(exclusions) if source not in counts]
    for source, lineCount in sorted(counts.items()):
        path = pathlib.Path(root) / source
        if not path.is_file():
            found.append(f"{source}: in the report, but not under {root}")
            continue
        markers = readMarkers(path.read_text(encoding="utf-8"), source)
        listed = {marker for listedSource, marker in exclusions if listedSource == source}
        found += [f"{source}: uncovered({marker}) is not an exclusion of the baseline"
                  for marker in sorted(markers.keys() - listed)]
        found += [f"{source}: exclusion '{marker}' has no marker in the source"
                  for marker in sorted(listed - markers.keys())]
        excused = set()
        for marker in sorted(listed & markers.keys()):
            mappedLines = sorted(markers[marker].checked & lineCount.keys())
            if not mappedLines:
                found.append(f"{source}: uncovered({marker}) marks no code")
            found += [f"{source}:{line}: covered, but marked uncovered({marker})"
                      for line in mappedLines if lineCount[line] > 0]
            excused |= markers[marker].lines
        found += [f"{source}:{line}: uncovered, and no exclusion marks it"
                  for line in sorted(lineCount) if lineCount[line] == 0 and line not in excused]
    return found


def readBaseline(path):
    """Returns the baseline's comment lines, its exclusions, (path, marker) to reason, and its
    entries, path to (line%, branch%)."""
    comments = []
    exclusions = {}
    entries = {}
    for number, line in enumerate(pathlib.Path(path).read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip() or line.startswith("#"):
            comments.append(line)
            continue
        fields = line.split()
        where = f"{path}:{number}"
        if fields[0] == "exclude":
            if len(fields) < 4:
                raise SystemExit(f"{where}: expected 'exclude <path> <marker> <reason>', got '{line}'")
            exclusions[(fields[1], fields[2])] = " ".join(fields[3:])
            continue
        if len(fields) != 3:
            raise SystemExit(f"{where}: expected '<path> <line%> <branch%>', got '{line}'")
        lines = parsePercent(fields[1], where)
        if lines is None:
            raise SystemExit(f"{where}: line coverage cannot be '{NO_BRANCHES}'")
        entries[fields[0]] = (lines, parsePercent(fields[2], where))
    return comments, exclusions, entries


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


def writeBaseline(path, comments, exclusions, entries):
    width = max((len(source) for source in entries), default=0)
    lines = list(comments)
    while lines and not lines[-1].strip():
        lines.pop()
    if lines:
        lines.append("")
    if exclusions:
        lines += [f"exclude {source} {marker} {reason}" for (source, marker), reason in sorted(exclusions.items())]
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

    coverage, counts = readCoverage(arguments.coverage, arguments.root.replace("\\", "/"))
    if not coverage:
        print(f"{arguments.coverage}: covers no file under {SOURCE_PREFIX}", file=sys.stderr)
        return 1
    comments, exclusions, entries = readBaseline(arguments.baseline) if arguments.baseline.exists() else ([], {}, {})

    if arguments.update:
        root = pathlib.Path(arguments.root)
        entries = ratcheted(coverage, entries,
                            lambda source: not arguments.prune or (root / source).exists())
        writeBaseline(arguments.baseline, comments, exclusions, entries)
        print(f"{arguments.baseline}: {len(coverage)} files")

    # After an update too: it never lowers an entry, so a file that lost coverage still fails.
    found = regressions(coverage, entries)
    for message in found:
        print(message, file=sys.stderr)
    if found:
        print(f"coverage fell below {arguments.baseline}; add tests, or justify the lines no test "
              "can reach in it", file=sys.stderr)
    unexcused = exclusionFindings(counts, exclusions, arguments.root)
    for message in unexcused:
        print(message, file=sys.stderr)
    if unexcused:
        print(f"the uncovered lines differ from the exclusions of {arguments.baseline}; cover them, or "
              "mark each line no test can reach and list its marker with the reason", file=sys.stderr)
    if found or unexcused:
        return 1
    print(f"coverage meets {arguments.baseline}: {len(coverage)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
