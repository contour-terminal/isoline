#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of coverage-check.py: runs it on fixture reports, as CI does, and checks its verdicts."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "coverage-check.py"
ROOT = pathlib.PurePosixPath("/repo")


def fileEntry(path, lines, branches, root=ROOT):
    """One file of an llvm-cov export: (covered, count) lines and branches."""
    def summary(covered, count):
        return {"count": count, "covered": covered, "percent": 100.0 * covered / count if count else 0}
    return {"filename": f"{pathlib.PurePath(root).as_posix()}/{path}",
            "summary": {"lines": summary(*lines), "branches": summary(*branches)}}


class CoverageCheckTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.report = pathlib.Path(self.directory.name) / "coverage.json"
        self.baseline = pathlib.Path(self.directory.name) / "coverage-baseline.txt"

    def run_(self, files, baseline, *options, root=ROOT):
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


if __name__ == "__main__":
    unittest.main()
