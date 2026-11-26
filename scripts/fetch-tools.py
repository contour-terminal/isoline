#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Downloads the prebuilt tools pinned in scripts/tools.lock and verifies them.

Each archive is downloaded into its own directory under the cache, checked against the SHA-256 the
lock file records for this platform, and only then extracted. A build never runs this script; it
serves the maintainer tooling that produces committed test inputs (scripts/import-spec-tests.py).

Usage: python3 -I scripts/fetch-tools.py [--lock <tools.lock>] [--cache <dir>]
Prints the path of the verified wasm-tools executable.
"""

import argparse
import hashlib
import os
import pathlib
import platform
import shutil
import sys
import tarfile
import urllib.request
import zipfile

DEFAULT_LOCK = pathlib.Path(__file__).resolve().parent / "tools.lock"
DEFAULT_CACHE = pathlib.Path.home() / ".cache" / "isoline-tools"
RELEASE_URL = "https://github.com/bytecodealliance/wasm-tools/releases/download/v{version}/{archive}"

# (sys.platform prefix, machine names) -> the platform name wasm-tools' release assets use.
PLATFORMS = [
    ("linux", {"x86_64", "amd64"}, "x86_64-linux"),
    ("linux", {"aarch64", "arm64"}, "aarch64-linux"),
    ("darwin", {"arm64", "aarch64"}, "aarch64-macos"),
    ("win32", {"amd64", "x86_64"}, "x86_64-windows"),
]


# Seconds a download may stall (connecting, or between reads) before it fails instead of hanging.
DOWNLOAD_TIMEOUT = 60


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
    raise SystemExit(f"no pinned wasm-tools for {sys.platform}/{machine}")


def sha256Of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url, target):
    """Downloads url to target through a temporary file, so a broken download leaves nothing."""
    partial = target.with_name(target.name + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "isoline-fetch-tools"})
    with urllib.request.urlopen(request, timeout=DOWNLOAD_TIMEOUT) as response, open(partial, "wb") as out:
        shutil.copyfileobj(response, out)
    partial.replace(target)


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


def ensureWasmTools(lockPath=DEFAULT_LOCK, cache=DEFAULT_CACHE):
    """Returns the path of the pinned wasm-tools, downloading and verifying it when not cached."""
    lock = readLock(lockPath)
    version = lock["wasm-tools.version"]
    name = hostPlatform()
    expected = lock[f"wasm-tools.sha256.{name}"].lower()
    suffix = ".zip" if name.endswith("windows") else ".tar.gz"
    stem = f"wasm-tools-{version}-{name}"
    archiveName = stem + suffix

    cache = pathlib.Path(cache)
    downloads = cache / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / archiveName
    if not archive.exists() or sha256Of(archive) != expected:
        download(RELEASE_URL.format(version=version, archive=archiveName), archive)
    actual = sha256Of(archive)
    if actual != expected:
        archive.unlink()
        raise SystemExit(f"{archiveName}: SHA-256 {actual} does not match the pinned {expected}")

    extracted = cache / stem
    executable = extracted / stem / ("wasm-tools.exe" if name.endswith("windows") else "wasm-tools")
    if not executable.exists():
        if extracted.exists():
            shutil.rmtree(extracted)
        extract(archive, extracted)
    if not executable.exists():
        raise SystemExit(f"{archiveName}: no {executable.name} at {executable}")
    if not name.endswith("windows"):
        executable.chmod(executable.stat().st_mode | 0o111)
    return executable


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lock", type=pathlib.Path, default=DEFAULT_LOCK, help="the tools.lock to read")
    parser.add_argument("--cache", type=pathlib.Path, default=DEFAULT_CACHE, help="the cache directory")
    arguments = parser.parse_args()
    print(os.fspath(ensureWasmTools(arguments.lock, arguments.cache)))


if __name__ == "__main__":
    main()
