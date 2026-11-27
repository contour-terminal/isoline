#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""The export lint of a C++ module: generates its interface unit (--write) and checks it (--check).

A module's surface is data: its row (`isoline_add_module()` in cmake/IsolineModules.cmake) writes a
manifest, `<name>.surface.json`, naming the library's header set, the public and internal namespaces,
the modules it re-exports, the foreign names it re-exports, and what compiling the headers takes. The
headers are the single source of truth; the interface unit (`isoline.cppm`) declares nothing. Its global
module fragment includes the headers that declare into a public namespace, and its purview holds one
`export namespace N { using N::leaf; ... }` block per public namespace.

The lint compiles one umbrella translation unit of the header set with Clang's JSON AST dump, filtered by
the common root of the public namespaces, and walks the direct children of every public namespace. What a
child is decides what the lint does with it (DECLARATION_KINDS): a name to export, something to ignore, or,
for a kind the table does not know, a finding: the lint fails closed. A namespace under a public one is
public, internal, or a finding (`unclassified namespace`).

--write regenerates the interface unit from the surface. --check reports, one per line, what differs
(RULES), then how to fix it.

Exit status: 0 clean, 1 findings, 2 a tool error (no Clang, an unreadable manifest, headers Clang cannot
parse, a dump over the ceiling, and any failure the lint did not foresee).

Usage: python3 -I scripts/check-module-exports.py --clang <clang++> --manifest <surface.json>
                                                  (--check | --write) [--work-directory <dir>]
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field

EXIT_CLEAN = 0
EXIT_FINDINGS = 1
EXIT_TOOL_ERROR = 2

# The ceiling of the filtered AST dump. Above it the lint stops with a tool error naming the size, so
# that growth of an internal namespace (which the root filter dumps too) is noticed rather than becoming
# slow. Measured for module isoline (Clang 22.1.8, 2026-11-27): 94.1 MB in 40 roots.
DUMP_SIZE_LIMIT_BYTES = 512 * 1024 * 1024

# The last components that make a namespace internal wherever it is.
ALWAYS_INTERNAL_LEAVES = ("testing", "detail")

# What a namespace's child is to the lint.
EXPORT = "export"  # its name is public
EXPORT_ENUMERATION = "export enumeration"  # its name, and an unscoped enumeration's enumerators
TEMPLATED = "templated"  # what its templated declaration is (its first declaration child that is no parameter)
IGNORE = "ignore"  # it names nothing an importer could spell through a using-declaration
REFUSE = "refuse"  # it names something an interface unit of using-declarations cannot export: a finding

# The declaration-kind table: (Clang AST kind, action, why). A kind not listed is a finding. Implicit nodes
# are ignored before the table is read, so it has no row for what Clang declares only implicitly: the
# UsingShadowDecl beside each UsingDecl, and implicit deduction guides. Nor for FriendDecl: a friend is a
# child of its class, never of a namespace, so the hidden friends of a public class are not seen at all.
DECLARATION_KIND_ROWS = (
    ("CXXRecordDecl", EXPORT, "a class, struct or union"),
    ("EnumDecl", EXPORT_ENUMERATION, "an enumeration"),
    ("VarDecl", EXPORT, "a variable"),
    ("FunctionDecl", EXPORT, "a function; one name covers its overload set"),
    ("TypeAliasDecl", EXPORT, "an alias"),
    ("TypedefDecl", EXPORT, "a typedef"),
    ("ConceptDecl", EXPORT, "a concept"),
    ("UsingDecl", EXPORT, "a re-export; its name is qualified, the leaf is the name"),
    ("FunctionTemplateDecl", TEMPLATED, "a function template, a deduction guide or an out-of-line member"),
    ("ClassTemplateDecl", TEMPLATED, "a class template"),
    ("VarTemplateDecl", TEMPLATED, "a variable template"),
    ("TypeAliasTemplateDecl", TEMPLATED, "an alias template"),
    ("ClassTemplateSpecializationDecl", IGNORE, "an explicit specialization; the class template is the name"),
    ("ClassTemplatePartialSpecializationDecl", IGNORE, "a partial specialization; the class template is the name"),
    ("VarTemplateSpecializationDecl", IGNORE, "an explicit specialization; the variable template is the name"),
    ("VarTemplatePartialSpecializationDecl", IGNORE,
     "a partial specialization; the variable template is the name"),
    ("CXXMethodDecl", IGNORE, "an out-of-line member definition; the class is the name"),
    ("CXXDeductionGuideDecl", IGNORE, "a deduction guide; the class template is the name"),
    ("StaticAssertDecl", IGNORE, "declares nothing"),
    ("UsingDirectiveDecl", IGNORE, "declares nothing"),
    ("FullComment", IGNORE, "a namespace's documentation comment"),
    ("EmptyDecl", IGNORE, "a stray `;`, declares nothing"),
    ("NamespaceAliasDecl", REFUSE,
     "a using-declaration cannot name a namespace, so only a declaration in the purview could export the "
     "alias, and the interface unit declares nothing; alias it in an internal namespace, or name the namespace"),
)
DECLARATION_KINDS = {kind: action for kind, action, _ in DECLARATION_KIND_ROWS}
DECLARATION_REASONS = {kind: why for kind, _, why in DECLARATION_KIND_ROWS}

# The kinds of a template's children that are its parameters, not its templated declaration.
TEMPLATE_PARAMETER_KINDS = ("TemplateTypeParmDecl", "NonTypeTemplateParmDecl", "TemplateTemplateParmDecl")

# How the lint shows a child that has no name.
UNNAMED = "<unnamed>"

# The include line, the re-exported module and the export block of an interface unit, as --check reads
# them. The strict grammar of the purview is not checked here.
INCLUDE_LINE = re.compile(r"^\s*#\s*include\s*<([^>]+)>", re.MULTILINE)
EXPORT_IMPORT_LINE = re.compile(r"^\s*export\s+import\s+([\w.:]+)\s*;", re.MULTILINE)
EXPORT_BLOCK = re.compile(r"export\s+namespace\s+([A-Za-z_][\w:]*)\s*\{(.*?)\}", re.DOTALL)
USING_DECLARATION = re.compile(r"using\s+([^;]+?)\s*;")


class ToolError(Exception):
    """A failure of the lint itself, not a finding: exit status 2."""


@dataclass
class Manifest:
    """A module's row, as its surface manifest records it. Relative paths are relative to the manifest."""

    path: pathlib.Path
    name: str
    source: pathlib.Path
    headerSet: list
    headerBaseDirectories: list
    namespaces: list
    internalNamespaces: list
    imports: list
    foreignNames: list
    includeDirectories: list
    systemIncludeDirectories: list
    compileDefinitions: list
    cxxStandard: str


@dataclass
class Surface:
    """What the headers declare into the public namespaces, in document order."""

    # Qualified public name -> the file of its definition, or of its first declaration (_addName()).
    names: dict = field(default_factory=dict)
    # Header -> the public namespaces it declares a name into, directly or through a row file it includes.
    declaringFiles: dict = field(default_factory=dict)
    # Qualified names of namespaces under a public one that are neither public nor internal.
    unclassifiedNamespaces: list = field(default_factory=list)
    # (kind, qualified name, file) of the children whose kind DECLARATION_KINDS does not list.
    unknownKinds: list = field(default_factory=list)
    # (kind, qualified name, file, why) of the children whose kind DECLARATION_KINDS refuses.
    unexportable: list = field(default_factory=list)


@dataclass
class InterfaceUnit:
    """What --check reads from an interface unit."""

    text: str
    includes: list
    imports: list
    # Qualified name -> how often a block of its namespace exports it.
    exportCounts: dict


@dataclass
class Context:
    """Everything a rule looks at."""

    manifest: Manifest
    surface: Surface
    unit: InterfaceUnit
    generatedText: str


def resolvePath(base, value):
    path = pathlib.Path(value)
    return path if path.is_absolute() else (base / path)


def readManifest(path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ToolError(f"unreadable manifest {path}: {error}") from error
    base = path.parent
    try:
        return Manifest(
            path=path,
            name=data["name"],
            source=resolvePath(base, data["source"]),
            headerSet=[resolvePath(base, header) for header in data["headerSet"]],
            headerBaseDirectories=[resolvePath(base, directory) for directory in data["headerBaseDirectories"]],
            namespaces=list(data["namespaces"]),
            internalNamespaces=list(data["internalNamespaces"]),
            imports=[resolvePath(base, imported) for imported in data["imports"]],
            foreignNames=list(data["foreignNames"]),
            includeDirectories=[resolvePath(base, directory) for directory in data["includeDirectories"]],
            systemIncludeDirectories=[resolvePath(base, directory)
                                      for directory in data["systemIncludeDirectories"]],
            compileDefinitions=list(data["compileDefinitions"]),
            cxxStandard=str(data["cxxStandard"]),
        )
    except (KeyError, TypeError) as error:
        raise ToolError(f"unreadable manifest {path}: missing or malformed field {error}") from error


def importedModuleNames(manifest):
    """The names of the modules the row re-exports, read from their manifests."""
    return [readManifest(imported).name for imported in manifest.imports]


def umbrellaHeaders(manifest):
    """The headers of the set the umbrella includes: the `.hpp` entries. A `.def` is an X-macro row file its
    own header includes, never a header of its own."""
    return [header for header in manifest.headerSet if header.suffix == ".hpp"]


def includeSpelling(manifest, header):
    """How the interface unit includes @p header: relative to the first base directory holding it."""
    resolved = header.resolve()
    for directory in manifest.headerBaseDirectories:
        try:
            return resolved.relative_to(directory.resolve()).as_posix()
        except ValueError:
            continue
    raise ToolError(f"{header} is under none of the header base directories of {manifest.path}")


def shownFile(manifest, file):
    """A file as a finding names it: relative to the interface unit's directory where it is under it,
    else as the interface unit includes it."""
    resolved = pathlib.Path(file).resolve()
    try:
        return resolved.relative_to(manifest.source.parent.resolve()).as_posix()
    except ValueError:
        pass
    try:
        return includeSpelling(manifest, resolved)
    except ToolError:
        return resolved.as_posix()


def commonRoot(namespaces):
    """The longest common leading components of @p namespaces, as a qualified name."""
    split = [namespace.split("::") for namespace in namespaces]
    common = []
    for components in zip(*split):
        if any(component != components[0] for component in components):
            break
        common.append(components[0])
    return "::".join(common)


def dumpAst(manifest, clang, workDirectory, dumpSizeLimitBytes):
    """Compiles the umbrella translation unit with the filtered JSON AST dump; returns the dump's path."""
    root = commonRoot(manifest.namespaces)
    if not root:
        raise ToolError(f"the namespaces of {manifest.path} share no root: {', '.join(manifest.namespaces)}")
    umbrella = workDirectory / f"{manifest.name}.umbrella.cpp"
    umbrella.write_text("".join(f'#include "{header.resolve().as_posix()}"\n'
                                for header in umbrellaHeaders(manifest)), encoding="utf-8")
    systemDirectories = [directory.resolve() for directory in manifest.systemIncludeDirectories]
    arguments = [clang, f"-std=c++{manifest.cxxStandard}", "-fsyntax-only", "-x", "c++"]
    for directory in manifest.includeDirectories:
        if directory.resolve() not in systemDirectories:
            arguments.append(f"-I{directory.resolve().as_posix()}")
    for directory in systemDirectories:
        arguments += ["-isystem", directory.as_posix()]
    arguments += [f"-D{definition}" for definition in manifest.compileDefinitions]
    arguments += ["-Xclang", "-ast-dump=json", "-Xclang", "-ast-dump-filter", "-Xclang", root, str(umbrella)]
    dump = workDirectory / f"{manifest.name}.ast.json"
    try:
        with dump.open("wb") as output:
            result = subprocess.run(arguments, stdout=output, stderr=subprocess.PIPE, check=False)
    except OSError as error:
        raise ToolError(f"cannot run Clang {clang}: {error.strerror}") from error
    if result.returncode != 0:
        diagnostics = result.stderr.decode("utf-8", "replace").splitlines()
        errors = [line for line in diagnostics if "error:" in line] or diagnostics
        raise ToolError(f"Clang failed on the header set of {manifest.path} (exit {result.returncode}): "
                        + " | ".join(errors[:3]))
    size = dump.stat().st_size
    if size > dumpSizeLimitBytes:
        raise ToolError(f"the AST dump is {size} bytes, over the ceiling of {dumpSizeLimitBytes} bytes "
                        "(DUMP_SIZE_LIMIT_BYTES): an internal namespace has grown; raise the ceiling with "
                        "the measurement")
    return dump, root


def readRoots(dump):
    """The dump's top-level JSON values, one at a time: Clang writes one per declaration the filter
    matches, concatenated."""
    try:
        text = dump.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise ToolError(f"unreadable AST dump: {error}") from error
    decoder = json.JSONDecoder()
    whitespace = re.compile(r"\s*")
    position = whitespace.match(text, 0).end()
    while position < len(text):
        try:
            value, position = decoder.raw_decode(text, position)
        except ValueError as error:
            raise ToolError(f"unreadable AST dump: {error}") from error
        yield value
        position = whitespace.match(text, position).end()


def lastFileIn(value, skipInner=False):
    """The file of the last source location in @p value, in document order, or None. Clang writes a
    location's file only when it differs from the previous location's, so this is the file in force
    after @p value. A location's `includedFrom` is not a location of its own."""
    if isinstance(value, list):
        for item in reversed(value):
            found = lastFileIn(item)
            if found is not None:
                return found
        return None
    if not isinstance(value, dict):
        return None
    for key in reversed(list(value.keys())):
        if key == "includedFrom" or (skipInner and key == "inner"):
            continue
        if key == "file":
            return value["file"]
        found = lastFileIn(value[key])
        if found is not None:
            return found
    return None


def fileOfLocation(location, currentFile):
    """The file a declaration's `loc` names, given the file in force before it, and the file that includes
    it (None for the main file): the expansion's, where the location is in a macro expansion."""
    parts = [location[key] for key in ("spellingLoc", "expansionLoc") if key in location] or [location]
    for part in parts:
        if "file" in part:
            currentFile = part["file"]
    return currentFile, parts[-1].get("includedFrom", {}).get("file")


class SurfaceWalker:
    """Walks the dump's roots and collects the Surface."""

    def __init__(self, manifest, root):
        self._headers = {str(header.resolve()) for header in umbrellaHeaders(manifest)}
        self._public = set(manifest.namespaces)
        self._internal = set(manifest.internalNamespaces)
        self._rootName = root.split("::")[-1]
        self._root = root
        self._currentFile = None
        self._defined = set()
        self.surface = Surface()

    def walkRoot(self, node):
        if node.get("kind") == "NamespaceDecl" and node.get("name") == self._rootName:
            self._walkNamespace(node, self._root)
        else:
            self._advancePast(node)

    def _advancePast(self, node, skipInner=False):
        found = lastFileIn(node, skipInner)
        if found is not None:
            self._currentFile = found

    def _isInternal(self, qualifiedName, node):
        return ("name" not in node or qualifiedName in self._internal
                or qualifiedName.split("::")[-1] in ALWAYS_INTERNAL_LEAVES)

    def _walkNamespace(self, node, qualifiedName):
        self._advancePast(node, skipInner=True)
        isPublic = qualifiedName in self._public
        for child in node.get("inner", []):
            if child.get("kind") == "NamespaceDecl":
                self._walkChildNamespace(child, qualifiedName, isPublic)
            elif isPublic:
                self._classify(child, qualifiedName)
            else:
                self._advancePast(child)

    def _walkChildNamespace(self, child, parent, parentIsPublic):
        qualifiedName = f"{parent}::{child.get('name', UNNAMED)}"
        if qualifiedName in self._public:
            self._walkNamespace(child, qualifiedName)
        elif self._isInternal(qualifiedName, child):
            self._advancePast(child)  # pruned: nothing in it is public
        elif parentIsPublic:
            if qualifiedName not in self.surface.unclassifiedNamespaces:
                self.surface.unclassifiedNamespaces.append(qualifiedName)
            self._advancePast(child)
        else:
            # Neither public nor under a public namespace, as the root may be: it can hold a public one.
            self._walkNamespace(child, qualifiedName)

    def _classify(self, child, namespace):
        file, includer = fileOfLocation(child.get("loc", {}), self._currentFile)
        header = self._declaringHeader(file, includer)
        self._advancePast(child)
        if child.get("isImplicit"):
            return
        kind = child.get("kind")
        action = DECLARATION_KINDS.get(kind)
        declaration = child
        if action == TEMPLATED:
            # The first declaration that is no parameter: a requires-clause is an expression child before it.
            declaration = next((inner for inner in child.get("inner", [])
                                if inner.get("kind", "").endswith("Decl")
                                and inner.get("kind") not in TEMPLATE_PARAMETER_KINDS), {})
            kind = f"{kind} of {declaration.get('kind')}"
            action = DECLARATION_KINDS.get(declaration.get("kind"))
            if action == TEMPLATED:  # no declaration is a template of a template
                action = None
        if action is None:
            self.surface.unknownKinds.append((kind, f"{namespace}::{child.get('name', UNNAMED)}", file))
            return
        if action == REFUSE:
            self.surface.unexportable.append((kind, f"{namespace}::{child.get('name', UNNAMED)}", file,
                                              DECLARATION_REASONS[kind]))
            return
        # A declaration whose scope is another one (an out-of-line definition of a member, or of a
        # nested namespace's function) is exported where it is declared.
        if action == IGNORE or "parentDeclContextId" in child:
            return
        # Clang marks a class's definition; an enumeration's is the one with its enumerators.
        isDefinition = bool(declaration.get("completeDefinition")) or (
            action == EXPORT_ENUMERATION
            and any(inner.get("kind") == "EnumConstantDecl" for inner in child.get("inner", [])))
        if "name" in child:
            self._addName(namespace, child["name"].split("::")[-1], file, header, isDefinition)
        # An unscoped enumeration's enumerators are names of the namespace, the enumeration named or not.
        if action == EXPORT_ENUMERATION and "scopedEnumTag" not in child:
            for enumerator in child.get("inner", []):
                if enumerator.get("kind") == "EnumConstantDecl":
                    self._addName(namespace, enumerator["name"], file, header, isDefinition)

    def _declaringHeader(self, file, includer):
        """The header of the set a declaration in @p file comes from: @p file itself, or the header that
        includes it, where it is a row file (an X-macro `.def`) that no interface unit can include."""
        if file is not None and str(pathlib.Path(file).resolve()) in self._headers:
            return file
        if includer is not None and str(pathlib.Path(includer).resolve()) in self._headers:
            return includer
        return file

    def _addName(self, namespace, leaf, file, header, isDefinition):
        """Records a public name. Its file is that of its definition where a class or enumeration has one in
        the headers, else that of its first declaration: a forward declaration does not say where it lives."""
        name = f"{namespace}::{leaf}"
        if name not in self.surface.names or (isDefinition and name not in self._defined):
            self.surface.names[name] = file
        if isDefinition:
            self._defined.add(name)
        if header is not None:
            self.surface.declaringFiles.setdefault(str(pathlib.Path(header).resolve()), set()).add(namespace)


def collectSurface(manifest, clang, workDirectory, dumpSizeLimitBytes):
    dump, root = dumpAst(manifest, clang, workDirectory, dumpSizeLimitBytes)
    walker = SurfaceWalker(manifest, root)
    for node in readRoots(dump):
        walker.walkRoot(node)
    dump.unlink()
    return walker.surface


def declaringHeaders(manifest, surface):
    """The headers of the set that declare a name into a public namespace: (spelling, namespaces)."""
    headers = []
    for header in umbrellaHeaders(manifest):
        namespaces = surface.declaringFiles.get(str(header.resolve()))
        if namespaces:
            headers.append((includeSpelling(manifest, header), sorted(namespaces)))
    return sorted(headers)


def splitQualified(name):
    namespace, _, leaf = name.rpartition("::")
    return namespace, leaf


def usingOrder(leaf):
    """The order of a block's using-declarations: clang-format's (SortUsingDeclarations), case-insensitive
    first, so that the generated unit is formatted as it is written."""
    return (leaf.lower(), leaf)


def generate(manifest, surface):
    """The interface unit's text, as --write writes it."""
    lines = [
        "// SPDX-License-Identifier: Apache-2.0",
        "//",
        f"// The interface unit of module {manifest.name}, generated by scripts/check-module-exports.py --write",
        "// from the module's surface manifest. Do not edit it: the headers are the source of truth, and the",
        "// target isoline_write_module_exports regenerates it.",
        "module;",
        "",
    ]
    lines += [f"#include <{spelling}>" for spelling, _ in declaringHeaders(manifest, surface)]
    lines += ["", f"export module {manifest.name};", ""]
    imports = importedModuleNames(manifest)
    if imports:
        lines += [f"export import {name};" for name in imports] + [""]
    blocks = {}
    for name in list(surface.names) + manifest.foreignNames:
        namespace, leaf = splitQualified(name)
        blocks.setdefault(namespace, set()).add(leaf)
    publicBlocks = [namespace for namespace in sorted(manifest.namespaces) if namespace in blocks]
    foreignBlocks = sorted(namespace for namespace in blocks if namespace not in manifest.namespaces)
    for namespace in publicBlocks + foreignBlocks:
        lines += [f"export namespace {namespace}", "{"]
        lines += [f"using {namespace}::{leaf};" for leaf in sorted(blocks[namespace], key=usingOrder)]
        lines += [f"}} // namespace {namespace}", ""]
    return "\n".join(lines).rstrip("\n") + "\n"


def readInterfaceUnit(path):
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise ToolError(f"unreadable interface unit {path}: {error.strerror}") from error
    except UnicodeDecodeError as error:
        raise ToolError(f"unreadable interface unit {path}: {error}") from error
    exportCounts = {}
    for block in EXPORT_BLOCK.finditer(text):
        namespace = block.group(1)
        for using in USING_DECLARATION.finditer(block.group(2)):
            name = f"{namespace}::{using.group(1).split('::')[-1]}"
            exportCounts[name] = exportCounts.get(name, 0) + 1
    return InterfaceUnit(text=text, includes=INCLUDE_LINE.findall(text), imports=EXPORT_IMPORT_LINE.findall(text),
                         exportCounts=exportCounts)


def expectedNames(context):
    return list(context.surface.names) + context.manifest.foreignNames


def checkUnclassifiedNamespaces(context):
    return [{"namespace": namespace} for namespace in context.surface.unclassifiedNamespaces]


def checkUnknownKinds(context):
    return [{"kind": kind, "name": name, "file": shownFile(context.manifest, file) if file else "?"}
            for kind, name, file in context.surface.unknownKinds]


def checkUnexportable(context):
    return [{"kind": kind, "name": name, "file": shownFile(context.manifest, file) if file else "?", "why": why}
            for kind, name, file, why in context.surface.unexportable]


def checkMissing(context):
    findings = []
    for name in expectedNames(context):
        if name in context.unit.exportCounts:
            continue
        file = context.surface.names.get(name)
        where = f"declared in {shownFile(context.manifest, file)}" if file else "a foreign name of the row"
        findings.append({"name": name, "where": where})
    return findings


def checkStale(context):
    expected = set(expectedNames(context))
    return [{"name": name} for name in context.unit.exportCounts if name not in expected]


def checkDuplicates(context):
    return [{"name": name} for name, count in context.unit.exportCounts.items() if count > 1]


def checkHeadersIncluded(context):
    included = set(context.unit.includes)
    return [{"header": spelling, "namespaces": ", ".join(namespaces)}
            for spelling, namespaces in declaringHeaders(context.manifest, context.surface)
            if spelling not in included]


def checkGeneratedForm(context):
    return [] if context.unit.text == context.generatedText else [{}]


# The rules --check applies: (rule, check, message). A check returns the findings' fields. A rule marked
# alone reports only where no other rule did: it says the unit is not what --write writes, which every
# other finding implies.
RULES = (
    ("unclassifiedNamespace", checkUnclassifiedNamespaces, "unclassified namespace: {namespace}"),
    ("unknownKind", checkUnknownKinds, "unknown declaration kind: {kind} {name} ({file})"),
    ("unexportable", checkUnexportable, "not exportable: {kind} {name} ({file}): {why}"),
    ("missing", checkMissing, "missing: {name} ({where})"),
    ("stale", checkStale, "stale: {name}"),
    ("duplicate", checkDuplicates, "duplicate: {name}"),
    ("headerNotIncluded", checkHeadersIncluded, "header not included: {header} declares into {namespaces}"),
)
ALONE_RULES = (
    ("generatedForm", checkGeneratedForm, "not in generated form: run --write"),
)

# The rules whose findings leave the surface undetermined, so that --write refuses to write.
SURFACE_RULES = ("unclassifiedNamespace", "unknownKind", "unexportable")


def applyRules(context, rules):
    return [message.format(**fields) for _, check, message in rules for fields in check(context)]


def runCheck(manifest, surface):
    unit = readInterfaceUnit(manifest.source)
    context = Context(manifest=manifest, surface=surface, unit=unit, generatedText=generate(manifest, surface))
    findings = applyRules(context, RULES) or applyRules(context, ALONE_RULES)
    for finding in findings:
        print(finding)
    if findings:
        print(f"hint: regenerate {manifest.source} with --write (the target isoline_write_module_exports), "
              "once what it cannot fix above is fixed in the headers or in the row")
        return EXIT_FINDINGS
    return EXIT_CLEAN


def runWrite(manifest, surface):
    context = Context(manifest=manifest, surface=surface, unit=None, generatedText="")
    findings = applyRules(context, [rule for rule in RULES if rule[0] in SURFACE_RULES])
    if findings:
        for finding in findings:
            print(finding)
        print(f"error: {manifest.source} not written: the surface is undetermined", file=sys.stderr)
        return EXIT_FINDINGS
    manifest.source.write_text(generate(manifest, surface), encoding="utf-8")
    return EXIT_CLEAN


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang", required=True, help="the clang++ that dumps the AST (the pinned LLVM)")
    parser.add_argument("--manifest", required=True, type=pathlib.Path, help="the module's surface.json")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="report what the interface unit lacks")
    mode.add_argument("--write", action="store_true", help="regenerate the interface unit")
    parser.add_argument("--work-directory", type=pathlib.Path,
                        help="where the umbrella and the dump go (default: a temporary directory)")
    parser.add_argument("--dump-size-limit-bytes", type=int, default=DUMP_SIZE_LIMIT_BYTES,
                        help="the ceiling of the AST dump")
    arguments = parser.parse_args()
    try:
        manifest = readManifest(arguments.manifest.resolve())
        if arguments.work_directory is not None:
            arguments.work_directory.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=arguments.work_directory) as workDirectory:
            surface = collectSurface(manifest, arguments.clang, pathlib.Path(workDirectory),
                                     arguments.dump_size_limit_bytes)
            if arguments.write:
                return runWrite(manifest, surface)
            return runCheck(manifest, surface)
    except ToolError as error:
        print(f"error: {error}", file=sys.stderr)
        return EXIT_TOOL_ERROR
    except Exception as error:
        # A defect of the lint, or input it did not foresee (a manifest field of another type): exit 2, so
        # that it never reads as exit 1's findings.
        print(f"error: unexpected {type(error).__name__}: {error}", file=sys.stderr)
        return EXIT_TOOL_ERROR


if __name__ == "__main__":
    sys.exit(main())
