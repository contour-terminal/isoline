#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Times Isoline's interpreter against Contour's WAMR, and reports it (docs/performance.md).

The runners are a table (RUNNERS): every Isoline engine and dispatch through isoline-benchmark, Contour's
WAMR, its classic interpreter and WAMR's defaults through isoline-benchmark-wamr, and the pinned Wasmtime.
Each runs a guest of benchmarks/guests (pinned in benchmarks/guests.lock) as a whole process, timed by this script's own monotonic
clock around it: at n = N and at n = 0 iterations, in alternating pairs on a pinned core, the runners
interleaved within each round, and the median of the paired differences t(N) - t(0) over N is the kernel's
time per iteration, which every table and ratio uses. Each runner has its own N, calibrated so that its
t(N) - t(0) takes about a second, or pinned from a committed file (--pinned-n). t(0) (load, lowering,
instantiation and process start) is published as the cold-start row. Every series is run twice (A/A); a
spread of 2 % or more between the two medians says the machine was not quiet, and the run is to be repeated
(exit status 3).

The bar (published, not blocking: the owner's ruling is that a miss is tuned in a follow-up): the geomean over
the shootout guests of the best interpreter dispatch's time over wamr-contour's is at most 1.00; the switch
dispatch's (Clang and GCC) at most 1.25. The micro-kernels are reported apart, and the SIMD guest has no
WAMR column.

Every C and C++ build of the runners takes the one flag table (FLAG_TABLE), recorded in benchmark.json with
the compilers' versions; `build` configures the trees with it.

Usage: python3 -I scripts/benchmark.py build [--jobs N] [--pace-scheduler HOST:PORT]
       python3 -I scripts/benchmark.py run [--runners a,b,...] [--guests a,b,...] [--runs 10] [--core 2]
                                           [--iterations N | --pinned-n FILE] [--write-pinned-n FILE]
                                           [--no-aa] [--tree BUILD=DIR ...] [--json out/benchmark.json]
       python3 -I scripts/benchmark.py calls [--runners a,b,...] [--invocations 100000] [--tree BUILD=DIR ...]
       python3 -I scripts/benchmark.py instructions (--update | --compare) [--tree BUILD=DIR ...]
       python3 -I scripts/benchmark.py merge BASE.json RERUN.json... [--json out/benchmark.json]
       python3 -I scripts/benchmark.py report [--json out/benchmark.json] [--doc docs/performance.md]
"""

import argparse
import dataclasses
import hashlib
import json
import math
import os
import pathlib
import platform
import shlex
import statistics
import subprocess
import sys
import time

SCRIPTS = pathlib.Path(__file__).resolve().parent
ROOT = SCRIPTS.parent
BENCHMARKS = ROOT / "benchmarks"
GUESTS = BENCHMARKS / "guests"
GUESTS_LOCK = BENCHMARKS / "guests.lock"
WAMR_PATCHES_LOCK = BENCHMARKS / "wamr" / "patches.lock"
WAMR_PATCHES = BENCHMARKS / "wamr" / "patches"
TOOLS_LOCK = SCRIPTS / "tools.lock"
INSTRUCTIONS_BASELINE = BENCHMARKS / "baseline-instructions.json"
DEFAULT_JSON = ROOT / "out" / "benchmark.json"
DEFAULT_DOC = ROOT / "docs" / "performance.md"
REPORT_BEGIN = "<!-- benchmark:begin -->"
REPORT_END = "<!-- benchmark:end -->"


@dataclasses.dataclass(frozen=True)
class Flag:
    """One flag of every C and C++ build of the runners: where it goes (compile, link or both), the machines
    it applies to (empty: every one, as platform.machine() names them), and why."""
    flag: str
    stage: str
    machines: tuple
    reason: str


# The flag table: the one set of flags every runner's C and C++ code is built with, Isoline's and WAMR's alike,
# so that neither gains from a flag the other lacks. The release optimization flags, with LTO off on both and
# nothing aimed at the building machine (no -march=native), so the binaries are what a distribution would
# ship; then Contour's production hardening (cmake/ProductionBuild.cmake at Contour 8c3afaa0c, the rows of a
# Linux build), which Contour's WAMR is built with. _FORTIFY_SOURCE is undefined first, as a toolchain may
# predefine another level.
FLAG_TABLE = (
    Flag("-O2", "compile", (), "the release optimization level distributions build with"),
    Flag("-DNDEBUG", "compile", (), "assertions off, as in a release"),
    Flag("-fno-lto", "both", (), "LTO off on both, so that neither is optimized across its library boundary"),
    Flag("-fPIE", "compile", (), "Contour's hardening: a position-independent executable"),
    Flag("-U_FORTIFY_SOURCE", "compile", (), "Contour's hardening: drop a predefined fortify level first"),
    Flag("-D_FORTIFY_SOURCE=3", "compile", (), "Contour's hardening: checked libc calls"),
    Flag("-fstack-protector-strong", "both", (), "Contour's hardening: stack protector"),
    Flag("-fstack-clash-protection", "both", (), "Contour's hardening: stack clash protection"),
    Flag("-fcf-protection", "both", ("x86_64",), "Contour's hardening: control-flow protection (x86-64)"),
    Flag("-pie", "link", (), "Contour's hardening: a position-independent executable"),
    Flag("-Wl,-z,relro", "link", (), "Contour's hardening: read-only relocations"),
    Flag("-Wl,-z,now", "link", (), "Contour's hardening: bind every symbol at load"),
)


def flagsFor(stage, machine=None):
    """The flag table's flags of a compile or a link on machine (default: this one), in the table's order."""
    machine = machine or platform.machine()
    return [row.flag for row in FLAG_TABLE
            if row.stage in (stage, "both") and (not row.machines or machine in row.machines)]


@dataclasses.dataclass(frozen=True)
class Build:
    """A build tree of the runners: its compilers and where `build` puts it."""
    name: str
    cCompiler: str
    cxxCompiler: str
    directory: pathlib.Path


# The compiler checks CMake runs at configure (try_compile, the ABI detection among them) take the Release
# flags, as the build does: the flag table links with -pie, which an object built without -fPIE (the default
# Debug configuration's flags) cannot link into, and a failed ABI detection leaves the pointer size unset.
TRY_COMPILE_RELEASE = "-DCMAKE_TRY_COMPILE_CONFIGURATION=Release"

# The trees `build` configures with the flag table, one per compiler.
BUILDS = (
    Build("clang", "clang", "clang++", ROOT / "out" / "benchmark-clang"),
    Build("gcc", "gcc", "g++", ROOT / "out" / "benchmark-gcc"),
)


@dataclasses.dataclass(frozen=True)
class Runner:
    """One runner: how its command is formed, and its role in the report.

    kind is isoline, wamr or wasmtime (how the command is formed and its output read); build is the tree its
    binary comes from (empty for the pinned Wasmtime); arguments follow the guest and the iterations. Its
    roles are baseline (wamr-contour, the denominator), best (a candidate for the bar's best dispatch),
    secondary (held to the secondary target) or information. simd says whether it runs the SIMD guest."""
    name: str
    kind: str
    build: str
    arguments: tuple
    roles: tuple
    simd: bool


ISOLINE_BINARY = "benchmarks/isoline-benchmark"
WAMR_BINARY = "benchmarks/wamr/wamr-{variant}/isoline-benchmark-wamr"

# The runner table. Adding a runner is adding a row.
RUNNERS = (
    Runner("isoline-interpreter-tailcall", "isoline", "clang", ("--engine", "interpreter", "--dispatch", "tailcall"),
           ("best",), True),
    Runner("isoline-interpreter-switch", "isoline", "clang", ("--engine", "interpreter", "--dispatch", "switch"),
           ("best", "secondary"), True),
    Runner("isoline-interpreter-switch-gcc", "isoline", "gcc", ("--engine", "interpreter", "--dispatch", "switch"),
           ("secondary",), True),
    Runner("isoline-reference", "isoline", "clang", ("--engine", "reference"), ("information",), True),
    Runner("wamr-contour", "wamr", "clang", ("contour",), ("baseline",), False),
    Runner("wamr-classic", "wamr", "clang", ("classic",), ("information",), False),
    Runner("wamr-default", "wamr", "clang", ("default",), ("information",), False),
    Runner("wasmtime", "wasmtime", "", (), ("information",), True),
)

# The bar (spec §9): the geomean over the shootout guests of the best dispatch over wamr-contour, and of each
# secondary runner over it. Published, never an exit status (the owner's ruling).
BAR = 1.00
SECONDARY_TARGET = 1.25
# The A/A spread a runner's two medians may differ by before the run is repeated.
AA_SPREAD_LIMIT = 0.02
AA_VERDICT_REPEAT = "repeat on a quieter machine"
AA_VERDICT_OK = "ok"
# What each runner's t(N) - t(0) is calibrated to take, in seconds.
TARGET_SECONDS = 1.0
# The iterations every runner runs a guest at once, so that their checksums can be compared (each runner's
# own N differs).
CHECKSUM_ITERATIONS = 3
# The fewest shootout guests the bar is computed over: all of them. A run of fewer (a subset, or a baseline
# that refused some) gets no bar verdict.
BAR_MINIMUM_SHOOTOUT_GUESTS = 19
# Where the calibrated N per guest and runner is committed, for --pinned-n.
PINNED_ITERATIONS = BENCHMARKS / "pinned-iterations.json"
# The calibration's probe grows until t(probe) - t(0) reaches this, in seconds.
CALIBRATION_SECONDS = 0.1
# The exit status of a run whose A/A spread says the machine was not quiet.
EXIT_REPEAT = 3
# The guest the host→guest call round trip runs, at 0 iterations: run(0) does its setup alone.
CALLS_GUEST = "micro-loop.wasm"
# The iterations every guest runs under callgrind, and the spread of its instruction count allowed.
INSTRUCTION_ITERATIONS = 1
INSTRUCTION_TOLERANCE = 0.01
# The runner kinds callgrind counts: Isoline's and WAMR's interpreters. Wasmtime's compiler runs on threads of
# its own and its count is mostly compilation, so it has no row.
INSTRUCTION_KINDS = ("isoline", "wamr")
# The host→guest call row of the instruction counts: run(0) of CALLS_GUEST called CALL_INSTRUCTION_INVOCATIONS
# more times in one process, per call. The call's whole work, not a kernel less its start: an instruction
# count of the round trip itself, so that a cost every call pays (a 64 KiB memset once did) fails --compare.
CALL_INSTRUCTION_ROW = "host-to-guest-call"
CALL_INSTRUCTION_INVOCATIONS = 1000
# Runners of those kinds callgrind cannot run, and why.
INSTRUCTION_EXCLUDED = {
    "wamr-default": "its hardware bounds checks catch SIGSEGV, which valgrind's own handler does not pass on",
}
# The core every timed process is pinned to.
PIN_COMMAND = ("taskset", "-c", "{core}")


class Refused(Exception):
    """A pin does not hold: a guest or a patch is not the one its lock names, or a runner misbehaved."""


# ---- the locks -------------------------------------------------------------------------------------------

def readLock(path):
    """The `key = value` rows of a lock file; '#' starts a comment."""
    rows = {}
    for line in pathlib.Path(path).read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        key, separator, value = line.partition("=")
        if not separator:
            raise Refused(f"{path}: '{line}' is not a `key = value` row")
        rows[key.strip()] = value.strip()
    return rows


@dataclasses.dataclass(frozen=True)
class Guest:
    """A guest of the lock: its file, its group (shootout or micro), its features, whether it uses SIMD."""
    file: str
    sha256: str
    features: str

    @property
    def name(self):
        return self.file.removesuffix(".wasm")

    @property
    def group(self):
        return "shootout" if self.file.startswith("shootout-") else "micro"

    @property
    def simd(self):
        return "simd" in self.features.split(",")


def checkGuests(lock=GUESTS_LOCK, directory=GUESTS):
    """The guests of the lock, each checked against its pinned SHA-256; Refused for one that differs or is
    missing, and for a committed guest the lock does not name."""
    rows = readLock(lock)
    guests = []
    for key, value in rows.items():
        if key.endswith(".features"):
            continue
        features = rows.get(f"{key}.features")
        if features is None:
            raise Refused(f"{lock}: {key} has no features row")
        path = pathlib.Path(directory) / key
        if not path.is_file():
            raise Refused(f"{path} is missing: the lock names it")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != value:
            raise Refused(f"{path} is not the guest {lock} pins: its SHA-256 is {actual}, not {value}; "
                          "rebuild it with benchmarks/build-guests.sh")
        guests.append(Guest(key, value, features))
    named = {guest.file for guest in guests}
    for path in sorted(pathlib.Path(directory).glob("*.wasm")):
        if path.name not in named:
            raise Refused(f"{path} is not in {lock}")
    return guests


def checkWamrPatches(lock=WAMR_PATCHES_LOCK, directory=WAMR_PATCHES):
    """The pinned WAMR patches and the Contour commit they came from; Refused for a patch whose SHA-256 is
    not the pinned one, as ApplyPatches.cmake refuses it."""
    rows = readLock(lock)
    patches = rows.get("patches", "").split()
    if not patches:
        raise Refused(f"{lock} names no patches")
    hashes = {}
    for patch in patches:
        pinned = rows.get(f"sha256.{patch}")
        if pinned is None:
            raise Refused(f"{lock} pins no SHA-256 for {patch}")
        actual = hashlib.sha256((pathlib.Path(directory) / patch).read_bytes()).hexdigest()
        if actual != pinned:
            raise Refused(f"{patch} is not the patch {lock} pins: its SHA-256 is {actual}, not {pinned}")
        hashes[patch] = pinned
    return {"contour_commit": rows.get("contour.commit", ""), "patches": hashes}


# ---- the statistics --------------------------------------------------------------------------------------

def pairedDifferenceMedian(pairs):
    """The median of t(N) - t(0) over the (t(N), t(0)) pairs."""
    return statistics.median(timeN - time0 for timeN, time0 in pairs)


def coldStart(pairs):
    """The median t(0) of the pairs: the cold-start row."""
    return statistics.median(time0 for _, time0 in pairs)


def iterationsFor(perIteration, target=TARGET_SECONDS):
    """The N whose kernel time per iteration perIteration (seconds) reaches target: at least 1."""
    return max(1, math.ceil(round(target / perIteration, 9)))


def aaSpread(first, second):
    """How far apart two medians of the same runner are, relative to their mean."""
    mean = (first + second) / 2
    return abs(first - second) / mean if mean > 0 else 0.0


def aaVerdict(spreads):
    """{runner: (worst spread, verdict)} for {runner: [spread per guest]}: ok below AA_SPREAD_LIMIT, else
    the run is to be repeated."""
    verdicts = {}
    for runner, values in spreads.items():
        worst = max(values) if values else 0.0
        verdicts[runner] = (worst, AA_VERDICT_OK if worst < AA_SPREAD_LIMIT else AA_VERDICT_REPEAT)
    return verdicts


def spreadsOf(kernels, runners):
    """{runner: [A/A spread per guest it ran]} of the kernels of a run."""
    return {r.name: [e["runners"][r.name]["aa_spread"] for e in kernels.values()
                     if r.name in e["runners"] and "aa_spread" in e["runners"][r.name]] for r in runners}


def worstSpread(kernel):
    """The worst A/A spread over the runners of one guest's measurement."""
    return max((entry.get("aa_spread", 0.0) for entry in kernel["runners"].values()), default=0.0)


def mergeRuns(base, reruns):
    """A benchmark.json of @p base with the guests @p reruns (benchmark.json results of the same runners, or
    Refused)
    measured again spliced in: a guest's re-measurement replaces its measurement where its worst A/A spread
    is smaller, in the order given. The A/A verdicts and the bar are computed again over the merged
    kernels, and `merged` records, per guest, which input its numbers come from (0 the base, i the i-th
    re-run)."""
    merged = json.loads(json.dumps(base))
    sources = {guest: 0 for guest in merged["kernels"]}
    for index, rerun in enumerate(reruns, start=1):
        if rerun["runners"] != base["runners"]:
            raise Refused(f"re-run {index} ran other runners than the base: {rerun['runners']}")
        for guest, kernel in rerun["kernels"].items():
            if guest not in merged["kernels"]:
                raise Refused(f"re-run {index} measured {guest}, which the base has no row for")
            if worstSpread(kernel) < worstSpread(merged["kernels"][guest]):
                merged["kernels"][guest] = kernel
                sources[guest] = index
    runners = [runnerNamed(name) for name in merged["runners"]]
    merged["aa"] = aaVerdict(spreadsOf(merged["kernels"], runners))
    merged["bar"] = barVerdict(merged["kernels"], runners)
    merged["merged"] = sources
    return merged


def geomean(ratios):
    """The geometric mean of the ratios."""
    ratios = list(ratios)
    return math.exp(sum(math.log(ratio) for ratio in ratios) / len(ratios))


def runnerNamed(name):
    """The row of the runner named name."""
    for runner in RUNNERS:
        if runner.name == name:
            return runner
    raise Refused(f"unknown runner '{name}'; the runners are {', '.join(r.name for r in RUNNERS)}")


def ratiosOver(kernels, runner, baseline, group="shootout"):
    """{guest: runner's time per iteration / baseline's} over the guests of group both ran; kernels is
    {guest: {"group": …, "runners": {runner: {"per_iteration": seconds}}}}."""
    ratios = {}
    for guest, entry in kernels.items():
        if entry["group"] != group:
            continue
        times = entry["runners"]
        if runner in times and baseline in times:
            ratios[guest] = times[runner]["per_iteration"] / times[baseline]["per_iteration"]
    return ratios


def barVerdict(kernels, runners, minimum=BAR_MINIMUM_SHOOTOUT_GUESTS):
    """The bar and the secondary target over the shootout guests: the geomean of each best-role and
    secondary-role runner over the baseline; the best dispatch is the best-role runner of the lowest geomean.
    Micro-kernels do not enter; a guest the baseline did not run (the SIMD one) has no ratio. A runner with
    ratios over fewer than minimum shootout guests gets no geomean, and the verdict says why ("refused")."""
    baseline = next((r.name for r in runners if "baseline" in r.roles), None)
    result = {"baseline": baseline, "bar": BAR, "secondary_target": SECONDARY_TARGET, "minimum": minimum,
              "guests": {}, "geomeans": {}, "best": None, "met": None, "secondary": {}, "refused": []}
    if baseline is None:
        result["refused"].append("no baseline runner (wamr-contour) ran")
        return result
    for runner in runners:
        if "best" not in runner.roles and "secondary" not in runner.roles:
            continue
        ratios = ratiosOver(kernels, runner.name, baseline)
        result["guests"][runner.name] = len(ratios)
        if len(ratios) < minimum:
            result["refused"].append(f"{runner.name} has ratios over {len(ratios)} shootout guests, fewer than "
                                     f"the {minimum} the bar is computed over")
            continue
        result["geomeans"][runner.name] = geomean(ratios.values())
    candidates = {r.name: result["geomeans"][r.name] for r in runners
                  if "best" in r.roles and r.name in result["geomeans"]}
    if candidates:
        best = min(candidates, key=candidates.get)
        result["best"] = best
        result["met"] = candidates[best] <= BAR
    for runner in runners:
        if "secondary" in runner.roles and runner.name in result["geomeans"]:
            value = result["geomeans"][runner.name]
            result["secondary"][runner.name] = {"geomean": value, "met": value <= SECONDARY_TARGET}
    return result


# ---- running ---------------------------------------------------------------------------------------------

class ProcessExecutor:
    """Runs a runner's command and returns its standard output; the real one, over subprocess."""

    def run(self, command):
        completed = subprocess.run(command, capture_output=True, text=True, check=False)
        if completed.returncode != 0:
            raise Refused(f"{shlex.join(command)} failed ({completed.returncode}): {completed.stderr.strip()}")
        return completed.stdout


def checksumOf(runner, output):
    """The checksum a runner printed, as an unsigned 64-bit number."""
    if runner.kind == "wasmtime":
        lines = [line for line in output.splitlines() if line.strip()]
        if not lines:
            raise Refused(f"{runner.name} printed no result")
        return int(lines[-1].strip()) & ((1 << 64) - 1)
    return int(json.loads(output)["checksum"])


def innerTimes(runner, output):
    """The load and invocation times a runner reported itself, in seconds; nothing for Wasmtime's CLI."""
    if runner.kind == "wasmtime":
        return None
    report = json.loads(output)
    return {"load": report["load_ns"] / 1e9, "invoke": report["invoke_ns"] / 1e9}


class Timer:
    """Times runners' processes with an injected clock, pinned to a core."""

    def __init__(self, commandOf, executor, clock, pin=()):
        self.commandOf = commandOf
        self.executor = executor
        self.clock = clock
        self.pin = tuple(pin)

    def time(self, runner, guest, iterations, invocations=1):
        """(seconds the process took, its standard output)."""
        command = list(self.pin) + self.commandOf(runner, guest, iterations, invocations)
        start = self.clock()
        output = self.executor.run(command)
        return self.clock() - start, output

    def calibrate(self, runner, guest):
        """The kernel's time per iteration: the probe doubles until t(probe) - t(0) reaches
        CALIBRATION_SECONDS."""
        time0, _ = self.time(runner, guest, 0)
        probe = 1
        while True:
            timeProbe, _ = self.time(runner, guest, probe)
            if timeProbe - time0 >= CALIBRATION_SECONDS or probe >= (1 << 30):
                return max(timeProbe - time0, 1e-9) / probe
            probe *= 2

    def pairs(self, runners, guest, iterations, runs):
        """runs rounds of (t(N), t(0)) pairs, each round timing every runner in turn (interleaved, so that a
        change of the machine's state between rounds falls on every runner alike); within a round the even
        rounds time N first, the odd ones 0 first. iterations is {runner: N}. Also each runner's checksum of
        its N, which must be the same every time: {runner: (pairs, checksum)}."""
        pairs = {runner.name: [] for runner in runners}
        checksums = {runner.name: set() for runner in runners}
        for index in range(runs):
            for runner in runners:
                count = iterations[runner.name]
                if index % 2 == 0:
                    timeN, output = self.time(runner, guest, count)
                    time0, _ = self.time(runner, guest, 0)
                else:
                    time0, _ = self.time(runner, guest, 0)
                    timeN, output = self.time(runner, guest, count)
                checksums[runner.name].add(checksumOf(runner, output))
                pairs[runner.name].append((timeN, time0))
        result = {}
        for runner in runners:
            if len(checksums[runner.name]) != 1:
                raise Refused(f"{runner.name} returned different checksums for {guest.name}: "
                              f"{sorted(checksums[runner.name])}")
            result[runner.name] = (pairs[runner.name], checksums[runner.name].pop())
        return result


def measureGuest(timer, runners, guest, runs, iterations=None, aa=True):
    """One guest on every runner that runs it: each runner's N (calibrated to TARGET_SECONDS unless iterations
    gives it: a number for every runner, or {runner: N}), the interleaved pairs, the median, the time per
    iteration, the cold start and the A/A spread; and a check that every runner's checksum at
    CHECKSUM_ITERATIONS agrees."""
    runners = [r for r in runners if r.simd or not guest.simd]
    checksums = {r.name: checksumOf(r, timer.time(r, guest, CHECKSUM_ITERATIONS)[1]) for r in runners}
    if len(set(checksums.values())) > 1:
        raise Refused(f"the runners disagree on {guest.name}'s checksum at {CHECKSUM_ITERATIONS} iterations: "
                      f"{checksums}")
    if iterations is None:
        counts = {r.name: iterationsFor(timer.calibrate(r, guest)) for r in runners}
    elif isinstance(iterations, dict):
        missing = [r.name for r in runners if r.name not in iterations]
        if missing:
            raise Refused(f"no pinned N for {guest.name} on {', '.join(missing)}")
        counts = {r.name: iterations[r.name] for r in runners}
    else:
        counts = {r.name: iterations for r in runners}
    entry = {"group": guest.group, "simd": guest.simd, "checksum": checksums[runners[0].name] if runners else None,
             "runners": {}}
    series = timer.pairs(runners, guest, counts, runs)
    repeat = timer.pairs(runners, guest, counts, runs) if aa else None
    for runner in runners:
        pairs, checksum = series[runner.name]
        median = pairedDifferenceMedian(pairs)
        row = {"iterations": counts[runner.name], "pairs": pairs, "median": median,
               "per_iteration": median / counts[runner.name], "cold_start": coldStart(pairs),
               "checksum": checksum}
        if repeat is not None:
            row["aa_spread"] = aaSpread(median, pairedDifferenceMedian(repeat[runner.name][0]))
        entry["runners"][runner.name] = row
    return entry


def pinnedIterations(kernels):
    """{guest: {runner: N}} of a run's kernels: what --write-pinned-n commits and --pinned-n reads back."""
    return {guest: {runner: row["iterations"] for runner, row in entry["runners"].items()}
            for guest, entry in sorted(kernels.items())}


def commandFactory(trees, wasmtime):
    """How each runner's command is formed, from the build trees ({build: directory}) and Wasmtime's path."""

    def commandOf(runner, guest, iterations, invocations):
        path = str(GUESTS / guest.file)
        if runner.kind == "wasmtime":
            if invocations != 1:
                raise Refused("Wasmtime's command line calls run() once")
            return [str(wasmtime), "run", "-C", "cache=n", "--invoke", "run", path, str(iterations)]
        tree = pathlib.Path(trees[runner.build])
        if runner.kind == "isoline":
            command = [str(tree / ISOLINE_BINARY), path, str(iterations), *runner.arguments]
        else:
            command = [str(tree / WAMR_BINARY.format(variant=runner.arguments[0])), path, str(iterations)]
        if invocations != 1:
            command += ["--invocations", str(invocations)]
        return command

    return commandOf


def compilerVersion(compiler):
    """The first line of compiler --version, or why there is none."""
    try:
        completed = subprocess.run([compiler, "--version"], capture_output=True, text=True, check=False)
    except OSError as error:
        return f"unavailable: {error}"
    return completed.stdout.splitlines()[0] if completed.stdout else f"unavailable: {completed.stderr.strip()}"


def provenance(guests):
    """What benchmark.json records besides the times: the machine, the flag table, every compiler's
    version, the guests' compiler and features, WAMR's source and patches."""
    guestLock = pathlib.Path(GUESTS_LOCK).read_text().splitlines()
    guestCompiler = next((line.split("=", 1)[1].strip() for line in guestLock if line.startswith("# compiler =")), "")
    tools = readLock(TOOLS_LOCK)
    return {
        "machine": {"system": platform.system(), "machine": platform.machine(), "processor": platform.processor()},
        "flags": [dataclasses.asdict(flag) for flag in FLAG_TABLE],
        "flags_used": {"compile": flagsFor("compile"), "link": flagsFor("link")},
        "compilers": {build.name: {"c": compilerVersion(build.cCompiler), "cxx": compilerVersion(build.cxxCompiler)}
                      for build in BUILDS},
        "guest_compiler": guestCompiler,
        "guests": {guest.file: {"sha256": guest.sha256, "features": guest.features} for guest in guests},
        "wamr": {"release": tools.get("wamr.release", ""), "commit": tools.get("wamr.commit", ""),
                 **checkWamrPatches()},
    }


# ---- the report ------------------------------------------------------------------------------------------

def formatSeconds(seconds):
    return f"{seconds * 1e3:.1f} ms"


def formatPerIteration(seconds):
    """A time per iteration, in the unit that keeps three digits."""
    for unit, scale in (("s", 1.0), ("ms", 1e-3), ("µs", 1e-6)):
        if seconds >= scale:
            return f"{seconds / scale:.3g} {unit}"
    return f"{seconds / 1e-9:.3g} ns"


def instructionsMarkdown(instructions, baseline):
    """The instruction-count table of a baseline-instructions.json: every guest's kernel count per runner, and
    its ratio over @p baseline's where that runner counted the guest."""
    runners = sorted(instructions)
    guests = sorted({guest for counts in instructions.values() for guest in counts})
    lines = ["### Instructions", "", "callgrind's instruction count of one run() iteration (1 iteration less 0), "
             f"and its ratio over {baseline}'s: deterministic, whatever the machine's load "
             "(`benchmark.py instructions`).", "",
             "| guest | " + " | ".join(runners) + " |", "|---|" + "---:|" * len(runners)]
    for guest in guests:
        reference = instructions.get(baseline, {}).get(guest)
        cells = []
        for runner in runners:
            count = instructions[runner].get(guest)
            if count is None:
                cells.append("–")
            elif reference and runner != baseline:
                cells.append(f"{count:,} ({count / reference:.2f}×)".replace(",", "'"))
            else:
                cells.append(f"{count:,}".replace(",", "'"))
        lines.append(f"| {guest} | " + " | ".join(cells) + " |")
    return lines + [""]


def reportMarkdown(result, instructions=None):
    """The report of a benchmark.json: the bar, the shootout and micro tables (ratios over wamr-contour), the
    cold-start row, the A/A verdicts, the call round trip, the instruction counts (@p instructions, a
    baseline-instructions.json, where given) and the provenance."""
    kernels = result["kernels"]
    runners = result["runners"]
    gate = result["bar"]
    baseline = gate["baseline"]
    lines = [f"Measured {result.get('date', '')} on {result['provenance']['machine']['machine']}, "
             f"{result['runs']} pairs per runner and guest, by `scripts/benchmark.py run`.", ""]
    if gate["best"] is not None:
        verdict = "met" if gate["met"] else "missed (a follow-up tunes it)"
        lines.append(f"**Bar** (geomean over the {gate['guests'][gate['best']]} shootout guests of the best "
                     f"dispatch over {baseline}, at most {gate['bar']:.2f}): {gate['best']} at "
                     f"{gate['geomeans'][gate['best']]:.4f}, {verdict}.")
    for refusal in gate.get("refused", []):
        lines.append(f"**No bar verdict**: {refusal}.")
    for name, secondary in gate["secondary"].items():
        verdict = "met" if secondary["met"] else "missed"
        lines.append(f"**Secondary target** (over {gate['guests'][name]} shootout guests, at most "
                     f"{gate['secondary_target']:.2f}): {name} at {secondary['geomean']:.4f}, {verdict}.")
    lines += ["", "Each cell is the median time per iteration (the median of t(N) - t(0), over the runner's own N, "
              f"in parentheses), and its ratio over {baseline}'s.", ""]
    for group, title in (("shootout", "Shootout guests"), ("micro", "Micro-kernels")):
        lines += [f"### {title}", "", "| guest | " + " | ".join(runners) + " |", "|---|" + "---:|" * len(runners)]
        for guest, entry in sorted(kernels.items()):
            if entry["group"] != group:
                continue
            cells = []
            for runner in runners:
                row = entry["runners"].get(runner)
                if row is None:
                    cells.append("–")
                    continue
                cell = f"{formatPerIteration(row['per_iteration'])} (N {row['iterations']})"
                if baseline in entry["runners"] and runner != baseline:
                    ratio = row["per_iteration"] / entry["runners"][baseline]["per_iteration"]
                    cell += f", {ratio:.2f}×"
                cells.append(cell)
            lines.append(f"| {guest} | " + " | ".join(cells) + " |")
        lines.append("")
    lines += ["### Cold start", "", "t(0): process start, load, lowering and instantiation, the median over every "
              "guest's pairs.", "", "| runner | t(0) |", "|---|---:|"]
    for runner in runners:
        colds = [entry["runners"][runner]["cold_start"] for entry in kernels.values() if runner in entry["runners"]]
        if colds:
            lines.append(f"| {runner} | {formatSeconds(statistics.median(colds))} |")
    if result.get("aa"):
        lines += ["", "### A/A", "", "| runner | worst spread | verdict |", "|---|---:|---|"]
        for runner, (spread, verdict) in sorted(result["aa"].items()):
            lines.append(f"| {runner} | {spread * 100:.2f} % | {verdict} |")
    else:
        lines += ["", "No A/A run: these numbers are not checked for a quiet machine."]
    if result.get("calls"):
        lines += ["", "### Host → guest call round trip", "", "| runner | per call |", "|---|---:|"]
        for runner, perCall in sorted(result["calls"].items()):
            lines.append(f"| {runner} | {perCall * 1e9:.0f} ns |")
    if instructions:
        lines += [""] + instructionsMarkdown(instructions, baseline)
    flags = " ".join(f"`{flag['flag']}`" for flag in result["provenance"]["flags"])
    lines += ["", f"The flag table of every C and C++ build: {flags}.", ""]
    return "\n".join(lines)


def writeReport(doc, markdown):
    """Replaces what doc holds between REPORT_BEGIN and REPORT_END by markdown."""
    text = pathlib.Path(doc).read_text()
    begin = text.find(REPORT_BEGIN)
    end = text.find(REPORT_END)
    if begin < 0 or end < begin:
        raise Refused(f"{doc} has no {REPORT_BEGIN} … {REPORT_END} block")
    text = text[:begin + len(REPORT_BEGIN)] + "\n" + markdown + "\n" + text[end:]
    pathlib.Path(doc).write_text(text)


# ---- callgrind -------------------------------------------------------------------------------------------

def callgrindInstructions(output):
    """The instruction count of a callgrind output file's summary line."""
    for line in output.splitlines():
        if line.startswith("summary:") or line.startswith("totals:"):
            return int(line.split()[1])
    raise Refused("callgrind wrote no summary line")


def kernelInstructions(atN, at0):
    """The instructions of the kernel alone: a run at INSTRUCTION_ITERATIONS less one at 0, which leaves out
    the process start, the load and the lowering."""
    if atN < at0:
        raise Refused(f"the run at {INSTRUCTION_ITERATIONS} iterations counted fewer instructions ({atN}) than "
                      f"the one at 0 ({at0})")
    return atN - at0


def callInstructions(atMany, atOne, invocations=CALL_INSTRUCTION_INVOCATIONS):
    """The instructions of one host→guest call: a process making invocations + 1 calls less one making one,
    over the invocations; rounded to a whole instruction."""
    if atMany < atOne:
        raise Refused(f"{invocations + 1} calls counted fewer instructions ({atMany}) than one ({atOne})")
    return round((atMany - atOne) / invocations)


def compareInstructions(baseline, measured, tolerance=INSTRUCTION_TOLERANCE):
    """The kernels whose count moved by more than tolerance from the baseline, and those the baseline lacks:
    [(runner, guest, baseline count or None, measured count)]."""
    moved = []
    for runner, guests in measured.items():
        for guest, count in guests.items():
            before = baseline.get(runner, {}).get(guest)
            if before is None or abs(count - before) > tolerance * before:
                moved.append((runner, guest, before, count))
    return moved


# ---- the commands ----------------------------------------------------------------------------------------

def treesOf(arguments):
    """{build: directory}: BUILDS' directories, with --tree BUILD=DIR overriding them."""
    trees = {build.name: build.directory for build in BUILDS}
    for override in arguments.tree or ():
        name, separator, directory = override.partition("=")
        if not separator or name not in trees:
            raise Refused(f"--tree takes BUILD=DIR with BUILD one of {', '.join(trees)}, not '{override}'")
        trees[name] = pathlib.Path(directory).resolve()
    return trees


def wasmtimePath():
    """The pinned Wasmtime, fetched (once) by scripts/fetch-tools.py."""
    completed = subprocess.run([sys.executable, "-I", str(SCRIPTS / "fetch-tools.py"), "--tool", "wasmtime"],
                               capture_output=True, text=True, check=True)
    return pathlib.Path(completed.stdout.strip().splitlines()[-1])


def selected(arguments, guests):
    runners = [runnerNamed(name) for name in arguments.runners.split(",")] if arguments.runners else list(RUNNERS)
    if arguments.guests:
        wanted = set(arguments.guests.split(","))
        guests = [guest for guest in guests if guest.name in wanted or guest.file in wanted]
    return runners, guests


def commandRun(arguments):
    guests = checkGuests()
    runners, guests = selected(arguments, guests)
    wasmtime = wasmtimePath() if any(r.kind == "wasmtime" for r in runners) else None
    timer = Timer(commandFactory(treesOf(arguments), wasmtime), ProcessExecutor(), time.monotonic,
                  [part.format(core=arguments.core) for part in PIN_COMMAND])
    pinned = json.loads(pathlib.Path(arguments.pinned_n).read_text()) if arguments.pinned_n else None
    kernels = {}
    for guest in guests:
        print(f"{guest.name} …", file=sys.stderr)
        if pinned is not None and guest.name not in pinned:
            raise Refused(f"{arguments.pinned_n} pins no N for {guest.name}")
        iterations = pinned[guest.name] if pinned is not None else arguments.iterations
        kernels[guest.name] = measureGuest(timer, runners, guest, arguments.runs, iterations, not arguments.no_aa)
    aa = aaVerdict(spreadsOf(kernels, runners)) if not arguments.no_aa else {}
    result = {"schema": 1, "date": time.strftime("%Y-%m-%d"), "runs": arguments.runs,
              "runners": [r.name for r in runners], "kernels": kernels, "aa": aa,
              "bar": barVerdict(kernels, runners), "provenance": provenance(guests)}
    pathlib.Path(arguments.json).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(arguments.json).write_text(json.dumps(result, indent=2) + "\n")
    print(f"wrote {arguments.json}", file=sys.stderr)
    if arguments.write_pinned_n:
        pathlib.Path(arguments.write_pinned_n).write_text(json.dumps(pinnedIterations(kernels), indent=2) + "\n")
        print(f"wrote {arguments.write_pinned_n}", file=sys.stderr)
    if any(verdict == AA_VERDICT_REPEAT for _, verdict in aa.values()):
        print(f"the A/A spread is {AA_SPREAD_LIMIT * 100:.0f} % or more: {AA_VERDICT_REPEAT}", file=sys.stderr)
        return EXIT_REPEAT
    return 0


def commandCalls(arguments):
    """The host→guest call round trip: run(0) of CALLS_GUEST, --invocations times in one process, by the
    runner's own clock; per call, in seconds. Wasmtime's command line calls once, so it has no row."""
    guests = {guest.file: guest for guest in checkGuests()}
    runners, _ = selected(arguments, [])
    commandOf = commandFactory(treesOf(arguments), None)
    executor = ProcessExecutor()
    calls = {}
    for runner in runners:
        if runner.kind == "wasmtime":
            continue
        output = executor.run(commandOf(runner, guests[CALLS_GUEST], 0, arguments.invocations))
        calls[runner.name] = innerTimes(runner, output)["invoke"] / arguments.invocations
        print(f"{runner.name}: {calls[runner.name] * 1e9:.0f} ns per call")
    if arguments.json and pathlib.Path(arguments.json).is_file():
        result = json.loads(pathlib.Path(arguments.json).read_text())
        result["calls"] = calls
        pathlib.Path(arguments.json).write_text(json.dumps(result, indent=2) + "\n")
    return 0


def commandInstructions(arguments):
    """callgrind on every runner of INSTRUCTION_KINDS and guest at INSTRUCTION_ITERATIONS less at 0: --update
    writes the baseline, --compare fails on a kernel that moved by more than the tolerance."""
    guests = checkGuests()
    guestsByFile = {guest.file: guest for guest in guests}
    runners, guests = selected(arguments, guests)
    commandOf = commandFactory(treesOf(arguments), None)
    measured = {}
    scratch = ROOT / "out" / "benchmark-callgrind"
    scratch.mkdir(parents=True, exist_ok=True)
    for runner in runners:
        if runner.kind not in INSTRUCTION_KINDS or runner.name in INSTRUCTION_EXCLUDED:
            continue
        for guest in [g for g in guests if runner.simd or not g.simd]:
            counts = {}
            for iterations in (INSTRUCTION_ITERATIONS, 0):
                out = scratch / f"{runner.name}.{guest.name}.{iterations}.callgrind"
                command = ["valgrind", "--tool=callgrind", f"--callgrind-out-file={out}",
                           *commandOf(runner, guest, iterations, 1)]
                subprocess.run(command, capture_output=True, check=True)
                counts[iterations] = callgrindInstructions(out.read_text())
            measured.setdefault(runner.name, {})[guest.name] = kernelInstructions(counts[INSTRUCTION_ITERATIONS],
                                                                                  counts[0])
        calls = {}
        for invocations in (CALL_INSTRUCTION_INVOCATIONS + 1, 1):
            out = scratch / f"{runner.name}.{CALL_INSTRUCTION_ROW}.{invocations}.callgrind"
            command = ["valgrind", "--tool=callgrind", f"--callgrind-out-file={out}",
                       *commandOf(runner, guestsByFile[CALLS_GUEST], 0, invocations)]
            subprocess.run(command, capture_output=True, check=True)
            calls[invocations] = callgrindInstructions(out.read_text())
        measured[runner.name][CALL_INSTRUCTION_ROW] = callInstructions(calls[CALL_INSTRUCTION_INVOCATIONS + 1],
                                                                       calls[1])
    if arguments.update:
        INSTRUCTIONS_BASELINE.write_text(json.dumps(measured, indent=2, sort_keys=True) + "\n")
        print(f"wrote {INSTRUCTIONS_BASELINE}")
        return 0
    baseline = json.loads(INSTRUCTIONS_BASELINE.read_text()) if INSTRUCTIONS_BASELINE.is_file() else {}
    moved = compareInstructions(baseline, measured)
    for runner, guest, before, count in moved:
        print(f"{runner} {guest}: {before} -> {count} instructions")
    return 1 if moved else 0


def commandMerge(arguments):
    """Splices re-measured guests into a run (mergeRuns()) and writes the merged benchmark.json."""
    base = json.loads(pathlib.Path(arguments.base).read_text())
    reruns = [json.loads(pathlib.Path(path).read_text()) for path in arguments.reruns]
    merged = mergeRuns(base, reruns)
    pathlib.Path(arguments.json).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(arguments.json).write_text(json.dumps(merged, indent=2) + "\n")
    print(f"wrote {arguments.json}")
    return 0


def commandReport(arguments):
    result = json.loads(pathlib.Path(arguments.json).read_text())
    instructions = json.loads(INSTRUCTIONS_BASELINE.read_text()) if INSTRUCTIONS_BASELINE.is_file() else None
    writeReport(arguments.doc, reportMarkdown(result, instructions))
    print(f"wrote {arguments.doc}")
    return 0


def commandBuild(arguments):
    """Configures and builds every tree of BUILDS with the flag table: ISOLINE_BENCHMARKS on, no tests."""
    compileFlags = " ".join(flagsFor("compile"))
    linkFlags = " ".join(flagsFor("link"))
    for build in BUILDS:
        configure = ["cmake", "-S", str(ROOT), "-B", str(build.directory), "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=Release", TRY_COMPILE_RELEASE, f"-DCMAKE_C_COMPILER={build.cCompiler}",
                     f"-DCMAKE_CXX_COMPILER={build.cxxCompiler}", f"-DCMAKE_C_FLAGS_RELEASE={compileFlags}",
                     f"-DCMAKE_CXX_FLAGS_RELEASE={compileFlags}", f"-DCMAKE_EXE_LINKER_FLAGS={linkFlags}",
                     "-DISOLINE_BENCHMARKS=ON", "-DISOLINE_TESTING=OFF",
                     f"-DISOLINE_BENCHMARK_WAMR_JOBS={arguments.jobs}"]
        if arguments.pace_scheduler:
            configure.append(f"-DPACE_SCHEDULER={arguments.pace_scheduler}")
        if arguments.wamr_archive:
            configure.append(f"-DISOLINE_BENCHMARK_WAMR_ARCHIVE={arguments.wamr_archive}")
        subprocess.run(configure, check=True)
        targets = ["isoline-benchmark", "isoline-benchmark-simd-lanes", "wamr-contour", "wamr-classic", "wamr-default"]
        subprocess.run(["cmake", "--build", str(build.directory), "-j", str(arguments.jobs), "--target", *targets],
                       check=True)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    build = commands.add_parser("build", help="configure and build the runners' trees with the flag table")
    build.add_argument("--jobs", type=int, default=2)
    build.add_argument("--pace-scheduler", default="")
    build.add_argument("--wamr-archive", default="")

    def common(command):
        command.add_argument("--runners", default="")
        command.add_argument("--guests", default="")
        command.add_argument("--tree", action="append", help="BUILD=DIR: take BUILD's binaries from DIR")

    run = commands.add_parser("run", help="time the runners")
    common(run)
    run.add_argument("--runs", type=int, default=10)
    run.add_argument("--core", type=int, default=2)
    pinning = run.add_mutually_exclusive_group()
    pinning.add_argument("--iterations", type=int, default=None, help="N for every guest and runner (a smoke run)")
    pinning.add_argument("--pinned-n", default="", help=f"take each (guest, runner)'s N from this file, as "
                         f"--write-pinned-n wrote it (the committed one is {PINNED_ITERATIONS.relative_to(ROOT)})")
    run.add_argument("--write-pinned-n", default="", help="write the N of every (guest, runner) to this file")
    run.add_argument("--no-aa", action="store_true", help="skip the A/A run")
    run.add_argument("--json", default=str(DEFAULT_JSON))

    calls = commands.add_parser("calls", help="the host→guest call round trip per runner")
    common(calls)
    calls.add_argument("--invocations", type=int, default=100_000)
    calls.add_argument("--json", default=str(DEFAULT_JSON), help="adds the rows to this benchmark.json if it exists")

    instructions = commands.add_parser("instructions", help="callgrind instruction counts of isoline-benchmark")
    common(instructions)
    mode = instructions.add_mutually_exclusive_group(required=True)
    mode.add_argument("--update", action="store_true")
    mode.add_argument("--compare", action="store_true")

    merge = commands.add_parser("merge", help="splice re-measured guests into a run, its bar and A/A again")
    merge.add_argument("base", help="the run's benchmark.json")
    merge.add_argument("reruns", nargs="+", help="benchmark.json files of guests measured again, in order")
    merge.add_argument("--json", default=str(DEFAULT_JSON), help="where the merged benchmark.json goes")

    report = commands.add_parser("report", help="write docs/performance.md from a benchmark.json")
    report.add_argument("--json", default=str(DEFAULT_JSON))
    report.add_argument("--doc", default=str(DEFAULT_DOC))

    arguments = parser.parse_args(argv)
    handlers = {"build": commandBuild, "run": commandRun, "calls": commandCalls,
                "instructions": commandInstructions, "merge": commandMerge, "report": commandReport}
    try:
        return handlers[arguments.command](arguments)
    except Refused as refusal:
        print(f"benchmark.py: {refusal}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
