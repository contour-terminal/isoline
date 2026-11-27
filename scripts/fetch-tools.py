#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Downloads a prebuilt tool pinned in scripts/tools.lock and verifies it.

Each archive is downloaded into its own directory under the cache, checked against the SHA-256 the
lock file records for this platform, and only then extracted. A build never runs this script; it
serves the maintainer tooling that produces committed test inputs (scripts/import-spec-tests.py) and
the CI lanes that install a pinned CMake, Ninja or LLVM.

Every tool is a set of lock rows, so pinning another one is adding rows, not code:
  <tool>.version                the release
  <tool>.url                    its download URL, a template of {version} and {archive}
  <tool>.archive.<platform>     the release asset for a platform, a template of {version}
  <tool>.executable.<platform>  the executable's path inside that archive, a template of {version}
  <tool>.sha256.<platform>      the asset's SHA-256

Concurrent runs may share a cache (parallel worktrees do). A run of one tool holds an exclusive lock on
downloads/<stem>/.lock from its first look at the cache to its last, so runs of the same tool, version
and platform take turns and none removes or replaces a tree another is using; runs of other tools do
not wait. The lock is advisory: it orders runs of this script only. A tree is complete when it carries
the completion marker, which holds the archive's SHA-256 and is written into the staging directory
before that is renamed into place; a tree without it, or with another archive's, was left by an
interrupted run (or an older script) and is replaced under the lock. Downloads go to a temporary name
and are renamed into place too, so a run killed mid-download leaves no file that looks finished; the next
run of the tool sweeps such staging directories and partial files away under the lock.

Usage: python3 -I scripts/fetch-tools.py [--tool <name>] [--lock <tools.lock>] [--cache <dir>]
                                         [--platform <platform>]
Prints the path of the verified executable (by default wasm-tools').

       python3 -I scripts/fetch-tools.py --tool <name> --print-version
Prints the tool's pinned version, fetching nothing.

       python3 -I scripts/fetch-tools.py --check-version <executable> --tool <name> [--expected <version>]
                                         [--version-argument <argument>]
Runs the executable with its version argument (default --version; empty for none, as cl.exe takes) and
fails unless it exits with 0 and its version line (the first line, of standard output and then of
standard error, that the tool's VERSION_LINES row recognizes; --tool names the row, also for a tool the
lock does not pin: gcc, msvc, python) names the expected version (default: the tool's pinned version) or
one of its releases (expected 22 accepts 22.1.8), with no pre-release or development suffix (4.3.0-rc1,
1.13.0.git, a GCC snapshot's "(experimental)"). A CI lane runs it on every tool its row pins, so a runner whose PATH holds another
one fails instead of building with it.
"""

import argparse
import contextlib
import errno
import hashlib
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile

DEFAULT_LOCK = pathlib.Path(__file__).resolve().parent / "tools.lock"
DEFAULT_CACHE = pathlib.Path.home() / ".cache" / "isoline-tools"
DEFAULT_TOOL = "wasm-tools"

# (sys.platform prefix, machine names) -> the platform name the lock file's rows end with.
PLATFORMS = [
    ("linux", {"x86_64", "amd64"}, "x86_64-linux"),
    ("linux", {"aarch64", "arm64"}, "aarch64-linux"),
    ("darwin", {"arm64", "aarch64"}, "aarch64-macos"),
    ("win32", {"amd64", "x86_64"}, "x86_64-windows"),
]


# Seconds a download may stall (connecting, or between reads) before it fails instead of hanging.
DOWNLOAD_TIMEOUT = 60
# Seconds a run waits for another run of the same tool to release the cache, before it fails naming the
# lock. A run holds it for one download and one extraction.
LOCK_TIMEOUT = 1800
# Seconds between two attempts to take a held lock.
LOCK_POLL_INTERVAL = 0.1
# The lock file, in the tool's download directory.
LOCK_NAME = ".lock"
# The file a complete extracted tree carries at its root: the SHA-256 of the archive it came from.
COMPLETION_MARKER = ".fetch-tools-complete"

# How to take and release a non-blocking exclusive lock on an open file, per platform: fcntl on POSIX,
# msvcrt (a one-byte region at offset 0) on Windows. Each tries once and raises OSError when it fails, and
# LOCK_HELD_ERRORS are the errno values that mean another holder has it: flock(2) reports EWOULDBLOCK
# (EAGAIN, and EACCES on some systems), msvcrt's _locking EACCES (and EDEADLOCK once its retries are
# spent). Any other error, such as ENOLCK or EOPNOTSUPP where a file system (NFS) cannot lock, is raised
# at once: waiting would not change it.
try:
    import fcntl

    LOCK_HELD_ERRORS = frozenset({errno.EAGAIN, errno.EWOULDBLOCK, errno.EACCES})

    def tryLockFile(stream):
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)

    def unlockFile(stream):
        fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
except ImportError:
    import msvcrt

    LOCK_HELD_ERRORS = frozenset({errno.EDEADLOCK, errno.EACCES})

    def tryLockFile(stream):
        stream.seek(0)
        msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)

    def unlockFile(stream):
        stream.seek(0)
        msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


class ToolLock(contextlib.AbstractContextManager):
    """An exclusive lock on the file at path, held while the context is entered. Taking it waits up to
    timeout seconds for another holder, then fails naming the file; an error that says the file cannot be
    locked at all is raised as it is. The operating system releases it when its holder exits, so a killed
    run never leaves it held."""

    def __init__(self, path, timeout):
        self._path = pathlib.Path(path)
        self._timeout = timeout
        self._stream = None

    def __enter__(self):
        stream = open(self._path, "a+b")
        deadline = time.monotonic() + self._timeout
        while True:
            try:
                tryLockFile(stream)
                break
            except OSError as error:
                if error.errno not in LOCK_HELD_ERRORS:
                    stream.close()
                    raise
                if time.monotonic() >= deadline:
                    stream.close()
                    raise SystemExit(f"another run holds {self._path} for more than {self._timeout} s") from None
                time.sleep(LOCK_POLL_INTERVAL)
        self._stream = stream
        return self

    def __exit__(self, *exception):
        stream, self._stream = self._stream, None
        try:
            unlockFile(stream)
        finally:
            stream.close()
        return False


def readLock(path):
    """Returns the lock file's `key = value` lines as a dict."""
    entries = {}
    for number, line in enumerate(pathlib.Path(path).read_text(encoding="utf-8").splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        key, separator, value = line.partition("=")
        if not separator:
            raise SystemExit(f"{path}:{number}: expected `key = value`")
        entries[key.strip()] = value.strip()
    return entries


def hostPlatform():
    """Returns this host's platform name as the lock file spells it."""
    machine = platform.machine().lower()
    for prefix, machines, name in PLATFORMS:
        if sys.platform.startswith(prefix) and machine in machines:
            return name
    raise SystemExit(f"no platform name for {sys.platform}/{machine}")


def sha256Of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url, target):
    """Downloads url to target through a temporary file of its own, so a broken download leaves
    nothing and a concurrent one never writes into it."""
    request = urllib.request.Request(url, headers={"User-Agent": "isoline-fetch-tools"})
    descriptor, partial = tempfile.mkstemp(dir=target.parent, prefix=target.name + ".", suffix=".part")
    try:
        # The file is closed before it is removed or renamed, which Windows requires: it is opened
        # first, so a failing request closes it too.
        with os.fdopen(descriptor, "wb") as out, \
                urllib.request.urlopen(request, timeout=DOWNLOAD_TIMEOUT) as response:
            shutil.copyfileobj(response, out)
        os.replace(partial, target)
    except BaseException:
        pathlib.Path(partial).unlink(missing_ok=True)
        raise


def extract(archive, destination):
    """Extracts archive into destination, refusing members that would land outside it."""
    destination.mkdir(parents=True, exist_ok=True)
    root = destination.resolve()
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as bundle:
            for member in bundle.namelist():
                if not (root / member).resolve().is_relative_to(root):
                    raise SystemExit(f"{archive}: member {member!r} escapes the extraction directory")
            bundle.extractall(root)
    else:
        with tarfile.open(archive) as bundle:
            if hasattr(tarfile, "data_filter"):
                bundle.extractall(root, filter="data")
            else:
                for member in bundle.getmembers():
                    target = (root / member.name).resolve()
                    if not target.is_relative_to(root) or member.issym() or member.islnk():
                        raise SystemExit(f"{archive}: unsafe member {member.name!r}")
                bundle.extractall(root)


def isComplete(extracted, digest):
    """Whether the tree extracted carries the completion marker of the archive whose SHA-256 is digest."""
    marker = extracted / COMPLETION_MARKER
    return marker.is_file() and marker.read_text(encoding="utf-8").strip() == digest


def extractInPlace(archive, extracted, digest):
    """Extracts archive to the directory extracted through a temporary sibling that receives the
    completion marker (digest) and is then renamed into place. The caller holds the tool's lock and has
    removed any incomplete tree; a staging copy is dropped when anything fails."""
    staging = pathlib.Path(tempfile.mkdtemp(dir=extracted.parent, prefix=extracted.name + ".", suffix=".part"))
    try:
        extract(archive, staging)
        (staging / COMPLETION_MARKER).write_text(digest, encoding="utf-8")
        staging.rename(extracted)
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def sweepStaleStaging(cache, stem, downloads):
    """Removes what a killed run of the tool stem left behind: its staging directories (<stem>.*.part in
    the cache) and its partial downloads (*.part in its download directory). The caller holds the tool's
    lock, so no live run owns them: every run makes and drops its own under that lock."""
    for staging in cache.glob(f"{stem}.*.part"):
        if staging.is_dir():
            shutil.rmtree(staging)
    for partial in downloads.glob("*.part"):
        partial.unlink(missing_ok=True)


def lockRow(lock, key, lockPath):
    """Returns lock[key], or fails naming the missing row."""
    if key not in lock:
        raise SystemExit(f"{lockPath}: no row `{key}`")
    return lock[key]


def ensureTool(tool, lockPath=DEFAULT_LOCK, cache=DEFAULT_CACHE, name=None):
    """Returns the path of the pinned tool's executable, downloading and verifying it when not cached.
    name is the platform to fetch for (default: this host's)."""
    lock = readLock(lockPath)
    name = name or hostPlatform()
    version = lockRow(lock, f"{tool}.version", lockPath)
    archiveName = lockRow(lock, f"{tool}.archive.{name}", lockPath).format(version=version)
    executablePath = lockRow(lock, f"{tool}.executable.{name}", lockPath).format(version=version)
    expected = lockRow(lock, f"{tool}.sha256.{name}", lockPath).lower()
    url = lockRow(lock, f"{tool}.url", lockPath).format(version=version, archive=archiveName)
    stem = f"{tool}-{version}-{name}"

    cache = pathlib.Path(cache)
    downloads = cache / "downloads" / stem
    downloads.mkdir(parents=True, exist_ok=True)
    with ToolLock(downloads / LOCK_NAME, LOCK_TIMEOUT):
        sweepStaleStaging(cache, stem, downloads)
        archive = downloads / archiveName
        extracted = cache / stem
        executable = extracted / executablePath
        if isComplete(extracted, expected) and executable.exists():
            return markExecutable(executable, name)
        if not archive.exists() or sha256Of(archive) != expected:
            download(url, archive)
        actual = sha256Of(archive)
        if actual != expected:
            archive.unlink()
            raise SystemExit(f"{archiveName}: SHA-256 {actual} does not match the pinned {expected}")
        if extracted.exists():
            shutil.rmtree(extracted)
        extractInPlace(archive, extracted, expected)
        if not executable.exists():
            raise SystemExit(f"{archiveName}: no {executable.name} at {executable}")
        return markExecutable(executable, name)


def markExecutable(executable, name):
    """Sets executable's executable bits where platform name has them (a zip loses the mode)."""
    if not name.endswith("windows"):
        executable.chmod(executable.stat().st_mode | 0o111)
    return executable


def ensureWasmTools(lockPath=DEFAULT_LOCK, cache=DEFAULT_CACHE):
    """Returns the path of the pinned wasm-tools, downloading and verifying it when not cached."""
    return ensureTool("wasm-tools", lockPath, cache)


def pinnedVersion(tool, lockPath=DEFAULT_LOCK):
    """Returns the version the lock pins for tool."""
    return lockRow(readLock(lockPath), f"{tool}.version", lockPath)


def versionMatches(actual, expected):
    """Whether the version actual is expected or one of its releases: 22.1.8 matches 22 and 22.1.8, not 2."""
    return actual == expected or actual.startswith(expected + ".")


# A release version, and what may follow it on its line: a letter, alone or after a dot or dash, marks a
# pre-release or development build (4.3.0-rc1, 1.13.0.git, 19.1.0rc3), never a pinned release.
VERSION = r"(?P<version>\d+(?:\.\d+)+)(?P<suffix>[.-]?[A-Za-z]\S*)?"

# The line each tool names its own version on: (tool, its version line, what on that line marks a build
# that is no release, or None). --check-version reads the row of its --tool, and a line no row of that
# tool recognizes (a wine or loader diagnostic, a copyright line, another tool's banner) is never read.
VERSION_LINES = [
    ("cmake", re.compile(rf"^cmake version {VERSION}"), None),
    ("ninja", re.compile(rf"^{VERSION}$"), None),
    # clang, clang++, clang-scan-deps ("Ubuntu clang version 22.1.8 (++2026)") and clang-tidy
    # ("LLVM version 22.1.8").
    ("llvm", re.compile(rf"\b(?:clang|LLVM) version {VERSION}"), None),
    # g++ ("g++ (GCC) 15.2.0", "g++ (Ubuntu 14.2.0-4ubuntu2) 14.2.0", "g++ (GCC) 16.2.1 20260819 (Red Hat
    # 16.2.1-2)"); a snapshot says so after its date: "g++ (GCC) 16.0.0 20260101 (experimental)".
    ("gcc", re.compile(rf"^\S+ \(.*?\) {VERSION}(?:\s|$)"), re.compile(r"\((?:experimental|prerelease)\)")),
    # cl.exe, run without arguments: "Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35217 for x64".
    ("msvc", re.compile(rf"\bCompiler Version {VERSION}(?:\s|$)"), None),
    ("wasm-tools", re.compile(rf"^wasm-tools {VERSION}"), None),
    # "wasmtime 50.0.3 (9bbe3e2f8 2026-11-19)".
    ("wasmtime", re.compile(rf"^wasmtime {VERSION}"), None),
    # CPython ("Python 3.14.0"), the selftest's executable of a known version.
    ("python", re.compile(rf"^Python {VERSION}$"), None),
]


def versionLineTools():
    """Returns the tools VERSION_LINES has a row for."""
    return [tool for tool, _, _ in VERSION_LINES]


def reportedVersion(tool, stdout, stderr=""):
    """Returns (version, suffix) from tool's version line, or None when no line is one.

    The version line is the first line, of stdout and then of stderr, that tool's VERSION_LINES row matches;
    suffix is what marks a pre-release or development build ("(experimental)" for a refused GCC snapshot),
    or None for a release."""
    rows = [(line, refusal) for name, line, refusal in VERSION_LINES if name == tool]
    if not rows:
        raise SystemExit(f"no version line row for the tool {tool} (VERSION_LINES: {', '.join(versionLineTools())})")
    for text in stdout.splitlines() + stderr.splitlines():
        for row, refusal in rows:
            found = row.search(text.strip())
            if not found:
                continue
            suffix = found.group("suffix")
            refused = refusal.search(text) if refusal else None
            return found.group("version"), suffix or (f" {refused.group(0)}" if refused else None)
    return None


def checkVersion(executable, tool, expected, versionArgument="--version"):
    """Fails unless executable exits with 0 and its version line (tool's VERSION_LINES row) names expected
    (versionMatches) as a release; returns the version it reports."""
    command = [executable] + ([versionArgument] if versionArgument else [])
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
    except OSError as error:
        raise SystemExit(f"{executable}: cannot run it: {error}")
    if completed.returncode != 0:
        raise SystemExit(f"{executable}: exits with {completed.returncode} when asked its version")
    reported = reportedVersion(tool, completed.stdout, completed.stderr)
    if reported is None:
        raise SystemExit(f"{executable}: prints no version line of {tool}")
    actual, suffix = reported
    if suffix is not None:
        raise SystemExit(f"{executable} is {actual}{suffix}, a pre-release or development build; "
                         f"its row pins the release {expected}")
    if not versionMatches(actual, expected):
        raise SystemExit(f"{executable} is {actual}, but its row pins {expected}")
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tool", help=f"the tool to fetch (default: {DEFAULT_TOOL}), or whose version line "
                        "--check-version reads (required there)")
    parser.add_argument("--lock", type=pathlib.Path, default=DEFAULT_LOCK, help="the tools.lock to read")
    parser.add_argument("--cache", type=pathlib.Path, default=DEFAULT_CACHE, help="the cache directory")
    parser.add_argument("--platform", help="the platform to fetch for, as the lock spells it (default: this host's)")
    parser.add_argument("--print-version", action="store_true", help="print the tool's pinned version")
    parser.add_argument("--check-version", metavar="EXECUTABLE", help="check the executable's version")
    parser.add_argument("--expected", help="the version --check-version expects (default: the tool's pinned one)")
    parser.add_argument("--version-argument", default="--version", help="what makes the executable print it")
    arguments = parser.parse_args()
    if arguments.check_version and not arguments.tool:
        parser.error("--check-version needs --tool, which names the version line to read")
    arguments.tool = arguments.tool or DEFAULT_TOOL
    if arguments.print_version:
        print(pinnedVersion(arguments.tool, arguments.lock))
    elif arguments.check_version:
        expected = arguments.expected or pinnedVersion(arguments.tool, arguments.lock)
        actual = checkVersion(arguments.check_version, arguments.tool, expected, arguments.version_argument)
        print(f"{arguments.check_version}: {actual}, as its row pins ({expected})")
    else:
        print(os.fspath(ensureTool(arguments.tool, arguments.lock, arguments.cache, arguments.platform)))


if __name__ == "__main__":
    main()
