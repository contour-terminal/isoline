#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""The x86-64 encoding oracle: llvm-mc must read each sample's bytes as the instruction it is meant to be.

isoline_encoding_samples (tests/encoding) prints one `hex<TAB>intended<TAB>row` line per sample of every
row of src/isoline/compiler/x86_64/Encodings.def: the encoder's bytes, and the row's Intel template
rendered with the sample's operands, which knows nothing of the encoder. This script has llvm-mc
disassemble every sample's bytes in Intel syntax in one batch, each sample on its own (an atomic `[...]`
block, then an `int3` block as a fence), normalizes both texts by the rows of NORMALIZATION, and fails
when any sample decodes to other than exactly its intended instruction: another instruction, more than
one, or none because llvm-mc reports the encoding invalid. A dropped REX.B fails because llvm-mc reads
`mov rax, rcx` where the intent says `mov rax, r9`, not because a golden changed. Every disagreement is
printed with the Encodings.def row it belongs to.

With --golden, the samples must also equal the cached agreement, tests/encoding/x86_64.golden, line for
line, which Encoder.matchesTheCachedAgreement checks offline; --update rewrites it, and only once llvm-mc
agrees with every sample. The golden is never hand-written.

Usage: python3 -I scripts/check-encodings.py (--samples <isoline_encoding_samples> | --input <file>)
           --llvm-mc <llvm-mc> [--golden <x86_64.golden> [--update]]
The llvm-mc is the release scripts/fetch-tools.py pins (`--tool llvm-mc`).
"""

import argparse
import pathlib
import re
import subprocess
import sys

# The instruction that fences one sample's decoding from the next: no row of the table encodes it.
FENCE_BYTES = "0xcc"
FENCE_TEXT = "int3"

# Seconds llvm-mc may take over the whole table.
LLVM_MC_TIMEOUT = 600

# How many mismatches are printed before the rest are only counted.
REPORTED_MISMATCHES = 40

# How both texts are made comparable, applied in row order to the intended text and to llvm-mc's alike:
# (name, pattern, replacement, an example and what it becomes). A row only respells: it never drops what
# an encoding decides. An operand size (`dword ptr` against `qword ptr` is a dropped REX.W), a register
# (`rcx` against `r9` is a dropped REX.B), an operand's order and a displacement's sign stay significant.
# check-encodings_test.py applies each row's example with and without the row.
NORMALIZATION = [
    # llvm-mc may annotate an instruction (`# imm = 0x7FFFFFFF`); the annotation is no part of it.
    ("comment", re.compile(r"\s*#.*$"), "", ("add ebx, 2147483647 # imm = 0x7FFFFFFF", "add ebx, 2147483647")),
    # llvm-mc separates the mnemonic by a tab.
    ("whitespace", re.compile(r"\s+"), " ", ("mov\trax,  rcx", "mov rax, rcx")),
    ("trim", re.compile(r"^ | $"), "", (" mov rax, rcx ", "mov rax, rcx")),
    # Mnemonics, registers and size keywords are case-blind.
    ("case", re.compile(r"[A-Z]+"), lambda match: match.group(0).lower(), ("MOV RAX, QWORD PTR [RCX]",
                                                                            "mov rax, qword ptr [rcx]")),
    # An immediate or displacement in hex, of either case, is its decimal value.
    ("hexRadix", re.compile(r"(?<![\w])(-?)0x([0-9a-f]+)\b"),
     lambda match: str(int(match.group(1) + match.group(2), 16)), ("add rbx, 0x7f", "add rbx, 127")),
    # The 16-byte memory operand: NASM's spelling, `oword`, is LLVM's `xmmword`.
    ("ptrSize", re.compile(r"\boword ptr\b"), "xmmword ptr",
     ("movdqu xmm0, oword ptr [rax]", "movdqu xmm0, xmmword ptr [rax]")),
    # The low byte of r8-r15: Intel's manual spells it r8l as well as r8b, and LLVM r8b.
    ("registerAlias", re.compile(r"\b(r(?:8|9|1[0-5]))l\b"), r"\1b", ("mov r9l, al", "mov r9b, al")),
]

# One diagnostic of llvm-mc on its input: `<stdin>:<line>:<column>: <severity>: <message>`.
DIAGNOSTIC = re.compile(r"^<stdin>:(?P<line>\d+):\d+: (?P<severity>warning|error): (?P<message>.*)$")


def parseSamples(text):
    """The samples of isoline_encoding_samples' output: (hex, intended, row) per line."""
    samples = []
    for number, line in enumerate(text.splitlines(), 1):
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) != 3 or not fields[0]:
            raise SystemExit(f"sample line {number}: expected hex<TAB>intended<TAB>row, got {line!r}")
        samples.append(tuple(fields))
    return samples


def llvmInput(samples):
    """llvm-mc's input: each sample's bytes as one atomic block on line 2i + 1, each followed by the fence."""
    lines = []
    for hexText, _, _ in samples:
        lines.append("[" + " ".join("0x" + byte for byte in hexText.split()) + "]")
        lines.append(f"[{FENCE_BYTES}]")
    return "\n".join(lines) + "\n"


def normalize(text, rules=NORMALIZATION):
    """An instruction's text, comparable: the rules applied in order."""
    for _, pattern, replacement, _ in rules:
        text = pattern.sub(replacement, text)
    return text


def splitAtFences(decoded):
    """llvm-mc's instructions, split into one list per sample at each fence."""
    groups = []
    current = []
    for line in decoded:
        if line == FENCE_TEXT:
            groups.append(current)
            current = []
        else:
            current.append(line)
    if current:
        groups.append(current)
    return groups


def diagnosticsBySample(stderr, count):
    """llvm-mc's diagnostics, by the index of the sample whose block they are on: {index: [message]}. One on
    a fence, or on no line of the input, makes the whole output unusable."""
    found = {}
    for line in stderr.splitlines():
        diagnostic = DIAGNOSTIC.match(line)
        if not diagnostic:
            continue
        number = int(diagnostic.group("line"))
        index, isFence = divmod(number - 1, 2)
        if isFence or index >= count:
            raise SystemExit(f"llvm-mc reports on input line {number}, no sample's: {line}")
        found.setdefault(index, []).append(f"{diagnostic.group('severity')}: {diagnostic.group('message')}")
    return found


def disassemble(llvmMc, samples):
    """llvm-mc's reading of each sample, in order: (its normalized instructions, its diagnostics) per sample."""
    completed = subprocess.run(
        [llvmMc, "--disassemble", "-triple=x86_64", "--output-asm-variant=1"],
        input=llvmInput(samples),
        capture_output=True,
        text=True,
        timeout=LLVM_MC_TIMEOUT,
        check=False,
    )
    # An invalid encoding is a warning and a non-zero exit, yet its fence still decodes: the sample reads as
    # nothing and carries the warning, which compare() reports. Only a missing fence makes the output unusable.
    decoded = [normalize(line) for line in completed.stdout.splitlines()]
    groups = splitAtFences([line for line in decoded if line and not line.startswith(".")])
    if len(groups) != len(samples):
        raise SystemExit(f"llvm-mc decoded {len(groups)} fenced samples, expected {len(samples)} "
                         f"(exit {completed.returncode}): {completed.stderr.strip()[:2000]}")
    diagnostics = diagnosticsBySample(completed.stderr, len(samples))
    if completed.returncode != 0 and not diagnostics:
        raise SystemExit(f"llvm-mc exits with {completed.returncode} naming no sample: "
                         f"{completed.stderr.strip()[:2000]}")
    return [(group, diagnostics.get(index, [])) for index, group in enumerate(groups)]


def compare(samples, readings):
    """The samples llvm-mc does not read as intended: (index, sample, what llvm-mc read and reported)."""
    mismatches = []
    for index, (sample, (group, diagnostics)) in enumerate(zip(samples, readings)):
        if diagnostics or group != [normalize(sample[1])]:
            mismatches.append((index, sample, group + diagnostics))
    return mismatches


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--samples", help="the isoline_encoding_samples executable")
    source.add_argument("--input", help="a file holding its output")
    parser.add_argument("--llvm-mc", required=True, help="the pinned llvm-mc")
    parser.add_argument("--golden", help="tests/encoding/x86_64.golden")
    parser.add_argument("--update", action="store_true", help="rewrite the golden once llvm-mc agrees")
    args = parser.parse_args(argv)
    if args.update and not args.golden:
        parser.error("--update needs --golden")

    if args.samples:
        produced = subprocess.run([args.samples], capture_output=True, text=True, check=False)
        if produced.returncode != 0:
            raise SystemExit(f"{args.samples} failed: {produced.stderr.strip()[:2000]}")
        output = produced.stdout
    else:
        output = pathlib.Path(args.input).read_text(encoding="utf-8")
    samples = parseSamples(output)
    if not samples:
        raise SystemExit("no samples")

    mismatches = compare(samples, disassemble(args.llvm_mc, samples))
    rows = {sample[2] for sample in samples}
    for index, (hexText, intended, row), read in mismatches[:REPORTED_MISMATCHES]:
        print(f"{row}: sample {index + 1}: {hexText}: intended `{intended}`, llvm-mc read {read}")
    if mismatches:
        badRows = sorted({sample[2] for _, sample, _ in mismatches})
        print(f"{len(mismatches)} of {len(samples)} samples disagree, in {len(badRows)} of {len(rows)} rows: "
              + ", ".join(badRows))
        if args.update:
            print(f"{args.golden} is left as it was: the golden caches only an agreement")
        return 1
    print(f"llvm-mc agrees with all {len(samples)} samples of {len(rows)} rows")

    if args.golden:
        golden = pathlib.Path(args.golden)
        lines = ["\t".join(sample) for sample in samples]
        if args.update:
            golden.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
            print(f"wrote {golden}")
        elif not golden.exists() or golden.read_text(encoding="utf-8").splitlines() != lines:
            print(f"{golden} is stale: rerun with --update")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
