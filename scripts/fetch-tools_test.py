#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of fetch-tools.py, offline: fixture archives built in a temporary directory stand in for
the releases, and a lock file names them by file:// URL."""

import errno
import hashlib
import importlib.util
import io
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
import zipfile
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parent
SCRIPT = SCRIPTS / "fetch-tools.py"


def loadFetchTools():
    """Loads fetch-tools.py by path: its name is not an importable module name."""
    spec = importlib.util.spec_from_file_location("fetch_tools", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fetchTools = loadFetchTools()

# The fixture tools, one per archive shape the real lock pins: a versioned directory in a tarball
# (wasm-tools, CMake), a bare executable in a zip (Ninja), and a Windows zip.
TOOLS = {
    "nested": ("nested-{version}.tar.gz", "nested-{version}/bin/nested"),
    "flat": ("flat.zip", "flat"),
}
PLATFORM = "x86_64-linux"
WINDOWS = "x86_64-windows"
# Windows derives a file's mode from its name, so the executable bit is checked on POSIX hosts only.
HAS_EXECUTABLE_BIT = os.name == "posix"


def tarball(path, member, payload):
    with tarfile.open(path, "w:gz") as bundle:
        info = tarfile.TarInfo(member)
        info.size = len(payload)
        info.mode = 0o644
        bundle.addfile(info, io.BytesIO(payload))


def zipped(path, member, payload):
    with zipfile.ZipFile(path, "w") as bundle:
        bundle.writestr(member, payload)


class FetchToolsTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        self.releases = self.root / "releases"
        self.releases.mkdir()
        self.cache = self.root / "cache"
        self.lock = self.root / "tools.lock"
        self.digests = {}
        for tool, (archive, executable) in TOOLS.items():
            name = archive.format(version="1.0")
            member = executable.format(version="1.0")
            payload = f"#!/bin/sh\necho {tool}\n".encode()
            (tarball if name.endswith(".tar.gz") else zipped)(self.releases / name, member, payload)
            self.digests[tool] = hashlib.sha256((self.releases / name).read_bytes()).hexdigest()
        zipped(self.releases / "flat-win.zip", "flat.exe", b"MZ")
        self.digests["flat-win"] = hashlib.sha256((self.releases / "flat-win.zip").read_bytes()).hexdigest()
        self.writeLock()

    def writeLock(self, overrides=None):
        """Writes a lock pinning every fixture tool; overrides replaces rows by key."""
        rows = {}
        for tool, (archive, executable) in TOOLS.items():
            rows[f"{tool}.version"] = "1.0"
            rows[f"{tool}.url"] = self.releases.as_uri() + "/{archive}"
            rows[f"{tool}.archive.{PLATFORM}"] = archive
            rows[f"{tool}.executable.{PLATFORM}"] = executable
            rows[f"{tool}.sha256.{PLATFORM}"] = self.digests[tool]
        rows[f"flat.archive.{WINDOWS}"] = "flat-win.zip"
        rows[f"flat.executable.{WINDOWS}"] = "flat.exe"
        rows[f"flat.sha256.{WINDOWS}"] = self.digests["flat-win"]
        rows.update(overrides or {})
        self.lock.write_text("# fixture\n" + "".join(f"{key} = {value}\n" for key, value in rows.items()),
                             encoding="utf-8")

    def fetch(self, tool, name=PLATFORM):
        return fetchTools.ensureTool(tool, self.lock, self.cache, name)

    def test_aNestedTarballIsVerifiedAndExtracted(self):
        executable = self.fetch("nested")
        self.assertEqual(executable, self.cache / f"nested-1.0-{PLATFORM}" / "nested-1.0" / "bin" / "nested")
        if HAS_EXECUTABLE_BIT:
            self.assertTrue(executable.stat().st_mode & 0o111, "the executable is made executable")

    def test_aFlatZipIsVerifiedAndExtracted(self):
        executable = self.fetch("flat")
        self.assertEqual(executable, self.cache / f"flat-1.0-{PLATFORM}" / "flat")
        if HAS_EXECUTABLE_BIT:
            self.assertTrue(executable.stat().st_mode & 0o111, "a zip loses the mode; it is restored")

    def test_thePinnedVersionIsTheLocksRow(self):
        self.assertEqual(fetchTools.pinnedVersion("nested", self.lock), "1.0")

    def test_aVersionMatchesItselfAndItsReleasesOnly(self):
        cases = [("22.1.8", "22", True), ("22.1.8", "22.1.8", True), ("1.11.1", "1.11.1", True),
                 ("1.13.2", "1.11.1", False), ("2.1", "22", False), ("22.1.8", "22.1", True),
                 ("19.1.0", "19.1.01", False), ("4.3.0", "4.3", True), ("14.2.0", "4.3", False),
                 ("22.1.8", "2", False), ("1.110.0", "1.11", False)]
        for actual, expected, matches in cases:
            with self.subTest(actual=actual, expected=expected):
                self.assertEqual(fetchTools.versionMatches(actual, expected), matches)

    def test_theVersionLineIsReported(self):
        # (tool, stdout, stderr) -> (version, suffix): the first line the tool's row recognizes, stdout first.
        cases = [
            ("cmake", "cmake version 4.3.0\n\nCMake suite maintained", "", ("4.3.0", None)),
            ("ninja", "1.11.1\n", "", ("1.11.1", None)),
            ("llvm", "Ubuntu clang version 22.1.8 (++2026)", "", ("22.1.8", None)),
            ("llvm", "clang version 22.1.8 (Fedora 22.1.8-1.fc44)", "", ("22.1.8", None)),
            ("llvm", "LLVM (http://llvm.org/):\n  LLVM version 22.1.8\n  Optimized build.", "", ("22.1.8", None)),
            ("gcc", "g++ (Ubuntu 14.2.0-4ubuntu2) 14.2.0\nCopyright (C) 2024", "", ("14.2.0", None)),
            ("gcc", "g++ (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2)", "", ("16.2.1", None)),
            ("msvc", "", "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35217 for x64", ("19.44.35217", None)),
            # A diagnostic before the version line, with a dotted number of its own, is not it.
            ("msvc", "", "0024:err:winediag:nodrv_CreateWindow libwine 9.0 has no driver\n"
                     "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35217 for x64", ("19.44.35217", None)),
            ("llvm", "ccache 4.10.2 wraps it\nclang version 19.1.0", "", ("19.1.0", None)),
            ("cmake", "cmake version 4.3.0", "warning: libuv 1.48.0 is old", ("4.3.0", None)),
            # Another tool's version line is not this tool's: ninja's bare version is no CMake line.
            ("cmake", "1.13.2\n", "", None),
            ("cmake", "cmake version 4.3.0-rc1", "", ("4.3.0", "-rc1")),
            ("ninja", "1.13.0.git\n", "", ("1.13.0", ".git")),
            ("llvm", "clang version 19.1.0rc3", "", ("19.1.0", "rc3")),
            ("gcc", "g++ (GCC) 16.0.0 20260101 (experimental)", "", ("16.0.0", " (experimental)")),
            ("gcc", "g++ (GCC) 15.0.1 20250301 (prerelease)", "", ("15.0.1", " (prerelease)")),
            ("cmake", "no version here", "nor 1.2.3 a version line", None),
            ("wasmtime", "wasmtime 50.0.3 (9bbe3e2f8 2026-11-19)", "", ("50.0.3", None)),
            ("wasmtime", "wasmtime 51.0.0-dev", "", ("51.0.0", "-dev")),
            ("llvm-mc", "LLVM (http://llvm.org/):\n  LLVM version 22.1.8\n  Optimized build.", "", ("22.1.8", None)),
            ("llvm-mc", "LLVM (http://llvm.org/):\n  LLVM version 23.0.0git\n", "", ("23.0.0", "git")),
        ]
        for tool, stdout, stderr, reported in cases:
            with self.subTest(tool=tool, stdout=stdout, stderr=stderr):
                self.assertEqual(fetchTools.reportedVersion(tool, stdout, stderr), reported)

    def test_aToolWithoutARowIsRefused(self):
        with self.assertRaises(SystemExit) as failure:
            fetchTools.reportedVersion("no-such-tool", "1.0")
        self.assertIn("no version line row for the tool no-such-tool", str(failure.exception))

    def checkVersionOf(self, tool, expected, stdout="", stderr="", returncode=0):
        """Runs checkVersion for tool against one that prints stdout and stderr and exits with returncode."""
        completed = subprocess.CompletedProcess(["tool", "--version"], returncode, stdout, stderr)
        with mock.patch.object(fetchTools.subprocess, "run", return_value=completed):
            return fetchTools.checkVersion("tool", tool, expected)

    def test_aToolFailingToReportFailsTheCheck(self):
        self.assertEqual(self.checkVersionOf("cmake", "4.3.0", "cmake version 4.3.0\n"), "4.3.0")
        with self.assertRaises(SystemExit) as failure:
            self.checkVersionOf("cmake", "4.3.0", "cmake version 4.3.0\n", "a broken install", returncode=1)
        self.assertIn("tool: exits with 1 when asked its version", str(failure.exception))

    def test_aPreReleaseOrDevelopmentBuildFailsTheCheck(self):
        for tool, stdout, expected in [("cmake", "cmake version 4.3.0-rc1", "4.3.0"), ("ninja", "1.13.0.git", "1.13.0"),
                                       ("llvm", "clang version 19.1.0-rc3 (https://github.com/llvm/llvm-project)",
                                        "19.1.0"),
                                       ("llvm", "clang version 19.1.0rc3", "19.1.0"),
                                       ("cmake", "cmake version 4.3.20260101-gabc1234", "4.3"),
                                       ("gcc", "g++ (GCC) 16.0.0 20260101 (experimental)", "16")]:
            with self.subTest(stdout=stdout):
                with self.assertRaises(SystemExit) as failure:
                    self.checkVersionOf(tool, expected, stdout)
                self.assertIn("a pre-release or development build", str(failure.exception))
        self.assertEqual(self.checkVersionOf("llvm", "22", "Ubuntu clang version 22.1.8 (++20260101+1~exp1)"), "22.1.8")

    def test_aNumberOutsideTheVersionLineIsNeverTheVersion(self):
        # The combined output's first dotted number is the diagnostic's 9.0; the version line says 19.44.
        diagnostic = "0024:err:winediag: libwine 9.0 has no driver\n"
        banner = "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35217 for x64\n"
        self.assertEqual(self.checkVersionOf("msvc", "19.44", stderr=diagnostic + banner), "19.44.35217")
        with self.assertRaises(SystemExit) as failure:
            self.checkVersionOf("msvc", "9.0", stderr=diagnostic + banner)
        self.assertIn("tool is 19.44.35217, but its row pins 9.0", str(failure.exception))
        with self.assertRaises(SystemExit) as failure:
            self.checkVersionOf("msvc", "9.0", stderr=diagnostic)
        self.assertIn("tool: prints no version line of msvc", str(failure.exception))

    def test_anExecutableOfAnotherVersionFailsTheCheck(self):
        # The interpreter running the test is an executable of a known version: `-V` prints it.
        running = f"{sys.version_info.major}.{sys.version_info.minor}"
        self.assertTrue(fetchTools.checkVersion(sys.executable, "python", running, "-V").startswith(running + "."))
        with self.assertRaises(SystemExit) as failure:
            fetchTools.checkVersion(sys.executable, "python", "2.7", "-V")
        self.assertIn(f"but its row pins 2.7", str(failure.exception))

    def test_aMissingExecutableFailsTheCheck(self):
        with self.assertRaises(SystemExit) as failure:
            fetchTools.checkVersion(os.fspath(self.root / "no-such-tool"), "cmake", "1.0")
        self.assertIn("cannot run it", str(failure.exception))

    def test_aPlatformPicksItsOwnRows(self):
        executable = self.fetch("flat", WINDOWS)
        self.assertEqual(executable, self.cache / f"flat-1.0-{WINDOWS}" / "flat.exe")
        self.assertEqual(executable.read_bytes(), b"MZ")

    def test_aCorruptedDigestFailsTheFetchAndKeepsNothing(self):
        self.writeLock({f"nested.sha256.{PLATFORM}": "0" * 64})
        with self.assertRaises(SystemExit) as failure:
            self.fetch("nested")
        self.assertIn("does not match the pinned", str(failure.exception.code))
        self.assertFalse((self.cache / f"nested-1.0-{PLATFORM}").exists(), "nothing is extracted")
        self.assertEqual(list((self.cache / "downloads").rglob("*.tar.gz")), [], "the download is removed")
        self.assertEqual(list(self.cache.rglob("*.part")), [], "no partial file is left")

    def test_aFailedDownloadLeavesNoPartialFile(self):
        (self.releases / "nested-1.0.tar.gz").unlink()
        with self.assertRaises(OSError):
            self.fetch("nested")
        # The lock file stays: removing it would let a run lock a file another has just replaced.
        left = [p for p in self.cache.rglob("*") if p.is_file() and p.name != fetchTools.LOCK_NAME]
        self.assertEqual(left, [], "nothing but the lock is left behind")

    def test_aCachedArchiveIsNotDownloadedAgain(self):
        self.fetch("nested")
        (self.releases / "nested-1.0.tar.gz").unlink()
        self.assertTrue(self.fetch("nested").exists())

    def test_aCorruptedCachedArchiveIsDownloadedAgain(self):
        self.fetch("nested")
        archive = next((self.cache / "downloads").rglob("nested-1.0.tar.gz"))
        archive.write_bytes(b"corrupt")
        executable = self.cache / f"nested-1.0-{PLATFORM}" / "nested-1.0" / "bin" / "nested"
        executable.unlink()
        self.assertTrue(self.fetch("nested").exists())
        self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), self.digests["nested"])

    def test_aCompleteTreeIsKept(self):
        executable = self.fetch("nested")
        executable.write_text("kept", encoding="utf-8")
        self.assertEqual(self.fetch("nested").read_text(encoding="utf-8"), "kept", "a marked tree is used as it is")

    def test_aTreeWithoutTheMarkerIsReplaced(self):
        # An interrupted extraction, or a tree from before the marker: the executable may even exist.
        extracted = self.cache / f"flat-1.0-{PLATFORM}"
        extracted.mkdir(parents=True)
        (extracted / "flat").write_text("stale", encoding="utf-8")
        (extracted / "leftover").write_text("stale", encoding="utf-8")
        executable = self.fetch("flat")
        self.assertEqual(executable.read_text(encoding="utf-8"), "#!/bin/sh\necho flat\n")
        self.assertFalse((extracted / "leftover").exists(), "the incomplete tree is replaced, not merged into")
        self.assertTrue((extracted / fetchTools.COMPLETION_MARKER).is_file())

    def test_aMarkerOfAnotherArchiveIsIncomplete(self):
        extracted = self.cache / f"flat-1.0-{PLATFORM}"
        extracted.mkdir(parents=True)
        (extracted / "flat").write_text("stale", encoding="utf-8")
        (extracted / fetchTools.COMPLETION_MARKER).write_text("0" * 64, encoding="utf-8")
        self.assertEqual(self.fetch("flat").read_text(encoding="utf-8"), "#!/bin/sh\necho flat\n")

    def test_theMarkerIsInTheTreeBeforeItIsRenamedIntoPlace(self):
        renamed = []
        rename = pathlib.Path.rename

        def checkedRename(source, target):
            renamed.append((source / fetchTools.COMPLETION_MARKER).is_file())
            return rename(source, target)

        with mock.patch.object(pathlib.Path, "rename", checkedRename):
            self.fetch("flat")
        self.assertEqual(renamed, [True], "the one rename moves a tree that carries its marker")

    def test_aRunWaitsForTheRunHoldingTheLock(self):
        lockPath = self.cache / "downloads" / f"flat-1.0-{PLATFORM}" / fetchTools.LOCK_NAME
        lockPath.parent.mkdir(parents=True)
        holder = fetchTools.ToolLock(lockPath, timeout=5)
        holder.__enter__()
        # The holder's run finishes while this one waits: it then sees the holder's complete tree.
        release = threading.Timer(0.3, holder.__exit__, (None, None, None))
        release.start()
        self.addCleanup(release.join)
        started = time.monotonic()
        self.assertTrue(self.fetch("flat").exists())
        self.assertGreaterEqual(time.monotonic() - started, 0.25, "the fetch waited for the lock")

    def test_aLockHeldPastTheTimeoutFailsNamingIt(self):
        lockPath = self.cache / "downloads" / f"flat-1.0-{PLATFORM}" / fetchTools.LOCK_NAME
        lockPath.parent.mkdir(parents=True)
        with fetchTools.ToolLock(lockPath, timeout=5), mock.patch.object(fetchTools, "LOCK_TIMEOUT", 0.3):
            with self.assertRaises(SystemExit) as failure:
                self.fetch("flat")
        self.assertIn(f"another run holds {lockPath}", str(failure.exception.code))
        self.assertFalse((self.cache / f"flat-1.0-{PLATFORM}").exists(), "nothing is touched without the lock")

    def test_aLockErrorOtherThanHeldFailsAtOnce(self):
        # ENOLCK (or EOPNOTSUPP, on NFS) says the file cannot be locked at all: waiting would not help.
        lockPath = self.cache / "downloads" / f"flat-1.0-{PLATFORM}" / fetchTools.LOCK_NAME
        lockPath.parent.mkdir(parents=True)
        refusal = OSError(errno.ENOLCK, "No locks available")
        with mock.patch.object(fetchTools, "tryLockFile", side_effect=refusal) as tryLock, \
                mock.patch.object(fetchTools, "LOCK_TIMEOUT", 5):
            started = time.monotonic()
            with self.assertRaises(OSError) as failure:
                self.fetch("flat")
        self.assertIs(failure.exception, refusal, "the refusal is raised as it is")
        self.assertEqual(tryLock.call_count, 1, "a lock that cannot be taken is not retried")
        self.assertLess(time.monotonic() - started, 1, "the fetch does not wait out the timeout")

    def test_everyHeldErrorOfThePlatformIsWaitedFor(self):
        # flock(2)'s "held" errors on POSIX, msvcrt's on Windows; nothing else is waited for.
        # Each platform's set is spelled only there: errno.EDEADLOCK exists on Windows, not on macOS.
        if os.name == "nt":
            held = {errno.EDEADLOCK, errno.EACCES}
        else:
            held = {errno.EAGAIN, errno.EWOULDBLOCK, errno.EACCES}
        self.assertEqual(fetchTools.LOCK_HELD_ERRORS, held)
        lockPath = self.root / "held.lock"
        for code in sorted(fetchTools.LOCK_HELD_ERRORS):
            attempts = iter([OSError(code, os.strerror(code)), None])

            def tryLock(stream):
                attempt = next(attempts)
                if attempt is not None:
                    raise attempt

            with self.subTest(errno=errno.errorcode.get(code, code)), \
                    mock.patch.object(fetchTools, "tryLockFile", tryLock), \
                    mock.patch.object(fetchTools, "unlockFile"):
                with fetchTools.ToolLock(lockPath, timeout=5):
                    pass

    def test_staleStagingDirectoriesAreSweptUnderTheLock(self):
        # A run killed while extracting leaves its staging directory, and one killed while downloading its
        # partial file; the next run of the same tool removes both. Another tool's are not its to touch.
        stem = f"flat-1.0-{PLATFORM}"
        self.cache.mkdir()
        staging = self.cache / f"{stem}.k1ll3d.part"
        staging.mkdir()
        (staging / "flat").write_text("half", encoding="utf-8")
        downloads = self.cache / "downloads" / stem
        downloads.mkdir(parents=True)
        partial = downloads / "flat.zip.k1ll3d.part"
        partial.write_bytes(b"half")
        other = self.cache / f"nested-1.0-{PLATFORM}.k1ll3d.part"
        other.mkdir()
        self.assertTrue(self.fetch("flat").exists())
        self.assertFalse(staging.exists(), "the stale staging directory is removed")
        self.assertFalse(partial.exists(), "the stale partial download is removed")
        self.assertTrue(other.exists(), "another tool's staging directory is left to its own runs")

    def test_theSweepRunsUnderTheLock(self):
        stem = f"flat-1.0-{PLATFORM}"
        self.cache.mkdir()
        (self.cache / f"{stem}.k1ll3d.part").mkdir()
        held = []
        sweep = fetchTools.sweepStaleStaging

        def checkedSweep(*arguments):
            lockPath = self.cache / "downloads" / stem / fetchTools.LOCK_NAME
            with self.assertRaises(SystemExit), mock.patch.object(fetchTools, "LOCK_POLL_INTERVAL", 0.01):
                with fetchTools.ToolLock(lockPath, timeout=0.05):
                    pass
            held.append(True)
            return sweep(*arguments)

        with mock.patch.object(fetchTools, "sweepStaleStaging", checkedSweep):
            self.fetch("flat")
        self.assertEqual(held, [True], "the sweep ran while this run held the tool's lock")

    def test_concurrentRunsShareOneCompleteTree(self):
        command = [sys.executable, "-I", SCRIPT, "--tool", "nested", "--lock", self.lock, "--cache", self.cache,
                   "--platform", PLATFORM]
        runs = [subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                for _ in range(4)]
        for run in runs:
            output, errors = run.communicate(timeout=120)
            self.assertEqual(run.returncode, 0, errors)
            self.assertTrue(pathlib.Path(output.strip()).is_file())
        extracted = self.cache / f"nested-1.0-{PLATFORM}"
        self.assertEqual((extracted / fetchTools.COMPLETION_MARKER).read_text(encoding="utf-8"),
                         self.digests["nested"])
        self.assertEqual([p.name for p in self.cache.iterdir() if p.name != "downloads"], [extracted.name],
                         "no staging copy is left")

    def test_anArchiveWithoutTheExecutableFails(self):
        self.writeLock({f"flat.executable.{PLATFORM}": "missing"})
        with self.assertRaises(SystemExit) as failure:
            self.fetch("flat")
        self.assertIn("no missing at", str(failure.exception.code))

    def test_aMissingRowIsNamed(self):
        with self.assertRaises(SystemExit) as failure:
            self.fetch("flat", "aarch64-macos")
        self.assertIn("no row `flat.archive.aarch64-macos`", str(failure.exception.code))

    def writeBundle(self, entries):
        """Writes the tarball bundle-1.0.tar.xz of entries ((name, payload), payload None for a symlink to
        the first entry) and pins it as the tool `bundle`, its members bundle-1.0/bin/tool alone."""
        archive = self.releases / "bundle-1.0.tar.xz"
        with tarfile.open(archive, "w:xz") as bundle:
            for name, payload in entries:
                info = tarfile.TarInfo(name)
                if payload is None:
                    info.type = tarfile.SYMTYPE
                    info.linkname = entries[0][0].rsplit("/", 1)[1]
                    bundle.addfile(info)
                else:
                    info.size = len(payload)
                    info.mode = 0o755
                    bundle.addfile(info, io.BytesIO(payload))
        self.writeLock({
            "bundle.version": "1.0",
            "bundle.url": self.releases.as_uri() + "/{archive}",
            f"bundle.archive.{PLATFORM}": "bundle-{version}.tar.xz",
            f"bundle.executable.{PLATFORM}": "bundle-{version}/bin/tool",
            f"bundle.members.{PLATFORM}": "bundle-{version}/bin/tool",
            f"bundle.sha256.{PLATFORM}": hashlib.sha256(archive.read_bytes()).hexdigest(),
        })

    def test_onlyTheMembersAreExtracted(self):
        self.writeBundle([("bundle-1.0/bin/other", b"other"), ("bundle-1.0/bin/tool", b"tool"),
                          ("bundle-1.0/lib/libbig.a", b"big")])
        executable = self.fetch("bundle")
        self.assertEqual(executable.read_bytes(), b"tool")
        tree = executable.parents[2]
        self.assertEqual(sorted(path.relative_to(tree).as_posix() for path in tree.rglob("*") if path.is_file()),
                         [fetchTools.COMPLETION_MARKER, "bundle-1.0/bin/tool"])

    def test_aMissingMemberFailsNamingIt(self):
        self.writeBundle([("bundle-1.0/bin/other", b"other")])
        with self.assertRaises(SystemExit) as failure:
            self.fetch("bundle")
        self.assertIn("no member bundle-1.0/bin/tool", str(failure.exception.code))
        self.assertFalse((self.cache / f"bundle-1.0-{PLATFORM}").exists(), "no tree is left")

    def test_aMemberThatIsNoRegularFileIsRefused(self):
        self.writeBundle([("bundle-1.0/bin/other", b"other"), ("bundle-1.0/bin/tool", None)])
        with self.assertRaises(SystemExit) as failure:
            self.fetch("bundle")
        self.assertIn("is not a regular file", str(failure.exception.code))

    def test_aMemberOutsideTheTreeIsRefused(self):
        self.writeBundle([("bundle-1.0/bin/tool", b"tool")])
        self.writeLock({**fetchTools.readLock(self.lock), f"bundle.members.{PLATFORM}": "../escape"})
        with self.assertRaises(SystemExit) as failure:
            self.fetch("bundle")
        self.assertIn("escapes the extraction directory", str(failure.exception.code))

    def test_theCachedToolIsFoundWithoutADownload(self):
        self.assertIsNone(fetchTools.cachedTool("nested", self.lock, self.cache, PLATFORM), "nothing is cached yet")
        self.assertFalse(self.cache.exists(), "asking downloads nothing")
        executable = self.fetch("nested")
        self.assertEqual(fetchTools.cachedTool("nested", self.lock, self.cache, PLATFORM), executable)
        self.assertIsNone(fetchTools.cachedTool("nested", self.lock, self.cache, WINDOWS), "no row, no tool")
        (executable.parents[2] / fetchTools.COMPLETION_MARKER).unlink()
        self.assertIsNone(fetchTools.cachedTool("nested", self.lock, self.cache, PLATFORM), "an incomplete tree")

    def test_theCachedCommandLinePrintsNothingWithoutTheTool(self):
        command = [sys.executable, "-I", SCRIPT, "--tool", "nested", "--lock", self.lock, "--cache", self.cache,
                   "--platform", PLATFORM, "--cached"]
        before = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual((before.returncode, before.stdout), (0, ""), before.stderr)
        executable = self.fetch("nested")
        after = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual((after.returncode, after.stdout.strip()), (0, os.fspath(executable)), after.stderr)

    def checkLibrariesOf(self, stdout, returncode=0, stderr=""):
        """Runs checkLibraries against an ldd that prints stdout and stderr and exits with returncode."""
        completed = subprocess.CompletedProcess(["ldd", "tool"], returncode, stdout, stderr)
        with mock.patch.object(fetchTools.subprocess, "run", return_value=completed):
            return fetchTools.checkLibraries("tool")

    def test_onlySystemLibrariesPassTheLibraryCheck(self):
        system = ("\tlinux-vdso.so.1 (0x00007f122583a000)\n"
                  "\tlibm.so.6 => /lib64/libm.so.6 (0x00007f1222ce9000)\n"
                  "\tlibz.so.1 => /lib/x86_64-linux-gnu/libz.so.1 (0x00007f12257e1000)\n"
                  "\tlibstdc++.so.6 => /usr/lib/x86_64-linux-gnu/libstdc++.so.6 (0x00007f1222a00000)\n"
                  "\t/lib64/ld-linux-x86-64.so.2 (0x00007f122583c000)\n")
        self.assertEqual(len(self.checkLibrariesOf(system)), 4)
        self.assertEqual(self.checkLibrariesOf("", 1, "\tnot a dynamic executable\n"), [])
        for line, named in [("\tlibLLVM.so.22 => /opt/llvm/lib/libLLVM.so.22 (0x1)\n", "/opt/llvm/lib/libLLVM.so.22"),
                            ("\tlibxml2.so.2 => not found\n", "libxml2.so.2 from not found"),
                            ("\tlibz.so.1 => /home/me/lib/libz.so.1 (0x1)\n", "/home/me/lib/libz.so.1")]:
            with self.subTest(line=line):
                with self.assertRaises(SystemExit) as failure:
                    self.checkLibrariesOf(system + line)
                self.assertIn(named, str(failure.exception))
        with self.assertRaises(SystemExit) as failure:
            self.checkLibrariesOf("", 1, "ldd: tool: No such file or directory\n")
        self.assertIn("exits with 1", str(failure.exception))

    def test_theCommandLinePrintsTheExecutable(self):
        result = subprocess.run([sys.executable, "-I", SCRIPT, "--tool", "nested", "--lock", self.lock,
                                 "--cache", self.cache, "--platform", PLATFORM],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(pathlib.Path(result.stdout.strip()).name, "nested")


class LockTest(unittest.TestCase):
    """Every tool the real lock pins has its rows for each platform it names."""

    def test_everyPinnedPlatformIsComplete(self):
        lock = fetchTools.readLock(fetchTools.DEFAULT_LOCK)
        tools = {key.removesuffix(".version") for key in lock if key.endswith(".version")}
        self.assertLessEqual({"wasm-tools", "cmake", "ninja", "wasmtime"}, tools)
        for tool in tools:
            self.assertIn(f"{tool}.url", lock)
            platforms = {key.rsplit(".", 1)[1] for key in lock if key.startswith(f"{tool}.sha256.")}
            self.assertTrue(platforms, tool)
            for name in platforms:
                for column in ("archive", "executable"):
                    self.assertIn(f"{tool}.{column}.{name}", lock)
                self.assertRegex(lock[f"{tool}.sha256.{name}"], r"^[0-9a-f]{64}$")

    def test_theEncodingOraclesLlvmMcIsPinnedAlone(self):
        # Only bin/llvm-mc of the LLVM release is extracted, and it is the executable the lock names.
        lock = fetchTools.readLock(fetchTools.DEFAULT_LOCK)
        self.assertRegex(lock["llvm-mc.version"], r"^\d+\.\d+\.\d+$")
        self.assertEqual(lock["llvm-mc.members.x86_64-linux"], lock["llvm-mc.executable.x86_64-linux"])
        self.assertTrue(lock["llvm-mc.executable.x86_64-linux"].endswith("/bin/llvm-mc"))

    def test_theWamrSourceIsPinned(self):
        # A source archive, not a tool: a release, the commit it is, and the archive's URL and digest.
        lock = fetchTools.readLock(fetchTools.DEFAULT_LOCK)
        self.assertNotIn("wamr.version", lock)
        self.assertRegex(lock["wamr.release"], r"^\d+\.\d+\.\d+$")
        self.assertRegex(lock["wamr.commit"], r"^[0-9a-f]{40}$")
        self.assertIn("{commit}", lock["wamr.source.url"])
        self.assertRegex(lock["wamr.source.sha256"], r"^[0-9a-f]{64}$")


if __name__ == "__main__":
    unittest.main()
