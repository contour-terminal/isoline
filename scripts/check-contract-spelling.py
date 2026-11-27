#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Checks how Isoline's sources spell their preconditions (docs/language-features.md).

The rules are rows of RULES: what a set of files must not spell. Each row names the files it reads
(globs under the repository, the paths it exempts, and whether it reads only the installed headers), the
pattern it forbids, and the message that names the fix. A finding prints as
`<path>:<line>: <rule>: <message>`.

Comments are never code, and string and character literals are blanked for every pattern but an
`#include`. Any finding fails the check. On success it prints the count of the Require()s installed
headers keep: they cannot become contracts until an installed declaration may carry one.

Usage: python3 -I scripts/check-contract-spelling.py [--root <repository>]
"""

import argparse
import dataclasses
import enum
import pathlib
import re
import sys

SOURCES = pathlib.PurePosixPath("src/isoline")


class Text(enum.Enum):
    """What of a file a rule's pattern reads."""

    CODE = "code"  # comments and the contents of literals blanked
    CODE_AND_LITERALS = "code and literals"  # comments blanked: an `#include "…"` is a literal


class Scope(enum.Enum):
    """Which of the globbed files a rule reads."""

    EVERY_FILE = "every file"
    INSTALLED_HEADERS = "installed headers"  # those a FILE_SET HEADERS of src/isoline/**/CMakeLists.txt lists


@dataclasses.dataclass(frozen=True)
class Rule:
    """One spelling rule: where it applies, what it forbids, and the fix."""

    name: str  # What the rule protects, as findings name it.
    globs: tuple[str, ...]  # The files it reads, relative to the repository.
    exempt: tuple[str, ...]  # Paths under the globs it does not read, relative to the repository.
    scope: Scope  # Every globbed file, or the installed headers among them.
    text: Text  # What of each file the pattern reads.
    forbidden: re.Pattern[str]  # What it refuses.
    message: str  # The finding's text.


# The contracts seam: the two headers CMake selects between, the only files that spell a contract.
SEAM_HEADERS = ("src/isoline/meta/contracts/Check.hpp", "src/isoline/meta/fallback/contracts/Check.hpp")

RULES = (
    Rule(name="raw-contract-assert",
         globs=("src/isoline/**/*.cpp",),
         exempt=(),
         scope=Scope.EVERY_FILE,
         text=Text.CODE,
         forbidden=re.compile(r"\bcontract_assert\b"),
         message="a contract_assert outside the seam: spell it ISOLINE_PRECONDITION or ISOLINE_INVARIANT "
                 "(<isoline/meta/Check.hpp>), whose native expansion proves the function noexcept"),
    # A contract specifier after a declarator (`) pre(`, `) const noexcept post(`), not a call such as
    # `loop.post(`.
    Rule(name="contract-in-header",
         globs=("src/isoline/**/*.hpp", "src/isoline/**/*.def"),
         exempt=SEAM_HEADERS,
         scope=Scope.EVERY_FILE,
         text=Text.CODE,
         forbidden=re.compile(r"\bISOLINE_PRECONDITION\b|\bISOLINE_INVARIANT\b|\bcontract_assert\b"
                              r"|\)\s*(?:const\s*)?(?:noexcept\s*)?(?:pre|post)\s*\("),
         message="contracts stay out of headers until P19 Task 16"),
    Rule(name="meta-include-in-installed-header",
         globs=("src/isoline/**/*.hpp", "src/isoline/**/*.def"),
         exempt=(),
         scope=Scope.INSTALLED_HEADERS,
         text=Text.CODE_AND_LITERALS,
         forbidden=re.compile(r'#\s*include\s*[<"]isoline/meta/(?!EnumRows\.hpp[>"])'),
         message="an installed header includes only meta/EnumRows.hpp"),
)

# The checks installed headers keep.
CHECK_CALL = re.compile(r"\b(?:Require|Guarantee)\s*\(")

# A comment, line or block, or a string or character literal; the literals are matched so that a `//`
# inside one is not taken for a comment.
COMMENT_OR_LITERAL = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.DOTALL)


def blanked(token):
    """@p token with every character but a line break a space."""
    return re.sub(r"[^\n]", " ", token)


def withoutComments(text):
    """Returns @p text with every comment blanked, keeping its line breaks so that lines still count."""

    def blank(match):
        token = match.group(0)
        return blanked(token) if token.startswith(("//", "/*")) else token

    return COMMENT_OR_LITERAL.sub(blank, text)


def codeOnly(text):
    """Returns @p text with every comment blanked, and the contents of every literal, its quotes kept."""

    def blank(match):
        token = match.group(0)
        if token.startswith(("//", "/*")):
            return blanked(token)
        return token[0] + blanked(token[1:-1]) + token[-1]

    return COMMENT_OR_LITERAL.sub(blank, text)


def readText(path, text):
    """What of the file at @p path a rule reading @p text sees."""
    content = path.read_text(encoding="utf-8")
    return codeOnly(content) if text == Text.CODE else withoutComments(content)


def installedHeaders(root):
    """The headers every `FILE_SET HEADERS … FILES …` of src/isoline/**/CMakeLists.txt lists, relative to the
    repository; generated ones (`${…}`) are left out."""
    headers = set()
    for listFile in sorted((root / SOURCES).rglob("CMakeLists.txt")):
        code = re.sub(r"#[^\n]*", "", listFile.read_text(encoding="utf-8"))
        for fileSet in re.finditer(r"\bFILE_SET\s+HEADERS\b(?P<body>[^)]*)", code):
            files = re.search(r"\bFILES\b(?P<files>.*)", fileSet.group("body"), re.DOTALL)
            if files is None:
                continue
            for token in re.findall(r'"[^"]*"|\S+', files.group("files")):
                token = token.strip('"')
                if re.fullmatch(r"[A-Z_]+", token):
                    break  # the next keyword of target_sources()
                if "${" not in token:
                    headers.add((listFile.parent / token).relative_to(root).as_posix())
    return headers


def ruleFindings(root, rules, installed):
    """Every finding of the pattern rules @p rules, as `<path>:<line>: <rule>: <message>`."""
    result = []
    for rule in rules:
        paths = sorted({path for glob in rule.globs for path in root.glob(glob) if path.is_file()})
        for path in paths:
            relative = path.relative_to(root).as_posix()
            if relative in rule.exempt or (rule.scope == Scope.INSTALLED_HEADERS and relative not in installed):
                continue
            code = readText(path, rule.text)
            for match in rule.forbidden.finditer(code):
                line = code.count("\n", 0, match.start()) + 1
                result.append(f"{relative}:{line}: {rule.name}: {rule.message}")
    return result


def installedRequires(root, installed):
    """The number of Require()/Guarantee()s the installed headers spell."""
    return sum(len(CHECK_CALL.findall(codeOnly((root / header).read_text(encoding="utf-8"))))
               for header in sorted(installed) if (root / header).is_file())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent,
                        help="the repository (default: this script's)")
    arguments = parser.parse_args(argv)
    root = arguments.root
    installed = installedHeaders(root)
    problems = ruleFindings(root, RULES, installed)
    for problem in problems:
        print(problem)
    if problems:
        print(f"contract-spelling: {len(problems)} finding(s)")
        return 1
    print(f"contract-spelling: every precondition is spelled as its rule says; installed headers keep "
          f"{installedRequires(root, installed)} Require/Guarantee")
    return 0

if __name__ == "__main__":
    sys.exit(main())
