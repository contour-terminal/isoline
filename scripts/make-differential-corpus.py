#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Generates the differential corpus: one module per seed, by the pinned `wasm-tools smith`.

Each seed's entropy is derived from the seed alone (SHA-256 in counter mode), so seed N is the same
module on every machine with the same wasm-tools. smith is given the features of the cut
(scripts/tools.lock's cut.features) and the store limits of the harness (tests/differential's
FuzzLimits), and every module it makes is validated with the cut's features; one that does not validate
is left out and counted. The modules go to out/corpus/differential/<seed>.wasm, never committed: CI
caches the directory by the hash of tools.lock and this script.

Usage: python3 -I scripts/make-differential-corpus.py [--seeds N] [--first N] [--out DIRECTORY]
                                                      [--wasm-tools PATH] [--jobs N]

Then `ctest -L differential` replays it (tests/differential/CorpusReplay_test.cpp), and
`isoline-differential-replay out/corpus/differential/<seed>.wasm <seed>` replays one module.
"""

import argparse
import concurrent.futures
import hashlib
import importlib.util
import os
import pathlib
import subprocess
import sys

SCRIPTS = pathlib.Path(__file__).resolve().parent
ROOT = SCRIPTS.parent
LOCK = SCRIPTS / "tools.lock"
DEFAULT_OUT = ROOT / "out" / "corpus" / "differential"
DEFAULT_SEEDS = 10_000

# The bytes of entropy smith reads per module: enough for modules of several functions and bodies of a few
# hundred instructions before smith runs out and ends the module.
ENTROPY_BYTES = 8_192
# What the entropy of every seed is derived from, besides the seed.
ENTROPY_DOMAIN = b"isoline-differential-corpus"

# 64 MiB: FuzzLimits::maxMemoryBytes, so that a memory smith makes fits the harness's store.
MAX_MEMORY_BYTES = 64 << 20

# The smith flags each feature of the cut turns on: (cut feature, its smith flags). A cut feature without
# a row is refused, so that a new feature in tools.lock is a decision here, not a silent gap.
FEATURE_FLAGS = [
    ("wasm3", [
        "--bulk-memory-enabled", "true", "--reference-types-enabled", "true", "--multi-value-enabled", "true",
        "--saturating-float-to-int-enabled", "true", "--sign-extension-ops-enabled", "true",
        "--simd-enabled", "true", "--relaxed-simd-enabled", "true", "--exceptions-enabled", "true",
        "--memory64-enabled", "true", "--tail-call-enabled", "true", "--extended-const-enabled", "true",
        "--max-memories", "4",
    ]),
    ("-gc", ["--gc-enabled", "false"]),
    ("wide-arithmetic", ["--wide-arithmetic-enabled", "true"]),
    ("compact-imports", ["--compact-imports-enabled", "true"]),
    ("custom-page-sizes", ["--custom-page-sizes-enabled", "true"]),
]

# At least this many functions, imports included: smith's default of none left the average module of the
# first corpus with less than one function, so that a fault in a float kernel went unseen by all 10'000.
MIN_FUNCS = 8
# Bodies of up to this many instructions, four times smith's default, so that a call gets past its first
# few instructions to the loops, calls, accesses and throws further in.
MAX_INSTRUCTIONS = 400

# The flags every module is generated with, whatever the cut: no threads (not in the cut), memories within
# the harness's limit, every function exported so that the harness calls it, NaNs left as they are (the
# harness compares their bits), and enough functions and instructions to reach the edges.
COMMON_FLAGS = [
    "--threads-enabled", "false",
    "--shared-everything-threads-enabled", "false",
    "--max-memory32-bytes", str(MAX_MEMORY_BYTES),
    "--max-memory64-bytes", str(MAX_MEMORY_BYTES),
    "--export-everything", "true",
    "--canonicalize-nans", "false",
    "--min-funcs", str(MIN_FUNCS),
    "--max-instructions", str(MAX_INSTRUCTIONS),
]


def loadFetchTools():
    """Loads fetch-tools.py by path: its name is not an importable module name."""
    sys.dont_write_bytecode = True  # leave no __pycache__ in the source tree
    spec = importlib.util.spec_from_file_location("fetch_tools", SCRIPTS / "fetch-tools.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def entropyOf(seed, size=ENTROPY_BYTES):
    """The entropy smith reads for seed: SHA-256 of the domain, the seed and a counter, block after block."""
    blocks = []
    counter = 0
    while sum(map(len, blocks)) < size:
        blocks.append(hashlib.sha256(ENTROPY_DOMAIN + seed.to_bytes(8, "little") + counter.to_bytes(8, "little")).digest())
        counter += 1
    return b"".join(blocks)[:size]


def smithFlags(cutFeatures):
    """smith's flags for the cut's features (a comma-separated list), then the common ones."""
    rows = dict(FEATURE_FLAGS)
    flags = []
    for feature in [name.strip() for name in cutFeatures.split(",") if name.strip()]:
        if feature not in rows:
            raise SystemExit(f"the cut's feature {feature!r} has no FEATURE_FLAGS row in {pathlib.Path(__file__).name}")
        flags += rows[feature]
    return flags + COMMON_FLAGS


def generate(wasmTools, seed, flags, cutFeatures, out):
    """Generates seed's module into out/<seed>.wasm; returns whether it validated (and was kept)."""
    smith = subprocess.run([os.fspath(wasmTools), "smith", *flags], input=entropyOf(seed), capture_output=True)
    if smith.returncode != 0:
        raise SystemExit(f"seed {seed}: wasm-tools smith failed: {smith.stderr.decode(errors='replace')}")
    validate = subprocess.run([os.fspath(wasmTools), "validate", "--features", cutFeatures, "-"],
                              input=smith.stdout, capture_output=True)
    if validate.returncode != 0:
        return False
    target = out / f"{seed}.wasm"
    partial = target.with_name(target.name + ".part")
    partial.write_bytes(smith.stdout)
    partial.replace(target)
    return True


def generateCorpus(wasmTools, seeds, cutFeatures, out, jobs):
    """Generates every seed's module; returns how many validated and were kept."""
    out.mkdir(parents=True, exist_ok=True)
    flags = smithFlags(cutFeatures)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        kept = sum(pool.map(lambda seed: generate(wasmTools, seed, flags, cutFeatures, out), seeds))
    return kept


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--seeds", type=int, default=DEFAULT_SEEDS, help="how many seeds (default 10000)")
    parser.add_argument("--first", type=int, default=0, help="the first seed (default 0)")
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT, help="the corpus directory")
    parser.add_argument("--wasm-tools", type=pathlib.Path, help="the wasm-tools to use (default: the pinned one)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1, help="parallel generators")
    arguments = parser.parse_args()
    if arguments.seeds < 0 or arguments.first < 0 or arguments.jobs < 1:
        parser.error("--seeds and --first must not be negative, and --jobs must be positive")

    fetchTools = loadFetchTools()
    lock = fetchTools.readLock(LOCK)
    wasmTools = arguments.wasm_tools or fetchTools.ensureWasmTools(LOCK)
    if arguments.wasm_tools:
        print(f"warning: {arguments.wasm_tools} is not checked against the wasm-tools pinned in {LOCK.name}; "
              "the corpus is reproducible only with the pinned build", file=sys.stderr)
    seeds = range(arguments.first, arguments.first + arguments.seeds)
    kept = generateCorpus(wasmTools, seeds, lock["cut.features"], arguments.out, arguments.jobs)
    print(f"{kept} of {len(seeds)} modules validated, in {arguments.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
