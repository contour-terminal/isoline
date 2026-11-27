#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of c-api-symbols.py: what it finds in fixture headers and in the vendored wasm.h, how it
judges a list file, and how it reads each object format's export listing.

The C preprocessor is the one the build uses, given as the first argument (default: cc)."""

import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "c-api-symbols.py"
INCLUDES = [ROOT / "src", ROOT / "vendor" / "wasm-c-api" / "include"]
COMPILER = "cc"

specification = importlib.util.spec_from_file_location("c_api_symbols", SCRIPT)
symbols = importlib.util.module_from_spec(specification)
specification.loader.exec_module(symbols)


class CApiSymbolsTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def declared(self, text):
        header = pathlib.Path(self.directory.name) / "fixture.h"
        header.write_text(text, encoding="utf-8")
        return symbols.declaredNames(symbols.preprocess(COMPILER, INCLUDES, header))

    def run_(self, *arguments):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), *map(str, arguments)], capture_output=True,
                              text=True, check=False)

    def writeList(self, names):
        path = pathlib.Path(self.directory.name) / "list.txt"
        path.write_text("".join(f"{name}\n" for name in names), encoding="utf-8")
        return path

    def testIsolineDeclarationIsFound(self):
        found = self.declared("#include <isoline/c/export.h>\n#include <stdint.h>\n"
                              "ISOLINE_API uint32_t ISOLINE_CALL isoline_x(void) ISOLINE_NOEXCEPT;\n"
                              "uint32_t ISOLINE_CALL isoline_not_exported(void) ISOLINE_NOEXCEPT;\n")
        self.assertEqual(found, ["isoline_x"])

    def testDeclareVecGivesFiveNames(self):
        wasmAlone = set(self.declared("#include <wasm.h>\n"))
        withVector = set(self.declared("#include <wasm.h>\ntypedef char wasm_fixture_t;\n"
                                       "WASM_DECLARE_VEC(fixture, char)\n"))
        self.assertEqual(sorted(withVector - wasmAlone),
                         [f"wasm_fixture_vec_{suffix}" for suffix in
                          ("copy", "delete", "new", "new_empty", "new_uninitialized")])

    def testVendoredWasmHeaderDeclares293Functions(self):
        # Plan decision D1: wasm-c-api 9d6b937 declares 293 WASM_API_EXTERN functions after preprocessing.
        found = self.declared("#include <wasm.h>\n")
        self.assertEqual(len(found), 293)
        self.assertTrue(all(name.startswith("wasm_") for name in found))

    def testTheCheckedInListIsAnImplementedSubset(self):
        result = self.run_("check-list", "--compiler", COMPILER, *[f"-I{include}" for include in INCLUDES],
                           "--list", ROOT / "src/isoline/c/exported-symbols.txt", "--implemented",
                           ROOT / "src/isoline/c/isoline.h")
        self.assertEqual(result.returncode, 0, result.stderr)

    def testListProblems(self):
        declared = ["isoline_a", "isoline_b"]
        self.assertEqual(symbols.listProblems(["isoline_a"], declared, implemented=True), [])
        self.assertEqual(symbols.listProblems(["isoline_a"], declared, implemented=False),
                         ["isoline_b: declared, but not listed"])
        self.assertEqual(symbols.listProblems(["isoline_c"], declared, implemented=True),
                         ["isoline_c: listed, but the header declares no such function"])

    def testListFormIsChecked(self):
        _, problems = symbols.readList(self.writeList(["isoline_b", "isoline_a"]))
        self.assertIn("the names are not sorted", problems[0])
        _, problems = symbols.readList(self.writeList(["isoline_a", "isoline_a"]))
        self.assertIn("a name appears twice", problems[0])
        _, problems = symbols.readList(self.writeList(["isoline a"]))
        self.assertIn("is not one name", problems[0])

    def testElfExportsIgnoreTheVersionNode(self):
        listing = ("0000000000000000 A ISOLINE_C_0.1\n"
                   "0000000000001100 T isoline_abi_version@@ISOLINE_C_0.1\n"
                   "0000000000001000 T _init\n")
        exported, unreadable = symbols.exportedNames(listing, "elf")
        self.assertEqual(unreadable, [])
        self.assertEqual(exported, ["_init", "isoline_abi_version"])
        self.assertEqual(symbols.exportProblems(exported, ["isoline_abi_compatible", "isoline_abi_version"]),
                         ["_init: exported, but not listed", "isoline_abi_compatible: listed, but not exported"])

    def testPeAndMachOExports(self):
        pe = ("File: isoline_c-0.1.dll\nFormat: COFF-x86-64\nExport {\n  Ordinal: 1\n"
              "  Name: isoline_abi_compatible\n  RVA: 0x1000\n}\n")
        self.assertEqual(symbols.exportedNames(pe, "pe"), (["isoline_abi_compatible"], []))
        macho = "0000000000003f80 T _isoline_abi_version\n"
        self.assertEqual(symbols.exportedNames(macho, "macho"), (["isoline_abi_version"], []))

    def testUnreadableListingLinesAreRefused(self):
        # A tool whose output changed shape must fail the check, not export nothing and pass.
        for exportFormat, listing in (("elf", "isoline_abi_version is at 0x1100\n"),
                                      ("pe", "Export {\n  Symbol: isoline_abi_version\n}\n"),
                                      ("macho", "isoline_abi_version\n")):
            with self.subTest(exportFormat):
                names, unreadable = symbols.exportedNames(listing, exportFormat)
                self.assertEqual(names, [])
                self.assertEqual(len(unreadable), 1, unreadable)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        COMPILER = sys.argv.pop(1)
    unittest.main()
