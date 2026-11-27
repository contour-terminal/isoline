#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of c-api-vectors.py: it writes five definitions per row and checks its own output, and it reports
a stale file, a malformed or repeated row, an ownership that is none, and rows that differ from wasm.h's
families."""

import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "c-api-vectors.py"

ROWS = """// Rows.
#if !defined(ISOLINE_VECTOR_ROW)
    #error "define ISOLINE_VECTOR_ROW"
#endif
ISOLINE_VECTOR_ROW(byte, Plain)
ISOLINE_VECTOR_ROW(valtype, Owned)
ISOLINE_VECTOR_ROW(val, Value)
"""

HEADER = """#define WASM_DECLARE_VEC(name, ptr_or_none) \\
  typedef struct wasm_##name##_vec_t { size_t size; wasm_##name##_t ptr_or_none* data; } wasm_##name##_vec_t;
#define WASM_DECLARE_TYPE(name) \\
  WASM_DECLARE_VEC(name, *)
WASM_DECLARE_VEC(byte, )
WASM_DECLARE_TYPE(valtype)
WASM_DECLARE_VEC(val, )
"""


class CApiVectorsTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.rows = self.write("VectorRows.def", ROWS)
        self.header = self.write("wasm.h", HEADER)
        self.output = pathlib.Path(self.directory.name) / "WasmVectors.cpp"

    def write(self, name, text):
        path = pathlib.Path(self.directory.name) / name
        path.write_text(text, encoding="utf-8")
        return path

    def run_(self, command, *options):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), command, "--rows", str(self.rows), "--output",
                               str(self.output), *options], capture_output=True, text=True, check=False)

    def assertReports(self, expected, command="check", *options):
        result = self.run_(command, *options)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(expected, result.stderr)

    def testWritesFiveDefinitionsPerRowAndChecksThem(self):
        self.assertEqual(self.run_("write").returncode, 0)
        text = self.output.read_text(encoding="utf-8")
        for name in ("byte", "valtype", "val"):
            for operation in ("new_empty", "new_uninitialized", "new", "copy", "delete"):
                self.assertEqual(text.count(f"void wasm_{name}_vec_{operation}("), 1, f"{name} {operation}")
        self.assertIn("wasm_valtype_t* const data[]", text)
        self.assertIn("ElementOwnership::Value>(out, size, data)", text)
        result = self.run_("check", "--header", str(self.header))
        self.assertEqual(result.returncode, 0, result.stderr)

    def testStaleFileIsReported(self):
        self.assertEqual(self.run_("write").returncode, 0)
        self.output.write_text(self.output.read_text(encoding="utf-8") + "// edited\n", encoding="utf-8")
        self.assertReports("differs from what")

    def testNewRowMakesTheFileStale(self):
        self.assertEqual(self.run_("write").returncode, 0)
        self.rows.write_text(ROWS + "ISOLINE_VECTOR_ROW(frame, Owned)\n", encoding="utf-8")
        self.assertReports("differs from what")

    def testMissingFileIsReported(self):
        self.assertReports("differs from what")

    def testMalformedRowIsReported(self):
        self.rows.write_text(ROWS + "ISOLINE_VECTOR_ROW(frame)\n", encoding="utf-8")
        self.assertReports("is not ISOLINE_VECTOR_ROW(name, ownership)", "write")

    def testUnknownOwnershipIsReported(self):
        self.rows.write_text(ROWS + "ISOLINE_VECTOR_ROW(frame, Borrowed)\n", encoding="utf-8")
        self.assertReports("'Borrowed' is no ownership", "write")

    def testRepeatedRowIsReported(self):
        self.rows.write_text(ROWS + "ISOLINE_VECTOR_ROW(byte, Plain)\n", encoding="utf-8")
        self.assertReports("byte: a second row of the family", "write")

    def testNoRowIsReported(self):
        self.rows.write_text("// nothing\n", encoding="utf-8")
        self.assertReports("no row", "write")

    def testFamilyWithoutRowIsReported(self):
        self.assertEqual(self.run_("write").returncode, 0)
        self.header.write_text(HEADER + "WASM_DECLARE_VEC(frame, *)\n", encoding="utf-8")
        self.assertReports("wasm_frame_vec_t: declared by wasm.h, and no row", "check", "--header", str(self.header))

    def testRowWithoutFamilyIsReported(self):
        self.rows.write_text(ROWS + "ISOLINE_VECTOR_ROW(frame, Owned)\n", encoding="utf-8")
        self.assertEqual(self.run_("write").returncode, 0)
        self.assertReports("wasm_frame_vec_t: a row of VectorRows.def, and wasm.h declares no such family", "check",
                           "--header", str(self.header))

    def testOwnershipDifferingFromWasmHIsReported(self):
        self.rows.write_text(ROWS.replace("valtype, Owned", "valtype, Plain"), encoding="utf-8")
        self.assertEqual(self.run_("write").returncode, 0)
        self.assertReports("wasm_valtype_vec_t: wasm.h's elements are own pointers, and the row says Plain",
                           "check", "--header", str(self.header))


if __name__ == "__main__":
    unittest.main()
