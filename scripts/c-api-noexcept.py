#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""The shape of the C/C++ boundary of Isoline's C API: no C++ exception crosses it, and every call across it
has the one calling convention (plan P17a, C2 and the export rules).

Each file is checked by its kind:

A C header (.h) of the C API:
  - every isoline_ function declaration is `ISOLINE_API <result> ISOLINE_CALL isoline_<name>(…)
    ISOLINE_NOEXCEPT;`: exported, of the C API's convention, and noexcept to a C++ caller (so its C++
    definition must be noexcept too);
  - every function pointer it declares, a callback typedef, a finalizer parameter or a struct's field, is
    `(ISOLINE_CALL* <name>)(…)`: the embedder's function is called with the convention the C API calls it
    with. (A function pointer type is not noexcept: C functions are not, and the boundary's trampoline,
    which is, ends the process on a C++ exception that leaves one; detail/Callback.hpp.)

A C++ source (.cpp) that implements it:
  - every isoline_ definition is `… ISOLINE_CALL isoline_<name>(…) noexcept { … }`;
  - every wasm_ definition's body is exactly one statement, a call of detail::boundary(), which is noexcept
    and catches std::bad_alloc: wasm.h declares its functions without noexcept, so a mismatching definition
    would not compile, and the body itself must therefore be what keeps exceptions in.

Comments, string literals and preprocessor lines are ignored. Given files of neither kind, or files that
declare and define nothing the rules apply to, it fails: a lint that checks nothing passes nothing.

Usage: c-api-noexcept.py <file>...
"""

import pathlib
import re
import sys

IDENTIFIER = r"[A-Za-z_][A-Za-z0-9_]*"

# The first identifier followed by `(` outside any parenthesis: a declaration's declarator name, or the
# name of what a statement calls.
DECLARATOR = re.compile(r"(" + IDENTIFIER + r")\s*\(")

# The shapes a declaration and a definition of an isoline_ function must have.
ISOLINE_DECLARATION = re.compile(
    r"^ISOLINE_API\s+\S(?:.*\S)?\s*\bISOLINE_CALL\s+isoline_\w+\s*\(.*\)\s*ISOLINE_NOEXCEPT$", re.DOTALL)
ISOLINE_DEFINITION = re.compile(r"\bISOLINE_CALL\s+isoline_\w+\s*\(.*\)\s*noexcept$", re.DOTALL)

# A function pointer declarator: `(<qualifiers> * <name>)(`, the name possibly absent (an abstract one).
FUNCTION_POINTER = re.compile(r"\(\s*(?P<prefix>[^()*]*?)\s*\*\s*(?P<name>" + IDENTIFIER + r")?\s*\)\s*\(")

# A wasm_ body's one statement: a call of detail::boundary(), returned or not.
BOUNDARY_CALL = re.compile(r"^(?:return\s+)?(?:(?:::)?isoline::c::)?detail::boundary\s*\(.*\)$", re.DOTALL)


def stripped(text):
    """The text without comments, the contents of string and character literals, and preprocessor
    lines, each replaced by spaces so that offsets and line numbers stay."""
    out = []
    index = 0
    length = len(text)
    atLineStart = True
    while index < length:
        char = text[index]
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = length if end == -1 else end
            out.append(" " * (end - index))
            index = end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end == -1 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[index:end]))
            index = end
            continue
        if atLineStart and char == "#":
            end = index
            while True:
                newline = text.find("\n", end)
                if newline == -1:
                    end = length
                    break
                if text[newline - 1] == "\\":
                    end = newline + 1
                    continue
                end = newline
                break
            out.append(re.sub(r"[^\n]", " ", text[index:end]))
            index = end
            continue
        if char in "\"'":
            end = index + 1
            while end < length and text[end] != char:
                end += 2 if text[end] == "\\" else 1
            end = min(end + 1, length)
            out.append(char + " " * (end - index - 2) + char)
            index = end
            continue
        out.append(char)
        if char == "\n":
            atLineStart = True
        elif not char.isspace():
            atLineStart = False
        index += 1
    return "".join(out)


def lineOf(text, offset):
    return text.count("\n", 0, offset) + 1


def normalized(fragment):
    return " ".join(fragment.split())


def declaratorName(fragment):
    """The name of what the fragment declares or calls: the first identifier before a `(` at parenthesis
    depth 0, or None."""
    depth = 0
    for index, char in enumerate(fragment):
        if char == "(":
            if depth == 0:
                match = re.search(r"(" + IDENTIFIER + r")\s*$", fragment[:index])
                if match:
                    return match.group(1)
            depth += 1
        elif char == ")":
            depth -= 1
    return None


def headerProblems(path, text):
    """What of a header's declarations is not of the boundary's shape, and how many were checked."""
    code = stripped(text)
    problems = []
    checked = 0
    start = 0
    for match in re.finditer(r"[;{}]", code + ";"):
        fragment = code[start:match.start()]
        where = f"{path}:{lineOf(code, start + len(fragment) - len(fragment.lstrip()))}"
        start = match.end()
        text_ = normalized(fragment)
        if not text_:
            continue
        for pointer in FUNCTION_POINTER.finditer(text_):
            checked += 1
            if pointer["prefix"] != "ISOLINE_CALL":
                name = pointer["name"] or "an unnamed function pointer"
                problems.append(f"{where}: {name}: a function pointer is (ISOLINE_CALL* name)(...): {text_}")
        if text_.startswith("typedef "):
            continue
        name = declaratorName(text_)
        if name is None or not name.startswith("isoline_"):
            continue
        checked += 1
        if not ISOLINE_DECLARATION.match(text_):
            problems.append(f"{where}: {name}: a declaration is "
                            f"ISOLINE_API <result> ISOLINE_CALL {name}(...) ISOLINE_NOEXCEPT: {text_}")
    return problems, checked


def matchingBrace(code, open_):
    """The offset just past the brace that closes the one at @p open_."""
    depth = 0
    for index in range(open_, len(code)):
        if code[index] == "{":
            depth += 1
        elif code[index] == "}":
            depth -= 1
            if depth == 0:
                return index + 1
    return len(code)


def topLevelStatements(body):
    """The statements of a function body, split at the semicolons outside any parenthesis or brace."""
    statements = []
    depth = 0
    start = 0
    for index, char in enumerate(body):
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == ";" and depth == 0:
            statements.append(body[start:index])
            start = index + 1
    rest = body[start:]
    if rest.strip():
        statements.append(rest)
    return [statement for statement in statements if statement.strip()]


def sourceProblems(path, text):
    """What of a source's wasm_ and isoline_ definitions is not of the boundary's shape, and how many
    were checked. Only definitions at file scope, or within an extern "C" block, are C API functions: a
    namespace holds helpers, and is skipped whole, as is every other braced body."""
    code = stripped(text)
    problems = []
    checked = 0
    index = 0
    headStart = 0
    while index < len(code):
        char = code[index]
        if char in ";}":
            headStart = index + 1
            index += 1
            continue
        if char != "{":
            index += 1
            continue
        head = normalized(code[headStart:index])
        if re.fullmatch(r'extern\s*""', head):
            headStart = index + 1
            index += 1
            continue
        end = matchingBrace(code, index)
        name = declaratorName(head)
        if name is not None and name.startswith(("wasm_", "isoline_")) \
                and not head.startswith(("namespace", "struct", "class", "union", "enum")):
            checked += 1
            where = f"{path}:{lineOf(code, index)}"
            if name.startswith("isoline_") and not ISOLINE_DEFINITION.search(head):
                problems.append(f"{where}: {name}: a definition is ... ISOLINE_CALL {name}(...) noexcept: {head}")
            if name.startswith("wasm_"):
                statements = topLevelStatements(code[index + 1:end - 1])
                if len(statements) != 1 or not BOUNDARY_CALL.match(normalized(statements[0])):
                    problems.append(f"{where}: {name}: its body is one statement, a call of detail::boundary(); "
                                    f"it has {len(statements)}")
        headStart = end
        index = end
    return problems, checked


def main(arguments):
    if not arguments:
        print(__doc__, file=sys.stderr)
        return 2
    problems = []
    checked = 0
    for argument in arguments:
        path = pathlib.Path(argument)
        text = path.read_text(encoding="utf-8")
        if path.suffix == ".h":
            found, count = headerProblems(path, text)
        elif path.suffix == ".cpp":
            found, count = sourceProblems(path, text)
        else:
            found, count = [f"{path}: neither a C header (.h) nor a C++ source (.cpp)"], 0
        problems += found
        checked += count
    if checked == 0:
        problems.append("no declaration or definition the rules apply to: the files check nothing")
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"c-api-noexcept: {checked} declarations and definitions of the boundary's shape")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
