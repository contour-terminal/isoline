#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""The x86-64 encoding oracle: llvm-mc must read each sample's bytes as the instruction it is meant to be.

isoline_encoding_samples (tests/encoding) prints one `hex<TAB>intended<TAB>row` line per sample of every
row of src/isoline/compiler/x86_64/Encodings.def: the encoder's bytes, and the row's Intel template
rendered with the sample's operands, which knows nothing of the encoder. This script has llvm-mc
disassemble every sample's bytes in Intel syntax, each sample on its own (an atomic `[...]` block, then an
`int3` block as a fence), and asserts that each decodes to exactly one instruction whose text equals the
intended one after normalization (whitespace, case, hex immediates as decimal). A dropped REX.B fails
because llvm-mc reads `mov rax, rcx` where the intent says `mov rax, r9`, not because a golden changed.

With --golden, the samples must also equal the cached agreement, tests/encoding/x86_64.golden, line for
line, which Encoder.matchesTheCachedAgreement checks offline; --write-golden rewrites it once llvm-mc
agrees with every sample. The golden is never hand-written.

Usage: python3 -I scripts/check-encodings.py (--samples <isoline_encoding_samples> | --input <file>)
           --llvm-mc <llvm-mc> [--golden <x86_64.golden> [--write-golden]]
The llvm-mc is the LLVM release scripts/fetch-tools.py pins (LLVM 22).
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
    """llvm-mc's input: each sample's bytes as one atomic block, each followed by the fence."""
    lines = []
    for hexText, _, _ in samples:
        lines.append("[" + " ".join("0x" + byte for byte in hexText.split()) + "]")
        lines.append(f"[{FENCE_BYTES}]")
    return "\n".join(lines) + "\n"


def normalize(text):
    """An instruction's text, comparable: lowercase, single spaces, hex immediates in decimal."""
    text = text.split("#", 1)[0].strip().lower()
    text = re.sub(r"\s+", " ", text)
    text = re.sub(r"(?<![\w])(-?)0x([0-9a-f]+)\b",
                  lambda match: str(int(match.group(1) + match.group(2), 16)), text)
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


def disassemble(llvmMc, samples):
    """llvm-mc's instructions for each sample, in order: a list of normalized texts per sample."""
    completed = subprocess.run(
        [llvmMc, "--disassemble", "-triple=x86_64", "-output-asm-variant=1"],
        input=llvmInput(samples),
        capture_output=True,
        text=True,
        timeout=LLVM_MC_TIMEOUT,
        check=False,
    )
    # An invalid encoding is a warning and a non-zero exit, yet its fence still decodes: the sample reads as
    # nothing, which compare() reports. Only a missing fence makes the output unusable.
    decoded = [normalize(line) for line in completed.stdout.splitlines()]
    groups = splitAtFences([line for line in decoded if line])
    if len(groups) != len(samples):
        raise SystemExit(f"llvm-mc decoded {len(groups)} fenced samples, expected {len(samples)} "
                         f"(exit {completed.returncode}): {completed.stderr.strip()[:2000]}")
    return groups


def compare(samples, groups):
    """The samples llvm-mc does not read as intended: (index, sample, what llvm-mc read)."""
    mismatches = []
    for index, (sample, group) in enumerate(zip(samples, groups)):
        if group != [normalize(sample[1])]:
            mismatches.append((index, sample, group))
    return mismatches


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--samples", help="the isoline_encoding_samples executable")
    source.add_argument("--input", help="a file holding its output")
    parser.add_argument("--llvm-mc", required=True, help="the pinned llvm-mc")
    parser.add_argument("--golden", help="tests/encoding/x86_64.golden")
    parser.add_argument("--write-golden", action="store_true", help="rewrite the golden once llvm-mc agrees")
    args = parser.parse_args(argv)
    if args.write_golden and not args.golden:
        parser.error("--write-golden needs --golden")

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
    for index, (hexText, intended, row), group in mismatches[:REPORTED_MISMATCHES]:
        print(f"{row}: sample {index + 1}: {hexText}: intended `{intended}`, llvm-mc read {group}")
    if mismatches:
        badRows = sorted({sample[2] for _, sample, _ in mismatches})
        print(f"{len(mismatches)} of {len(samples)} samples disagree, in {len(badRows)} of {len(rows)} rows: "
              + ", ".join(badRows))
        return 1
    print(f"llvm-mc agrees with all {len(samples)} samples of {len(rows)} rows")

    if args.golden:
        golden = pathlib.Path(args.golden)
        lines = ["\t".join(sample) for sample in samples]
        if args.write_golden:
            golden.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
            print(f"wrote {golden}")
        elif golden.read_text(encoding="utf-8").splitlines() != lines:
            print(f"{golden} is stale: rerun with --write-golden")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
