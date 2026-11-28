#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Checks that the interpreter's Spectre select survives optimization (docs/interpreter.md, "Spectre").

Every memory-0 load and store masks its offset with accessMask() (src/isoline/interpreter/Bounds.hpp), a
data-dependent select derived from the bound, not from the check's outcome. An optimizer may legally fold
it into the check's branch, which leaves the guest's offset to a speculative access. This script
disassembles the tail-call handler of every Load and Store row of the Memory family of Operations.def in
an optimized object of TailCallMemory.cpp and refuses a handler without a conditional select (x86-64:
cmov*, AArch64: csel/csinv/csneg/cset) or without the object holding it. A handler that calls the access
out of line (the lane accesses do) has its select in the callee: the interpreter's own functions it calls
directly, in the same object, are searched too, to any depth.

Usage: check-spectre-select.py --object OBJECT... [--objdump objdump] [--operations Operations.def]
The objects may be a library's whole list ($<TARGET_OBJECTS:...>): the one of TailCallMemory.cpp is checked.
"""

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OPERATIONS = ROOT / "src" / "isoline" / "interpreter" / "Operations.def"
# The forms and family of the rows whose handlers bounds-check memory 0.
CHECKED_FORMS = ("Load", "Store")
CHECKED_FAMILY = "Memory"
# A conditional select, per instruction set: what the mask compiles to where it survives.
SELECT = re.compile(r"\s(cmov[a-z]*|csel|csinv|csneg|cset[m]?)\s")
# A direct call's relocation to one of the interpreter's own functions: its mangled name.
CALLEE = re.compile(r"R_(?:X86_64_PLT32|X86_64_PC32|AARCH64_CALL26|AARCH64_JUMP26)\s+(_ZN7isoline11interpreter\w+?)(?:[-+]0x[0-9a-f]+)?$",
                    re.MULTILINE)
ROW = re.compile(r'ISOLINE_INTERPRETER_OPERATION\((\w+), "([^"]*)", (\w+), \d+, \d+, (\w+)')


def checkedOperations(operationsText):
    """[(OperationId value, name)] of the rows whose handler masks a memory-0 access, by row order."""
    rows = [line for line in operationsText.splitlines() if line.startswith("ISOLINE_INTERPRETER_OPERATION(")]
    checked = []
    for index, line in enumerate(rows):
        match = ROW.match(line)
        if match and match.group(3) in CHECKED_FORMS and match.group(4) == CHECKED_FAMILY:
            checked.append((index, match.group(1)))
    return checked


def handlerSection(operation):
    """The section a handler<OperationId(operation)> template instance is emitted into (-ffunction-sections
    or COMDAT): its mangled name after .text."""
    return (f".text._ZN7isoline11interpreter7handlerILNS0_11OperationIdE{operation}EEENS0_4ExitEPKmPmPNS0_7"
            "ContextEPSt4bytemm")


def selects(disassembleSection, section):
    """Whether @p section, or a function of the interpreter it calls directly (to any depth), holds a
    conditional select."""
    pending = [section]
    seen = set()
    while pending:
        current = pending.pop()
        if current in seen:
            continue
        seen.add(current)
        text = disassembleSection(current)
        if SELECT.search(text):
            return True
        pending.extend(f".text.{callee}" for callee in CALLEE.findall(text))
    return False


def findings(disassembleSection, operations):
    """The handlers without a select, as messages; @p disassembleSection gives a section's disassembly with
    its relocations, or an empty string where the object has no such section."""
    problems = []
    for value, name in operations:
        section = handlerSection(value)
        if not disassembleSection(section).strip():
            problems.append(f"{name}: no handler in the object")
        elif not selects(disassembleSection, section):
            problems.append(f"{name}: no conditional select: the Spectre mask was folded into a branch")
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--object", required=True, nargs="+")
    parser.add_argument("--objdump", default="objdump")
    parser.add_argument("--operations", default=str(OPERATIONS))
    arguments = parser.parse_args(argv)
    operations = checkedOperations(pathlib.Path(arguments.operations).read_text())
    # A CMake list reaches the script as one argument where the generator expression is not expanded.
    objects = [path for argument in arguments.object for path in argument.split(";") if path]
    memoryObjects = [path for path in objects if pathlib.Path(path).name.startswith("TailCallMemory.")]
    if len(memoryObjects) != 1:
        print(f"check-spectre-select: no one TailCallMemory object among {objects}", file=sys.stderr)
        return 2

    def disassemble(section):
        completed = subprocess.run([arguments.objdump, "-dr", "--no-show-raw-insn", "-j", section, memoryObjects[0]],
                                   capture_output=True, text=True, check=False)
        return completed.stdout if completed.returncode == 0 and "<" in completed.stdout else ""

    problems = findings(disassemble, operations)
    for problem in problems:
        print(problem)
    if problems:
        print(f"check-spectre-select: {len(problems)} of {len(operations)} handlers", file=sys.stderr)
        return 1
    print(f"check-spectre-select: {len(operations)} memory-0 handlers each select their offset")
    return 0


if __name__ == "__main__":
    sys.exit(main())
