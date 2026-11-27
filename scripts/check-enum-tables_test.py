#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of check-enum-tables.py: lints fixture trees with a real Clang and checks its verdicts.

Usage: python3 -I scripts/check-enum-tables_test.py [--clang <clang++>]
"""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "check-enum-tables.py"
CLANG = "clang++"

HEADER = """\
#pragma once
#include <array>
#include <cstdint>
namespace isoline::lib
{
enum class Kind : uint8_t { A, B };
enum Plain : uint8_t { P };
/// A row a source file's table uses.
struct HeaderRow
{
    Kind kind;
    int payload;
};
/// A header's namespace-scope table: the reflection checks', not the lint's.
inline constexpr auto HeaderTable = std::to_array<HeaderRow>({ { Kind::A, 1 } });
template <typename T>
struct Pair
{
    T first;
    int second;
};
} // namespace isoline::lib
"""

LOCAL_HEADER = """\
#pragma once
#include "Thing.hpp"
namespace isoline::lib
{
/// A header's function-local table: the lint's, named by its header.
inline int headerLocal()
{
    static constexpr auto InHeaderFunction = std::to_array<HeaderRow>({ { Kind::A, 1 } });
    return InHeaderFunction[0].payload;
}
} // namespace isoline::lib
"""

# Every form the lint must see, each a table of enum-first rows, and the forms it must not flag.
FORMS = """\
#include "Local.hpp"
#define ISOLINE_FIXTURE_TABLE(name) constexpr auto name = std::to_array<Row>({ { Kind::A, 1 } });
namespace isoline::lib
{
namespace remote
{
    enum class Remote : uint8_t { X };
} // namespace remote
namespace elsewhere
{
    /// A class named like the enum: Kind alone cannot tell the two apart, the scope can.
    struct Kind
    {
        int value;
    };
} // namespace elsewhere
namespace
{
    using remote::Remote;
    using Alias = Kind;
    struct Row
    {
        Kind kind;
        int payload;
    };
    struct EastRow
    {
        Kind const kind;
        int payload;
    };
    struct AliasRow
    {
        Alias kind;
        int payload;
    };
    struct UsingRow
    {
        Remote remote;
        int payload;
    };
    struct IntRow
    {
        int number;
        Kind kind;
    };
    struct PlainRow
    {
        Plain plain;
        int payload;
    };
    constexpr std::array<Row, 1> build() { return { { { Kind::A, 1 } } }; }

    constexpr auto WestConst = std::to_array<Row>({ { Kind::A, 1 } });
    auto const EastConst = std::to_array<Row>({ { Kind::A, 1 } });
    std::array<Row, 1> const ArrayConst { { { Kind::A, 1 } } };
    constexpr auto const AutoConst = std::to_array<Row>({ { Kind::A, 1 } });
    constexpr auto Deduced = std::to_array({ Row { Kind::A, 1 } });
    constexpr std::array Ctad { Row { Kind::A, 1 } };
    constexpr auto TemplateRow = std::to_array<Pair<Kind>>({ { Kind::A, 1 } });
    constexpr auto EastMember = std::to_array<EastRow>({ { Kind::A, 1 } });
    constexpr auto AliasMember = std::to_array<AliasRow>({ { Kind::A, 1 } });
    constexpr auto UsingMember = std::to_array<UsingRow>({ { Remote::X, 1 } });
    constexpr auto FromHeader = std::to_array<HeaderRow>({ { Kind::A, 1 } });
    ISOLINE_FIXTURE_TABLE(FromMacro)
    constexpr auto FromHelper = build();

    constexpr auto NotEnumFirst = std::to_array<IntRow>({ { 1, Kind::A } });
    constexpr auto UnscopedFirst = std::to_array<PlainRow>({ { P, 1 } });
    constexpr auto TemplateIntRow = std::to_array<Pair<int>>({ { 1, 1 } });
    constexpr auto Enums = std::to_array<Kind>({ Kind::A });
} // namespace

/// Namespace scope of a source file, outside any anonymous namespace.
constexpr auto InNamespace = std::to_array<Row>({ { Kind::A, 1 } });

int locals()
{
    struct Local
    {
        Kind kind;
    };
    static constexpr auto StaticLocal = std::to_array<Row>({ { Kind::A, 1 } });
    auto const plainLocal = std::to_array<Local>({ { Kind::B } });
    return StaticLocal[0].payload + static_cast<int>(plainLocal[0].kind) + headerLocal();
}
} // namespace isoline::lib
"""

FLAGGED = {"WestConst", "EastConst", "ArrayConst", "AutoConst", "Deduced", "Ctad", "TemplateRow", "EastMember",
           "AliasMember", "UsingMember", "FromHeader", "FromMacro", "FromHelper", "InNamespace", "StaticLocal", "plainLocal",
           "InHeaderFunction"}
NOT_FLAGGED = {"NotEnumFirst", "UnscopedFirst", "TemplateIntRow", "Enums", "HeaderTable"}

SIMPLE = """\
#include "Thing.hpp"
namespace isoline::lib
{
namespace
{
    struct Row
    {
        Kind kind;
        int payload;
    };
    constexpr auto Registered = std::to_array<Row>({ { Kind::A, 1 } });
} // namespace
int use() { return Registered[0].payload; }
} // namespace isoline::lib
"""

ROW = 'ISOLINE_SOURCE_LOCAL_TABLE("lib/Simple.cpp", Registered, "the reason")\n'
DOC_LINE = "- `lib/Simple.cpp`: `Registered`, the reason"


# Arrays outside namespace isoline, where the dump does not reach: each is flagged by its line, whatever
# its row, xisoline's included; the mentions in comments, strings and a file-scope macro are not, nor
# those in a function defined by its name under isoline.
ESCAPED = """\
#include <array>
#define ISOLINE_FIXTURE_MACRO(name) constexpr auto name = std::to_array<int>({ 1 });
// std::array in a comment, /* std::to_array */ in a block comment
namespace
{
constexpr auto InGlobalAnonymous = std::to_array<int>({ 1 });
} // namespace
namespace xisoline
{
std::array<int, 1> const InOther { 1 };
int defined();
} // namespace xisoline
namespace isoline::lib
{
ISOLINE_FIXTURE_MACRO(FromMacroInside)
constexpr auto Separated = std::to_array<int>({ 1'0 });
constexpr auto Brace = '}';
constexpr auto Raw = R"x(" } std::array ")x";
constexpr auto Inside = std::to_array<int>({ Separated[0] });
int qualified();
} // namespace isoline::lib
int isoline::lib::qualified()
{
    constexpr auto InQualified = std::to_array<int>({ 1 });
    return InQualified[0];
}
int xisoline::defined()
{
    constexpr auto InOtherQualified = std::to_array<int>({ 1 });
    return InOtherQualified[0];
}
int main()
{
    char const* text = "std::array in a string";
    constexpr auto InMain = std::to_array<int>({ 1 });
    return InMain[0] + Inside[0] + (text != nullptr ? 0 : 1);
}
int const Global = isoline::lib::qualified() + [] {
    constexpr auto InLambda = std::to_array<int>({ 1 });
    return InLambda[0];
}();
"""
ESCAPED_LINES = {6, 10, 29, 35, 39}


# The contracts seam's macros, spelled as the fallback spells them: what the lint reads is the macro's
# name where it is invoked, not its expansion.
CHECK = """\
#pragma once
#define Require(cond) do { if (!(cond)) { __builtin_abort(); } } while (false)
#define ISOLINE_PRECONDITION(condition) Require(condition)
#define ISOLINE_INVARIANT(condition) Require(condition)
"""

# The macros inside a try block or a function-try-block, a constructor's included, and in a handler of
# a try nested in one, are flagged (by line); in a handler of an outermost try, after a try, as a plain
# Require, and in a noexcept lambda defined inside a try block, whose own noexcept boundary ends the
# process before any handler of the enclosing function is reached, they are not. A try block in such a
# lambda is the lambda's own, and flagged.
TRY_BLOCKS = """\
#include "Check.hpp"
namespace isoline::lib
{
int inTryBlock(int x) noexcept
{
    try
    {
        ISOLINE_PRECONDITION(x > 0);
        Require(x > 1);
    }
    catch (...)
    {
        ISOLINE_INVARIANT(x > 2);
    }
    ISOLINE_INVARIANT(x > 3);
    return x;
}
int functionTryBlock(int x) noexcept
try
{
    ISOLINE_INVARIANT(x > 4);
    return x;
}
catch (...)
{
    return 0;
}
int lambdaInTryBlock(int x) noexcept
{
    try
    {
        auto const check = [x]() noexcept { ISOLINE_PRECONDITION(x > 5); };
        check();
        auto const ownTry = [x]() noexcept {
            try
            {
                ISOLINE_INVARIANT(x > 6);
            }
            catch (...)
            {
            }
        };
        ownTry();
        try
        {
        }
        catch (...)
        {
            ISOLINE_INVARIANT(x > 7);
        }
    }
    catch (...)
    {
    }
    return x;
}
struct Holder
{
    explicit Holder(int x) noexcept
    try: value { x }
    {
        ISOLINE_INVARIANT(x > 8);
    }
    catch (...)
    {
    }
    int value;
};
} // namespace isoline::lib
"""
TRY_LINES = {8, 21, 37, 49, 62}


# Conditions of the contract macros that write, each flagged by its line, and those that only read, which
# pass: a const member call (through a reference, a pointer, or on a const object), a free function
# taking a const reference or a value, comparisons, and a plain Require that writes (Require is no
# contract, so its condition is not this rule's).
SIDE_EFFECTS = """\
#include "Check.hpp"
namespace isoline::lib
{
struct Store
{
    int intern() noexcept;
    bool alive() const noexcept;
    Store& operator++() noexcept;
    bool operator==(Store const&) const noexcept = default;
    static bool valid(int value) noexcept;
};
bool touch(int& value) noexcept;
bool point(int* value) noexcept;
bool look(int const& value) noexcept;
bool byValue(int value) noexcept;
struct Holder
{
    Store& store;
    Store* pointer;
    int count;
    void writes() noexcept
    {
        ISOLINE_PRECONDITION(++count > 0);
        ISOLINE_INVARIANT(count-- > 0);
        ISOLINE_INVARIANT(store.intern() > 0);
        ISOLINE_INVARIANT(pointer->intern() > 0);
        ISOLINE_INVARIANT((count = 3) > 0);
        ISOLINE_INVARIANT((count += 3) > 0);
        ISOLINE_INVARIANT(touch(count));
        ISOLINE_INVARIANT(point(&count));
        ISOLINE_INVARIANT((++store).alive());
    }
    void reads() const noexcept
    {
        ISOLINE_PRECONDITION(store.alive());
        ISOLINE_INVARIANT(pointer->alive());
        ISOLINE_INVARIANT(look(count) && byValue(count));
        ISOLINE_INVARIANT(count > 0 && store == *pointer && Store::valid(count));
        Require(store.intern() > 0);
    }
    void readsInAWritingFunction() noexcept
    {
        ISOLINE_INVARIANT(store.alive() && pointer->alive() && look(count));
    }
};
} // namespace isoline::lib
"""
SIDE_EFFECT_LINES = set(range(23, 32))

# A known gap of the side-effect rule: a lambda defined before the check whose operator() is const (the
# default) but writes through a by-reference capture. The call in the condition is a const member call, so
# nothing in the expansion writes, and the lint passes it. It stays a review question (see the script's
# docstring and docs/language-features.md); this fixture pins the gap, so closing it shows here.
NAMED_LAMBDA = """\
#include "Check.hpp"
namespace isoline::lib
{
int counted(int count) noexcept
{
    auto const bump = [&count]() noexcept { return ++count > 0; };
    ISOLINE_INVARIANT(bump());
    return count;
}
} // namespace isoline::lib
"""


def document(lines):
    return "# Doc\n\n<!-- source-local-tables:begin -->\n" + "".join(f"{line}\n" for line in lines) \
        + "<!-- source-local-tables:end -->\n\nAfter.\n"


class CheckEnumTablesTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.sources = []
        self.write("src/isoline/lib/Thing.hpp", HEADER)
        self.write("src/isoline/lib/Local.hpp", LOCAL_HEADER)
        self.addSource("lib/Simple.cpp", SIMPLE)
        self.write("src/isoline/meta/EnumTables.def", '// ISOLINE_SOURCE_LOCAL_TABLE("x.cpp", Commented, "")\n' + ROW)
        self.write("docs/language-features.md", document([DOC_LINE]))

    def tearDown(self):
        self.directory.cleanup()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(text), encoding="utf-8")

    def addSource(self, relative, text):
        self.write(f"src/isoline/{relative}", text)
        self.sources.append(relative)

    def run_check(self, *arguments):
        commands = [{"directory": str(self.root), "file": f"src/isoline/{relative}",
                     "command": f"{CLANG} -std=c++23 -Isrc/isoline/lib -c src/isoline/{relative} -o {relative}.o"}
                    for relative in self.sources]
        (self.root / "compile_commands.json").write_text(json.dumps(commands), encoding="utf-8")
        return subprocess.run([sys.executable, "-I", str(SCRIPT), "--root", str(self.root), "--compile-commands",
                               str(self.root / "compile_commands.json"), "--jobs", "2", *arguments],
                              capture_output=True, text=True)

    def flagged(self, result):
        prefix = "src/isoline/"
        return {line.split(": ", 1)[1].split(" ", 1)[0] for line in result.stderr.splitlines()
                if line.startswith(prefix) and "is an enum-first table" in line}

    def test_registeredTablePasses(self):
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_everyFormIsFlagged(self):
        self.addSource("lib/Forms.cpp", FORMS)
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.flagged(result), FLAGGED, result.stderr)
        self.assertFalse(self.flagged(result) & NOT_FLAGGED)

    def test_headerFunctionLocalIsNamedByItsHeader(self):
        self.addSource("lib/Forms.cpp", FORMS)
        result = self.run_check()
        self.assertIn("src/isoline/lib/Local.hpp: InHeaderFunction is an enum-first table", result.stderr)

    def test_testsAndHelpersAreNotLinted(self):
        for relative in ("lib/Forms_test.cpp", "lib/testing/Forms.cpp", "meta/reflection/Forms.cpp"):
            self.addSource(relative, FORMS.replace('"Local.hpp"', '"../lib/Local.hpp"'))
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_staleRowFails(self):
        self.write("src/isoline/meta/EnumTables.def", ROW + 'ISOLINE_SOURCE_LOCAL_TABLE("lib/Simple.cpp", Gone, "r")\n')
        self.write("docs/language-features.md", document([DOC_LINE, "- `lib/Simple.cpp`: `Gone`, r"]))
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn('ISOLINE_SOURCE_LOCAL_TABLE("lib/Simple.cpp", Gone, …) is stale', result.stderr)

    def test_documentOutOfDateFailsAndUpdates(self):
        self.write("docs/language-features.md", document([]))
        result = self.run_check()
        self.assertEqual(result.returncode, 1)
        self.assertIn("differs from the registry's rows", result.stderr)
        self.assertEqual(self.run_check("--update").returncode, 0)
        self.assertEqual((self.root / "docs/language-features.md").read_text(encoding="utf-8"), document([DOC_LINE]))

    def test_arraysOutsideNamespaceIsolineAreFlagged(self):
        self.addSource("lib/Escaped.cpp", ESCAPED.replace("Inside[0]", "isoline::lib::Inside[0]"))
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stderr)
        prefix = "src/isoline/lib/Escaped.cpp:"
        lines = {int(line[len(prefix):].split(":", 1)[0]) for line in result.stderr.splitlines()
                 if line.startswith(prefix) and "outside namespace isoline" in line}
        self.assertEqual(lines, ESCAPED_LINES, result.stderr)

    def test_everyFormSitsInsideNamespaceIsoline(self):
        self.addSource("lib/Forms.cpp", FORMS)
        result = self.run_check()
        self.assertNotIn("outside namespace isoline", result.stderr)

    def test_contractMacrosInsideATryBlockAreFlagged(self):
        self.write("src/isoline/lib/Check.hpp", CHECK)
        self.addSource("lib/Try.cpp", TRY_BLOCKS)
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stderr)
        prefix = "src/isoline/lib/Try.cpp:"
        lines = {int(line[len(prefix):].split(":", 1)[0]) for line in result.stderr.splitlines()
                 if line.startswith(prefix) and "inside a try block" in line}
        self.assertEqual(lines, TRY_LINES, result.stderr)
        self.assertIn("src/isoline/lib/Try.cpp:8: ISOLINE_PRECONDITION inside a try block", result.stderr)
        self.assertIn("src/isoline/lib/Try.cpp:21: ISOLINE_INVARIANT inside a try block", result.stderr)

    def test_contractMacrosOutsideTryBlocksPass(self):
        self.write("src/isoline/lib/Check.hpp", CHECK)
        self.addSource("lib/Checked.cpp", "#include \"Check.hpp\"\nnamespace isoline::lib\n{\n"
                       "int checked(int x) noexcept\n{\n    ISOLINE_PRECONDITION(x > 0);\n    return x;\n}\n"
                       "} // namespace isoline::lib\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_contractConditionsThatWriteAreFlagged(self):
        self.write("src/isoline/lib/Check.hpp", CHECK)
        self.addSource("lib/Effects.cpp", SIDE_EFFECTS)
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stderr)
        prefix = "src/isoline/lib/Effects.cpp:"
        lines = {int(line[len(prefix):].split(":", 1)[0]) for line in result.stderr.splitlines()
                 if line.startswith(prefix) and "a check only reads" in line}
        self.assertEqual(lines, SIDE_EFFECT_LINES, result.stderr)
        self.assertIn("src/isoline/lib/Effects.cpp:25: ISOLINE_INVARIANT condition calls the non-const member "
                      "intern()", result.stderr)
        self.assertIn("src/isoline/lib/Effects.cpp:29: ISOLINE_INVARIANT condition calls touch(), which takes "
                      "int &", result.stderr)

    def test_namedLambdaWritingThroughACaptureIsAKnownGap(self):
        self.write("src/isoline/lib/Check.hpp", CHECK)
        self.addSource("lib/Lambda.cpp", NAMED_LAMBDA)
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("a check only reads", result.stderr)

    def test_clangFailureIsAToolError(self):
        self.addSource("lib/Broken.cpp", "namespace isoline { int broken = ; }\n")
        result = self.run_check()
        self.assertEqual(result.returncode, 2)
        self.assertIn("Clang failed", result.stderr)

    def test_cachedResultIsReusedUntilTheSourceChanges(self):
        cache = self.root / "cache"
        self.assertEqual(self.run_check("--cache", str(cache)).returncode, 0)
        self.assertEqual(len(list(cache.iterdir())), 1)
        # A new time stamp alone keeps the cached result, which is keyed by content: nothing is dumped
        # again, so the entry is not rewritten.
        source = self.root / "src/isoline/lib/Simple.cpp"
        entry = next(cache.iterdir())
        stored = json.loads(entry.read_text(encoding="utf-8"))
        stat = source.stat()
        os.utime(source, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000))
        self.assertEqual(self.run_check("--cache", str(cache)).returncode, 0)
        self.assertEqual(json.loads(entry.read_text(encoding="utf-8")), stored)
        # Changed content, its old time stamp kept: the cache no longer stands for it, so it is compiled.
        source.write_text("namespace isoline { int broken = ; }\n", encoding="utf-8")
        os.utime(source, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        self.assertEqual(self.run_check("--cache", str(cache)).returncode, 2)

    def test_cachedResultIsDroppedWhenClangChanges(self):
        cache = self.root / "cache"
        wrapper = self.root / "clang-wrapper"
        real = shutil.which(CLANG)
        wrapper.write_text(f'#!/bin/sh\nexec "{real}" "$@"\n', encoding="utf-8")
        wrapper.chmod(0o755)
        self.assertEqual(self.run_check("--cache", str(cache), "--clang", str(wrapper)).returncode, 0)
        # Another compiler behind the same path, failing: the cached result no longer stands for it.
        wrapper.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
        self.assertEqual(self.run_check("--cache", str(cache), "--clang", str(wrapper)).returncode, 2)

    def test_cachedResultIsDroppedWhenAHeaderChanges(self):
        cache = self.root / "cache"
        self.assertEqual(self.run_check("--cache", str(cache)).returncode, 0)
        header = self.root / "src/isoline/lib/Thing.hpp"
        stat = header.stat()
        header.write_text(HEADER.replace("int payload;", "int payload = ;", 1), encoding="utf-8")
        os.utime(header, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        self.assertEqual(self.run_check("--cache", str(cache)).returncode, 2)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--clang":
        CLANG = sys.argv[2]
        del sys.argv[1:3]
    if shutil.which(CLANG) is None:
        print(f"check-enum-tables_test: no {CLANG}", file=sys.stderr)
        sys.exit(1)
    unittest.main()
