#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Imports WebAssembly spec test suites into tests/spec/ as .isotest scripts and .wasm files.

Each row of tests/spec/files.txt names a suite source and a path in it, `<source> <path> <skips>`.
The importer converts test/<path>.wast of the source's repository, at the commit scripts/tools.lock
pins as `source.<source>.repository` and `source.<source>.commit`, with the pinned
`wasm-tools json-from-wast`, translates the JSON commands into
tests/spec/<source>/<path>/<name>.isotest, and copies the binary modules beside it. The output is
committed, so neither a build nor ctest needs wasm-tools. A row `<source> <path> exclude <reason…>`
names a file that is deliberately not imported.

What becomes a `skip` line is data, not a code path:
- an assertion of a module given as text, for the commands in TEXT_SKIPPED, as Isoline has no
  text-format parser;
- a module given as text (a `module` or `module_definition`): the root of a poisoning;
- every command that uses a poisoned module: an action or a `register` naming it, or naming no
  module while it is the current one; a `module_instance` of a poisoned definition; a module that
  imports from a name a poisoned module was registered under. A poisoned module, instance or
  registration poisons in turn, and a module command that is not poisoned makes its name (and, as
  the current module, the unnamed actions) clean again.
- the GC cut, for every source but those tools.lock lists in `cut.exempt`: the pinned oracle,
  `wasm-tools validate --features=<cut.features>`, validates the module of every command in
  CUT_EXPECTATIONS. One it rejects with a message of GC_REJECTIONS (the innermost cause of the
  oracle's error) becomes a skip `gc: <message>`, and a `module` or `module_definition` so cut is the
  root of a poisoning. Where the suite expects the module valid, any other rejection fails the
  import; where it expects it invalid or malformed, any other rejection keeps the assertion, and an
  acceptance fails the import. The cut never hides a difference that is not GC's.
A poisoned line's reason names its root, `<root's reason> (root wast line N; wast line M)`. Any
other command the .isotest format cannot express fails the import, so that no assertion is dropped
unseen. Each row pins how many skip lines its file has; the import fails when a file's count
differs, and so does the spec runner, which CTest passes the pinned count.

Usage: python3 -I scripts/import-spec-tests.py [--source <name>=<checkout>]… [--wasm-tools <path>]
                                               [--out <tests/spec>]
"""

import argparse
import importlib.util
import json
import pathlib
import re
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
    """An assertion of a module given as text, of a kind in TEXT_SKIPPED: it becomes a skip line."""


# The assertions whose module, given as text, becomes a skip line rather than failing the import:
# they only check that the text is rejected, so no later command depends on them.
TEXT_SKIPPED = {"assert_invalid", "assert_malformed", "assert_malformed_custom"}

# The module commands a text module poisons, and every command that uses it with them.
MODULE_COMMANDS = {"module", "module_definition"}

# The commands that link a module against the registered ones: a module among them that imports
# from a poisoned registration is skipped too.
LINKING_COMMANDS = {"module", "module_definition", "assert_unlinkable", "assert_uninstantiable"}

# The commands whose action uses a module, by name or as the current one.
ACTION_COMMANDS = {"action", "assert_return", "assert_trap", "assert_exhaustion", "assert_exception"}

# The .isotest commands whose first argument is a .wasm file to copy beside the script.
FILE_COMMANDS = {"module", "module_definition", "assert_invalid", "assert_malformed", "assert_unlinkable",
                 "assert_uninstantiable", "assert_invalid_custom"}

NAN_PATTERNS = ("nan:canonical", "nan:arithmetic")

# What the suite expects of the binary module of a command the cut validates.
VALID = "valid"
REJECTED = "rejected"

# The commands whose binary module the cut validates, and what the suite expects of it.
CUT_EXPECTATIONS = {
    "module": VALID,
    "module_definition": VALID,
    "assert_unlinkable": VALID,
    "assert_uninstantiable": VALID,
    "assert_trap": VALID,  # the module form
    "assert_invalid_custom": VALID,
    "assert_invalid": REJECTED,
    "assert_malformed": REJECTED,
}

# The oracle's messages for a construct of the GC proposal: a module it rejects with one is cut.
GC_REJECTIONS = (
    "requires `gc`",
    "without the gc feature",
    "because the GC proposal is disabled",
    "gc proposal must be enabled",
    "gc support is not enabled",
    "global.get of locally defined global",
)


def isGcRejection(message):
    return any(rejection in message for rejection in GC_REJECTIONS)


# JSON value type -> how its .isotest token is formed from the JSON value.
def numberToken(prefix, width):
    def token(value):
        number = value["value"]
        if number in NAN_PATTERNS:
            return f"{prefix}:{number}"
        # wasm-tools spells some values signed; the token carries the bits.
        return f"{prefix}:{int(number) & ((1 << width) - 1):x}"

    return token


def referenceToken(heap):
    def token(value):
        address = value.get("value")
        if address == "null":
            return f"ref.null:{heap}"
        if address is None:
            if heap != "func":
                raise Unsupported(f"any {heap}ref")
            return "ref.func"
        return f"ref.{heap}:{int(address)}"

    return token


# A v128's JSON lane type -> its lane count and width, and the lane pattern prefix of a float lane.
VECTOR_LANES = {
    "i8": (16, 8, None),
    "i16": (8, 16, None),
    "i32": (4, 32, None),
    "i64": (2, 64, None),
    "f32": (4, 32, "v128:f32x4:"),
    "f64": (2, 64, "v128:f64x2:"),
}


def vectorToken(value):
    """A v128: its 128 bits in hex, or, when a float lane is a NaN pattern, each lane on its own."""
    laneType = value.get("lane_type")
    if laneType not in VECTOR_LANES:
        raise Unsupported(f"v128 lane type {laneType}")
    count, width, lanePrefix = VECTOR_LANES[laneType]
    lanes = value["value"]
    if len(lanes) != count:
        raise Unsupported(f"v128 of {len(lanes)} {laneType} lanes")
    mask = (1 << width) - 1
    if lanePrefix and any(lane in NAN_PATTERNS for lane in lanes):
        return lanePrefix + ",".join(lane if lane in NAN_PATTERNS else f"{int(lane) & mask:x}" for lane in lanes)
    bits = 0
    for index, lane in enumerate(lanes):
        bits |= (int(lane) & mask) << (index * width)
    return f"v128:{bits:032x}"


def eitherToken(value):
    alternatives = value["values"]
    if any(alternative["type"] == "either" for alternative in alternatives):
        raise Unsupported("an either within an either")
    return "either(" + ";".join(valueToken(alternative) for alternative in alternatives) + ")"


VALUE_TOKENS = {
    "i32": numberToken("i32", 32),
    "i64": numberToken("i64", 64),
    "f32": numberToken("f32", 32),
    "f64": numberToken("f64", 64),
    "funcref": referenceToken("func"),
    "externref": referenceToken("extern"),
    "refnull": lambda value: "ref.null",  # the null of any heap
    "v128": vectorToken,
    "either": eitherToken,
}


def valueToken(value):
    kind = value["type"]
    if kind not in VALUE_TOKENS:
        raise Unsupported(f"value type {kind}")
    return VALUE_TOKENS[kind](value)


def moduleToken(action):
    return hexOf(action["module"]) if "module" in action else "-"


def actionTokens(action, expected):
    """The tokens `<kind> <export> <module|-> <arg>* -> <expected>*` of an invoke or a get."""
    kind = action["type"]
    if kind not in ("invoke", "get"):
        raise Unsupported(f"action {kind}")
    arguments = [valueToken(value) for value in action.get("args", [])]
    return [kind, hexOf(action["field"]), moduleToken(action), *arguments, "->", *expected]


def invokeTokens(command, expected):
    """The tokens of an action that must be an invoke: the assertions on a call's outcome."""
    tokens = actionTokens(command["action"], expected)
    if tokens[0] != "invoke":
        raise Unsupported(f"{command['type']} of a get")
    return tokens


def binaryFile(command):
    if command.get("module_type") != "binary":
        failure = TextOnly if command["type"] in TEXT_SKIPPED else Unsupported
        raise failure(f"{command['type']} of a text module")
    return command["filename"]


def definitionOf(command):
    """The definition a module_instance instantiates; the format names it, so it must be named."""
    if "module" not in command:
        raise Unsupported("module_instance of the latest definition, without its name")
    return command["module"]


def optionalName(command, key):
    return [hexOf(command[key])] if key in command else []


def translate(command):
    """Translates one JSON command into its .isotest tokens."""
    kind = command["type"]
    if kind in MODULE_COMMANDS:
        return [kind, binaryFile(command), *optionalName(command, "name")]
    if kind == "module_instance":
        return [kind, hexOf(command["instance"]) if "instance" in command else "-", hexOf(definitionOf(command))]
    if kind == "register":
        return ["register", hexOf(command["as"]), *optionalName(command, "name")]
    if kind == "action":
        return invokeTokens(command, [])
    if kind == "assert_return":
        expected = [valueToken(value) for value in command["expected"]]
        return ["assert_return", *actionTokens(command["action"], expected)]
    if kind in ("assert_trap", "assert_exhaustion"):
        return [kind, *invokeTokens(command, [hexOf(command["text"])])]
    if kind == "assert_exception":
        return [kind, *invokeTokens(command, [])]
    if kind in ("assert_invalid", "assert_malformed", "assert_unlinkable", "assert_uninstantiable",
                "assert_invalid_custom", "assert_malformed_custom"):
        filename = binaryFile(command)
        if kind == "assert_malformed_custom":
            raise Unsupported(f"{kind} of a binary module")
        return [kind, filename, hexOf(command["text"])]
    raise Unsupported(f"command {kind}")


def readLeb(data, position):
    """Reads an unsigned LEB128 number at position; returns it and the position after it."""
    value = 0
    shift = 0
    while True:
        if position >= len(data):
            raise Unsupported("a truncated LEB128 number in an import section")
        byte = data[position]
        position += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, position


def skipReferenceOrValueType(data, position):
    """Skips a value type or a reference type: one byte, or 0x63/0x64 and a heap type."""
    kind = data[position]
    if kind in (0x63, 0x64):
        return readLeb(data, position + 1)[1]
    return position + 1


def skipLimits(data, position):
    flags = data[position]
    position = readLeb(data, position + 1)[1]
    if flags & 0x01:  # a maximum
        position = readLeb(data, position)[1]
    if flags & 0x08:  # a custom page size
        position = readLeb(data, position)[1]
    return position


# An import's kind byte -> how its descriptor is skipped to reach the next import.
IMPORT_DESCRIPTORS = {
    0x00: lambda data, position: readLeb(data, position)[1],  # func: a type index
    0x01: lambda data, position: skipLimits(data, skipReferenceOrValueType(data, position)),  # table
    0x02: skipLimits,  # memory
    0x03: lambda data, position: skipReferenceOrValueType(data, position) + 1,  # global: type, mutability
    0x04: lambda data, position: readLeb(data, position + 1)[1],  # tag: attribute, type index
}

# The byte after an empty field name that introduces a compact import group.
COMPACT_NAMES_THEN_TYPES = 0x7F  # a list of (name, type)
COMPACT_TYPE_THEN_NAMES = 0x7E  # one type, then a list of names


def readName(data, position):
    length, position = readLeb(data, position)
    return data[position:position + length].decode("utf-8"), position + length


def skipDescriptor(data, position):
    kind = data[position]
    if kind not in IMPORT_DESCRIPTORS:
        raise Unsupported(f"import kind 0x{kind:02x}")
    return IMPORT_DESCRIPTORS[kind](data, position + 1)


def importModuleNames(data):
    """The module names a binary module's import section imports from, in order."""
    try:
        return readImportModuleNames(data)
    except (IndexError, UnicodeDecodeError) as error:
        raise Unsupported(f"an import section that does not decode ({error})") from None


def readImportModuleNames(data):
    if data[:4] != b"\0asm":
        raise Unsupported("a module without the wasm magic")
    position = 8
    while position < len(data):
        section = data[position]
        size, position = readLeb(data, position + 1)
        end = position + size
        if section != 2:
            position = end
            continue
        names = []
        count, position = readLeb(data, position)
        for _ in range(count):
            module, position = readName(data, position)
            field, position = readName(data, position)
            group = data[position] if position < end else None
            if field == "" and group == COMPACT_NAMES_THEN_TYPES:
                entries, position = readLeb(data, position + 1)
                for _ in range(entries):
                    position = skipDescriptor(data, readName(data, position)[1])
            elif field == "" and group == COMPACT_TYPE_THEN_NAMES:
                position = skipDescriptor(data, position + 1)
                entries, position = readLeb(data, position)
                for _ in range(entries):
                    position = readName(data, position)[1]
            else:
                position = skipDescriptor(data, position)
            names.append(module)
        return names
    return []


class Poison:
    """Why a module is skipped: the reason of the command it was rooted at, and that command's line."""

    def __init__(self, reason, line):
        self.reason = reason
        self.line = line

    def rootReason(self):
        return f"{self.reason} (wast line {self.line})"

    def dependentReason(self, line):
        return f"{self.reason} (root wast line {self.line}; wast line {line})"


class Translator:
    """Translates the JSON commands of one .wast file in order, keeping which modules are poisoned.

    readImports(filename) returns the module names a binary module file imports from, and
    validate(filename) the cut's message for a binary module file, or None when the cut accepts it;
    validate is None for a source the cut does not apply to."""

    def __init__(self, source, readImports, validate):
        self.source = source
        self.readImports = readImports
        self.validate = validate
        self.current = None  # the Poison of the current module, or None while it is usable
        self.names = {}  # module, definition and instance name -> its Poison, or None
        self.registered = {}  # registered name -> the Poison of the module registered under it, or None

    def poisonOf(self, command):
        """The poison of the module command uses, or None when it uses none that is poisoned."""
        kind = command["type"]
        if kind in ACTION_COMMANDS or kind == "register":
            name = command["action"].get("module") if kind in ACTION_COMMANDS else command.get("name")
            return self.current if name is None else self.names.get(name)
        if kind == "module_instance":
            return self.names.get(definitionOf(command))
        if kind in LINKING_COMMANDS and command.get("module_type") == "binary" and any(self.registered.values()):
            for module in self.readImports(command["filename"]):
                if self.registered.get(module):
                    return self.registered[module]
        return None

    def update(self, command, poison):
        """Records what command defines: poisoned with poison, or usable when it is None."""
        kind = command["type"]
        if kind in ("module", "module_instance"):
            self.current = poison
        name = command.get("instance" if kind == "module_instance" else "name")
        if kind in MODULE_COMMANDS | {"module_instance"} and name is not None:
            self.names[name] = poison
        if kind == "register":
            self.registered[command["as"]] = poison

    def lineOf(self, command):
        """The .isotest line of one JSON command, and whether it is a skip line. Fails the import on
        any other command the format cannot express, and on a module the cut and the suite disagree
        on."""
        try:
            return self.translateLine(command)
        except Unsupported as unsupported:
            raise SystemExit(f"{self.source}:{command['line']}: {unsupported}: the .isotest format cannot "
                             "express it, and only the skips the importer names may be skipped") from None

    def cutOf(self, command):
        """The Poison of a module the cut removes, or None when it keeps it. Fails the import when
        the oracle disagrees with the suite on anything but GC."""
        kind = command["type"]
        if self.validate is None or kind not in CUT_EXPECTATIONS or command.get("module_type") != "binary":
            return None
        where = command["line"]
        message = self.validate(command["filename"])
        if message is not None and isGcRejection(message):
            return Poison(f"gc: {message}", where)
        if CUT_EXPECTATIONS[kind] == VALID and message is not None:
            raise SystemExit(f"{self.source}:{where}: {kind}: the cut rejects a module the suite expects "
                             f"valid, and not for GC: {message}")
        if CUT_EXPECTATIONS[kind] == REJECTED and message is None:
            raise SystemExit(f"{self.source}:{where}: {kind}: the cut accepts a module the suite expects "
                             "rejected")
        return None

    def translateLine(self, command):
        where = command["line"]
        poison = self.poisonOf(command)
        reason = poison.dependentReason(where) if poison else None
        if poison is None and command["type"] in MODULE_COMMANDS and command.get("module_type") != "binary":
            poison = Poison(f"text: {command['type']} of a text module", where)
            reason = poison.rootReason()
        if poison is None:
            poison = self.cutOf(command)
            reason = poison.rootReason() if poison else None
        if poison is not None:
            self.update(command, poison)
            return f"skip {hexOf(reason)}", True
        try:
            tokens = translate(command)
        except TextOnly as skipped:
            return f"skip {hexOf(f'{skipped} (wast line {where})')}", True
        self.update(command, None)
        return " ".join(tokens), False


class Row:
    """One row of files.txt: a suite source's .wast file, imported or excluded."""

    def __init__(self, source, path, skips=None, exclude=None):
        self.source = source
        self.path = path
        self.skips = skips  # the pinned skip count of an imported file
        self.exclude = exclude  # why an excluded file is not imported

    def __eq__(self, other):
        return vars(self) == vars(other)

    def __repr__(self):
        return f"Row({self.source!r}, {self.path!r}, {self.skips!r}, {self.exclude!r})"

    def directory(self, root):
        """Where the import of the file lives under root, the tests/spec directory."""
        return root / self.source / self.path


def readFiles(path):
    """The rows of files.txt, in order."""
    rows = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) == 3 and fields[2].isdigit():
            rows.append(Row(fields[0], fields[1], skips=int(fields[2])))
        elif len(fields) >= 4 and fields[2] == "exclude":
            rows.append(Row(fields[0], fields[1], exclude=" ".join(fields[3:])))
        else:
            raise SystemExit(f"{path}:{number}: expected '<source> <path> <skips>' or "
                             f"'<source> <path> exclude <reason>', got '{line}'")
    return rows


def oracleMessage(wasmTools, features, path):
    """What the cut, `wasm-tools validate --features=<features>`, says of a binary module: None when
    it accepts it, else its message without the `error: ` prefix and the `(at offset …)` suffix."""
    result = subprocess.run([str(wasmTools), "validate", f"--features={features}", str(path)],
                            capture_output=True, text=True, check=False)
    if result.returncode == 0:
        return None
    message = rejectionMessage(result.stderr)
    if message is None:
        raise SystemExit(f"{path.name}: wasm-tools validate failed without a message")
    return message


# A cause in the chain wasm-tools prints after `Caused by:`, the innermost one last.
CAUSE = re.compile(r"^\s*\d+: (?P<message>.*)$")
OFFSET_SUFFIX = re.compile(r" \(at offset 0x[0-9a-f]+\)$")


def rejectionMessage(stderr):
    """The reason in what `wasm-tools validate` prints on a rejection: the innermost cause of a
    `Caused by:` chain (a function body's error comes as `func N failed to validate` first), else
    the error line itself; without the `error: ` prefix and the `(at offset …)` suffix. None when
    stderr holds no message."""
    lines = [line for line in stderr.strip().splitlines() if line.strip()]
    if not lines:
        return None
    causes = [cause["message"] for cause in map(CAUSE.match, lines) if cause]
    message = causes[-1] if causes else lines[0].removeprefix("error: ")
    return OFFSET_SUFFIX.sub("", message)


def cutFeaturesOf(source, cutFeatures, cutExempt):
    """The features the cut runs with on a source's files: cutFeatures, or None, no cut, for a source
    in cutExempt. The cut is opt-out, so a new source cannot lose it by being left unlisted."""
    return None if source in cutExempt else cutFeatures


def convert(wasmTools, cutFeatures, wast, outDir, work):
    """Converts one .wast file into outDir/<name>.isotest and its .wasm files, using work as scratch,
    and cutting what the oracle run with cutFeatures rejects as GC; cutFeatures None cuts nothing."""
    name = wast.stem
    work.mkdir(parents=True)
    jsonPath = work / f"{name}.json"
    result = subprocess.run(
        [str(wasmTools), "json-from-wast", str(wast), "-o", str(jsonPath), "--wasm-dir", str(work)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(f"{wast.name}: wasm-tools json-from-wast failed:\n{result.stderr}")

    translator = Translator(wast.name, lambda filename: importModuleNames((work / filename).read_bytes()),
                            None if cutFeatures is None
                            else lambda filename: oracleMessage(wasmTools, cutFeatures, work / filename))
    lines = []
    skipped = 0
    for command in json.loads(jsonPath.read_text(encoding="utf-8"))["commands"]:
        line, isSkip = translator.lineOf(command)
        lines.append(line)
        skipped += isSkip

    if outDir.exists():
        shutil.rmtree(outDir)
    outDir.mkdir(parents=True)
    for line in lines:
        kind, _, rest = line.partition(" ")
        if kind in FILE_COMMANDS:
            filename = rest.split(" ")[0]
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


def checkout(source, lock, given, temporary):
    """A checkout of source at its pinned commit: the one given, or a fresh fetch into temporary."""
    commit = lock.get(f"source.{source}.commit")
    repository = lock.get(f"source.{source}.repository")
    if commit is None or repository is None:
        raise SystemExit(f"{LOCK.name} pins no suite source '{source}'")
    path = given.get(source)
    if path is None:
        path = temporary / "sources" / source
        fetchSpec(repository, commit, path)
    head = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], capture_output=True, text=True,
                          check=True).stdout.strip()
    if head != commit:
        raise SystemExit(f"{path} is at {head}, not the pinned {commit} of source '{source}'")
    dirty = subprocess.run(["git", "-C", str(path), "status", "--porcelain"], capture_output=True, text=True,
                           check=True).stdout
    if dirty:
        print(f"warning: {path} has uncommitted changes; the import may differ from the pinned commit",
              file=sys.stderr)
    return path


def sourceArgument(text):
    name, separator, path = text.partition("=")
    if not separator or not name or not path:
        raise argparse.ArgumentTypeError(f"expected <name>=<checkout>, got '{text}'")
    return name, pathlib.Path(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=sourceArgument, action="append", default=[], metavar="NAME=CHECKOUT",
                        help="an existing checkout of a suite source at its pinned commit (repeatable)")
    parser.add_argument("--wasm-tools", type=pathlib.Path, help="the wasm-tools to use (default: fetched)")
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "tests" / "spec", help="the output root")
    arguments = parser.parse_args()

    fetchTools = loadFetchTools()
    lock = fetchTools.readLock(LOCK)
    wasmTools = arguments.wasm_tools or fetchTools.ensureWasmTools(LOCK)
    if arguments.wasm_tools:
        print(f"warning: {arguments.wasm_tools} is not checked against the wasm-tools hash pinned in "
              f"{LOCK.name}; the import is reproducible only with the pinned build", file=sys.stderr)
    cutFeatures = lock.get("cut.features")
    if cutFeatures is None:
        raise SystemExit(f"{LOCK.name} pins no cut.features")
    cutExempt = lock.get("cut.exempt", "").split()
    rows = [row for row in readFiles(arguments.out / "files.txt") if row.exclude is None]
    given = dict(arguments.source)

    with tempfile.TemporaryDirectory(prefix="isoline-spec-") as temporary:
        temporary = pathlib.Path(temporary)
        checkouts = {}
        mismatches = []
        for row in rows:
            if row.source not in checkouts:
                checkouts[row.source] = checkout(row.source, lock, given, temporary)
            work = temporary / "json" / row.source / row.path
            commands, skipped = convert(wasmTools, cutFeaturesOf(row.source, cutFeatures, cutExempt),
                                        checkouts[row.source] / "test" / f"{row.path}.wast",
                                        row.directory(arguments.out), work)
            print(f"{row.source} {row.path}: {commands} commands, {skipped} skipped")
            if skipped != row.skips:
                mismatches.append(f"{row.source} {row.path}: {skipped} skipped, files.txt pins {row.skips}")
        if mismatches:
            raise SystemExit("skip counts differ from files.txt; update it only if each new skip is meant:\n  "
                             + "\n  ".join(mismatches))


if __name__ == "__main__":
    sys.exit(main())
