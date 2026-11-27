#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of check-contract-spelling.py on the fixture trees of testdata/contract-spelling/.

Each case is a directory holding a repository's worth of files the checker reads, and `expected.txt`:
its first line `exit: <status>`, then every line the checker must print, findings and summary alike.
The findings it prints must be exactly the expected ones; a summary line need only be among them.
"""

import contextlib
import importlib.util
import io
import pathlib
import sys
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "check-contract-spelling.py"
CASES = pathlib.Path(__file__).resolve().parent / "testdata" / "contract-spelling"

# Every case the fixtures must hold: a case dropped from testdata fails here rather than passing unseen.
EXPECTED_CASES = {
    "comments-are-not-code", "contract-in-header", "loop-post-is-not-a-contract",
    "meta-include-in-installed-header", "raw-contract-assert-in-cpp", "require-in-cpp",
    "require-over-its-row-count", "row-that-is-not-a-row", "seam-headers-exempt", "stale-row",
    "stale-row-named-like-a-call",
    "the-macros-and-rows-pass",
}
SUMMARY = "contract-spelling:"


def loadChecker():
    """Loads check-contract-spelling.py by path: its name is not an importable module name."""
    sys.dont_write_bytecode = True  # leave no __pycache__ in the source tree
    spec = importlib.util.spec_from_file_location("check_contract_spelling", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


checker = loadChecker()


class ContractSpelling(unittest.TestCase):
    def test_theCasesAreThere(self):
        self.assertEqual({case.name for case in CASES.iterdir() if case.is_dir()}, EXPECTED_CASES)

    def test_everyCase(self):
        for case in sorted(path for path in CASES.iterdir() if path.is_dir()):
            with self.subTest(case=case.name):
                expectedLines = (case / "expected.txt").read_text(encoding="utf-8").splitlines()
                self.assertTrue(expectedLines[0].startswith("exit: "), case)
                status = int(expectedLines[0].removeprefix("exit: "))
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    actualStatus = checker.main(["--root", str(case)])
                printed = output.getvalue().splitlines()
                self.assertEqual(actualStatus, status, output.getvalue())
                expected = expectedLines[1:]
                self.assertEqual([line for line in printed if not line.startswith(SUMMARY)],
                                 [line for line in expected if not line.startswith(SUMMARY)], output.getvalue())
                for line in expected:
                    if line.startswith(SUMMARY):
                        self.assertIn(line, printed)

    def test_aLaterLineIsCountedAfterABlockComment(self):
        code = checker.withoutComments("/* one\n   two */\nint x;\ncontract_assert(x);\n")
        self.assertEqual(code.count("\n", 0, code.index("contract_assert")) + 1, 4)

    def test_literalsAreBlankedButKeepTheirQuotes(self):
        self.assertEqual(checker.codeOnly('auto s = "Require(x) // "; char c = \'"\';'),
                         'auto s = "              "; char c = \' \';')


if __name__ == "__main__":
    unittest.main()
