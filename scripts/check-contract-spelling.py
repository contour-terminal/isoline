#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Checks how Isoline's sources spell their preconditions (docs/language-features.md).

The rules are rows of RULES: the files a rule reads (a glob under the repository, and the paths it
exempts), the pattern it forbids, and the message that names the fix. Comments are not code: a pattern
in one is never a finding. Each finding prints as `<path>:<line>: <message>`, and any finding fails the
check.

Usage: python3 -I scripts/check-contract-spelling.py [--root <repository>]
"""

import argparse
import dataclasses
import pathlib
import re
import sys


@dataclasses.dataclass(frozen=True)
class Rule:
    """One spelling rule: where it applies, what it forbids, and the fix."""

    name: str  # What the rule protects, as findings name it.
    glob: str  # The files it reads, relative to the repository.
    exempt: tuple[str, ...]  # Paths under the glob it does not read, relative to the repository.
    forbidden: re.Pattern[str]  # What it refuses, in code with the comments removed.
    message: str  # The finding's text.


# The contracts seam: the two headers CMake selects between, the only files that spell a contract.
SEAM_HEADERS = ("src/isoline/meta/contracts/Check.hpp", "src/isoline/meta/fallback/contracts/Check.hpp")

RULES = (
    Rule(name="raw-contract-assert",
         glob="src/isoline/**/*.cpp",
         exempt=SEAM_HEADERS,
         forbidden=re.compile(r"\bcontract_assert\b"),
         message="a contract_assert outside the seam: spell it ISOLINE_PRECONDITION or ISOLINE_INVARIANT "
                 "(<isoline/meta/Check.hpp>), whose native expansion proves the function noexcept"),
)

# A comment, line or block, or a string or character literal, which the forbidden patterns skip; the
# literals are matched so that a `//` inside one is not taken for a comment.
COMMENT_OR_LITERAL = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.DOTALL)


def withoutComments(text):
    """Returns @p text with every comment blanked, keeping its line breaks so that lines still count."""

    def blank(match):
        token = match.group(0)
        if token.startswith(("//", "/*")):
            return re.sub(r"[^\n]", " ", token)
        return token

    return COMMENT_OR_LITERAL.sub(blank, text)


def findings(root, rules=RULES):
    """Every finding of @p rules over the repository at @p root, as `<path>:<line>: <message>`, sorted."""
    result = []
    for rule in rules:
        for path in sorted(root.glob(rule.glob)):
            relative = path.relative_to(root).as_posix()
            if relative in rule.exempt or not path.is_file():
                continue
            code = withoutComments(path.read_text(encoding="utf-8"))
            for match in rule.forbidden.finditer(code):
                line = code.count("\n", 0, match.start()) + 1
                result.append(f"{relative}:{line}: {rule.name}: {rule.message}")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent,
                        help="the repository (default: this script's)")
    arguments = parser.parse_args(argv)
    found = findings(arguments.root)
    for finding in found:
        print(finding)
    if found:
        print(f"contract-spelling: {len(found)} finding(s)")
        return 1
    print("contract-spelling: every precondition is spelled as its rule says")
    return 0


if __name__ == "__main__":
    sys.exit(main())
