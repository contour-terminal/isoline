#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of check-conformance.py on fixture trees: the block it expects, and when it fails."""

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "check-conformance.py"


def loadChecker():
    """Loads check-conformance.py by path: its name is not an importable module name."""
    sys.dont_write_bytecode = True  # leave no __pycache__ in the source tree
    spec = importlib.util.spec_from_file_location("check_conformance", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


checker = loadChecker()


def skip(reason):
    return "skip " + reason.encode("utf-8").hex()


GC_MESSAGE = "rec group usage requires `gc` proposal to be enabled"

FILES = ("wasm3 core/tag 3\n"
         "wasm3 core/gc/array exclude GC: the GC proposal's own tests\n")

# The block the fixture's data gives: the excluded file, then the cut assertions by wast line.
BLOCK = ("wasm3 core/gc/array: GC: the GC proposal's own tests\n"
         f"wasm3 core/tag:30: {GC_MESSAGE}\n"
         f"wasm3 core/tag:100: {GC_MESSAGE} (root wast line 30)\n")


# The text skips: by wast line, a dependent naming its root.
TEXT_BLOCK = ("wasm3 core/tag:40: assert_malformed of a text module\n"
              "wasm3 core/tag:50: text: module of a text module\n"
              "wasm3 core/tag:51: text: module of a text module (root wast line 50)\n")


def document(lines, textLines=TEXT_BLOCK):
    def block(name, content):
        return f"{checker.begin(name)}\n```text\n{content}```\n{checker.end(name)}\n"

    return (f"# Conformance\n\nIntro.\n\n{block(checker.EXCLUSIONS, lines)}\nMiddle.\n\n"
            f"{block(checker.TEXT_SKIPS, textLines)}\nAfter.\n")


class CheckTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        spec = self.root / "tests" / "spec"
        (spec / "wasm3" / "core" / "tag").mkdir(parents=True)
        (spec / "files.txt").write_text("# a comment\n" + FILES, encoding="utf-8")
        (spec / "wasm3" / "core" / "tag" / "tag.isotest").write_text(
            "\n".join(["module tag.0.wasm",
                       skip(f"gc: {GC_MESSAGE} (root wast line 30; wast line 100)"),
                       skip(f"gc: {GC_MESSAGE} (wast line 30)"),
                       skip("assert_malformed of a text module (wast line 40)"),
                       skip("text: module of a text module (root wast line 50; wast line 51)"),
                       skip("text: module of a text module (wast line 50)")]) + "\n",
            encoding="utf-8")
        (self.root / "docs").mkdir()
        self.doc = self.root / "docs" / "conformance.md"

    def tearDown(self):
        self.directory.cleanup()

    def check(self, *arguments):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = checker.main(["--root", str(self.root), *arguments])
        return status, output.getvalue()

    def test_theBlocksListExactlyTheExclusionsAndTextSkips(self):
        self.assertEqual(checker.generated(self.root / "tests" / "spec"),
                         {checker.EXCLUSIONS: BLOCK.splitlines(), checker.TEXT_SKIPS: TEXT_BLOCK.splitlines()})

    def test_aCurrentDocumentPasses(self):
        self.doc.write_text(document(BLOCK), encoding="utf-8")
        self.assertEqual(self.check()[0], 0)

    def test_aDocumentMissingALineFails(self):
        self.doc.write_text(document(BLOCK.splitlines(keepends=True)[0]), encoding="utf-8")
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn(f"+wasm3 core/tag:30: {GC_MESSAGE}", output)

    def test_aDocumentMissingATextSkipFails(self):
        self.doc.write_text(document(BLOCK, TEXT_BLOCK.splitlines(keepends=True)[0]), encoding="utf-8")
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn("+wasm3 core/tag:50: text: module of a text module", output)

    def test_aDocumentWithAnExtraLineFails(self):
        self.doc.write_text(document(BLOCK + "wasm3 core/gone: GC\n"), encoding="utf-8")
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn("-wasm3 core/gone: GC", output)

    def test_updateRewritesOnlyTheBlock(self):
        self.doc.write_text(document("", ""), encoding="utf-8")
        self.assertEqual(self.check("--update")[0], 0)
        self.assertEqual(self.doc.read_text(encoding="utf-8"), document(BLOCK))
        self.assertEqual(self.check()[0], 0)

    def test_aDocumentWithoutTheMarkersFails(self):
        for text in ("# Conformance\n", document(BLOCK).replace(checker.end(checker.TEXT_SKIPS), "")):
            with self.subTest(text=text):
                self.doc.write_text(text, encoding="utf-8")
                with self.assertRaises(SystemExit):
                    self.check()

    def test_aSkipWithoutItsLineFails(self):
        isotest = self.root / "tests" / "spec" / "wasm3" / "core" / "tag" / "tag.isotest"
        isotest.write_text(skip("gc: something") + "\n", encoding="utf-8")
        with self.assertRaises(SystemExit):
            checker.generated(self.root / "tests" / "spec")


if __name__ == "__main__":
    unittest.main()
