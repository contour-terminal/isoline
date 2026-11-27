#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of compare-module-surface.py on the fixture cases in testdata/module-surface/.

Every case compares the export lint's names of one fixture module (testdata/module-exports/list-names/) with
the names a stand-in for the reflected surface's program prints: the case's reflected.txt. expected.txt is what
the comparison prints, the table below its exit status. A case's umbrella.hpp, where it has one, is the program's
umbrella (--umbrella). The measured row --unreflected-kinds is UsingDecl, as
the real test's (cmake/IsolineModuleSurface.cmake).

Usage: python3 -I scripts/compare-module-surface_test.py --clang <clang++> [--work-directory <dir>]
Without --clang it exits 77, CTest's SKIP_RETURN_CODE.
"""

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

SCRIPTS = pathlib.Path(__file__).resolve().parent
SCRIPT = SCRIPTS / "compare-module-surface.py"
CASES_DIRECTORY = SCRIPTS / "testdata" / "module-surface"
MODULE = SCRIPTS / "testdata" / "module-exports" / "list-names"

EXIT_SKIP = 77

# The stand-in programs: one that prints the case's reflected.txt, one that fails.
PRINTS = "prints"
FAILS = "fails"
PROGRAMS = {
    PRINTS: lambda case: [sys.executable, "-I", "-c", "import sys; sys.stdout.write(open(sys.argv[1]).read())",
                          str(case / "reflected.txt")],
    FAILS: lambda case: [sys.executable, "-I", "-c", "raise SystemExit(3)"],
}

# (case, stand-in program, expected exit status).
CASES = (
    ("equal", PRINTS, 0),
    ("reflected-not-listed", PRINTS, 1),
    ("listed-not-reflected", PRINTS, 1),
    ("unreflected-kind-reflected", PRINTS, 1),
    ("program-fails", FAILS, 2),
    ("umbrella-differs", PRINTS, 1),
)


def runCase(clang, workDirectory, row):
    """Runs one row; returns its failures."""
    name, program, expectedExit = row
    case = CASES_DIRECTORY / name
    module = workDirectory / name
    shutil.rmtree(module, ignore_errors=True)
    shutil.copytree(MODULE, module)
    umbrella = ["--umbrella", str(case / "umbrella.hpp")] if (case / "umbrella.hpp").exists() else []
    result = subprocess.run([sys.executable, "-I", str(SCRIPT), "--clang", clang,
                             "--manifest", str(module / "surface.json"), "--work-directory", str(module / "work"),
                             *umbrella, "--unreflected-kinds", "UsingDecl", "--", *PROGRAMS[program](case)],
                            capture_output=True, text=True, check=False)
    failures = []
    if result.returncode != expectedExit:
        failures.append(f"exited {result.returncode}, expected {expectedExit}")
    lines = (result.stdout + result.stderr).splitlines()
    expectedLines = (case / "expected.txt").read_text(encoding="utf-8").splitlines()
    if lines != expectedLines:
        failures.append("printed\n    " + "\n    ".join(lines) + "\n  expected\n    " + "\n    ".join(expectedLines))
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang", help="the clang++ the export lint runs; without it the test is skipped")
    parser.add_argument("--work-directory", type=pathlib.Path, help="where the cases' copies go")
    arguments = parser.parse_args()
    if not arguments.clang:
        print("skipped: no --clang given")
        return EXIT_SKIP
    if arguments.work_directory is not None:
        arguments.work_directory.mkdir(parents=True, exist_ok=True)
    listed = {row[0] for row in CASES}
    failed = [f"{path.name}: a case directory no row runs" for path in sorted(CASES_DIRECTORY.iterdir())
              if path.name not in listed]
    with tempfile.TemporaryDirectory(dir=arguments.work_directory) as workDirectory:
        for row in CASES:
            failed += [f"{row[0]}: {failure}" for failure in runCase(arguments.clang, pathlib.Path(workDirectory), row)]
    for failure in failed:
        print(f"FAIL {failure}")
    print(f"compare-module-surface selftest: {len(CASES)} cases, {len(failed)} failures")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
