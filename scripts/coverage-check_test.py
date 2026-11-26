#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of coverage-check.py: runs it on fixture reports and sources, as CI does, and checks its
verdicts."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "coverage-check.py"
ROOT = pathlib.PurePosixPath("/repo")


def fileEntry(path, lines, branches, root=ROOT, segments=()):
    """One file of an llvm-cov export: (covered, count) lines and branches, and the segments that
    map its lines (none by default: no line is uncovered)."""
    def summary(covered, count):
        return {"count": count, "covered": covered, "percent": 100.0 * covered / count if count else 0}
    return {"filename": f"{pathlib.PurePath(root).as_posix()}/{path}", "segments": [list(s) for s in segments],
            "summary": {"lines": summary(*lines), "branches": summary(*branches)}}


def segment(line, count, hasCount=True, isRegionEntry=True, isGap=False):
    """One segment of an llvm-cov export, at column 1 of line."""
    return (line, 1, count, hasCount, isRegionEntry, isGap)


# A function whose line 3 no test reaches: line 1 starts a region run once, which ends on line 2,
# and line 3 starts one never run. Line 2 counts 1 (the region wraps onto it), line 3 counts 0.
ZERO_ON_LINE_3 = (segment(1, 1), segment(2, 0, hasCount=False, isRegionEntry=False), segment(3, 0),
                  segment(3, 0, hasCount=False, isRegionEntry=False))
COVERED_LINE_3 = (segment(1, 1), segment(2, 0, hasCount=False, isRegionEntry=False), segment(3, 1),
                  segment(3, 0, hasCount=False, isRegionEntry=False))


class CoverageCheckTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.report = pathlib.Path(self.directory.name) / "coverage.json"
        self.baseline = pathlib.Path(self.directory.name) / "coverage-baseline.txt"

    def run_(self, files, baseline, *options, root=ROOT, sources=None):
        if root == ROOT:  # the fixture root: a directory of this test, for the sources to live in
            real = pathlib.Path(self.directory.name) / "repo"
            files = [dict(entry, filename=entry["filename"].replace(ROOT.as_posix(), real.as_posix(), 1))
                     for entry in files]
            root = real
        # Every source the report names exists: empty, unless sources gives its text.
        for entry in files:
            path = pathlib.Path(entry["filename"])
            if pathlib.Path(root) not in path.parents:
                continue
            text = (sources or {}).get(path.relative_to(root).as_posix())
            if text is not None or not path.exists():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text or "", encoding="utf-8")
        export = {"type": "llvm.coverage.json.export", "version": "3.1.0", "data": [{"files": files}]}
        self.report.write_text(json.dumps(export), encoding="utf-8")
        if baseline is not None:
            self.baseline.write_text(baseline, encoding="utf-8")
        return subprocess.run([sys.executable, "-I", str(SCRIPT), str(self.report), str(self.baseline),
                               "--root", pathlib.PurePath(root).as_posix(), *options], capture_output=True, text=True)

    def test_atBaselinePasses(self):
        result = self.run_([fileEntry("src/isoline/A.cpp", (3, 4), (1, 2))], "src/isoline/A.cpp 75.00 50.00\n")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_aboveBaselinePasses(self):
        result = self.run_([fileEntry("src/isoline/A.cpp", (4, 4), (2, 2))], "src/isoline/A.cpp 75.00 50.00\n")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_linesBelowBaselineFail(self):
        result = self.run_([fileEntry("src/isoline/A.cpp", (2, 4), (1, 2))], "src/isoline/A.cpp 75.00 50.00\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/A.cpp: lines 50.00 % < baseline 75.00 %", result.stderr)

    def test_branchesBelowBaselineFail(self):
        result = self.run_([fileEntry("src/isoline/A.cpp", (3, 4), (0, 2))], "src/isoline/A.cpp 75.00 50.00\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("branches 0.00 % < baseline 50.00 %", result.stderr)

    def test_neverRoundsUp(self):
        # 2/3 is 66.666…: it meets 66.66 but not 66.67.
        files = [fileEntry("src/isoline/A.cpp", (2, 3), (0, 0))]
        self.assertEqual(self.run_(files, "src/isoline/A.cpp 66.66 -\n").returncode, 0)
        self.assertEqual(self.run_(files, "src/isoline/A.cpp 66.67 -\n").returncode, 1)

    def test_missingFileFails(self):
        files = [fileEntry("src/isoline/A.cpp", (1, 1), (0, 0)), fileEntry("src/isoline/B.cpp", (1, 1), (0, 0))]
        result = self.run_(files, "src/isoline/A.cpp 100.00 -\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/B.cpp: not in the baseline", result.stderr)

    def test_ignoresTestsHelpersAndOtherFiles(self):
        files = [fileEntry("src/isoline/A.cpp", (1, 1), (0, 0)),
                 fileEntry("src/isoline/A_test.cpp", (0, 1), (0, 0)),
                 fileEntry("src/isoline/binary/testing/Builder.hpp", (0, 1), (0, 0)),
                 fileEntry("tests/spec/SpecRunner.cpp", (0, 1), (0, 0)),
                 fileEntry("out/clang-coverage/src/isoline/Generated.hpp", (0, 1), (0, 0)),
                 {"filename": "/usr/include/c++/vector", "summary": fileEntry("x", (0, 1), (0, 0))["summary"]}]
        result = self.run_(files, "src/isoline/A.cpp 100.00 -\n")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_malformedBaselineIsAnError(self):
        result = self.run_([fileEntry("src/isoline/A.cpp", (1, 1), (0, 0))], "src/isoline/A.cpp 100.00\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expected '<path> <line%> <branch%>'", result.stderr)

    def test_baselineEntryMissingFromReportFails(self):
        # A file the export lost (unlinked, or filtered out) must not pass silently.
        files = [fileEntry("src/isoline/A.cpp", (1, 1), (0, 0))]
        result = self.run_(files, "src/isoline/A.cpp 100.00 -\nsrc/isoline/Lost.cpp 90.00 -\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/Lost.cpp: in the baseline, not in the report", result.stderr)

    def test_updateRaisesAddsAndKeepsComments(self):
        files = [fileEntry("src/isoline/A.cpp", (4, 4), (1, 2)), fileEntry("src/isoline/New.cpp", (1, 2), (0, 0))]
        baseline = "# src/isoline/A.cpp:9 unreachable\n\nsrc/isoline/A.cpp 75.00 50.00\n"
        result = self.run_(files, baseline, "--update")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.baseline.read_text(encoding="utf-8"),
                         "# src/isoline/A.cpp:9 unreachable\n\n"
                         "src/isoline/A.cpp   100.00 50.00\n"
                         "src/isoline/New.cpp 50.00 -\n")

    def test_updateKeepsEntriesMissingFromReport(self):
        files = [fileEntry("src/isoline/A.cpp", (1, 1), (0, 0))]
        result = self.run_(files, "src/isoline/A.cpp 100.00 -\nsrc/isoline/Lost.cpp 90.00 -\n", "--update")
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/Lost.cpp: in the baseline, not in the report", result.stderr)
        self.assertEqual(self.baseline.read_text(encoding="utf-8"),
                         "src/isoline/A.cpp    100.00 -\nsrc/isoline/Lost.cpp 90.00 -\n")

    def test_pruneDropsOnlyDeletedFiles(self):
        # --prune drops the entry of a file that no longer exists, never that of one the report lost.
        root = pathlib.Path(self.directory.name) / "repo"
        (root / "src" / "isoline").mkdir(parents=True)
        (root / "src" / "isoline" / "Lost.cpp").write_text("", encoding="utf-8")
        files = [fileEntry("src/isoline/A.cpp", (1, 1), (0, 0), root=root)]
        baseline = "src/isoline/A.cpp 100.00 -\nsrc/isoline/Gone.cpp 10.00 -\nsrc/isoline/Lost.cpp 90.00 -\n"
        result = self.run_(files, baseline, "--update", "--prune", root=root)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.baseline.read_text(encoding="utf-8"),
                         "src/isoline/A.cpp    100.00 -\nsrc/isoline/Lost.cpp 90.00 -\n")

    def test_updateNeverLowers(self):
        result = self.run_([fileEntry("src/isoline/A.cpp", (2, 4), (2, 2))], "src/isoline/A.cpp 75.00 50.00\n",
                           "--update")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.baseline.read_text(encoding="utf-8"), "src/isoline/A.cpp 75.00 100.00\n")



class ExclusionTest(CoverageCheckTest):
    """The uncovered lines must be exactly the lines the baseline's exclusions mark in the source."""

    SOURCE = "int f(int x) {\n    if (x) return 1;\n    return 0;\n}\n"
    MARKED = "int f(int x) {\n    if (x) return 1;\n    return 0; // uncovered(zero)\n}\n"
    ENTRY = "src/isoline/A.cpp 75.00 -\n"
    EXCLUSION = "exclude src/isoline/A.cpp zero no input returns zero\n"

    def check(self, segments, source, baseline, *options):
        return self.run_([fileEntry("src/isoline/A.cpp", (3, 4), (0, 0), segments=segments)], baseline, *options,
                         sources={"src/isoline/A.cpp": source})

    def test_markedUncoveredLinePasses(self):
        result = self.check(ZERO_ON_LINE_3, self.MARKED, self.EXCLUSION + self.ENTRY)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unmarkedUncoveredLineFails(self):
        result = self.check(ZERO_ON_LINE_3, self.SOURCE, self.ENTRY)
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/A.cpp:3: uncovered, and no exclusion marks it", result.stderr)

    def test_markerTheBaselineDoesNotListFails(self):
        result = self.check(ZERO_ON_LINE_3, self.MARKED, self.ENTRY)
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/A.cpp: uncovered(zero) is not an exclusion of the baseline", result.stderr)

    def test_exclusionWithoutMarkerFails(self):
        result = self.check(COVERED_LINE_3, self.SOURCE, self.EXCLUSION + self.ENTRY)
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/A.cpp: exclusion 'zero' has no marker in the source", result.stderr)

    def test_coveredMarkedLineFails(self):
        # A test that reaches an excluded line makes the exclusion stale.
        result = self.check(COVERED_LINE_3, self.MARKED, self.EXCLUSION + self.ENTRY)
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/A.cpp:3: covered, but marked uncovered(zero)", result.stderr)

    def test_markerOfNoCodeFails(self):
        source = "int f(int x) {\n    if (x) return 1;\n    return 0;\n}\n// uncovered(zero)\n"
        result = self.check(COVERED_LINE_3, source, self.EXCLUSION + self.ENTRY)
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/A.cpp: uncovered(zero) marks no code", result.stderr)

    def test_markersMoveWithTheirCode(self):
        # Two lines inserted above: the exclusion still holds, as it names no line number.
        source = "// a\n// b\n" + self.MARKED
        shifted = [(line + 2, *rest) for line, *rest in ZERO_ON_LINE_3]
        result = self.check(shifted, source, self.EXCLUSION + self.ENTRY)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rangeMarksTheLinesBetween(self):
        # Lines 4 to 6 are uncovered and marked; the range's own comment lines, which take a count
        # from the code around them, need be neither.
        source = ("int f(int x) {\n    if (x) return 1;\n    // uncovered-begin(tail)\n    g();\n    return 0;\n"
                  "}\n    // uncovered-end(tail)\n")
        segments = (segment(1, 1), segment(2, 0, hasCount=False, isRegionEntry=False), segment(4, 0),
                    segment(6, 0, hasCount=False, isRegionEntry=False))
        exclusion = "exclude src/isoline/A.cpp tail no input reaches the tail\n"
        self.assertEqual(self.check(segments, source, exclusion + self.ENTRY).returncode, 0)
        # Without the range, lines 4 to 6 are reported.
        result = self.check(segments, source.replace("uncovered-", "x-"), self.ENTRY)
        self.assertEqual(result.returncode, 1)
        for line in (4, 5, 6):
            self.assertIn(f"src/isoline/A.cpp:{line}: uncovered, and no exclusion marks it", result.stderr)

    def test_malformedRangesAreErrors(self):
        for source in ("// uncovered-begin(a)\n", "// uncovered-end(a)\n", "x(); // uncovered-begin(a)\n// uncovered-end(a)\n"):
            with self.subTest(source=source):
                result = self.check(ZERO_ON_LINE_3, source, self.ENTRY)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("uncovered-", result.stderr)

    def test_exclusionOfAFileNotInTheReportFails(self):
        result = self.check(ZERO_ON_LINE_3, self.MARKED,
                            self.EXCLUSION + "exclude src/isoline/B.cpp x gone\n" + self.ENTRY)
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/B.cpp: exclusion 'x' names a file not in the report", result.stderr)

    def test_updateKeepsExclusions(self):
        result = self.check(ZERO_ON_LINE_3, self.MARKED, "# c\n" + self.EXCLUSION + self.ENTRY, "--update")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.baseline.read_text(encoding="utf-8"), "# c\n\n" + self.EXCLUSION + "\n" + self.ENTRY)

    def test_aReportWithoutSegmentsIsAnError(self):
        entry = fileEntry("src/isoline/A.cpp", (1, 1), (0, 0))
        del entry["segments"]
        result = self.run_([entry], "src/isoline/A.cpp 100.00 -\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("export without -summary-only", result.stderr)


if __name__ == "__main__":
    unittest.main()
