#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Imports WebAssembly spec test suites into tests/spec/.

Each row of tests/spec/files.txt names a suite source and a path in it, `<source> <path> <skips>`. The
importer reads test/<path>.wast of the source's repository, at the commit scripts/tools.lock pins as
`source.<source>.repository` and `source.<source>.commit`, with the pinned `wasm-tools json-from-wast`.
The output is committed, so neither a build nor ctest needs wasm-tools. A row
`<source> <path> exclude <reason…>` names a file that is deliberately not imported.

A row is imported into tests/spec/<source>/<path>/:
- `<name>.wast`, the file verbatim, which the runner reads itself;
- `<name>.oracle`, the manifest keyed by command ordinal, the index of the JSON's command: `commands
  <n>`, then `<ordinal> <line> <kind> <value>` lines, the line the JSON's;
- `<name>.<ordinal>.wasm`, the oracle's binary of every module-bearing command (MODULE_BEARING) whose
  module the script gives as text, `quote` included, named by a `wasm` entry. A module given as bytes
  (`module binary`) gets none: the runner reads its bytes from the script. The JSON alone cannot tell
  the two apart, so scanScript() reads each command's form from the script, and the two must agree
  command for command.
- `skip` entries, which are data, not a code path:
  - the GC cut, for every source: the pinned oracle, `wasm-tools validate --features=<cut.features>`,
    validates the module of every command in CUT_EXPECTATIONS, a quote module's included. One it
    rejects with a message of GC_REJECTIONS (the innermost cause of the oracle's error) becomes a skip
    `gc: <message>`, and a `module` or `module_definition` so cut is the root of a poisoning. Where the
    suite expects the module valid, any other rejection fails the import; where it expects it invalid
    or malformed, any other rejection keeps the assertion, and an acceptance fails the import. The cut
    never hides a difference that is not GC's.
  - every command that uses a poisoned module: an action or a `register` naming it, or naming no
    module while it is the current one; a `module_instance` of a poisoned definition; a module that
    imports from a name a poisoned module was registered under. A poisoned module, instance or
    registration poisons in turn, and a module command that is not poisoned makes its name (and, as
    the current module, the unnamed actions) clean again.
  A skipped root's reason is `<reason> (wast line M)`, a poisoned command's names its root,
  `<root's reason> (root wast line N; wast line M)`. Nothing else is skipped.
- `message` and `parity` entries, the divergences tests/spec/divergences.txt records for the file,
  each checked against the command it names (DIVERGENCE_KINDS); a stale one fails the import.
Each row pins how many skips its file has; the import fails when a file's count differs, and so does
the spec runner, which CTest passes the pinned count.

Usage: python3 -I scripts/import-spec-tests.py [--source <name>=<checkout>]… [--wasm-tools <path>]
                                               [--out <tests/spec>]
       python3 -I scripts/import-spec-tests.py [--wasm-tools <path>] --list-commands <wast>
       python3 -I scripts/import-spec-tests.py [--wasm-tools <path>] --ordinals [<directory>]

--ordinals regenerates the offline fixture of the command ordinals, tests/spec/ordinals by default, from
its ordinals.wast: ordinals.json, `wasm-tools json-from-wast ordinals.wast` (which the self-tests read
instead of running wasm-tools), and ordinals.commands, its `--list-commands` listing (which
WastReader.ordinalsMatchWasmTools reads). Run it after editing ordinals.wast or bumping wasm-tools.
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
    """Hex-encodes a reason, message or divergence as UTF-8, as an .oracle manifest spells every one."""
    return text.encode("utf-8").hex()


class Unsupported(Exception):
    """What the importer cannot read, in a script or a module: the import fails on it."""


# The module commands that name a module: a skipped one poisons the name, and every command using it.
MODULE_COMMANDS = {"module", "module_definition"}

# The commands that link a module against the registered ones: a module among them that imports
# from a poisoned registration is skipped too.
LINKING_COMMANDS = {"module", "module_definition", "assert_unlinkable", "assert_uninstantiable"}

# The commands whose action uses a module, by name or as the current one.
ACTION_COMMANDS = {"action", "assert_return", "assert_trap", "assert_exhaustion", "assert_exception"}

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


def definitionOf(command):
    """The definition a module_instance instantiates. Whether it is poisoned is known only by its name, so
    it must be named."""
    if "module" not in command:
        raise Unsupported("module_instance of the latest definition, without its name")
    return command["module"]


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


def binaryModuleOf(command):
    """The binary module file of a command the JSON gives one of: a module wasm-tools encoded, from
    text or as bytes; None for a module given as text it could not encode (`module_type: text`)."""
    return command["filename"] if command.get("module_type") == "binary" else None


def encodedModuleOf(command):
    """The binary module file of a command that has a module wasm-tools encoded: that of a binary
    module, or a quote module's `binary_filename`; None for one it could not encode."""
    return binaryModuleOf(command) or command.get("binary_filename")


class Poison:
    """Why a module is skipped: the reason of the command it was rooted at, and that command's line."""

    def __init__(self, reason, line):
        self.reason = reason
        self.line = line

    def rootReason(self):
        return f"{self.reason} (wast line {self.line})"

    def dependentReason(self, line):
        return f"{self.reason} (root wast line {self.line}; wast line {line})"


class Poisoning:
    """Which modules of one .wast file are skipped, command by command in order: those the GC cut removes,
    and those that use a skipped one.

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

    def skipOf(self, command):
        """The reason the next command is skipped, or None when it is kept; records what it defines. Fails
        the import on what the importer cannot read, and on a module the cut and the suite disagree on."""
        where = command["line"]
        try:
            poison = self.poisonOf(command)
            reason = poison.dependentReason(where) if poison else None
            if poison is None:
                poison = self.cutOf(command)
                reason = poison.rootReason() if poison else None
        except Unsupported as unsupported:
            raise SystemExit(f"{self.source}:{where}: {unsupported}") from None
        self.update(command, poison)
        return reason

    def poisonOf(self, command):
        """The poison of the module command uses, or None when it uses none that is poisoned."""
        kind = command["type"]
        if kind in ACTION_COMMANDS or kind == "register":
            name = command["action"].get("module") if kind in ACTION_COMMANDS else command.get("name")
            return self.current if name is None else self.names.get(name)
        if kind == "module_instance":
            return self.names.get(definitionOf(command))
        filename = encodedModuleOf(command)
        if kind in LINKING_COMMANDS and filename is not None and any(self.registered.values()):
            for module in self.readImports(filename):
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

    def cutOf(self, command):
        """The Poison of a module the cut removes, or None when it keeps it. Fails the import when
        the oracle disagrees with the suite on anything but GC."""
        kind = command["type"]
        filename = encodedModuleOf(command)
        if self.validate is None or kind not in CUT_EXPECTATIONS or filename is None:
            return None
        where = command["line"]
        message = self.validate(filename)
        if message is not None and isGcRejection(message):
            return Poison(f"gc: {message}", where)
        if CUT_EXPECTATIONS[kind] == VALID and message is not None:
            raise SystemExit(f"{self.source}:{where}: {kind}: the cut rejects a module the suite expects "
                             f"valid, and not for GC: {message}")
        if CUT_EXPECTATIONS[kind] == REJECTED and message is None:
            raise SystemExit(f"{self.source}:{where}: {kind}: the cut accepts a module the suite expects "
                             "rejected")
        return None


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


# An import: the verbatim .wast and its oracle manifest, `<name>.oracle`.

# The forms a script gives a module in, as WastReader reads them: its text, its bytes
# (`module binary`), or strings of its text (`module quote`).
TEXT = "text"
BINARY = "binary"
QUOTE = "quote"

# The token after `module` (and its name) that gives a form other than text.
MODULE_FORMS = {"binary": BINARY, "quote": QUOTE}

# The tokens after `module` that say which module command it is, and the JSON command type of each; a
# `module` with neither is a `module`.
MODULE_VARIANTS = {"definition": "module_definition", "instance": "module_instance"}

# The script keywords whose JSON command type is another word; every other keyword is its own type.
SCRIPT_TYPES = {"invoke": "action", "get": "action"}

# The keywords of script commands. A script whose first list starts with another is an inline module:
# bare module fields, one `module` command of the whole text.
SCRIPT_KEYWORDS = {"module", "register", "invoke", "get", "assert_return", "assert_trap", "assert_exhaustion",
                   "assert_exception", "assert_invalid", "assert_malformed", "assert_unlinkable",
                   "assert_uninstantiable", "assert_invalid_custom", "assert_malformed_custom"}

# The JSON types of the commands that carry a module of their own: one given as text gets the oracle's
# binary of it as a `wasm` entry. assert_trap of a module is the JSON's assert_uninstantiable.
MODULE_BEARING = {"module", "module_definition", "assert_invalid", "assert_malformed", "assert_unlinkable",
                  "assert_uninstantiable", "assert_invalid_custom", "assert_malformed_custom"}


class ScriptCommand:
    """One top-level command of a .wast script as scanScript() reads it: its keyword, the line of its
    `(`, and the module it carries, if any."""

    def __init__(self, line):
        self.keyword = None
        self.line = line
        self.variant = None  # `definition` or `instance` after a `module` command's keyword
        self.hasModule = False  # whether it is, or holds, a `(module …)`
        self.form = None  # its module's form, TEXT, BINARY or QUOTE; None without one

    def jsonType(self):
        """The command type wasm-tools json-from-wast gives the command."""
        if self.keyword == "module":
            return MODULE_VARIANTS.get(self.variant, "module")
        if self.keyword == "assert_trap" and self.hasModule:
            return "assert_uninstantiable"
        return SCRIPT_TYPES.get(self.keyword, self.keyword)


def skipBlockComment(text, position, line):
    """Skips the block comment `(; … ;)` at position, nested ones included; returns the position after
    it and the line it ends on."""
    depth = 0
    while position < len(text):
        if text.startswith("(;", position):
            depth += 1
            position += 2
        elif text.startswith(";)", position):
            depth -= 1
            position += 2
            if depth == 0:
                return position, line
        else:
            line += text[position] == "\n"
            position += 1
    raise Unsupported(f"line {line}: an unterminated block comment")


def skipString(text, position, line):
    """Skips the string at position, its escapes included; returns the position after it and the line it
    ends on."""
    position += 1
    while position < len(text):
        character = text[position]
        if character == '"':
            return position + 1, line
        line += character == "\n"
        position += 2 if character == "\\" else 1
    raise Unsupported(f"line {line}: an unterminated string")


def skipAnnotation(text, position, line):
    """Skips the annotation `(@…)` at position, which a script reads as whitespace wherever it is, with
    the lists, strings and comments inside it; returns the position after it and the line it ends on."""
    depth = 0
    while position < len(text):
        character = text[position]
        if text.startswith(";;", position):
            end = text.find("\n", position)
            position = len(text) if end < 0 else end
        elif text.startswith("(;", position):
            position, line = skipBlockComment(text, position, line)
        elif character == '"':
            position, line = skipString(text, position, line)
        else:
            depth += (character == "(") - (character == ")")
            line += character == "\n"
            position += 1
            if depth == 0:
                return position, line
    raise Unsupported(f"line {line}: an unclosed annotation")


# The characters that end an atom: a string continues it, as in a quoted identifier `$"…"`.
DELIMITERS = set(" \t\r\n();")


def readToken(text, position, line):
    """Reads the atom or string at position; returns it, the position after it and the line it ends on."""
    start = position
    if text[position] == ";":  # a lone `;`, which is no comment
        return ";", position + 1, line
    while position < len(text) and text[position] not in DELIMITERS:
        if text[position] == '"':
            position, line = skipString(text, position, line)
        else:
            position += 1
    return text[start:position], position, line


def scanScript(text):
    """The top-level commands of a .wast script, in order, with the form of each one's module: one per
    directive, as wasm-tools json-from-wast counts them. A loop over the text, whatever its nesting."""
    commands = []
    depth = 0
    line = 1
    position = 0
    awaitingKeyword = False  # whether the next token is a list's keyword
    moduleDepth = None  # the depth of the `(module` whose form is not yet known
    current = None
    while position < len(text):
        character = text[position]
        if character == "\n":
            line += 1
            position += 1
        elif character in " \t\r":
            position += 1
        elif text.startswith(";;", position):
            end = text.find("\n", position)
            position = len(text) if end < 0 else end
        elif text.startswith("(;", position):
            position, line = skipBlockComment(text, position, line)
        elif text.startswith("(@", position):
            position, line = skipAnnotation(text, position, line)
        elif character in "()":
            if depth == moduleDepth:  # a field, or the end of an empty module: either is text
                current.form = TEXT
                moduleDepth = None
            if character == "(":
                depth += 1
                awaitingKeyword = True
                if depth == 1:
                    current = ScriptCommand(line)
                    commands.append(current)
            elif depth == 0:
                raise Unsupported(f"line {line}: an unbalanced ')'")
            else:
                depth -= 1
                awaitingKeyword = False
            position += 1
        else:
            token, position, line = readToken(text, position, line)
            if depth == 0:
                raise Unsupported(f"line {line}: '{token}' outside a command")
            if awaitingKeyword:
                awaitingKeyword = False
                if depth == 1:
                    current.keyword = token
                if token == "module" and depth <= 2 and not current.hasModule:
                    current.hasModule = True
                    moduleDepth = depth
            elif depth == moduleDepth:
                if token in MODULE_VARIANTS:
                    current.variant = token
                    if token == "instance":  # it instantiates a definition, and carries no module
                        current.hasModule = False
                        moduleDepth = None
                elif not token.startswith("$"):  # a name comes before the form
                    current.form = MODULE_FORMS.get(token, TEXT)
                    moduleDepth = None
    if depth != 0:
        raise Unsupported(f"line {line}: an unclosed '('")
    if commands and commands[0].keyword not in SCRIPT_KEYWORDS:
        inline = ScriptCommand(commands[0].line)
        inline.keyword = "module"
        inline.hasModule = True
        inline.form = TEXT
        return [inline]
    return commands


def moduleForms(source, script, commands):
    """The form of each JSON command's module, or None, from the script's own commands, which must be
    the JSON's one for one: the JSON alone cannot tell a module given as text from one given as bytes."""
    if len(script) != len(commands):
        raise SystemExit(f"{source}: the script reads as {len(script)} commands, but wasm-tools counts "
                         f"{len(commands)}")
    for ordinal, (scanned, command) in enumerate(zip(script, commands)):
        if scanned.jsonType() != command["type"]:
            raise SystemExit(f"{source}:{scanned.line}: command {ordinal} reads as {scanned.jsonType()}, but "
                             f"wasm-tools gives {command['type']} (line {command['line']})")
    return [scanned.form for scanned in script]


def listCommands(commands):
    """The `<ordinal> <type>` lines of JSON commands, as tests/spec/ordinals/ordinals.commands lists them."""
    return "".join(f"{ordinal} {command['type']}\n" for ordinal, command in enumerate(commands))


class Divergence:
    """A recorded divergence from the oracle, for one command of an imported file: an entry the
    importer writes into its .oracle, as the runner checks it (docs/conformance.md)."""

    def __init__(self, ordinal, line, kind, value):
        self.ordinal = ordinal
        self.line = line  # the line wasm-tools gives the command, so a moved command fails the import
        self.kind = kind
        self.value = value

    def __eq__(self, other):
        return vars(self) == vars(other)

    def __repr__(self):
        return f"Divergence({self.ordinal!r}, {self.line!r}, {self.kind!r}, {self.value!r})"


# A parity divergence's value: the first canonical offset the encodings differ at, as the runner prints it
# (lower-case hex, no leading zeros), the SHA-256 of Isoline's canonical encoding, then why.
PARITY_VALUE = re.compile(r"^0x(0|[1-9a-f][0-9a-f]*) sha256:[0-9a-f]{64} \S.*$")


def fitsMessage(divergence, command, form, entries):
    """Why a `message` divergence does not fit a command, or None: it is Isoline's message for an
    assert_malformed or assert_malformed_custom of a module given as text, and not the script's own."""
    if command["type"] not in ("assert_malformed", "assert_malformed_custom") or form not in (TEXT, QUOTE):
        module = f" of a {form} module" if form else ""
        return f"is no assert_malformed of a module given as text, but {command['type']}{module}"
    if divergence.value == command.get("text"):
        return "expects that very message: it is the script's own, so no divergence"
    return None


def fitsParity(divergence, command, form, entries):
    """Why a `parity` divergence does not fit a command, or None: the oracle gives a binary of its module."""
    if "wasm" not in entries:
        return "has no oracle binary"
    return None


# The kinds of recorded divergence: how its value is checked, and whether a command fits it.
DIVERGENCE_KINDS = {
    "message": {"value": re.compile(r"^\S.*$"), "fits": fitsMessage},
    "parity": {"value": PARITY_VALUE, "fits": fitsParity},
}


def readDivergences(path):
    """The recorded divergences of tests/spec/divergences.txt, by (source, path). A row is
    `<source> <path> <ordinal> <line> message <Isoline's message…>` or
    `<source> <path> <ordinal> <line> parity 0x<offset> sha256:<digest> <reason…>`."""
    divergences = {}
    if not path.exists():
        return divergences
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split(None, 5)
        kind = DIVERGENCE_KINDS.get(fields[4]) if len(fields) == 6 else None
        if kind is None or not fields[2].isdigit() or not fields[3].isdigit() or not kind["value"].match(fields[5]):
            raise SystemExit(f"{path}:{number}: expected '<source> <path> <ordinal> <line> message <message>' or "
                             f"'<source> <path> <ordinal> <line> parity 0x<offset> sha256:<digest> <reason>', got '{line}'")
        divergences.setdefault((fields[0], fields[1]), []).append(
            Divergence(int(fields[2]), int(fields[3]), fields[4], fields[5]))
    return divergences


# The order of a command's entries in the manifest.
ENTRY_ORDER = ("wasm", "skip", "message", "parity")


def oracleManifest(name, source, commands, forms, readImports, validate, divergences=()):
    """The .oracle manifest of an imported file, and the binaries it names.

    commands are its JSON commands, forms the form of each one's module (moduleForms), readImports and
    validate as for Poisoning, and divergences the file's recorded ones. Returns the manifest's text,
    the binaries to copy as {its file name: the JSON's file}, and how many commands it skips."""
    poisoning = Poisoning(source, readImports, validate)
    entries = [{} for _ in commands]
    binaries = {}
    for ordinal, (command, form) in enumerate(zip(commands, forms)):
        kind = command["type"]
        where = command["line"]
        encoded = encodedModuleOf(command)
        if kind in MODULE_BEARING and form in (TEXT, QUOTE) and encoded is not None:
            target = f"{name}.{ordinal}.wasm"
            binaries[target] = encoded
            entries[ordinal]["wasm"] = target
        if kind in MODULE_BEARING and encoded is None and CUT_EXPECTATIONS.get(kind) == VALID:
            raise SystemExit(f"{source}:{where}: {kind}: wasm-tools gives no binary of a module the suite "
                             "expects valid, so the cut cannot check it")
        reason = poisoning.skipOf(command)
        if reason is not None:
            entries[ordinal]["skip"] = hexOf(reason)
    for divergence in divergences:
        ordinal = divergence.ordinal
        what = f"{source}: the {divergence.kind} divergence of command {ordinal} (line {divergence.line})"
        if ordinal >= len(commands):
            raise SystemExit(f"{what}: the script has {len(commands)} commands")
        command = commands[ordinal]
        if command["line"] != divergence.line:
            raise SystemExit(f"{what}: the command is on line {command['line']}; the record is stale")
        if "skip" in entries[ordinal]:
            raise SystemExit(f"{what}: the command is skipped, so it is never checked")
        if divergence.kind in entries[ordinal]:
            raise SystemExit(f"{what}: a second one for the command")
        misfit = DIVERGENCE_KINDS[divergence.kind]["fits"](divergence, command, forms[ordinal], entries[ordinal])
        if misfit is not None:
            raise SystemExit(f"{what}: the command {misfit}")
        entries[ordinal][divergence.kind] = hexOf(divergence.value)
    lines = [f"commands {len(commands)}"]
    for ordinal, (command, entry) in enumerate(zip(commands, entries)):
        lines.extend(f"{ordinal} {command['line']} {kind} {entry[kind]}" for kind in ENTRY_ORDER if kind in entry)
    skipped = sum("skip" in entry for entry in entries)
    return "\n".join(lines) + "\n", binaries, skipped


def jsonFromWast(wasmTools, wast, work, cwd=None):
    """Runs `wasm-tools json-from-wast` on wast into the new directory work, from cwd if given (the JSON's
    source_filename is wast as given); returns its JSON commands."""
    work.mkdir(parents=True)
    jsonPath = work / f"{wast.stem}.json"
    result = subprocess.run(
        [str(wasmTools), "json-from-wast", str(wast), "-o", str(jsonPath), "--wasm-dir", str(work)],
        capture_output=True,
        text=True,
        check=False,
        cwd=cwd,
    )
    if result.returncode != 0:
        raise SystemExit(f"{wast.name}: wasm-tools json-from-wast failed:\n{result.stderr}")
    return json.loads(jsonPath.read_text(encoding="utf-8"))["commands"]


def convert(wasmTools, cutFeatures, wast, outDir, work, divergences):
    """Imports one .wast file into outDir: the verbatim <name>.wast, its <name>.oracle manifest
    and the oracle's binaries of its modules given as text, as <name>.<ordinal>.wasm; using work as
    scratch, and cutting what the oracle run with cutFeatures rejects as GC."""
    name = wast.stem
    commands = jsonFromWast(wasmTools, wast, work)
    script = wast.read_text(encoding="utf-8")
    try:
        forms = moduleForms(wast.name, scanScript(script), commands)
    except Unsupported as unsupported:
        raise SystemExit(f"{wast.name}: {unsupported}") from None
    manifest, binaries, skipped = oracleManifest(
        name, wast.name, commands, forms, lambda filename: importModuleNames((work / filename).read_bytes()),
        lambda filename: oracleMessage(wasmTools, cutFeatures, work / filename), divergences)

    if outDir.exists():
        shutil.rmtree(outDir)
    outDir.mkdir(parents=True)
    shutil.copyfile(wast, outDir / f"{name}.wast")
    for target, filename in binaries.items():
        shutil.copyfile(work / filename, outDir / target)
    (outDir / f"{name}.oracle").write_text(manifest, encoding="utf-8", newline="\n")
    return len(commands), skipped


# The recorded divergences from the oracle, beside files.txt.
DIVERGENCES = "divergences.txt"


def oracleDivergences(manifest):
    """The divergences an .oracle manifest carries, its entries of a kind in DIVERGENCE_KINDS, in manifest
    order: what oracleManifest() wrote of the recorded ones."""
    divergences = []
    for line in manifest.splitlines()[1:]:
        fields = line.split(" ")
        if len(fields) == 4 and fields[2] in DIVERGENCE_KINDS:
            divergences.append(Divergence(int(fields[0]), int(fields[1]), fields[2],
                                          bytes.fromhex(fields[3]).decode("utf-8")))
    return divergences


def checkOracleDivergences(spec, divergences, rows):
    """Fails unless the .oracle of every imported row under spec, the tests/spec directory,
    carries exactly the divergences recorded for it: an edit of divergences.txt not re-imported, or of an
    .oracle by hand, would otherwise go unseen until the next import."""
    def order(divergence):
        return divergence.ordinal, divergence.kind

    for row in rows:
        if row.exclude is not None:
            continue
        oracle = (row.directory(spec) / pathlib.PurePosixPath(row.path).name).with_suffix(".oracle")
        carried = sorted(oracleDivergences(oracle.read_text(encoding="utf-8")), key=order)
        recorded = sorted(divergences.get((row.source, row.path), []), key=order)
        if carried != recorded:
            raise SystemExit(f"{oracle}: carries the divergences {carried}, but {DIVERGENCES} records {recorded}; "
                             "re-import the file")


def checkDivergenceRows(divergences, rows):
    """Fails unless every recorded divergence names an imported row: an excluded or unlisted one is never
    imported, so its divergences would be stale unseen."""
    imported = {(row.source, row.path) for row in rows if row.exclude is None}
    for source, path in divergences:
        if (source, path) not in imported:
            raise SystemExit(f"{DIVERGENCES}: {source} {path} is not imported, so its divergences "
                             "are never checked")


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


# The offline fixture of the command ordinals (--ordinals).
ORDINALS = ROOT / "tests" / "spec" / "ordinals"


def ordinalsFixture(rawJson):
    """The files of the ordinals fixture, {name: text}, from the JSON `wasm-tools json-from-wast
    ordinals.wast` writes: that JSON with a final newline, and its `--list-commands` listing."""
    return {"ordinals.json": rawJson.rstrip("\n") + "\n",
            "ordinals.commands": listCommands(json.loads(rawJson)["commands"])}


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
    parser.add_argument("--list-commands", type=pathlib.Path, metavar="WAST",
                        help="print the `<ordinal> <type>` of each command of one .wast file, and import nothing")
    parser.add_argument("--ordinals", type=pathlib.Path, nargs="?", const=ORDINALS, metavar="DIRECTORY",
                        help="regenerate the ordinals fixture in DIRECTORY (default: tests/spec/ordinals) from "
                             "its ordinals.wast, and import nothing")
    arguments = parser.parse_args()

    fetchTools = loadFetchTools()
    lock = fetchTools.readLock(LOCK)
    wasmTools = arguments.wasm_tools or fetchTools.ensureWasmTools(LOCK)
    if arguments.wasm_tools:
        print(f"warning: {arguments.wasm_tools} is not checked against the wasm-tools hash pinned in "
              f"{LOCK.name}; the import is reproducible only with the pinned build", file=sys.stderr)
    if arguments.list_commands:
        with tempfile.TemporaryDirectory(prefix="isoline-spec-") as temporary:
            commands = jsonFromWast(wasmTools, arguments.list_commands, pathlib.Path(temporary) / "json")
        sys.stdout.write(listCommands(commands))
        return 0
    if arguments.ordinals:
        with tempfile.TemporaryDirectory(prefix="isoline-spec-") as temporary:
            work = pathlib.Path(temporary) / "json"
            # From the fixture's directory, so that the JSON names the script as the committed one does.
            jsonFromWast(wasmTools, pathlib.Path("ordinals.wast"), work, cwd=arguments.ordinals)
            rawJson = (work / "ordinals.json").read_text(encoding="utf-8")
        for name, text in ordinalsFixture(rawJson).items():
            (arguments.ordinals / name).write_text(text, encoding="utf-8", newline="\n")
        return 0
    cutFeatures = lock.get("cut.features")
    if cutFeatures is None:
        raise SystemExit(f"{LOCK.name} pins no cut.features")
    rows = [row for row in readFiles(arguments.out / "files.txt") if row.exclude is None]
    divergences = readDivergences(arguments.out / DIVERGENCES)
    checkDivergenceRows(divergences, rows)
    given = dict(arguments.source)

    with tempfile.TemporaryDirectory(prefix="isoline-spec-") as temporary:
        temporary = pathlib.Path(temporary)
        checkouts = {}
        mismatches = []
        for row in rows:
            if row.source not in checkouts:
                checkouts[row.source] = checkout(row.source, lock, given, temporary)
            work = temporary / "json" / row.source / row.path
            wast = checkouts[row.source] / "test" / f"{row.path}.wast"
            commands, skipped = convert(wasmTools, cutFeatures, wast, row.directory(arguments.out), work,
                                        divergences.get((row.source, row.path), []))
            print(f"{row.source} {row.path}: {commands} commands, {skipped} skipped")
            if skipped != row.skips:
                mismatches.append(f"{row.source} {row.path}: {skipped} skipped, files.txt pins {row.skips}")
        if mismatches:
            raise SystemExit("skip counts differ from files.txt; update it only if each new skip is meant:\n  "
                             + "\n  ".join(mismatches))


if __name__ == "__main__":
    sys.exit(main())
