#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of import-spec-tests.py: which commands become skip lines, and which fail the import."""

import importlib.util
import pathlib
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "import-spec-tests.py"


def loadImporter():
    """Loads import-spec-tests.py by path: its name is not an importable module name."""
    sys.dont_write_bytecode = True  # leave no __pycache__ in the source tree
    spec = importlib.util.spec_from_file_location("import_spec_tests", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


importer = loadImporter()


def hexText(token):
    return bytes.fromhex(token).decode("utf-8")


class LineOfTest(unittest.TestCase):
    def test_textAssertMalformedBecomesASkip(self):
        command = {"type": "assert_malformed", "line": 7, "module_type": "text", "filename": "m.wat",
                   "text": "unexpected token"}
        line, isSkip = importer.lineOf(command, "x.wast")
        self.assertTrue(isSkip)
        kind, reason = line.split(" ")
        self.assertEqual(kind, "skip")
        self.assertEqual(hexText(reason), "assert_malformed of a text module (wast line 7)")

    def test_binaryAssertMalformedIsKept(self):
        command = {"type": "assert_malformed", "line": 7, "module_type": "binary", "filename": "m.wasm",
                   "text": "magic header not detected"}
        line, isSkip = importer.lineOf(command, "x.wast")
        self.assertFalse(isSkip)
        self.assertTrue(line.startswith("assert_malformed m.wasm "))

    def test_otherTextAssertionsFailTheImport(self):
        for kind in ("assert_invalid", "assert_unlinkable", "assert_uninstantiable"):
            with self.subTest(kind=kind), self.assertRaises(SystemExit) as failure:
                importer.lineOf({"type": kind, "line": 3, "module_type": "text", "filename": "m.wat",
                                 "text": "type mismatch"}, "x.wast")
            self.assertIn("x.wast:3", str(failure.exception.code))

    def test_unsupportedValueFailsTheImport(self):
        # A SIMD assertion must not vanish into a skip line.
        command = {"type": "assert_return", "line": 12,
                   "action": {"type": "invoke", "field": "f", "args": []},
                   "expected": [{"type": "v128", "lane_type": "i32", "value": ["0", "0", "0", "0"]}]}
        with self.assertRaises(SystemExit) as failure:
            importer.lineOf(command, "simd.wast")
        self.assertIn("simd.wast:12: value type v128", str(failure.exception.code))

    def test_unknownCommandFailsTheImport(self):
        with self.assertRaises(SystemExit):
            importer.lineOf({"type": "assert_suspension", "line": 1}, "x.wast")

    def test_textModuleFailsTheImport(self):
        with self.assertRaises(SystemExit):
            importer.lineOf({"type": "module", "line": 1, "module_type": "text", "filename": "m.wat"}, "x.wast")


class ReadFilesTest(unittest.TestCase):
    def read(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "files.txt"
            path.write_text(text, encoding="utf-8")
            return importer.readFiles(path)

    def test_namesWithPinnedSkips(self):
        self.assertEqual(self.read("# comment\naddress 1\n\nbinary   0\n"), [("address", 1), ("binary", 0)])

    def test_aLineWithoutACountFails(self):
        with self.assertRaises(SystemExit):
            self.read("address\n")

    def test_theRepositoryListIsWellFormed(self):
        files = importer.readFiles(SCRIPT.parent.parent / "tests" / "spec" / "files.txt")
        self.assertTrue(files)
        for name, _ in files:
            self.assertTrue((SCRIPT.parent.parent / "tests" / "spec" / name / f"{name}.isotest").is_file(), name)


if __name__ == "__main__":
    unittest.main()
