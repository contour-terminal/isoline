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


def skipEntry(ordinal, line, reason):
    return f"{ordinal} {line} skip {reason.encode('utf-8').hex()}\n"


GC_MESSAGE = "rec group usage requires `gc` proposal to be enabled"

FILES = ("wasm3 core/tag 2\n"
         "wasm3 core/native 0\n"
         "wasm3 core/gc/array exclude GC: the GC proposal's own tests\n")

# The block the fixture's data gives: the excluded file, then the cut assertions by wast line.
# Each imported file's cut assertions come from the skip entries of its .oracle.
BLOCK = ("wasm3 core/gc/array: GC: the GC proposal's own tests\n"
         f"wasm3 core/native:14: {GC_MESSAGE}\n"
         f"wasm3 core/native:15: {GC_MESSAGE} (root wast line 14)\n"
         f"wasm3 core/tag:30: {GC_MESSAGE}\n"
         f"wasm3 core/tag:100: {GC_MESSAGE} (root wast line 30)\n")


# The divergences of the imported file, by wast line.
DIVERGENCE_BLOCK = ("wasm3 core/native:7: unknown local $x\n"
                    "wasm3 core/native:12: unknown operator nan:1\n")


# The parity divergences of the imported file.
PARITY_BLOCK = "wasm3 core/native:3: 0x1b sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef one type, as the reference\n"


def document(lines, divergenceLines=DIVERGENCE_BLOCK, parityLines=PARITY_BLOCK):
    def block(name, content):
        return f"{checker.begin(name)}\n```text\n{content}```\n{checker.end(name)}\n"

    return (f"# Conformance\n\nIntro.\n\n{block(checker.EXCLUSIONS, lines)}\nMiddle.\n\n"
            f"{block(checker.DIVERGENCES, divergenceLines)}\nAnd.\n\n"
            f"{block(checker.PARITY_DIVERGENCES, parityLines)}\nAfter.\n")


class CheckTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        spec = self.root / "tests" / "spec"
        (spec / "wasm3" / "core" / "tag").mkdir(parents=True)
        (spec / "files.txt").write_text("# a comment\n" + FILES, encoding="utf-8")
        # Out of wast-line order, so the block's order is the checker's.
        (spec / "wasm3" / "core" / "tag" / "tag.oracle").write_text(
            "commands 9\n"
            + skipEntry(8, 100, f"gc: {GC_MESSAGE} (root wast line 30; wast line 100)")
            + skipEntry(2, 30, f"gc: {GC_MESSAGE} (wast line 30)"),
            encoding="utf-8")
        (spec / "wasm3" / "core" / "native").mkdir(parents=True)
        (spec / "wasm3" / "core" / "native" / "native.oracle").write_text(
            "commands 9\n"
            f"5 12 message {'unknown operator nan:1'.encode().hex()}\n"
            "1 3 wasm native.0.wasm\n"
            f"1 3 parity {'0x1b sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef one type, as the reference'.encode().hex()}\n"
            f"4 7 message {'unknown local $x'.encode().hex()}\n"
            f"6 14 skip {f'gc: {GC_MESSAGE} (wast line 14)'.encode().hex()}\n"
            f"7 15 skip {f'gc: {GC_MESSAGE} (root wast line 14; wast line 15)'.encode().hex()}\n",
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

    def test_theBlocksListExactlyTheExclusionsAndDivergences(self):
        self.assertEqual(checker.generated(self.root / "tests" / "spec"),
                         {checker.EXCLUSIONS: BLOCK.splitlines(),
                          checker.DIVERGENCES: DIVERGENCE_BLOCK.splitlines(),
                          checker.PARITY_DIVERGENCES: PARITY_BLOCK.splitlines()})

    def test_aCurrentDocumentPasses(self):
        self.doc.write_text(document(BLOCK), encoding="utf-8")
        self.assertEqual(self.check()[0], 0)

    def test_aDocumentMissingALineFails(self):
        self.doc.write_text(document(BLOCK.splitlines(keepends=True)[0]), encoding="utf-8")
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn(f"+wasm3 core/tag:30: {GC_MESSAGE}", output)

    def test_aDocumentMissingADivergenceFails(self):
        self.doc.write_text(document(BLOCK, DIVERGENCE_BLOCK.splitlines(keepends=True)[0]),
                            encoding="utf-8")
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn("+wasm3 core/native:12: unknown operator nan:1", output)

    def test_aDocumentMissingAParityDivergenceFails(self):
        self.doc.write_text(document(BLOCK, DIVERGENCE_BLOCK, ""), encoding="utf-8")
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn("+wasm3 core/native:3: 0x1b sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef one type, as the reference", output)

    def test_aNativeSkipWithoutItsLineFails(self):
        oracle = self.root / "tests" / "spec" / "wasm3" / "core" / "native" / "native.oracle"
        oracle.write_text(f"commands 1\n0 1 skip {'gc: x'.encode().hex()}\n", encoding="utf-8")
        with self.assertRaises(SystemExit):
            checker.generated(self.root / "tests" / "spec")

    def test_aDocumentWithAnExtraLineFails(self):
        self.doc.write_text(document(BLOCK + "wasm3 core/gone: GC\n"), encoding="utf-8")
        status, output = self.check()
        self.assertEqual(status, 1)
        self.assertIn("-wasm3 core/gone: GC", output)

    def test_updateRewritesOnlyTheBlock(self):
        self.doc.write_text(document("", "", ""), encoding="utf-8")
        self.assertEqual(self.check("--update")[0], 0)
        self.assertEqual(self.doc.read_text(encoding="utf-8"), document(BLOCK))
        self.assertEqual(self.check()[0], 0)

    def test_aDocumentWithoutTheMarkersFails(self):
        for text in ("# Conformance\n", document(BLOCK).replace(checker.end(checker.DIVERGENCES), "")):
            with self.subTest(text=text):
                self.doc.write_text(text, encoding="utf-8")
                with self.assertRaises(SystemExit):
                    self.check()

    def test_aSkipThatIsNotTheCutsFails(self):
        # The importer writes no other skip: one that names a module given as text, as the retired .isotest
        # translation did, is an .oracle edited by hand.
        oracle = self.root / "tests" / "spec" / "wasm3" / "core" / "tag" / "tag.oracle"
        oracle.write_text("commands 1\n" + skipEntry(0, 40, "assert_malformed of a text module (wast line 40)"),
                          encoding="utf-8")
        with self.assertRaises(SystemExit) as failure:
            checker.generated(self.root / "tests" / "spec")
        self.assertIn("a skip that is not the GC cut's: assert_malformed of a text module", str(failure.exception.code))


if __name__ == "__main__":
    unittest.main()
