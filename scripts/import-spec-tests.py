#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Imports the WebAssembly spec test suite into tests/spec/ as .isotest scripts and .wasm files.

For each name in tests/spec/files.txt it converts test/core/<name>.wast of the spec repository, at
the commit scripts/tools.lock pins, with the pinned `wasm-tools json-from-wast`, translates the JSON
commands into tests/spec/<name>/<name>.isotest, and copies the binary modules beside it. The output
is committed, so neither a build nor ctest needs wasm-tools.

Only one kind of command may become a `skip` line: an `assert_malformed` of a module given as text,
as Isoline has no text-format parser. Any other command the .isotest format cannot express fails
the import, so that no assertion is dropped unseen. Each line of files.txt pins how many skip lines
its file has, `<name> <skips>`; the import fails when a file's count differs, and so does the spec
runner, which CTest passes the pinned count.

Usage: python3 -I scripts/import-spec-tests.py [--spec <checkout>] [--wasm-tools <path>]
                                               [--out <tests/spec>]
"""

import argparse
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

SCRIPTS = pathlib.Path(__file__).resolve().parent
ROOT = SCRIPTS.parent
LOCK = SCRIPTS / "tools.lock"


def loadFetchTools():
    """Loads fetch-tools.py by path: its name is not an importable module name."""
    sys.dont_write_bytecode = True  # leave no __pycache__ in the source tree
    spec = importlib.util.spec_from_file_location("fetch_tools", SCRIPTS / "fetch-tools.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def hexOf(text):
    """Hex-encodes a name or message as UTF-8, as .isotest spells every string."""
    return text.encode("utf-8").hex()


class Unsupported(Exception):
    """A command the .isotest format cannot express: the import fails on it."""


class TextOnly(Exception):
    """An assert_malformed of a module given as text: the one command that becomes a skip line."""


# JSON value type -> how its .isotest token is formed from the JSON value string.
def numberToken(prefix, width):
    def token(value):
        if value in ("nan:canonical", "nan:arithmetic"):
            return f"{prefix}:{value}"
        # wasm-tools spells some values signed; the token carries the bits.
        return f"{prefix}:{int(value) & ((1 << width) - 1):x}"

    return token


def referenceToken(heap):
    def token(value):
        if value == "null":
            return f"ref.null:{heap}"
        if heap == "func":
            return "ref.func" if value is None else f"ref.func:{int(value)}"
        return f"ref.extern:{int(value)}"

    return token


VALUE_TOKENS = {
    "i32": numberToken("i32", 32),
    "i64": numberToken("i64", 64),
    "f32": numberToken("f32", 32),
    "f64": numberToken("f64", 64),
    "funcref": referenceToken("func"),
    "externref": referenceToken("extern"),
}


def valueToken(value):
    kind = value["type"]
    if kind not in VALUE_TOKENS:
        raise Unsupported(f"value type {kind}")
    if isinstance(value.get("value"), list):
        raise Unsupported(f"vector value of type {kind}")
    return VALUE_TOKENS[kind](value.get("value"))


def moduleToken(action):
    return hexOf(action["module"]) if "module" in action else "-"


def actionTokens(action, expected):
    """The tokens `<kind> <export> <module|-> <arg>* -> <expected>*` of an invoke or a get."""
    kind = action["type"]
    if kind not in ("invoke", "get"):
        raise Unsupported(f"action {kind}")
    arguments = [valueToken(value) for value in action.get("args", [])]
    return [kind, hexOf(action["field"]), moduleToken(action), *arguments, "->", *expected]


def binaryFile(command):
    if command.get("module_type") != "binary":
        failure = TextOnly if command["type"] == "assert_malformed" else Unsupported
        raise failure(f"{command['type']} of a text module")
    return command["filename"]


def translate(command):
    """Translates one JSON command into its .isotest tokens."""
    kind = command["type"]
    if kind == "module":
        if command.get("module_type") != "binary":
            # A later command may use this module, so dropping it would fail them obscurely.
            raise SystemExit(f"text-only module at line {command['line']}: the format cannot skip it safely")
        name = [hexOf(command["name"])] if "name" in command else []
        return ["module", command["filename"], *name]
    if kind == "register":
        name = [hexOf(command["name"])] if "name" in command else []
        return ["register", hexOf(command["as"]), *name]
    if kind == "action":
        tokens = actionTokens(command["action"], [])
        if tokens[0] != "invoke":
            raise Unsupported("a get action outside assert_return")
        return tokens
    if kind == "assert_return":
        expected = [valueToken(value) for value in command["expected"]]
        return ["assert_return", *actionTokens(command["action"], expected)]
    if kind in ("assert_trap", "assert_exhaustion"):
        tokens = actionTokens(command["action"], [hexOf(command["text"])])
        if tokens[0] != "invoke":
            raise Unsupported(f"{kind} of a get")
        return [kind, *tokens]
    if kind in ("assert_invalid", "assert_malformed", "assert_unlinkable", "assert_uninstantiable"):
        return [kind, binaryFile(command), hexOf(command["text"])]
    raise Unsupported(f"command {kind}")


def lineOf(command, source):
    """The .isotest line of one JSON command of source: its tokens, or a skip line for a text-only
    assert_malformed. Fails the import on any other command the format cannot express."""
    try:
        tokens = translate(command)
    except TextOnly as reason:
        where = command["line"]
        return f"skip {hexOf(f'{reason} (wast line {where})')}", True
    except Unsupported as reason:
        raise SystemExit(f"{source}:{command['line']}: {reason}: the .isotest format cannot express it, "
                         "and only a text-only assert_malformed may be skipped") from None
    return " ".join(tokens), False


def readFiles(path):
    """The names files.txt lists, each with the skip count it pins, in order."""
    files = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 2 or not fields[1].isdigit():
            raise SystemExit(f"{path}:{number}: expected '<name> <skips>', got '{line}'")
        files.append((fields[0], int(fields[1])))
    return files


def convert(wasmTools, wast, outDir, scratch):
    """Converts one .wast file into outDir/<name>.isotest and its .wasm files."""
    name = wast.stem
    work = scratch / name
    work.mkdir()
    jsonPath = work / f"{name}.json"
    result = subprocess.run(
        [str(wasmTools), "json-from-wast", str(wast), "-o", str(jsonPath), "--wasm-dir", str(work)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(f"{wast.name}: wasm-tools json-from-wast failed:\n{result.stderr}")

    lines = []
    skipped = 0
    for command in json.loads(jsonPath.read_text(encoding="utf-8"))["commands"]:
        line, isSkip = lineOf(command, wast.name)
        lines.append(line)
        skipped += isSkip

    if outDir.exists():
        shutil.rmtree(outDir)
    outDir.mkdir(parents=True)
    for line in lines:
        if line.startswith(("module ", "assert_invalid ", "assert_malformed ", "assert_unlinkable ",
                            "assert_uninstantiable ")):
            filename = line.split(" ")[1]
            shutil.copyfile(work / filename, outDir / filename)
    (outDir / f"{name}.isotest").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return len(lines), skipped


def fetchSpec(repository, commit, target):
    """Fetches the spec repository at exactly commit into target."""
    def git(*arguments):
        subprocess.run(["git", "-C", str(target), *arguments], check=True)

    target.mkdir(parents=True)
    git("init", "-q")
    git("fetch", "-q", "--depth", "1", repository, commit)
    git("checkout", "-q", "FETCH_HEAD")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--spec", type=pathlib.Path, help="an existing spec checkout at the pinned commit")
    parser.add_argument("--wasm-tools", type=pathlib.Path, help="the wasm-tools to use (default: fetched)")
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "tests" / "spec", help="the output root")
    arguments = parser.parse_args()

    fetchTools = loadFetchTools()
    lock = fetchTools.readLock(LOCK)
    wasmTools = arguments.wasm_tools or fetchTools.ensureWasmTools(LOCK)
    if arguments.wasm_tools:
        print(f"warning: {arguments.wasm_tools} is not checked against the wasm-tools hash pinned in "
              f"{LOCK.name}; the import is reproducible only with the pinned build", file=sys.stderr)
    files = readFiles(arguments.out / "files.txt")

    with tempfile.TemporaryDirectory(prefix="isoline-spec-") as temporary:
        temporary = pathlib.Path(temporary)
        spec = arguments.spec
        if spec is None:
            spec = temporary / "spec"
            fetchSpec(lock["spec.repository"], lock["spec.commit"], spec)
        head = subprocess.run(["git", "-C", str(spec), "rev-parse", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
        if head != lock["spec.commit"]:
            raise SystemExit(f"{spec} is at {head}, not the pinned {lock['spec.commit']}")
        dirty = subprocess.run(["git", "-C", str(spec), "status", "--porcelain"], capture_output=True, text=True,
                               check=True).stdout
        if dirty:
            print(f"warning: {spec} has uncommitted changes; the import may differ from the pinned commit",
                  file=sys.stderr)
        scratch = temporary / "json"
        scratch.mkdir()
        mismatches = []
        for name, pinned in files:
            commands, skipped = convert(wasmTools, spec / "test" / "core" / f"{name}.wast",
                                        arguments.out / name, scratch)
            print(f"{name}: {commands} commands, {skipped} skipped")
            if skipped != pinned:
                mismatches.append(f"{name}: {skipped} skipped, files.txt pins {pinned}")
        if mismatches:
            raise SystemExit("skip counts differ from files.txt; update it only if each new skip is meant:\n  "
                             + "\n  ".join(mismatches))


if __name__ == "__main__":
    sys.exit(main())
