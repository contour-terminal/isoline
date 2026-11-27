#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of check-module-exports.py on the fixture cases in testdata/module-exports/.

Every case is a directory: a surface manifest (surface.json, paths relative to it), the headers of its
header set, an interface unit (module.cppm), and expected.txt, what the lint prints. The table below
runs each in a mode and expects an exit status. A case runs on a copy in the work directory, whose path
the output names as `<case>`, as it names the Clang as `<clang>`.

Usage: python3 -I scripts/check-module-exports_test.py --clang <clang++> [--work-directory <dir>]
Without --clang it exits 77, CTest's SKIP_RETURN_CODE.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

SCRIPT = pathlib.Path(__file__).resolve().parent / "check-module-exports.py"
CASES_DIRECTORY = pathlib.Path(__file__).resolve().parent / "testdata" / "module-exports"

EXIT_SKIP = 77

# How a case runs the lint: --check; --write; --write, then --check, with the written unit compared
# against the case's expected.cppm.
CHECK = "check"
WRITE = "write"
WRITE_THEN_CHECK = "write-then-check"

# Options of a case: a Clang that does not exist (no-clang), more arguments to the lint (arguments), another
# file of expected lines (expected), and patterns whose matches the output shows as `<masked>` (masks),
# for what varies with the Clang build.
NO_CLANG = "no-clang"

# (case, mode, expected exit status, options).
CASES = (
    ("complete", CHECK, 0, {}),
    ("missing-name", CHECK, 1, {}),
    ("stale-name", CHECK, 1, {}),
    ("duplicate-name", CHECK, 1, {}),
    ("overload-set-once", CHECK, 0, {}),
    ("hidden-friend-ignored", CHECK, 0, {}),
    ("deduction-guide-ignored", CHECK, 0, {}),
    ("template-parameter-ignored", CHECK, 0, {}),
    ("out-of-line-member-ignored", CHECK, 0, {}),
    ("anonymous-namespace-ignored", CHECK, 0, {}),
    ("using-declaration-leaf", CHECK, 0, {}),
    ("unscoped-enumeration-enumerators", CHECK, 0, {}),
    ("testing-namespace-excluded", CHECK, 0, {}),
    ("detail-namespace-excluded", CHECK, 0, {}),
    ("internal-namespace-excluded", CHECK, 0, {}),
    ("unclassified-namespace", CHECK, 1, {}),
    ("unclassified-namespace", WRITE, 1, {"expected": "expected-write.txt"}),
    ("unknown-kind", CHECK, 1, {}),
    ("header-not-included", CHECK, 1, {}),
    ("file-carried-forward", CHECK, 1, {}),
    ("not-generated-form", CHECK, 1, {}),
    ("write-then-check", WRITE_THEN_CHECK, 0, {}),
    ("requires-clause-function-template", CHECK, 0, {}),
    ("requires-clause-class-template", CHECK, 0, {}),
    ("enumeration-defined-elsewhere", CHECK, 1, {}),
    ("def-expanded-header-not-included", CHECK, 1, {}),
    ("class-specializations-ignored", CHECK, 0, {}),
    ("variable-specializations-ignored", CHECK, 0, {}),
    ("empty-declaration-ignored", CHECK, 0, {}),
    ("namespace-alias-refused", CHECK, 1, {}),
    ("namespace-alias-refused", WRITE, 1, {"expected": "expected-write.txt"}),
    ("no-clang", CHECK, 2, {NO_CLANG: True}),
    ("unexpected-failure", CHECK, 2, {}),
    ("undecodable-unit", CHECK, 2, {}),
    ("unreadable-manifest", CHECK, 2, {}),
    ("dump-over-ceiling", CHECK, 2, {"arguments": ["--dump-size-limit-bytes", "1"], "masks": [r"(?<=is )\d+"]}),
)


def runLint(clang, caseDirectory, mode, arguments):
    result = subprocess.run([sys.executable, "-I", str(SCRIPT), "--clang", clang,
                             "--manifest", str(caseDirectory / "surface.json"), f"--{mode}",
                             "--work-directory", str(caseDirectory / "work"), *arguments],
                            capture_output=True, text=True, check=False)
    return result.returncode, result.stdout + result.stderr


def normalized(output, caseDirectory, clang, masks):
    lines = []
    for line in output.splitlines():
        line = line.replace(str(caseDirectory), "<case>").replace(clang, "<clang>")
        for mask in masks:
            line = re.sub(mask, "<masked>", line)
        lines.append(line)
    return lines


def runCase(clang, workDirectory, row):
    """Runs one row; returns its failures."""
    name, mode, expectedExit, options = row
    caseDirectory = workDirectory / f"{name}.{mode}"
    shutil.rmtree(caseDirectory, ignore_errors=True)
    shutil.copytree(CASES_DIRECTORY / name, caseDirectory)
    if options.get(NO_CLANG):
        clang = str(caseDirectory / "no-such-clang++")
    arguments = options.get("arguments", [])
    failures = []
    if mode == WRITE_THEN_CHECK:
        exitStatus, output = runLint(clang, caseDirectory, WRITE, arguments)
        if exitStatus != 0:
            failures.append(f"--write exited {exitStatus}: {output}")
        written = (caseDirectory / "module.cppm").read_text(encoding="utf-8")
        expectedUnit = (caseDirectory / "expected.cppm").read_text(encoding="utf-8")
        if written != expectedUnit:
            failures.append(f"--write wrote\n{written}\nexpected\n{expectedUnit}")
        mode = CHECK
    exitStatus, output = runLint(clang, caseDirectory, mode, arguments)
    if exitStatus != expectedExit:
        failures.append(f"exited {exitStatus}, expected {expectedExit}")
    expectedLines = (caseDirectory / options.get("expected", "expected.txt")).read_text(encoding="utf-8").splitlines()
    lines = normalized(output, caseDirectory, clang, options.get("masks", []))
    if lines != expectedLines:
        failures.append("printed\n    " + "\n    ".join(lines) + "\n  expected\n    " + "\n    ".join(expectedLines))
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang", help="the clang++ the lint runs; without it the test is skipped")
    parser.add_argument("--work-directory", type=pathlib.Path, help="where the cases' copies go")
    arguments = parser.parse_args()
    if not arguments.clang:
        print("skipped: no --clang given")
        return EXIT_SKIP
    if arguments.work_directory is not None:
        arguments.work_directory.mkdir(parents=True, exist_ok=True)
    listed = {row[0] for row in CASES}
    unlisted = sorted(path.name for path in CASES_DIRECTORY.iterdir() if path.name not in listed)
    failed = [f"{name}: a case directory no row runs" for name in unlisted]
    with tempfile.TemporaryDirectory(dir=arguments.work_directory) as workDirectory:
        for row in CASES:
            failed += [f"{row[0]} ({row[1]}): {failure}" for failure in runCase(arguments.clang,
                                                                                pathlib.Path(workDirectory), row)]
    for failure in failed:
        print(f"FAIL {failure}")
    print(f"check-module-exports selftest: {len(CASES)} cases, {len(failed)} failures")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
