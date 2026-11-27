#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of make-differential-corpus.py: the entropy of a seed, smith's flags for the cut, and which
modules the corpus keeps, with a stand-in for wasm-tools."""

import importlib.util
import os
import pathlib
import stat
import sys
import tempfile
import textwrap
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "make-differential-corpus.py"


def loadGenerator():
    """Loads make-differential-corpus.py by path: its name is not an importable module name."""
    sys.dont_write_bytecode = True  # leave no __pycache__ in the source tree
    spec = importlib.util.spec_from_file_location("make_differential_corpus", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


generator = loadGenerator()

# A stand-in for wasm-tools: `smith` writes its standard input's first byte and the flags it got as the
# module, and `validate` accepts a module whose first byte is even. It records each call's flags.
FAKE_WASM_TOOLS = textwrap.dedent("""\
    #!{python} -I
    import pathlib, sys
    verb, flags = sys.argv[1], sys.argv[2:]
    data = sys.stdin.buffer.read()
    with open(pathlib.Path(__file__).with_name("calls.txt"), "a", encoding="utf-8") as log:
        log.write(verb + " " + " ".join(flags) + "\\n")
    if verb == "smith":
        sys.stdout.buffer.write(data[:1] + " ".join(flags).encode())
        sys.exit(0)
    if verb == "validate":
        sys.exit(0 if data[0] % 2 == 0 else 1)
    sys.exit(2)
    """)


class EntropyTest(unittest.TestCase):
    def test_aSeedsEntropyIsTheSameEveryTime(self):
        self.assertEqual(generator.entropyOf(7), generator.entropyOf(7))
        self.assertEqual(len(generator.entropyOf(7)), generator.ENTROPY_BYTES)

    def test_seedsHaveTheirOwnEntropy(self):
        self.assertNotEqual(generator.entropyOf(0), generator.entropyOf(1))
        self.assertNotEqual(generator.entropyOf(1)[:32], generator.entropyOf(1)[32:64])

    def test_theEntropyIsPinned(self):
        # Seed N must stay module N on every machine: the derivation is part of the corpus's identity.
        self.assertEqual(generator.entropyOf(0, 8).hex(), __import__("hashlib").sha256(
            generator.ENTROPY_DOMAIN + bytes(16)).digest()[:8].hex())


class FlagsTest(unittest.TestCase):
    def test_everyFeatureOfTheCutHasARow(self):
        lock = generator.loadFetchTools().readLock(generator.LOCK)
        flags = generator.smithFlags(lock["cut.features"])
        self.assertIn("--gc-enabled", flags)
        self.assertEqual(flags[flags.index("--gc-enabled") + 1], "false")
        self.assertEqual(flags[flags.index("--wide-arithmetic-enabled") + 1], "true")
        self.assertEqual(flags[-len(generator.COMMON_FLAGS):], generator.COMMON_FLAGS)

    def test_aFeatureWithoutARowIsRefused(self):
        with self.assertRaises(SystemExit) as raised:
            generator.smithFlags("wasm3,threads")
        self.assertIn("threads", str(raised.exception))

    def test_memoriesStayWithinTheHarnessLimit(self):
        flags = generator.smithFlags("wasm3")
        self.assertEqual(flags[flags.index("--max-memory32-bytes") + 1], str(64 << 20))
        self.assertEqual(flags[flags.index("--max-memory64-bytes") + 1], str(64 << 20))
        self.assertEqual(flags[flags.index("--export-everything") + 1], "true")


class GenerateTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="isoline-corpus-test-")
        self.root = pathlib.Path(self.directory.name)
        self.tool = self.root / "wasm-tools"
        self.tool.write_text(FAKE_WASM_TOOLS.format(python=sys.executable), encoding="utf-8")
        self.tool.chmod(self.tool.stat().st_mode | stat.S_IXUSR)

    def tearDown(self):
        self.directory.cleanup()

    def test_theValidModulesAreKeptByTheirSeed(self):
        out = self.root / "corpus"
        seeds = range(0, 40)
        kept = generator.generateCorpus(self.tool, seeds, "wasm3", out, jobs=4)
        expected = {f"{seed}.wasm" for seed in seeds if generator.entropyOf(seed)[0] % 2 == 0}
        self.assertEqual(kept, len(expected))
        self.assertEqual({path.name for path in out.iterdir()}, expected)
        for name in expected:
            module = (out / name).read_bytes()
            self.assertEqual(module[:1], generator.entropyOf(int(name.removesuffix(".wasm")))[:1])
            self.assertIn(f"--min-funcs {generator.MIN_FUNCS} --max-instructions {generator.MAX_INSTRUCTIONS}".encode(), module)

    def test_eachModuleIsValidatedWithTheCut(self):
        generator.generateCorpus(self.tool, range(3), "wasm3,-gc", self.root / "corpus", jobs=1)
        calls = (self.root / "calls.txt").read_text(encoding="utf-8").splitlines()
        self.assertEqual(sum(call.startswith("smith ") for call in calls), 3)
        self.assertEqual(sum(call == "validate --features wasm3,-gc -" for call in calls), 3)

    def test_aFailingSmithFailsTheRun(self):
        self.tool.write_text(f"#!{sys.executable} -I\nimport sys\nsys.exit(3)\n", encoding="utf-8")
        with self.assertRaises(SystemExit):
            generator.generateCorpus(self.tool, range(1), "wasm3", self.root / "corpus", jobs=1)


if __name__ == "__main__":
    if os.name == "nt":
        print("make-differential-corpus selftest: the stand-in for wasm-tools is a POSIX script; skipped")
        sys.exit(0)
    unittest.main()
