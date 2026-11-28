#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of check-encodings.py, offline: a fake llvm-mc, a table of the bytes it knows, stands in for
the real one, so every way the oracle must fail is shown failing without LLVM."""

import contextlib
import importlib.util
import io
import pathlib
import re
import subprocess
import tempfile
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parent
SCRIPT = SCRIPTS / "check-encodings.py"


def loadCheckEncodings():
    """Loads check-encodings.py by path: its name is not an importable module name."""
    spec = importlib.util.spec_from_file_location("check_encodings", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


checkEncodings = loadCheckEncodings()

# The fake llvm-mc's reading of the bytes it knows, as llvm-mc prints it (a tab after the mnemonic):
# a list of instructions, or None for an encoding it reports invalid.
READINGS = {
    "0x48 0x89 0xc8": ["mov\trax, rcx"],
    "0x4c 0x89 0xc8": ["mov\trax, r9"],
    "0x48 0x8b 0x01": ["mov\trax, qword ptr [rcx]"],
    "0x48 0x83 0xc0 0x7f": ["add\trax, 0x7F"],
    "0x90 0x90": ["nop", "nop"],
    "0x0f 0xff 0xff": None,
    "0xcc": ["int3"],
}


class FakeLlvmMc:
    """Stands in for llvm-mc --disassemble: reads its input's `[...]` blocks and answers each from READINGS,
    with llvm-mc's warning, on the block's line, for an encoding it reports invalid."""

    def __init__(self, readings=None):
        self.readings = readings or READINGS
        self.calls = 0

    def __call__(self, command, input, **_):
        self.calls += 1
        assert "--disassemble" in command and "-triple=x86_64" in command, command
        stdout = []
        stderr = []
        for number, line in enumerate(input.splitlines(), 1):
            block = line.strip("[]")
            reading = self.readings[block]
            if reading is None:
                stderr.append(f"<stdin>:{number}:2: warning: invalid instruction encoding\n{line}\n ^")
            else:
                stdout.extend("\t" + instruction for instruction in reading)
        return subprocess.CompletedProcess(command, 1 if stderr else 0, "\n".join(stdout) + "\n", "\n".join(stderr))


class CheckEncodingsTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        self.golden = self.root / "x86_64.golden"

    def run(self, result=None):
        # Every test runs against the fake: the real llvm-mc is never needed here.
        self.fake = FakeLlvmMc()
        with mock.patch.object(checkEncodings.subprocess, "run", self.fake):
            return super().run(result)

    def check(self, samples, *options):
        """Runs check-encodings on samples ((hex, intended, row) each); returns (exit status, what it printed)."""
        samplesFile = self.root / "samples.txt"
        samplesFile.write_text("".join(f"{hexText}\t{intended}\t{row}\n" for hexText, intended, row in samples),
                               encoding="utf-8")
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            status = checkEncodings.main(["--input", str(samplesFile), "--llvm-mc", "llvm-mc", *options])
        return status, printed.getvalue()

    AGREEING = [("48 89 c8", "mov rax, rcx", "MovRR64"), ("48 8b 01", "mov rax, qword ptr [rcx]", "MovRM64"),
                ("48 83 c0 7f", "add rax, 127", "AddRI64")]

    def test_aDisassemblyEqualToTheIntentPasses(self):
        status, printed = self.check(self.AGREEING)
        self.assertEqual(status, 0, printed)
        self.assertIn("llvm-mc agrees with all 3 samples of 3 rows", printed)
        self.assertEqual(self.fake.calls, 1, "every sample goes to llvm-mc in one batch")

    def test_anotherInstructionFailsNamingTheRowEvenWithUpdate(self):
        # A dropped REX.B: 48 89 c8 is `mov rax, rcx`, where the intent says r9 (4c 89 c8).
        samples = self.AGREEING + [("48 89 c8", "mov rax, r9", "MovRR64Extended")]
        for options in ([], ["--golden", str(self.golden), "--update"]):
            with self.subTest(options=options):
                status, printed = self.check(samples, *options)
                self.assertEqual(status, 1, printed)
                self.assertIn("MovRR64Extended: sample 4: 48 89 c8: intended `mov rax, r9`, llvm-mc read "
                              "['mov rax, rcx']", printed)
                self.assertIn("1 of 4 samples disagree, in 1 of 4 rows: MovRR64Extended", printed)
                self.assertFalse(self.golden.exists(), "a disagreement never reaches the golden")

    def test_anInvalidEncodingFails(self):
        status, printed = self.check(self.AGREEING + [("0f ff ff", "ud0 eax, edi", "Ud0")])
        self.assertEqual(status, 1, printed)
        self.assertIn("Ud0: sample 4: 0f ff ff: intended `ud0 eax, edi`, llvm-mc read "
                      "['warning: invalid instruction encoding']", printed)

    def test_anInvalidEncodingFailsEvenWhenItsTextMatches(self):
        # llvm-mc decoded the intended instruction, yet reported the block: the warning alone fails it.
        with mock.patch.object(checkEncodings, "diagnosticsBySample", return_value={0: ["warning: odd"]}):
            status, printed = self.check(self.AGREEING)
        self.assertEqual(status, 1, printed)
        self.assertIn("MovRR64: sample 1", printed)

    def test_twoInstructionsForOneLineFail(self):
        status, printed = self.check(self.AGREEING + [("90 90", "nop", "Nop")])
        self.assertEqual(status, 1, printed)
        self.assertIn("Nop: sample 4: 90 90: intended `nop`, llvm-mc read ['nop', 'nop']", printed)

    def test_aDiagnosticOnNoSamplesLineIsUnusable(self):
        with self.assertRaises(SystemExit) as failure:
            checkEncodings.diagnosticsBySample("<stdin>:2:2: warning: invalid instruction encoding", 3)
        self.assertIn("no sample's", str(failure.exception))
        with self.assertRaises(SystemExit):
            checkEncodings.diagnosticsBySample("<stdin>:7:2: error: unexpected token", 3)
        self.assertEqual(checkEncodings.diagnosticsBySample("<stdin>:5:2: warning: invalid instruction encoding", 3),
                         {2: ["warning: invalid instruction encoding"]})

    def test_aMissingFenceIsUnusable(self):
        self.fake.readings = {**READINGS, "0xcc": []}
        with self.assertRaises(SystemExit) as failure:
            self.check(self.AGREEING)
        self.assertIn("fenced samples, expected 3", str(failure.exception))

    def test_aFailingLlvmMcNamingNoSampleIsUnusable(self):
        # Every sample and fence decoded, yet llvm-mc failed with a diagnostic on no line of the input.
        decoded = "".join(f"\t{text}\n\tint3\n" for _, text, _ in self.AGREEING)
        broken = subprocess.CompletedProcess([], 1, decoded, "llvm-mc: error: unable to get target for 'x86_64'")
        with mock.patch.object(checkEncodings.subprocess, "run", return_value=broken):
            with self.assertRaises(SystemExit) as failure:
                self.check(self.AGREEING)
        self.assertIn("exits with 1 naming no sample", str(failure.exception))

    def test_theGoldenIsWrittenOnlyOnAgreementAndThenChecked(self):
        status, printed = self.check(self.AGREEING, "--golden", str(self.golden))
        self.assertEqual(status, 1, printed)
        self.assertIn("is stale: rerun with --update", printed)
        status, printed = self.check(self.AGREEING, "--golden", str(self.golden), "--update")
        self.assertEqual(status, 0, printed)
        self.assertEqual(self.golden.read_text(encoding="utf-8").splitlines(),
                         ["\t".join(sample) for sample in self.AGREEING])
        status, printed = self.check(self.AGREEING, "--golden", str(self.golden))
        self.assertEqual(status, 0, printed)
        status, printed = self.check(self.AGREEING[:2], "--golden", str(self.golden))
        self.assertEqual(status, 1, "a sample the golden holds and the encoder no longer produces")

    def test_updateNeedsAGolden(self):
        with self.assertRaises(SystemExit) as failure, contextlib.redirect_stderr(io.StringIO()):
            self.check(self.AGREEING, "--update")
        self.assertEqual(failure.exception.code, 2)

    def test_eachNormalizationRowIsExercised(self):
        # Each row turns its example into the expected text, and without that row the example stays apart.
        names = [name for name, _, _, _ in checkEncodings.NORMALIZATION]
        self.assertEqual(len(names), len(set(names)), "row names are unique")
        for name, _, _, (raw, expected) in checkEncodings.NORMALIZATION:
            with self.subTest(row=name):
                self.assertEqual(checkEncodings.normalize(raw), expected)
                others = [row for row in checkEncodings.NORMALIZATION if row[0] != name]
                self.assertNotEqual(checkEncodings.normalize(raw, others), expected, "the row is needed")

    def test_normalizationKeepsWhatAnEncodingDecides(self):
        # An operand size (REX.W), a register (REX.B/R/X), the operand order and a displacement's sign.
        for intended, read in [("mov qword ptr [rax], rcx", "mov dword ptr [rax], ecx"),
                               ("mov rax, r9", "mov rax, rcx"), ("mov rax, rcx", "mov rcx, rax"),
                               ("lea rax, [rcx - 8]", "lea rax, [rcx + 8]"),
                               ("movdqu xmm0, xmmword ptr [rax]", "movdqu xmm0, qword ptr [rax]"),
                               ("mov r9b, al", "mov r9w, ax"), ("add rax, 0x10", "add rax, 10")]:
            with self.subTest(intended=intended, read=read):
                self.assertNotEqual(checkEncodings.normalize(intended), checkEncodings.normalize(read))

    def test_theRowsAreAppliedInOrder(self):
        # Case comes before the radix: an upper-case hex immediate is still read as hex.
        self.assertEqual(checkEncodings.normalize("ADD\tRAX,  0X7F  # imm"), "add rax, 127")
        self.assertTrue(all(isinstance(pattern, re.Pattern) for _, pattern, _, _ in checkEncodings.NORMALIZATION))


if __name__ == "__main__":
    unittest.main()
