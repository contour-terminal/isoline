#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""The Spectre select check's decisions, on disassembly fixtures: no compiler or object needed."""

import importlib.util
import pathlib
import sys
import unittest

_spec = importlib.util.spec_from_file_location(
    "check_spectre_select", pathlib.Path(__file__).resolve().parent / "check-spectre-select.py")
check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check)

OPERATIONS = """\
ISOLINE_INTERPRETER_OPERATION(Nop, "nop", Nullary, 0, 0, Control, Unforwardable)
ISOLINE_INTERPRETER_OPERATION(I32Load, "i32.load", Load, 2, 1, Memory, AfterOperands)
ISOLINE_INTERPRETER_OPERATION(I32Store, "i32.store", Store, 2, 1, Memory, Unforwardable)
ISOLINE_INTERPRETER_OPERATION(LoadIndexed, "load.indexed", Load, 2, 2, Variable, Unforwardable)
"""

SELECTING = "  56:\tcmovns %r10,%r14\n  5a:\tmov    (%rcx,%r14,1),%r10d\n"
BRANCHING = "  3b:\tcmp    %r8,%rbx\n  3e:\tja     90 <x>\n  5a:\tmov    (%rcx,%r10,1),%r10d\n"
CALLING = ("  3d:\tcall   42 <x>\n\t\t\t3e: R_X86_64_PLT32\t"
           "_ZN7isoline11interpreter15memoryOperationILNS0_11OperationIdE1EEENS0_4FlowERNS0_5StateE-0x4\n")
CALLEE = ".text._ZN7isoline11interpreter15memoryOperationILNS0_11OperationIdE1EEENS0_4FlowERNS0_5StateE"


class SpectreSelectTest(unittest.TestCase):
    def test_checked_rows_are_the_memory_families_loads_and_stores(self):
        self.assertEqual(check.checkedOperations(OPERATIONS), [(1, "I32Load"), (2, "I32Store")])

    def test_a_handler_without_a_select_is_refused(self):
        sections = {check.handlerSection(1): SELECTING, check.handlerSection(2): BRANCHING}
        problems = check.findings(lambda section: sections.get(section, ""), [(1, "I32Load"), (2, "I32Store")])
        self.assertEqual(problems, ["I32Store: no conditional select: the Spectre mask was folded into a branch"])

    def test_a_missing_handler_is_refused(self):
        self.assertEqual(check.findings(lambda section: "", [(1, "I32Load")]), ["I32Load: no handler in the object"])

    def test_the_select_of_a_callee_counts(self):
        sections = {check.handlerSection(1): CALLING, CALLEE: SELECTING}
        self.assertEqual(check.findings(lambda section: sections.get(section, ""), [(1, "I32Load")]), [])
        sections[CALLEE] = BRANCHING
        self.assertEqual(len(check.findings(lambda section: sections.get(section, ""), [(1, "I32Load")])), 1)

    def test_an_aarch64_select_counts(self):
        sections = {check.handlerSection(1): "  40:\tcsel\tx10, x9, xzr, ge\n"}
        self.assertEqual(check.findings(lambda section: sections.get(section, ""), [(1, "I32Load")]), [])


if __name__ == "__main__":
    sys.exit(unittest.main())
