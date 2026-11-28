#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of benchmark.py: its statistics, the bar, the A/A gate, the pins and the report, with fake
runners on a fake clock, so that nothing is timed and nothing is built."""

import hashlib
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "benchmark.py"
_spec = importlib.util.spec_from_file_location("benchmark", SCRIPT)
benchmark = importlib.util.module_from_spec(_spec)
sys.modules["benchmark"] = benchmark
_spec.loader.exec_module(benchmark)


class FakeClock:
    """A monotonic clock that moves only when a fake runner runs."""

    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class FakeExecutor:
    """Runs a command by advancing the clock: startup seconds, plus perIteration seconds per iteration of the
    runner the command names (its first word), and prints the checksum JSON isoline-benchmark prints."""

    def __init__(self, clock, perIteration, startup=0.05, noise=None):
        self.clock = clock
        self.perIteration = perIteration
        self.startup = startup
        self.noise = noise or (lambda runner, call: 0.0)
        self.calls = {}

    def run(self, command):
        runner, iterations = command[0], int(command[2])
        call = self.calls.get(runner, 0)
        self.calls[runner] = call + 1
        self.clock.now += self.startup + self.perIteration[runner] * iterations + self.noise(runner, call)
        return json.dumps({"checksum": 42 + 0 * iterations, "load_ns": 1, "invoke_ns": 2})


def fakeCommandOf(runner, guest, iterations, invocations):
    return [runner.name, guest.file, str(iterations)]


def guest(file, features="mvp,bulk-memory"):
    return benchmark.Guest(file, "0" * 64, features)


def kernelsOf(ratios, baseline="wamr-contour", runner="isoline-interpreter-tailcall", group="shootout"):
    """Kernels whose runner takes ratio times the baseline's time per iteration."""
    return {f"k{index}": {"group": group, "runners": {baseline: {"per_iteration": 1e-3},
                                                       runner: {"per_iteration": ratio * 1e-3}}}
            for index, ratio in enumerate(ratios)}


class BenchmarkTest(unittest.TestCase):
    def test_paired_difference_median(self):
        # The fake clock gives each pair t(N) - t(0) exactly the kernel time; noise on one pair moves the mean
        # but not the median.
        clock = FakeClock()
        runner = benchmark.runnerNamed("wamr-contour")
        noise = lambda name, call: 0.5 if call == 4 else 0.0  # the third pair's t(N)
        executor = FakeExecutor(clock, {"wamr-contour": 0.01}, noise=noise)
        timer = benchmark.Timer(fakeCommandOf, executor, clock)
        pairs, checksum = timer.pairs([runner], guest("shootout-fib2.wasm"), {"wamr-contour": 100}, 5)["wamr-contour"]
        self.assertEqual(len(pairs), 5)
        self.assertEqual(checksum, 42)
        self.assertAlmostEqual(benchmark.pairedDifferenceMedian(pairs), 1.0)
        self.assertAlmostEqual(benchmark.pairedDifferenceMedian([(3.0, 1.0), (2.5, 1.0), (9.0, 1.0)]), 2.0)

    def test_runners_interleave_within_each_round(self):
        # Every round times each runner's pair in turn; the even rounds N first, the odd ones 0 first.
        clock = FakeClock()
        calls = []
        recorder = FakeExecutor(clock, {})
        recorder.run = lambda command: calls.append((command[0], int(command[2]))) or json.dumps({"checksum": 1})
        runners = [benchmark.runnerNamed("isoline-reference"), benchmark.runnerNamed("wamr-contour")]
        benchmark.Timer(fakeCommandOf, recorder, clock).pairs(runners, guest("g.wasm"),
                                                              {"isoline-reference": 7, "wamr-contour": 9}, 2)
        self.assertEqual(calls, [("isoline-reference", 7), ("isoline-reference", 0), ("wamr-contour", 9),
                                 ("wamr-contour", 0), ("isoline-reference", 0), ("isoline-reference", 7),
                                 ("wamr-contour", 0), ("wamr-contour", 9)])

    def test_iterations_reach_one_second(self):
        # Each runner is calibrated on its own: 2 ms per iteration takes N = 500, 4 ms N = 250, and both report
        # their time per iteration, so that the two compare whatever their N.
        clock = FakeClock()
        executor = FakeExecutor(clock, {"isoline-interpreter-tailcall": 0.002, "wamr-contour": 0.004})
        timer = benchmark.Timer(fakeCommandOf, executor, clock)
        runners = [benchmark.runnerNamed("isoline-interpreter-tailcall"), benchmark.runnerNamed("wamr-contour")]
        entry = benchmark.measureGuest(timer, runners, guest("shootout-fib2.wasm"), runs=2, aa=False)
        self.assertEqual(entry["runners"]["isoline-interpreter-tailcall"]["iterations"], 500)
        self.assertEqual(entry["runners"]["wamr-contour"]["iterations"], 250)
        self.assertAlmostEqual(entry["runners"]["isoline-interpreter-tailcall"]["per_iteration"], 0.002)
        self.assertAlmostEqual(entry["runners"]["wamr-contour"]["per_iteration"], 0.004)
        self.assertEqual(benchmark.iterationsFor(0.002), 500)
        self.assertEqual(benchmark.iterationsFor(0.003), 334)
        self.assertEqual(benchmark.iterationsFor(5.0), 1)

    def test_pinned_iterations(self):
        # A run pinned to a file takes each (guest, runner)'s N from it, refuses a runner it has none for, and
        # writes back exactly what it ran.
        clock = FakeClock()
        executor = FakeExecutor(clock, {"isoline-reference": 0.002, "wamr-contour": 0.001})
        timer = benchmark.Timer(fakeCommandOf, executor, clock)
        runners = [benchmark.runnerNamed("isoline-reference"), benchmark.runnerNamed("wamr-contour")]
        pinned = {"isoline-reference": 40, "wamr-contour": 80}
        entry = benchmark.measureGuest(timer, runners, guest("shootout-fib2.wasm"), runs=1, iterations=pinned,
                                       aa=False)
        self.assertEqual({name: row["iterations"] for name, row in entry["runners"].items()}, pinned)
        self.assertEqual(benchmark.pinnedIterations({"shootout-fib2": entry}), {"shootout-fib2": pinned})
        with self.assertRaisesRegex(benchmark.Refused, "no pinned N for shootout-fib2 on wamr-contour"):
            benchmark.measureGuest(timer, runners, guest("shootout-fib2.wasm"), runs=1,
                                   iterations={"isoline-reference": 40}, aa=False)

    def test_aa_spread_gate(self):
        for runner in benchmark.RUNNERS:
            with self.subTest(runner=runner.name):
                verdicts = benchmark.aaVerdict({runner.name: [0.001, 0.015]})
                self.assertEqual(verdicts[runner.name][1], benchmark.AA_VERDICT_OK)
                verdicts = benchmark.aaVerdict({runner.name: [0.001, 0.025]})
                self.assertEqual(verdicts[runner.name], (0.025, "repeat on a quieter machine"))
        self.assertAlmostEqual(benchmark.aaSpread(1.0, 1.015), 0.015 / 1.0075)
        # The A/A run is a second series of pairs of the same runner: a runner whose second series is 2.5 %
        # slower is caught there, wamr-contour included. Call 0 is the checksum run, 1 to 4 the first series.
        clock = FakeClock()
        noise = lambda name, call: 0.025 if call in (5, 8) else 0.0  # the A/A series' t(N)s
        executor = FakeExecutor(clock, {"wamr-contour": 0.001}, startup=0.0, noise=noise)
        timer = benchmark.Timer(fakeCommandOf, executor, clock)
        entry = benchmark.measureGuest(timer, [benchmark.runnerNamed("wamr-contour")], guest("g.wasm"), runs=2,
                                       iterations=1000, aa=True)
        self.assertGreater(entry["runners"]["wamr-contour"]["aa_spread"], benchmark.AA_SPREAD_LIMIT)

    def test_gate_uses_shootout_only(self):
        kernels = kernelsOf([0.9, 0.9])
        kernels.update({f"m{index}": entry for index, entry in enumerate(kernelsOf([5.0, 9.0], group="micro").values())})
        # The SIMD guest: no WAMR column, so no ratio, however slow the interpreter is.
        kernels["simd"] = {"group": "micro", "runners": {"isoline-interpreter-tailcall": {"median": 50.0}}}
        runners = [benchmark.runnerNamed("isoline-interpreter-tailcall"), benchmark.runnerNamed("wamr-contour")]
        verdict = benchmark.barVerdict(kernels, runners, minimum=2)
        self.assertAlmostEqual(verdict["geomeans"]["isoline-interpreter-tailcall"], 0.9)
        self.assertEqual(verdict["guests"]["isoline-interpreter-tailcall"], 2)
        self.assertTrue(verdict["met"])
        self.assertEqual(set(benchmark.ratiosOver(kernels, "isoline-interpreter-tailcall", "wamr-contour")),
                         {"k0", "k1"})
        self.assertFalse(benchmark.runnerNamed("wamr-contour").simd)
        self.assertFalse(benchmark.runnerNamed("wamr-default").simd)
        # measureGuest leaves the WAMR runners out of the SIMD guest.
        clock = FakeClock()
        executor = FakeExecutor(clock, {"isoline-interpreter-tailcall": 0.001, "wamr-contour": 0.001})
        timer = benchmark.Timer(fakeCommandOf, executor, clock)
        entry = benchmark.measureGuest(timer, runners, guest("micro-simd.wasm", "mvp,simd"), runs=1,
                                       iterations=1, aa=False)
        self.assertEqual(list(entry["runners"]), ["isoline-interpreter-tailcall"])

    def test_geomean_bar(self):
        # The cube root of 0.8 × 1.1 × 1.0 = 0.88.
        self.assertAlmostEqual(benchmark.geomean([0.8, 1.1, 1.0]), 0.9583, places=4)
        kernels = kernelsOf([0.8, 1.1, 1.0])
        for name, entry in kernelsOf([1.1, 1.3, 1.2], runner="isoline-interpreter-switch").items():
            kernels[name]["runners"].update({k: v for k, v in entry["runners"].items() if k != "wamr-contour"})
        for name, entry in kernelsOf([1.3, 1.3, 1.3], runner="isoline-interpreter-switch-gcc").items():
            kernels[name]["runners"].update({k: v for k, v in entry["runners"].items() if k != "wamr-contour"})
        verdict = benchmark.barVerdict(kernels, list(benchmark.RUNNERS), minimum=3)
        self.assertEqual(verdict["best"], "isoline-interpreter-tailcall")
        self.assertAlmostEqual(verdict["geomeans"]["isoline-interpreter-tailcall"], 0.9583, places=4)
        self.assertTrue(verdict["met"])
        self.assertEqual(set(verdict["secondary"]), {"isoline-interpreter-switch", "isoline-interpreter-switch-gcc"})
        self.assertTrue(verdict["secondary"]["isoline-interpreter-switch"]["met"])
        self.assertFalse(verdict["secondary"]["isoline-interpreter-switch-gcc"]["met"])
        self.assertFalse(benchmark.barVerdict(kernelsOf([1.2, 1.1]), list(benchmark.RUNNERS), minimum=2)["met"])

    def test_bar_refuses_too_few_shootout_guests(self):
        self.assertEqual(benchmark.BAR_MINIMUM_SHOOTOUT_GUESTS, 19)
        runners = [benchmark.runnerNamed("isoline-interpreter-tailcall"), benchmark.runnerNamed("wamr-contour")]
        verdict = benchmark.barVerdict(kernelsOf([0.5] * 18), runners)
        self.assertIsNone(verdict["met"])
        self.assertEqual(verdict["geomeans"], {})
        self.assertEqual(verdict["refused"], ["isoline-interpreter-tailcall has ratios over 18 shootout guests, "
                                              "fewer than the 19 the bar is computed over"])
        verdict = benchmark.barVerdict(kernelsOf([0.5] * 19), runners)
        self.assertTrue(verdict["met"])
        result = {"runs": 1, "runners": [r.name for r in runners], "kernels": kernelsOf([0.5] * 19), "aa": {},
                  "bar": verdict, "provenance": {"machine": {"machine": "x86_64"}, "flags": []}}
        for entry in result["kernels"].values():
            for row in entry["runners"].values():
                row.update({"iterations": 10, "cold_start": 0.01})
        self.assertIn("**Bar** (geomean over the 19 shootout guests", benchmark.reportMarkdown(result))
        result["bar"] = benchmark.barVerdict(kernelsOf([0.5] * 18), runners)
        self.assertIn("**No bar verdict**: isoline-interpreter-tailcall has ratios over 18",
                      benchmark.reportMarkdown(result))

    def test_cold_start_row(self):
        clock = FakeClock()
        executor = FakeExecutor(clock, {"isoline-reference": 0.001, "wamr-contour": 0.001}, startup=0.07)
        timer = benchmark.Timer(fakeCommandOf, executor, clock)
        runners = [benchmark.runnerNamed("isoline-reference"), benchmark.runnerNamed("wamr-contour")]
        entry = benchmark.measureGuest(timer, runners, guest("shootout-sieve.wasm"), runs=3, iterations=10, aa=False)
        for runner in ("isoline-reference", "wamr-contour"):
            self.assertAlmostEqual(entry["runners"][runner]["cold_start"], 0.07)
        result = {"runs": 3, "runners": [r.name for r in runners], "kernels": {"shootout-sieve": entry},
                  "aa": {}, "bar": benchmark.barVerdict({"shootout-sieve": entry}, runners),
                  "provenance": {"machine": {"machine": "x86_64"}, "flags": [{"flag": "-O2", "reason": ""}]}}
        markdown = benchmark.reportMarkdown(result)
        self.assertIn("### Cold start", markdown)
        self.assertIn("| isoline-reference | 70.0 ms |", markdown)
        self.assertIn("| wamr-contour | 70.0 ms |", markdown)

    def test_guest_lock_is_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = pathlib.Path(directory)
            (directory / "a.wasm").write_bytes(b"\0asm")
            digest = hashlib.sha256(b"\0asm").hexdigest()
            lock = directory / "guests.lock"
            lock.write_text(f"# pins\na.wasm = {digest}\na.wasm.features = mvp\n")
            self.assertEqual([g.file for g in benchmark.checkGuests(lock, directory)], ["a.wasm"])
            (directory / "a.wasm").write_bytes(b"\0asm\1")
            with self.assertRaisesRegex(benchmark.Refused, "is not the guest"):
                benchmark.checkGuests(lock, directory)
            (directory / "a.wasm").write_bytes(b"\0asm")
            (directory / "b.wasm").write_bytes(b"\0asm")
            with self.assertRaisesRegex(benchmark.Refused, "is not in"):
                benchmark.checkGuests(lock, directory)
            (directory / "b.wasm").unlink()
            lock.write_text(f"a.wasm = {digest}\n")
            with self.assertRaisesRegex(benchmark.Refused, "no features row"):
                benchmark.checkGuests(lock, directory)
        # The committed guests hold to their lock.
        self.assertEqual(len(benchmark.checkGuests()), 25)

    def test_wamr_patches_are_pinned(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = pathlib.Path(directory)
            # Bytes, not text: a text write on Windows ends the line with CRLF, and the hash is of the bytes.
            (directory / "one.patch").write_bytes(b"diff\n")
            digest = hashlib.sha256(b"diff\n").hexdigest()
            lock = directory / "patches.lock"
            lock.write_text(f"contour.commit = abc\npatches = one.patch\nsha256.one.patch = {digest}\n")
            self.assertEqual(benchmark.checkWamrPatches(lock, directory),
                             {"contour_commit": "abc", "patches": {"one.patch": digest}})
            (directory / "one.patch").write_bytes(b"diff changed\n")
            with self.assertRaisesRegex(benchmark.Refused, "is not the patch"):
                benchmark.checkWamrPatches(lock, directory)
            lock.write_text("patches = one.patch\n")
            with self.assertRaisesRegex(benchmark.Refused, "pins no SHA-256"):
                benchmark.checkWamrPatches(lock, directory)
        # The committed patches hold to their lock.
        self.assertEqual(len(benchmark.checkWamrPatches()["patches"]), 2)

    def test_flag_table_is_recorded(self):
        recorded = benchmark.provenance(benchmark.checkGuests())
        self.assertEqual([row["flag"] for row in recorded["flags"]], [flag.flag for flag in benchmark.FLAG_TABLE])
        for flag in ("-O2", "-fno-lto", "-fcf-protection", "-D_FORTIFY_SOURCE=3", "-fstack-clash-protection",
                     "-fstack-protector-strong", "-fPIE", "-pie", "-Wl,-z,relro", "-Wl,-z,now"):
            self.assertIn(flag, [row["flag"] for row in recorded["flags"]])
        self.assertNotIn("node", recorded["machine"])
        compile = benchmark.flagsFor("compile", "x86_64")
        self.assertLess(compile.index("-U_FORTIFY_SOURCE"), compile.index("-D_FORTIFY_SOURCE=3"))
        self.assertIn("-fcf-protection", compile)
        self.assertNotIn("-fcf-protection", benchmark.flagsFor("compile", "aarch64"))
        self.assertNotIn("-pie", compile)
        self.assertIn("-pie", benchmark.flagsFor("link", "x86_64"))
        self.assertNotIn("-D_FORTIFY_SOURCE=3", benchmark.flagsFor("link", "x86_64"))
        self.assertNotIn("-march=native", [row["flag"] for row in recorded["flags"]])
        self.assertEqual(set(recorded["compilers"]), {build.name for build in benchmark.BUILDS})
        for versions in recorded["compilers"].values():
            self.assertEqual(set(versions), {"c", "cxx"})
        self.assertIn("wasi-sdk", recorded["guest_compiler"])
        self.assertEqual(recorded["wamr"]["commit"], "25bd7eb63e828e4bd242cc9b38d260b4b31c6605")
        self.assertEqual(len(recorded["guests"]), 25)

    def test_checksums_must_agree(self):
        clock = FakeClock()
        executor = FakeExecutor(clock, {"isoline-reference": 0.001, "wamr-contour": 0.001})
        original = executor.run

        def lying(command):
            output = json.loads(original(command))
            if command[0] == "wamr-contour":
                output["checksum"] = 7
            return json.dumps(output)

        executor.run = lying
        timer = benchmark.Timer(fakeCommandOf, executor, clock)
        runners = [benchmark.runnerNamed("isoline-reference"), benchmark.runnerNamed("wamr-contour")]
        with self.assertRaisesRegex(benchmark.Refused, "disagree"):
            benchmark.measureGuest(timer, runners, guest("g.wasm"), runs=1, iterations=1, aa=False)

    def test_wasmtime_checksum_is_unsigned(self):
        runner = benchmark.runnerNamed("wasmtime")
        self.assertEqual(benchmark.checksumOf(runner, "warning: experimental\n-1\n"), (1 << 64) - 1)
        self.assertEqual(benchmark.checksumOf(benchmark.runnerNamed("wamr-contour"), '{"checksum": 5}'), 5)

    def test_report_replaces_its_block(self):
        with tempfile.TemporaryDirectory() as directory:
            doc = pathlib.Path(directory) / "performance.md"
            doc.write_text(f"# Performance\n\nbefore\n{benchmark.REPORT_BEGIN}\nold\n{benchmark.REPORT_END}\nafter\n")
            benchmark.writeReport(doc, "new")
            self.assertEqual(doc.read_text(), f"# Performance\n\nbefore\n{benchmark.REPORT_BEGIN}\nnew\n"
                                              f"{benchmark.REPORT_END}\nafter\n")
            doc.write_text("no markers\n")
            with self.assertRaises(benchmark.Refused):
                benchmark.writeReport(doc, "new")

    def test_instruction_tolerance(self):
        baseline = {"isoline-reference": {"a": 1000, "b": 1000}}
        measured = {"isoline-reference": {"a": 1010, "b": 1011, "c": 5}}
        self.assertEqual(benchmark.compareInstructions(baseline, measured),
                         [("isoline-reference", "b", 1000, 1011), ("isoline-reference", "c", None, 5)])
        self.assertEqual(benchmark.callgrindInstructions("events: Ir\nsummary: 12345\n"), 12345)
        # The kernel's count leaves out what a run at 0 iterations counts: start, load, lowering.
        self.assertEqual(benchmark.kernelInstructions(15000, 12000), 3000)
        with self.assertRaises(benchmark.Refused):
            benchmark.kernelInstructions(100, 200)

    def test_instruction_table(self):
        instructions = {"wamr-contour": {"a.wasm": 200, "b.wasm": 100}, "isoline-interpreter-tailcall": {"a.wasm": 100}}
        lines = benchmark.instructionsMarkdown(instructions, "wamr-contour")
        self.assertIn("| a.wasm | 100 (0.50×) | 200 |", lines)
        self.assertIn("| b.wasm | – | 100 |", lines)

    def test_runner_commands(self):
        commandOf = benchmark.commandFactory({"clang": pathlib.Path("/c"), "gcc": pathlib.Path("/g")},
                                             pathlib.Path("/w/wasmtime"))
        g = guest("shootout-fib2.wasm")
        path = str(benchmark.GUESTS / "shootout-fib2.wasm")
        # The binaries' paths as this platform spells them (backslashes on Windows).
        isoline = str(pathlib.Path("/g") / "benchmarks/isoline-benchmark")
        wamr = str(pathlib.Path("/c") / "benchmarks/wamr/wamr-contour/isoline-benchmark-wamr")
        self.assertEqual(commandOf(benchmark.runnerNamed("isoline-interpreter-switch-gcc"), g, 5, 1),
                         [isoline, path, "5", "--engine", "interpreter", "--dispatch", "switch"])
        self.assertEqual(commandOf(benchmark.runnerNamed("wamr-contour"), g, 5, 3),
                         [wamr, path, "5", "--invocations", "3"])
        self.assertEqual(commandOf(benchmark.runnerNamed("wasmtime"), g, 5, 1),
                         [str(pathlib.Path("/w/wasmtime")), "run", "-C", "cache=n", "--invoke", "run", path, "5"])
        with self.assertRaises(benchmark.Refused):
            commandOf(benchmark.runnerNamed("wasmtime"), g, 0, 10)


if __name__ == "__main__":
    unittest.main()
