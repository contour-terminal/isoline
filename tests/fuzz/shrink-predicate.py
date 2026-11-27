#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""The interestingness predicate `wasm-tools shrink` minimizes a diverging module with.

wasm-tools shrink runs it with a candidate module's path, and keeps the candidate when it exits with 0:
when isoline-differential-replay still reports a divergence (its exit status 1) with the same seed and
lane. A candidate the harness does not compare, or on which every engine agrees, is not interesting.

    ISOLINE_REPLAY=out/clang-asan/tests/differential/isoline-differential-replay ISOLINE_SEED=17 \\
        wasm-tools shrink tests/fuzz/shrink-predicate.py out/corpus/differential/17.wasm -o 17.wasm

ISOLINE_LANE (deterministic by default) picks the lane, and ISOLINE_FIELD, when set, keeps only a
candidate that diverges in that field, so that the minimization does not drift to another divergence.
"""

import os
import subprocess
import sys

# isoline-differential-replay's exit status for a divergence.
DIVERGES = 1


def isInteresting(candidate, replay, seed, lane, field=None):
    """Whether the candidate still diverges, in field when it is given."""
    result = subprocess.run([replay, candidate, seed, "--lane", lane], capture_output=True, text=True)
    if result.returncode != DIVERGES:
        return False
    return field is None or f" in {field}\n" in result.stdout


def main():
    if len(sys.argv) != 2:
        print("usage: shrink-predicate.py <candidate.wasm>, with ISOLINE_REPLAY and ISOLINE_SEED set", file=sys.stderr)
        return 2
    replay = os.environ.get("ISOLINE_REPLAY")
    seed = os.environ.get("ISOLINE_SEED")
    if not replay or not seed:
        print("shrink-predicate.py: ISOLINE_REPLAY and ISOLINE_SEED must be set", file=sys.stderr)
        return 2
    lane = os.environ.get("ISOLINE_LANE", "deterministic")
    field = os.environ.get("ISOLINE_FIELD") or None
    return 0 if isInteresting(sys.argv[1], replay, seed, lane, field) else 1


if __name__ == "__main__":
    sys.exit(main())
