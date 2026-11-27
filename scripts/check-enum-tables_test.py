#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of check-enum-tables.py: runs it on fixture trees and checks its verdicts."""

import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "check-enum-tables.py"

HEADER = """\
enum class Kind : uint8_t { A, B };
struct HeaderRow
{
    Kind kind; ///< Declared in a header, used by a source file's table.
    int payload;
};
"""

SOURCE = """\
namespace
{
    struct Row
    {
        Kind kind; ///< The key.
        int payload;
    };
    struct Plain
    {
        int number;
        Kind kind;
    };
    constexpr auto Registered = std::to_array<Row>({ { Kind::A, 1 } });
    constexpr auto NotEnumFirst = std::to_array<Plain>({ { 1, Kind::A } });
    constexpr auto Enums = std::to_array<Kind>({ Kind::A });
    // constexpr auto Quoted = std::to_array<Row>({}); is a comment, not a declaration.
}
"""

ROW = 'ISOLINE_SOURCE_LOCAL_TABLE("lib/Thing.cpp", Registered, "the reason")\n'
DOC_LINE = "- `lib/Thing.cpp`: `Registered`, the reason"


def document(lines):
    return "# Doc\n\n<!-- source-local-tables:begin -->\n" + "".join(f"{line}\n" for line in lines) \
        + "<!-- source-local-tables:end -->\n\nAfter.\n"


class CheckEnumTablesTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.write("src/isoline/lib/Thing.hpp", HEADER)
        self.write("src/isoline/lib/Thing.cpp", SOURCE)
        self.write("src/isoline/meta/EnumTables.def", "// ISOLINE_SOURCE_LOCAL_TABLE(\"x.cpp\", Commented, \"\")\n" + ROW)
        self.write("docs/language-features.md", document([DOC_LINE]))

    def tearDown(self):
        self.directory.cleanup()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text), encoding="utf-8")

    def append(self, relative, text):
        path = self.root / relative
        path.write_text(path.read_text(encoding="utf-8") + text, encoding="utf-8")

    def run_check(self, *arguments):
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--root", str(self.root), *arguments],
                              capture_output=True, text=True)

    def test_registeredTablesPass(self):
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unregisteredTableFails(self):
        self.append("src/isoline/lib/Thing.cpp", "constexpr auto Missing = std::to_array<Row>({});\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("src/isoline/lib/Thing.cpp: Missing is an enum-first table", result.stderr)

    def test_rowStructFromAHeaderCounts(self):
        self.append("src/isoline/lib/Thing.cpp", "constexpr auto FromHeader = std::to_array<HeaderRow>({});\n")
        result = self.run_check()
        self.assertIn("FromHeader is an enum-first table", result.stderr)

    def test_stdArrayDeclarationCounts(self):
        self.append("src/isoline/lib/Thing.cpp", "constexpr std::array<Row, 2> Spelled = {};\n")
        result = self.run_check()
        self.assertIn("Spelled is an enum-first table", result.stderr)

    def test_functionLocalTableCounts(self):
        self.append("src/isoline/lib/Thing.cpp",
                    "void f()\n{\n    static constexpr auto Local = std::to_array<Row>({});\n}\n")
        result = self.run_check()
        self.assertIn("Local is an enum-first table", result.stderr)

    def test_testsAndHelpersAreNotScanned(self):
        for relative in ("src/isoline/lib/Thing_test.cpp", "src/isoline/lib/testing/Helper.cpp",
                         "src/isoline/meta/reflection/Check.cpp"):
            self.write(relative, "constexpr auto Ignored = std::to_array<Row>({});\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_staleRowFails(self):
        self.append("src/isoline/meta/EnumTables.def", 'ISOLINE_SOURCE_LOCAL_TABLE("lib/Thing.cpp", Gone, "r")\n')
        self.write("docs/language-features.md", document([DOC_LINE, "- `lib/Thing.cpp`: `Gone`, r"]))
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn('ISOLINE_SOURCE_LOCAL_TABLE("lib/Thing.cpp", Gone, …) is stale', result.stderr)

    def test_documentOutOfDateFailsAndUpdates(self):
        self.write("docs/language-features.md", document([]))
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("differs from the registry's rows", result.stderr)
        self.assertEqual(self.run_check("--update").returncode, 0)
        self.assertEqual((self.root / "docs/language-features.md").read_text(encoding="utf-8"),
                         document([DOC_LINE]))
        self.assertEqual(self.run_check().returncode, 0)

    def test_documentWithoutBlockFails(self):
        self.write("docs/language-features.md", "# Doc\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("no block", result.stderr)


if __name__ == "__main__":
    unittest.main()
