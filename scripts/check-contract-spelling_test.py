#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of check-contract-spelling.py on fixture trees: one case per rule, and what it lets pass."""

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "check-contract-spelling.py"


def loadChecker():
    """Loads check-contract-spelling.py by path: its name is not an importable module name."""
    sys.dont_write_bytecode = True  # leave no __pycache__ in the source tree
    spec = importlib.util.spec_from_file_location("check_contract_spelling", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


checker = loadChecker()


class ContractSpelling(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_checker(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = checker.main(["--root", str(self.root)])
        return status, output.getvalue()

    def test_rawContractAssertInCpp(self):
        self.write("src/isoline/api/Engine.cpp",
                   "void f() noexcept\n{\n    contract_assert(true);\n}\n")
        status, output = self.run_checker()
        self.assertEqual(status, 1)
        self.assertIn("src/isoline/api/Engine.cpp:3: raw-contract-assert: a contract_assert outside the seam", output)

    def test_theMacrosPass(self):
        self.write("src/isoline/api/Engine.cpp",
                   "#include <isoline/meta/Check.hpp>\nvoid f() noexcept\n{\n    ISOLINE_PRECONDITION(true);\n}\n")
        status, output = self.run_checker()
        self.assertEqual(status, 0, output)

    def test_seamHeadersAreExempt(self):
        # The native header spells the contract itself; neither seam header is a .cpp, and both are exempt
        # by name all the same, so that a rule widened to headers keeps them out.
        for header in checker.SEAM_HEADERS:
            self.write(header, "#define ISOLINE_INVARIANT(c) contract_assert(c)\n")
        widened = tuple(checker.Rule(name=rule.name, glob="src/isoline/**/*.hpp", exempt=rule.exempt,
                                     forbidden=rule.forbidden, message=rule.message) for rule in checker.RULES)
        self.assertEqual(checker.findings(self.root, widened), [])

    def test_commentsAreNotCode(self):
        self.write("src/isoline/api/Extern.cpp",
                   "// a raw contract_assert is refused\n/* contract_assert(x)\n */\nauto s = \"// \"; int x;\n")
        status, output = self.run_checker()
        self.assertEqual(status, 0, output)

    def test_aLaterLineIsCountedAfterABlockComment(self):
        self.write("src/isoline/runtime/Store.cpp", "/* one\n   two */\nint x;\ncontract_assert(x);\n")
        found = checker.findings(self.root)
        self.assertEqual(len(found), 1)
        self.assertTrue(found[0].startswith("src/isoline/runtime/Store.cpp:4: "), found[0])


if __name__ == "__main__":
    unittest.main()
