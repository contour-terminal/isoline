#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of c-api-noexcept.py: it passes the C API's own shapes, and reports each way a declaration, a
function pointer or a definition can leave them."""

import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "c-api-noexcept.py"

GOOD_HEADER = """/* A header of the boundary's shape. */
#pragma once
#include <isoline/c/export.h>
#if defined(__cplusplus)
extern "C"
{
#endif
    typedef struct isoline_thing_t isoline_thing_t;
    /** A callback; "(*" in a comment or a string is no declarator. */
    typedef uint32_t(ISOLINE_CALL* isoline_callback_t)(void* context, uint32_t value);
    struct isoline_holder_t
    {
        void (ISOLINE_CALL* finalizer)(void*);
    };
    ISOLINE_API uint32_t ISOLINE_CALL isoline_value(void) ISOLINE_NOEXCEPT;
    ISOLINE_API isoline_thing_t* ISOLINE_CALL isoline_thing_new(
        isoline_callback_t callback, void* context, void (ISOLINE_CALL* finalizer)(void*)) ISOLINE_NOEXCEPT;
#if defined(__cplusplus)
}
#endif
"""

GOOD_SOURCE = """// A source of the boundary's shape.
#include <isoline/c/isoline.h>

namespace
{
    auto wasm_helper(int value) -> int { int doubled = value * 2; return doubled; } // a helper, not the C API's
}

uint32_t ISOLINE_CALL isoline_value() noexcept
{
    auto const unused = 1; // "{" in a string or a comment is no brace
    return 0;
}

wasm_engine_t* wasm_engine_new()
{
    return isoline::c::detail::boundary([] { auto* engine = make(); return engine; }, [] { return nullptr; });
}

void wasm_engine_delete(wasm_engine_t* engine)
{
    detail::boundary([&] { delete engine; }, [] {});
}
"""


class CApiNoexceptTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def write(self, name, text):
        path = pathlib.Path(self.directory.name) / name
        path.write_text(text, encoding="utf-8")
        return path

    def run_(self, *paths):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), *map(str, paths)], capture_output=True,
                              text=True, check=False)

    def assertPasses(self, *paths):
        result = self.run_(*paths)
        self.assertEqual(result.returncode, 0, result.stderr)

    def assertReports(self, path, expected, *options):
        result = self.run_(*options, path)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(expected, result.stderr)

    def testTheBoundarysShapesPass(self):
        self.assertPasses(self.write("good.h", GOOD_HEADER), self.write("good.cpp", GOOD_SOURCE))

    def testTheCApisOwnFilesPass(self):
        root = SCRIPT.parent.parent / "src" / "isoline" / "c"
        files = sorted(root.rglob("*.h")) + [path for path in sorted(root.rglob("*.cpp"))
                                             if not path.name.endswith("_test.cpp")]
        self.assertPasses("--symbols", root / "exported-symbols.txt", *files)

    def testDeclarationWithoutNoexceptIsReported(self):
        header = GOOD_HEADER.replace("isoline_value(void) ISOLINE_NOEXCEPT;", "isoline_value(void);")
        self.assertReports(self.write("bad.h", header), "isoline_value: a declaration is")

    def testDeclarationWithoutExportIsReported(self):
        header = GOOD_HEADER.replace("ISOLINE_API uint32_t ISOLINE_CALL isoline_value",
                                     "uint32_t ISOLINE_CALL isoline_value")
        self.assertReports(self.write("bad.h", header), "isoline_value: a declaration is")

    def testDeclarationWithoutConventionIsReported(self):
        header = GOOD_HEADER.replace("uint32_t ISOLINE_CALL isoline_value", "uint32_t isoline_value")
        self.assertReports(self.write("bad.h", header), "isoline_value: a declaration is")

    def testConventionInTheWrongPlaceIsReported(self):
        header = GOOD_HEADER.replace("ISOLINE_API uint32_t ISOLINE_CALL isoline_value",
                                     "ISOLINE_CALL ISOLINE_API uint32_t isoline_value")
        self.assertReports(self.write("bad.h", header), "isoline_value: a declaration is")

    def testCallbackTypedefWithoutConventionIsReported(self):
        header = GOOD_HEADER.replace("(ISOLINE_CALL* isoline_callback_t)", "(*isoline_callback_t)")
        self.assertReports(self.write("bad.h", header), "isoline_callback_t: a function pointer is")

    def testFinalizerParameterWithoutConventionIsReported(self):
        header = GOOD_HEADER.replace("void* context, void (ISOLINE_CALL* finalizer)(void*)",
                                     "void* context, void (*finalizer)(void*)")
        self.assertReports(self.write("bad.h", header), "finalizer: a function pointer is")

    def testFieldWithoutConventionIsReported(self):
        header = GOOD_HEADER.replace("        void (ISOLINE_CALL* finalizer)(void*);\n",
                                     "        void (*finalizer)(void*);\n")
        self.assertReports(self.write("bad.h", header), "finalizer: a function pointer is")

    def testIsolineDefinitionWithoutNoexceptIsReported(self):
        source = GOOD_SOURCE.replace("isoline_value() noexcept", "isoline_value()")
        self.assertReports(self.write("bad.cpp", source), "isoline_value: a definition is")

    def testWasmBodyOfTwoStatementsIsReported(self):
        source = GOOD_SOURCE.replace("    detail::boundary([&] { delete engine; }, [] {});\n",
                                     "    log();\n    detail::boundary([&] { delete engine; }, [] {});\n")
        self.assertReports(self.write("bad.cpp", source), "wasm_engine_delete: its body is one statement")

    def testWasmBodyWithoutBoundaryIsReported(self):
        source = GOOD_SOURCE.replace("    detail::boundary([&] { delete engine; }, [] {});\n",
                                     "    delete engine;\n")
        self.assertReports(self.write("bad.cpp", source), "wasm_engine_delete: its body is one statement")

    # Ways to fool the checks, each of which once did.

    def testDigitSeparatorOpensNoLiteral(self):
        # Read as a character literal, the quote of 4'096 would run to the next quote and hide what lies
        # between, the bad body among it.
        source = GOOD_SOURCE.replace(
            "void wasm_engine_delete(wasm_engine_t* engine)\n{\n    detail::boundary([&] { delete engine; }, [] {});\n}\n",
            "constexpr auto Bytes = 4'096;\nvoid wasm_engine_delete(wasm_engine_t* engine)\n{\n    log();\n"
            "    detail::boundary([&] { delete engine; }, [] {});\n}\nconstexpr auto More = 1'000;\n")
        self.assertReports(self.write("bad.cpp", source), "wasm_engine_delete: its body is one statement")

    def testCommaOperatorAfterBoundaryIsReported(self):
        source = GOOD_SOURCE.replace("    detail::boundary([&] { delete engine; }, [] {});\n",
                                     "    detail::boundary([&] { delete engine; }, [] {}), log();\n")
        self.assertReports(self.write("bad.cpp", source), "wasm_engine_delete: its body is one statement")

    def testCallAfterBoundaryIsReported(self):
        source = GOOD_SOURCE.replace("return isoline::c::detail::boundary([] {", "return isoline::c::detail::boundary([] {", 1)
        source = source.replace("[] { return nullptr; });", "[] { return nullptr; }) + offset();")
        self.assertReports(self.write("bad.cpp", source), "wasm_engine_new: its body is one statement")

    def testConstFunctionPointerWithoutConventionIsReported(self):
        header = GOOD_HEADER.replace("        void (ISOLINE_CALL* finalizer)(void*);\n",
                                     "        void (*const finalizer)(void*);\n")
        self.assertReports(self.write("bad.h", header), "finalizer: a function pointer is")

    def testConstFunctionPointerWithConventionPasses(self):
        header = GOOD_HEADER.replace("        void (ISOLINE_CALL* finalizer)(void*);\n",
                                     "        void (ISOLINE_CALL* const finalizer)(void*);\n")
        self.assertPasses(self.write("good.h", header))

    def testMacroGeneratedDefinitionIsReported(self):
        # A definition a macro makes is none the checks see: the symbol it exports has no checked definition.
        source = GOOD_SOURCE + ("#define DEFINE_DELETE(name) \\\n    void wasm_##name##_delete(wasm_##name##_t* x) { delete x; }\n"
                                "DEFINE_DELETE(store)\n")
        symbols = self.write("symbols.txt", "isoline_value\nwasm_engine_delete\nwasm_engine_new\nwasm_store_delete\n")
        self.assertReports(self.write("bad.cpp", source), "wasm_store_delete: exported, and has 0 checked definitions",
                           "--symbols", symbols)

    def testMacroHiddenBodyIsReported(self):
        source = GOOD_SOURCE + "#define BODY { delete store; }\nvoid wasm_store_delete(wasm_store_t* store) BODY\n"
        symbols = self.write("symbols.txt", "isoline_value\nwasm_engine_delete\nwasm_engine_new\nwasm_store_delete\n")
        self.assertReports(self.write("bad.cpp", source), "wasm_store_delete: exported, and has 0 checked definitions",
                           "--symbols", symbols)

    def testTwoDefinitionsOfASymbolAreReported(self):
        source = GOOD_SOURCE + "uint32_t ISOLINE_CALL isoline_value() noexcept\n{\n    return 1;\n}\n"
        symbols = self.write("symbols.txt", "isoline_value\nwasm_engine_delete\nwasm_engine_new\n")
        self.assertReports(self.write("bad.cpp", source), "isoline_value: exported, and has 2 checked definitions",
                           "--symbols", symbols)

    def testEverySymbolDefinedOncePasses(self):
        symbols = self.write("symbols.txt", "isoline_value\nwasm_engine_delete\nwasm_engine_new\n")
        self.assertPasses("--symbols", symbols, self.write("good.cpp", GOOD_SOURCE))

    def testFilesThatCheckNothingFail(self):
        self.assertReports(self.write("empty.h", "#pragma once\n"), "the files check nothing")

    def testAnotherKindOfFileFails(self):
        self.assertReports(self.write("other.c", "int x;\n"), "neither a C header")


if __name__ == "__main__":
    unittest.main()
