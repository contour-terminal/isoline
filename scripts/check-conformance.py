#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Checks that docs/conformance.md lists exactly what the imported spec suites exclude and skip.

The block between `<!-- exclusions:begin -->` and `<!-- exclusions:end -->` holds one line per
excluded file, `<source> <path>: <reason>`, from the `exclude` rows of tests/spec/files.txt, and one
line per assertion the GC cut skips, `<source> <path>:<wast line>: <oracle message>`, from the `skip`
entries of every imported .oracle, whose reasons all start `gc: `: a command skipped because it uses a
skipped module names its root, `… (root wast line N)`. A skip that is not the cut's fails the check,
as the importer writes no other. The block between `<!-- divergences:begin -->` and
`<!-- divergences:end -->` holds one line per expected divergence, a `message` entry of an .oracle,
`<source> <path>:<wast line>: <Isoline's message>`, and the block between
`<!-- parity-divergences:begin -->` and `<!-- parity-divergences:end -->` one line per `parity` entry,
`<source> <path>:<wast line>: 0x<offset> sha256:<digest> <reason>`. The component suites of
tests/component-files.txt have blocks of their own: `component-exclusions` (`<source> <path>: <reason>`),
`component-skips` (`<source> <path>: <count> × <reason>`, every skip by file and reason but the recorded
ones), `component-recorded-skips` (`<source> <path>:<wast line>: <reason>`, those tests/component-skips.txt
records) and `component-divergences` (`<source> <path>:<wast line>: <kind> <value>`). The lines are sorted
by source, path and wast line (or reason). The check fails, printing a diff, when a block differs from what the data says;
--update rewrites the blocks.

Usage: python3 -I scripts/check-conformance.py [--root <repository>] [--update]
"""

import argparse
import difflib
import importlib.util
import pathlib
import re
import sys

SCRIPTS = pathlib.Path(__file__).resolve().parent

# The generated blocks, by name: the markers `<!-- <name>:begin -->` and `<!-- <name>:end -->`.
EXCLUSIONS = "exclusions"
DIVERGENCES = "divergences"
PARITY_DIVERGENCES = "parity-divergences"
COMPONENT_EXCLUSIONS = "component-exclusions"
COMPONENT_SKIPS = "component-skips"
COMPONENT_RECORDED = "component-recorded-skips"
COMPONENT_DIVERGENCES = "component-divergences"
BLOCKS = (EXCLUSIONS, DIVERGENCES, PARITY_DIVERGENCES, COMPONENT_EXCLUSIONS, COMPONENT_SKIPS, COMPONENT_RECORDED,
          COMPONENT_DIVERGENCES)

# The reasons of the component suites' recorded skips (tests/component-skips.txt), each listed by its line;
# every other reason is listed by how many commands of a file it skips.
RECORDED_PREFIXES = ("oracle unavailable since ", "wasmtime restriction (restrictions.wast:",
                     "diverges from CM pin (definitions.py:")

# The skip reasons the importer writes: a root, `<reason> (wast line M)`, and a command that uses a
# skipped module, `<reason> (root wast line N; wast line M)`.
SKIP_REASON = re.compile(r"^(?P<reason>.*) \((?:root wast line (?P<root>\d+); )?wast line (?P<line>\d+)\)$")

# The reason prefix of the GC cut's skips, the only ones the importer writes; they belong to the exclusions.
GC_PREFIX = "gc: "


def begin(name):
    return f"<!-- {name}:begin -->"


def end(name):
    return f"<!-- {name}:end -->"


def loadImporter():
    """Loads import-spec-tests.py by path, for its files.txt reader: its name is no module name."""
    sys.dont_write_bytecode = True  # leave no __pycache__ in the source tree
    spec = importlib.util.spec_from_file_location("import_spec_tests", SCRIPTS / "import-spec-tests.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def oracleEntries(oracle, kind):
    """The (wast line, hex value) of each entry of one kind of one .oracle manifest,
    `<ordinal> <line> <kind> <hex value>`."""
    entries = []
    for line in oracle.read_text(encoding="utf-8").splitlines():
        fields = line.split(" ")
        if len(fields) == 4 and fields[2] == kind:
            entries.append((int(fields[1]), fields[3]))
    return entries


def skips(path, reasons):
    """The (wast line, text) of each skip, by its hex-encoded reason, of the oracle at path."""
    entries = []
    for reason in reasons:
        reason = bytes.fromhex(reason).decode("utf-8")
        match = SKIP_REASON.match(reason)
        if match is None:
            raise SystemExit(f"{path}: a skip without its wast line: {reason}")
        text = match["reason"]
        if not text.startswith(GC_PREFIX):
            raise SystemExit(f"{path}: a skip that is not the GC cut's: {reason}")
        text = text.removeprefix(GC_PREFIX)
        if match["root"]:
            text += f" (root wast line {match['root']})"
        entries.append((int(match["line"]), text))
    return entries


# The divergence entries of an .oracle manifest, each listed in the block of its kind.
DIVERGENCE_BLOCKS = {"message": DIVERGENCES, "parity": PARITY_DIVERGENCES}


def generated(spec):
    """Each block's lines for the suites imported under spec, the tests/spec directory, in order."""
    entries = {name: [] for name in BLOCKS}
    for row in loadImporter().readFiles(spec / "files.txt"):
        if row.exclude is not None:
            entries[EXCLUSIONS].append(((row.source, row.path, 0), f"{row.source} {row.path}: {row.exclude}"))
            continue
        oracle = (row.directory(spec) / pathlib.PurePosixPath(row.path).name).with_suffix(".oracle")
        for kind, name in DIVERGENCE_BLOCKS.items():
            for line, value in oracleEntries(oracle, kind):
                entries[name].append(((row.source, row.path, line),
                                      f"{row.source} {row.path}:{line}: {bytes.fromhex(value).decode('utf-8')}"))
        for line, text in skips(oracle, [reason for _, reason in oracleEntries(oracle, "skip")]):
            entries[EXCLUSIONS].append(((row.source, row.path, line), f"{row.source} {row.path}:{line}: {text}"))
    generatedComponents(spec.parent, entries)
    return {name: [text for _, text in sorted(lines)] for name, lines in entries.items()}


def generatedComponents(tests, entries):
    """Adds the component suites' lines (tests/component-files.txt) to entries: their exclusions, their skips
    by file and reason, their recorded skips by line, and their message and parity divergences."""
    files = tests / "component-files.txt"
    if not files.exists():
        return
    for row in loadImporter().readComponentFiles(files):
        if row.exclude is not None:
            entries[COMPONENT_EXCLUSIONS].append(((row.source, row.path, 0), f"{row.source} {row.path}: {row.exclude}"))
            continue
        oracle = (row.directory(tests) / pathlib.PurePosixPath(row.path).name).with_suffix(".oracle")
        counts = {}
        for line, value in oracleEntries(oracle, "skip"):
            reason = bytes.fromhex(value).decode("utf-8")
            if reason.startswith(RECORDED_PREFIXES):
                entries[COMPONENT_RECORDED].append(((row.source, row.path, line), f"{row.source} {row.path}:{line}: {reason}"))
            else:
                counts[reason] = counts.get(reason, 0) + 1
        for reason, count in counts.items():
            entries[COMPONENT_SKIPS].append(((row.source, row.path, reason), f"{row.source} {row.path}: {count} × {reason}"))
        for kind in ("message", "parity"):
            for line, value in oracleEntries(oracle, kind):
                entries[COMPONENT_DIVERGENCES].append(((row.source, row.path, line),
                    f"{row.source} {row.path}:{line}: {kind} {bytes.fromhex(value).decode('utf-8')}"))


def block(lines):
    """The text between the markers: the lines in a fenced block, so that Markdown keeps them apart."""
    return "\n```text\n" + "".join(f"{line}\n" for line in lines) + "```\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=SCRIPTS.parent, help="the repository root")
    parser.add_argument("--update", action="store_true", help="rewrite the block instead of checking it")
    arguments = parser.parse_args(argv)

    doc = arguments.root / "docs" / "conformance.md"
    text = doc.read_text(encoding="utf-8")
    wanted = generated(arguments.root / "tests" / "spec")
    differences = []
    for name in BLOCKS:
        start = text.find(begin(name))
        stop = text.find(end(name))
        if start < 0 or stop < start:
            raise SystemExit(f"{doc}: expected the markers {begin(name)} and then {end(name)}")
        start += len(begin(name))
        current = text[start:stop]
        expected = block(wanted[name])
        if current != expected:
            differences.extend(difflib.unified_diff(current.splitlines(keepends=True),
                                                    expected.splitlines(keepends=True),
                                                    f"{doc} {name} (committed)",
                                                    f"{doc} {name} (from the imported suites)"))
            text = text[:start] + expected + text[stop:]
    if not differences:
        return 0
    if arguments.update:
        doc.write_text(text, encoding="utf-8", newline="\n")
        print(f"{doc}: generated blocks updated")
        return 0
    sys.stdout.writelines(differences)
    print(f"{doc}: the generated blocks differ from the imported suites; run scripts/check-conformance.py --update")
    return 1


if __name__ == "__main__":
    sys.exit(main())
