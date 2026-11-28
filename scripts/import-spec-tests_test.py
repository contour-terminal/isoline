#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of import-spec-tests.py: which commands become oracle entries, skips among them, and which fail
the import."""

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "import-spec-tests.py"


def loadImporter():
    """Loads import-spec-tests.py by path: its name is not an importable module name."""
    sys.dont_write_bytecode = True  # leave no __pycache__ in the source tree
    spec = importlib.util.spec_from_file_location("import_spec_tests", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


importer = loadImporter()


def hexText(token):
    return bytes.fromhex(token).decode("utf-8")


def module(line, name=None, filename=None):
    """A JSON command of a module, given as bytes or encoded by wasm-tools from text."""
    command = {"type": "module", "line": line, "module_type": "binary", "filename": filename or f"m.{line}.wasm"}
    if name is not None:
        command["name"] = name
    return command


def invoke(line, module=None, kind="assert_return"):
    action = {"type": "invoke", "field": "f", "args": []}
    if module is not None:
        action["module"] = module
    command = {"type": kind, "line": line, "action": action}
    if kind == "assert_return":
        command["expected"] = []
    return command


def register(line, name, module=None):
    command = {"type": "register", "line": line, "as": name}
    if module is not None:
        command["name"] = module
    return command


GC_MESSAGE = "rec group usage requires `gc` proposal to be enabled"


def skipsOf(commands, cut, imports=None):
    """The skip reason of each of commands, None for one that is kept: imported in order as one file whose
    modules are given as bytes, import from the names imports maps them to, and are rejected by the cut with
    the message cut maps them to."""
    text, _, _ = manifest(commands, ["binary"] * len(commands), cut=cut, imports=imports)
    reasons = [None] * len(commands)
    for line in text.splitlines()[1:]:
        ordinal, _, kind, value = line.split(" ")
        if kind == "skip":
            reasons[int(ordinal)] = hexText(value)
    return reasons


def skipped(reasons):
    return [reason is not None for reason in reasons]


class PoisoningTest(unittest.TestCase):
    """A module the cut removes is skipped, and so is every command that uses it, until a module replaces it."""

    CUT = {"g.wasm": GC_MESSAGE}

    def skips(self, commands, imports=None):
        return skipsOf(commands, self.CUT, imports)

    def test_theCurrentModuleIsPoisonedUntilTheNextModule(self):
        reasons = self.skips([module(1, name="M"), module(2, filename="g.wasm"), invoke(3), invoke(4, module="M"),
                              module(5), invoke(6)])
        self.assertEqual(skipped(reasons), [False, True, True, False, False, False])
        self.assertEqual(reasons[2], f"gc: {GC_MESSAGE} (root wast line 2; wast line 3)")

    def test_aNamedModuleStaysPoisonedByName(self):
        reasons = self.skips([module(1, name="T", filename="g.wasm"), module(2), invoke(3, module="T"),
                              register(4, "t", module="T"), module(5, name="T"), invoke(6, module="T")])
        self.assertEqual(skipped(reasons), [True, False, True, True, False, False])

    def test_registrationsPoisonTheirImporters(self):
        reasons = self.skips([module(1, filename="g.wasm"), register(2, "t"), module(3, filename="a.wasm"), invoke(4),
                              module(5, filename="b.wasm"),
                              {"type": "assert_unlinkable", "line": 6, "module_type": "binary", "filename": "c.wasm",
                               "text": "unknown import"}],
                             imports={"a.wasm": ["spectest", "t"], "b.wasm": ["spectest"], "c.wasm": ["t"]})
        self.assertEqual(skipped(reasons), [True, True, True, True, False, True])
        self.assertEqual(reasons[3], f"gc: {GC_MESSAGE} (root wast line 1; wast line 4)")

    def test_aReRegistrationClearsTheName(self):
        reasons = self.skips([module(1, filename="g.wasm"), register(2, "t"), module(3), register(4, "t"),
                              module(5, filename="a.wasm")],
                             imports={"a.wasm": ["t"]})
        self.assertEqual(skipped(reasons), [True, True, False, False, False])

    def test_definitionsAndInstances(self):
        definition = {"type": "module_definition", "line": 1, "name": "D", "module_type": "binary",
                      "filename": "g.wasm"}
        reasons = self.skips([module(0), definition, invoke(2),
                              {"type": "module_instance", "line": 3, "instance": "I", "module": "D"},
                              invoke(4), invoke(5, module="I")])
        # A definition is no current module: the unnamed invoke after it still uses module 0.
        self.assertEqual(skipped(reasons), [False, True, False, True, True, True])
        self.assertEqual(reasons[1], f"gc: {GC_MESSAGE} (wast line 1)")

    def test_anAssertExceptionOfAPoisonedModule(self):
        reasons = self.skips([module(1, filename="g.wasm"), invoke(2, kind="assert_exception")])
        self.assertEqual(reasons, [f"gc: {GC_MESSAGE} (wast line 1)",
                                   f"gc: {GC_MESSAGE} (root wast line 1; wast line 2)"])

    def test_aBinaryDefinitionImportingAPoisonedRegistration(self):
        definition = {"type": "module_definition", "line": 3, "name": "D", "module_type": "binary",
                      "filename": "d.wasm"}
        reasons = self.skips([module(1, filename="g.wasm"), register(2, "t"), definition,
                              {"type": "module_instance", "line": 4, "instance": "I", "module": "D"},
                              invoke(5, module="I")],
                             imports={"d.wasm": ["t"]})
        self.assertEqual(skipped(reasons), [True, True, True, True, True])
        self.assertEqual(reasons[4], f"gc: {GC_MESSAGE} (root wast line 1; wast line 5)")

    def test_anInstanceOfTheLatestDefinitionFailsTheImport(self):
        # Whether a definition is poisoned is known by its name; the wast may leave it implicit.
        with self.assertRaises(SystemExit) as failure:
            self.skips([{"type": "module_instance", "line": 8, "instance": "I"}])
        self.assertIn("x.wast:8: module_instance of the latest definition", str(failure.exception.code))

    def test_aBadImportSectionFailsTheImport(self):
        def readImports(filename):
            return importer.importModuleNames(wasmModule((2, b"\x01\x01a")))  # truncated in the field name

        poisoning = importer.Poisoning("x.wast", readImports, lambda filename: self.CUT.get(filename))
        poisoning.skipOf(module(1, filename="g.wasm"))
        poisoning.skipOf(register(2, "t"))
        with self.assertRaises(SystemExit) as failure:
            poisoning.skipOf(module(3))
        self.assertIn("x.wast:3: a truncated LEB128 number in an import section", str(failure.exception.code))


def assertion(kind, line, filename, text="type mismatch"):
    return {"type": kind, "line": line, "module_type": "binary", "filename": filename, "text": text}


# What the pinned wasm-tools 1.263.0 prints for `validate --features=<cut.features>` on modules of
# `(rec (type (func)))`, `(func (local anyref))`, `(func (drop (ref.i31 (i32.const 0))))` and
# `(func (i32.add (i32.const 0) (i64.const 0)) drop)`.
ORACLE_REC = "error: rec group usage requires `gc` proposal to be enabled (at offset 0xb)\n"
ORACLE_LOCAL_ANYREF = ("error: func 0 failed to validate\n\nCaused by:\n"
                       "    0: heap types not supported without the gc feature (at offset 0x17)\n")
ORACLE_REF_I31 = ("error: func 0 failed to validate\n\nCaused by:\n"
                  "    0: gc support is not enabled (at offset 0x19)\n")
ORACLE_TYPE_MISMATCH = ("error: func 0 failed to validate\n\nCaused by:\n"
                        "    0: type mismatch: expected i32, found i64 (at offset 0x1b)\n")


class RejectionMessageTest(unittest.TestCase):
    def test_aSingleLineError(self):
        self.assertEqual(importer.rejectionMessage(ORACLE_REC), GC_MESSAGE)

    def test_theInnermostCauseOfAChain(self):
        self.assertEqual(importer.rejectionMessage(ORACLE_LOCAL_ANYREF),
                         "heap types not supported without the gc feature")
        self.assertEqual(importer.rejectionMessage(ORACLE_REF_I31), "gc support is not enabled")
        self.assertEqual(importer.rejectionMessage("error: a\n\nCaused by:\n    0: b\n    1: c (at offset 0x1)\n"), "c")

    def test_noMessage(self):
        self.assertIsNone(importer.rejectionMessage("\n"))

    def test_gcCausesAreCutAndOthersAbort(self):
        for stderr in (ORACLE_REC, ORACLE_LOCAL_ANYREF, ORACLE_REF_I31):
            with self.subTest(stderr=stderr):
                [reason] = skipsOf([module(1, filename="g.wasm")], {"g.wasm": importer.rejectionMessage(stderr)})
                self.assertTrue(reason.startswith("gc: "))
        with self.assertRaises(SystemExit) as failure:
            skipsOf([module(1, filename="v.wasm")], {"v.wasm": importer.rejectionMessage(ORACLE_TYPE_MISMATCH)})
        self.assertIn("not for GC: type mismatch: expected i32, found i64", str(failure.exception.code))


class CutSourcesTest(unittest.TestCase):
    def test_theLockCutsEverySource(self):
        lock = importer.loadFetchTools().readLock(SCRIPT.parent / "tools.lock")
        self.assertNotIn("cut.exempt", lock)
        self.assertIn("-gc", lock["cut.features"].split(","))


class CutTest(unittest.TestCase):
    """The oracle's GC rejections become skips; any other disagreement with the suite fails the import."""

    def test_everyGcMessageIsCut(self):
        for message in ("rec group usage requires `gc` proposal to be enabled",
                        "heap types not supported without the gc feature",
                        "type not valid because the GC proposal is disabled",
                        "gc proposal must be enabled",
                        "constant expression required: global.get of locally defined global"):
            with self.subTest(message=message):
                self.assertEqual(skipsOf([module(1, filename="g.wasm")], {"g.wasm": message}),
                                 [f"gc: {message} (wast line 1)"])

    def test_aCutModulePoisonsItsUses(self):
        reasons = skipsOf([module(1, name="G", filename="g.wasm"), invoke(2), invoke(3, module="G"),
                           register(4, "g", module="G"), module(5, filename="a.wasm"), invoke(6), module(7), invoke(8)],
                          {"g.wasm": GC_MESSAGE}, imports={"a.wasm": ["g"]})
        self.assertEqual(skipped(reasons), [True, True, True, True, True, True, False, False])
        self.assertEqual(reasons[5], f"gc: {GC_MESSAGE} (root wast line 1; wast line 6)")

    def test_aCutDefinitionPoisonsItsInstances(self):
        definition = {"type": "module_definition", "line": 1, "name": "D", "module_type": "binary",
                      "filename": "d.wasm"}
        reasons = skipsOf([definition, {"type": "module_instance", "line": 2, "instance": "I", "module": "D"},
                           invoke(3, module="I")],
                          {"d.wasm": GC_MESSAGE})
        self.assertEqual(skipped(reasons), [True, True, True])

    def test_assertionsOfGcModulesAreCut(self):
        for kind in ("assert_invalid", "assert_malformed", "assert_unlinkable", "assert_uninstantiable"):
            with self.subTest(kind=kind):
                self.assertEqual(skipsOf([assertion(kind, 9, "g.wasm")], {"g.wasm": GC_MESSAGE}),
                                 [f"gc: {GC_MESSAGE} (wast line 9)"])

    def test_otherRejectionsOfInvalidModulesAreKept(self):
        for kind in ("assert_invalid", "assert_malformed"):
            with self.subTest(kind=kind):
                self.assertEqual(skipsOf([assertion(kind, 9, "i.wasm")], {"i.wasm": "type mismatch: bad"}), [None])

    def test_anAcceptedInvalidModuleFailsTheImport(self):
        for kind in ("assert_invalid", "assert_malformed"):
            with self.subTest(kind=kind), self.assertRaises(SystemExit) as failure:
                skipsOf([assertion(kind, 9, "i.wasm")], {})
            self.assertIn(f"x.wast:9: {kind}: the cut accepts a module", str(failure.exception.code))

    def test_otherRejectionsOfValidModulesFailTheImport(self):
        for command in (module(4, filename="v.wasm"), assertion("assert_unlinkable", 4, "v.wasm"),
                        assertion("assert_uninstantiable", 4, "v.wasm"),
                        {"type": "module_definition", "line": 4, "module_type": "binary", "filename": "v.wasm"}):
            with self.subTest(kind=command["type"]), self.assertRaises(SystemExit) as failure:
                skipsOf([command], {"v.wasm": "type mismatch: bad"})
            self.assertIn(f"x.wast:4: {command['type']}: the cut rejects", str(failure.exception.code))

    def test_aSourceWithoutTheCutKeepsEveryModule(self):
        poisoning = importer.Poisoning("x.wast", lambda filename: [], None)
        self.assertIsNone(poisoning.skipOf(assertion("assert_invalid", 9, "i.wasm")))

    def test_aPoisonedModuleIsNotValidated(self):
        # A module importing from a cut registration is skipped as its dependent, whatever the oracle says.
        reasons = skipsOf([module(1, filename="g.wasm"), register(2, "g"), module(3, filename="a.wasm")],
                          {"g.wasm": GC_MESSAGE, "a.wasm": "unknown import"}, imports={"a.wasm": ["g"]})
        self.assertEqual(reasons[2], f"gc: {GC_MESSAGE} (root wast line 1; wast line 3)")


def wasmModule(*sections):
    """A binary module of the given (id, payload) sections."""
    data = b"\0asm\x01\0\0\0"
    for section, payload in sections:
        data += bytes([section, len(payload)]) + payload
    return data


def name(text):
    return bytes([len(text)]) + text.encode("utf-8")


class ImportModuleNamesTest(unittest.TestCase):
    def test_everyDescriptorKind(self):
        imports = (b"\x06"
                   + name("a") + name("f") + b"\x00\x00"                    # func, type 0
                   + name("b") + name("t") + b"\x01\x70\x01\x00\x0a"        # table funcref 0..10
                   + name("c") + name("t") + b"\x01\x64\x00\x00\x01"        # table (ref 0), min 1
                   + name("d") + name("m") + b"\x02\x09\x01\x02\x10"        # memory 1..2, page size 2^16
                   + name("e") + name("g") + b"\x03\x63\x70\x01"            # global (mut (ref null func))
                   + name("f") + name("x") + b"\x04\x00\x00")               # tag, type 0
        data = wasmModule((1, b"\x00"), (2, imports), (3, b"\x00"))
        self.assertEqual(importer.importModuleNames(data), ["a", "b", "c", "d", "e", "f"])

    def test_compactImports(self):
        imports = (b"\x02"
                   + name("a") + name("") + b"\x7f\x02" + name("f") + b"\x00\x00" + name("g") + b"\x03\x7f\x00"
                   + name("b") + name("") + b"\x7e\x00\x00\x02" + name("f") + name("g"))
        self.assertEqual(importer.importModuleNames(wasmModule((2, imports))), ["a", "b"])

    def test_noImportSection(self):
        self.assertEqual(importer.importModuleNames(wasmModule((1, b"\x00"))), [])

    def test_aTruncatedImportSectionIsUnsupported(self):
        for imports in (b"\x01\x01a", b"\x01" + name("a") + name("f"), b"\x01" + name("a") + b"\x01\xff\x00\x00"):
            with self.subTest(imports=imports), self.assertRaises(importer.Unsupported):
                importer.importModuleNames(wasmModule((2, imports)))


SPEC = SCRIPT.parent.parent / "tests" / "spec"
ORDINALS = SPEC / "ordinals"


def forms(script):
    return [(command.jsonType(), command.form, command.line) for command in importer.scanScript(script)]


class ScanScriptTest(unittest.TestCase):
    """What the JSON cannot say, read from the script: the form of each command's module."""

    def test_theFormsOfModules(self):
        script = ('(module (func))\n(module $m binary "\\00asm" "\\01\\00\\00\\00")\n(module quote "(func)")\n'
                  "(module)\n(module $m)\n(module definition $d binary \"\")\n(module instance $i $d)\n"
                  '(module $"a b" quote "")\n')
        self.assertEqual(forms(script),
                         [("module", "text", 1), ("module", "binary", 2), ("module", "quote", 3), ("module", "text", 4),
                          ("module", "text", 5), ("module_definition", "binary", 6), ("module_instance", None, 7),
                          ("module", "quote", 8)])

    def test_theModulesOfAssertions(self):
        script = ('(assert_trap (module (start 0)) "x")\n(assert_trap (invoke "f") "x")\n'
                  '(assert_malformed\n  (module binary "")\n  "x")\n'
                  '(assert_invalid (module quote "(module binary)") "x")\n'
                  '(assert_return (get "g")) (register "r" $m)\n')
        self.assertEqual(forms(script),
                         [("assert_uninstantiable", "text", 1), ("assert_trap", None, 2),
                          ("assert_malformed", "binary", 3), ("assert_invalid", "quote", 6),
                          ("assert_return", None, 7), ("register", None, 7)])

    def test_commentsStringsAndAnnotationsAreNoTokens(self):
        script = (';; (module binary\n(; (module binary (; nested ;) ;)\n(module "\\")" binary)\n'
                  '((@a (b) ")") module (@a) binary "")\n(module (; c ;) quote "")\n'
                  '(assert_invalid ;; (module binary\n (module (func)) "x")\n')
        self.assertEqual(forms(script),
                         [("module", "text", 3), ("module", "binary", 4), ("module", "quote", 5),
                          ("assert_invalid", "text", 6)])

    def test_anInlineModuleIsOneTextModule(self):
        self.assertEqual(forms("(func) (memory 0)\n(func (export \"f\"))\n"), [("module", "text", 1)])

    def test_unbalancedScriptsAreUnsupported(self):
        for script in ("(module", "(module))", 'module', '(module "x)', "(module (; x)", "((@a module)"):
            with self.subTest(script=script), self.assertRaises(importer.Unsupported):
                importer.scanScript(script)

    def test_theOrdinalsFixture(self):
        commands = json.loads((ORDINALS / "ordinals.json").read_text(encoding="utf-8"))["commands"]
        script = (ORDINALS / "ordinals.wast").read_text(encoding="utf-8")
        self.assertEqual(importer.moduleForms("ordinals.wast", importer.scanScript(script), commands),
                         ["text", None, None, None, None, "text", None, "text", "quote", "binary", "text", "text",
                          "binary", "quote", None, None, None, None, "text", "quote"])

    def test_aScriptThatDisagreesWithTheJsonFailsTheImport(self):
        commands = [{"type": "module", "line": 1}, {"type": "action", "line": 2}]
        with self.assertRaises(SystemExit) as failure:
            importer.moduleForms("x.wast", importer.scanScript("(module)"), commands)
        self.assertIn("x.wast: the script reads as 1 commands, but wasm-tools counts 2", str(failure.exception.code))
        with self.assertRaises(SystemExit) as failure:
            importer.moduleForms("x.wast", importer.scanScript('(module)\n(get "g")'),
                                 [commands[0], {"type": "assert_return", "line": 2}])
        self.assertIn("x.wast:2: command 1 reads as action, but wasm-tools gives assert_return",
                      str(failure.exception.code))


class ListCommandsTest(unittest.TestCase):
    def test_theOrdinalsFixtureIsWhatTheImporterLists(self):
        # ordinals.json is `wasm-tools json-from-wast ordinals.wast`; ordinals.commands, which
        # WastReader.ordinalsMatchWasmTools reads, is `--list-commands ordinals.wast`. `--ordinals`
        # regenerates both.
        commands = json.loads((ORDINALS / "ordinals.json").read_text(encoding="utf-8"))["commands"]
        self.assertEqual(importer.listCommands(commands), (ORDINALS / "ordinals.commands").read_text(encoding="utf-8"))

    def test_theOrdinalsModeWritesTheCommittedFixture(self):
        # What `--ordinals` writes of the JSON wasm-tools gives, which ends without a newline.
        rawJson = (ORDINALS / "ordinals.json").read_text(encoding="utf-8").rstrip("\n")
        self.assertEqual(importer.ordinalsFixture(rawJson),
                         {name: (ORDINALS / name).read_text(encoding="utf-8")
                          for name in ("ordinals.json", "ordinals.commands")})
        self.assertEqual(importer.ORDINALS, ORDINALS)


def textModule(line, filename, kind="module", text=None):
    """A JSON command of a module wasm-tools encoded from text."""
    command = {"type": kind, "line": line, "filename": filename, "module_type": "binary"}
    if text is not None:
        command["text"] = text
    return command


def manifest(commands, moduleForms, cut=None, imports=None, divergences=()):
    """The manifest, binaries and skip count of commands, the binaries the cut rejects as cut maps them."""
    return importer.oracleManifest("x", "x.wast", commands, moduleForms,
                                   lambda filename: (imports or {}).get(filename, []),
                                   lambda filename: (cut or {}).get(filename), divergences)


def hexOf(text):
    return text.encode("utf-8").hex()


class NativeManifestTest(unittest.TestCase):
    def test_aCutModuleAndTheAssertReturnItPoisons(self):
        commands = [textModule(3, "x.0.wasm"), invoke(7), textModule(9, "x.1.wasm"), invoke(10)]
        text, binaries, skipped = manifest(commands, ["text", None, "text", None], cut={"x.0.wasm": GC_MESSAGE})
        self.assertEqual(text, "commands 4\n"
                               "0 3 wasm x.0.wasm\n"
                               f"0 3 skip {hexOf(f'gc: {GC_MESSAGE} (wast line 3)')}\n"
                               f"1 7 skip {hexOf(f'gc: {GC_MESSAGE} (root wast line 3; wast line 7)')}\n"
                               "2 9 wasm x.2.wasm\n")
        self.assertEqual(binaries, {"x.0.wasm": "x.0.wasm", "x.2.wasm": "x.1.wasm"})
        self.assertEqual(skipped, 2)

    def test_aQuoteModuleNamesItsBinaryFilename(self):
        quote = {"type": "module", "line": 4, "filename": "x.3.wat", "module_type": "text",
                 "binary_filename": "x.3.wasm"}
        text, binaries, skipped = manifest([quote], ["quote"])
        self.assertEqual((text, binaries, skipped), ("commands 1\n0 4 wasm x.0.wasm\n", {"x.0.wasm": "x.3.wasm"}, 0))
        # The cut checks it, as any other binary.
        text, _, skipped = manifest([quote], ["quote"], cut={"x.3.wasm": GC_MESSAGE})
        self.assertIn(f"0 4 skip {hexOf(f'gc: {GC_MESSAGE} (wast line 4)')}", text)
        self.assertEqual(skipped, 1)

    def test_assertionsOfTextModulesNameTheirBinaries(self):
        commands = [textModule(5, "x.0.wasm", "assert_invalid", "type mismatch"),
                    textModule(8, "x.1.wasm", "assert_uninstantiable", "unreachable"),
                    textModule(11, "x.2.wasm", "assert_invalid_custom", "bad"),
                    {"type": "module_definition", "line": 12, "name": "d", "filename": "x.3.wasm",
                     "module_type": "binary"}]
        text, binaries, _ = manifest(commands, ["text"] * 4, cut={"x.0.wasm": "type mismatch: x"})
        self.assertEqual(text, "commands 4\n0 5 wasm x.0.wasm\n1 8 wasm x.1.wasm\n2 11 wasm x.2.wasm\n"
                               "3 12 wasm x.3.wasm\n")
        self.assertEqual(len(binaries), 4)

    def test_aMultiLineAssertUnlinkableIsKeyedByOrdinalWithTheJsonLine(self):
        # (assert_unlinkable on line 20, its (module on 21: wasm-tools gives 21.
        commands = [textModule(1, "x.0.wasm"), {"type": "register", "line": 2, "as": "m"},
                    textModule(21, "x.1.wasm", "assert_unlinkable", "unknown import")]
        text, _, _ = manifest(commands, ["text", None, "text"], imports={"x.1.wasm": ["m"]})
        self.assertEqual(text, "commands 3\n0 1 wasm x.0.wasm\n2 21 wasm x.2.wasm\n")

    def test_aModuleGivenAsBytesHasNoBinaryButIsCut(self):
        binary = textModule(2, "x.0.wasm")
        text, binaries, skipped = manifest([binary], ["binary"])
        self.assertEqual((text, binaries, skipped), ("commands 1\n", {}, 0))
        text, binaries, skipped = manifest([binary, invoke(3)], ["binary", None], cut={"x.0.wasm": GC_MESSAGE})
        self.assertEqual(text, "commands 2\n"
                               f"0 2 skip {hexOf(f'gc: {GC_MESSAGE} (wast line 2)')}\n"
                               f"1 3 skip {hexOf(f'gc: {GC_MESSAGE} (root wast line 2; wast line 3)')}\n")
        self.assertEqual((binaries, skipped), ({}, 2))

    def test_malformedTextHasNoBinary(self):
        malformed = {"type": "assert_malformed", "line": 6, "filename": "x.0.wat", "module_type": "text", "text": "x"}
        self.assertEqual(manifest([malformed], ["quote"]), ("commands 1\n", {}, 0))

    def test_aValidModuleWithoutABinaryFailsTheImport(self):
        with self.assertRaises(SystemExit) as failure:
            manifest([{"type": "module", "line": 6, "filename": "x.0.wat", "module_type": "text"}], ["quote"])
        self.assertIn("x.wast:6: module: wasm-tools gives no binary", str(failure.exception.code))

    def test_actionsHaveNoEntries(self):
        # assert_trap of an action is no module command: the JSON gives assert_uninstantiable for one of a
        # module.
        commands = [textModule(1, "x.0.wasm"), invoke(2, kind="assert_trap"), invoke(3, kind="action")]
        self.assertEqual(manifest(commands, ["text", None, None])[0], "commands 3\n0 1 wasm x.0.wasm\n")

    def test_theCutStillFailsTheImportOnOtherRejections(self):
        with self.assertRaises(SystemExit) as failure:
            manifest([textModule(6, "x.0.wasm")], ["text"], cut={"x.0.wasm": "type mismatch: x"})
        self.assertIn("x.wast:6: module: the cut rejects", str(failure.exception.code))


def malformedText(line, text="unexpected token"):
    return {"type": "assert_malformed", "line": line, "filename": "x.0.wat", "module_type": "text", "text": text}


# A well-formed parity divergence's value: the offset, the digest of Isoline's encoding, the reason.
DIGEST = "sha256:" + "0123456789abcdef" * 4
PARITY = f"0x1b {DIGEST} one type, as the reference"


class DivergenceTest(unittest.TestCase):
    """The divergences divergences.txt records become entries, each checked against its command."""

    COMMANDS = [textModule(3, "x.0.wasm"), malformedText(5), invoke(6), malformedText(7)]
    FORMS = ["text", "quote", None, "text"]

    def entries(self, *divergences, cut=None):
        return manifest(self.COMMANDS, self.FORMS, cut=cut, divergences=divergences)[0]

    def test_recordedDivergencesBecomeEntries(self):
        text = self.entries(importer.Divergence(1, 5, "message", "unknown local $x"),
                            importer.Divergence(0, 3, "parity", PARITY))
        self.assertEqual(text, "commands 4\n0 3 wasm x.0.wasm\n"
                               f"0 3 parity {hexOf(PARITY)}\n"
                               f"1 5 message {hexOf('unknown local $x')}\n")

    def test_divergencesThatDoNotFitFailTheImport(self):
        for divergence, message in (
                (importer.Divergence(4, 9, "message", "x"), "the script has 4 commands"),
                (importer.Divergence(1, 4, "message", "x"), "the command is on line 5; the record is stale"),
                (importer.Divergence(2, 6, "message", "x"), "is no assert_malformed of a module given as text"),
                (importer.Divergence(0, 3, "message", "x"), "is no assert_malformed of a module given as text"),
                (importer.Divergence(1, 5, "message", "unexpected token"), "expects that very message: it is the script's own"),
                (importer.Divergence(1, 5, "parity", PARITY), "the command has no oracle binary"),
                (importer.Divergence(2, 6, "parity", PARITY), "the command has no oracle binary")):
            with self.subTest(divergence=divergence), self.assertRaises(SystemExit) as failure:
                self.entries(divergence)
            self.assertIn(message, str(failure.exception.code))

    def test_aSecondDivergenceOfOneKindFailsTheImport(self):
        with self.assertRaises(SystemExit) as failure:
            self.entries(importer.Divergence(1, 5, "message", "a"), importer.Divergence(1, 5, "message", "b"))
        self.assertIn("a second one for the command", str(failure.exception.code))

    def test_aDivergenceOfASkippedCommandFailsTheImport(self):
        with self.assertRaises(SystemExit) as failure:
            self.entries(importer.Divergence(0, 3, "parity", PARITY), cut={"x.0.wasm": GC_MESSAGE})
        self.assertIn("the command is skipped, so it is never checked", str(failure.exception.code))

    def test_theOracleCarriesWhatIsRecorded(self):
        divergences = [importer.Divergence(1, 5, "message", "unknown local $x"),
                       importer.Divergence(0, 3, "parity", PARITY)]
        self.assertEqual(importer.oracleDivergences(self.entries(*divergences)), divergences[::-1])
        self.assertEqual(importer.oracleDivergences(self.entries()), [])


class ReadDivergencesTest(unittest.TestCase):
    def read(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "divergences.txt"
            path.write_text(text, encoding="utf-8")
            return importer.readDivergences(path)

    def test_rows(self):
        self.assertEqual(self.read(f"# comment\n\nwasm3 core/ref 0 3 parity 0x1b {DIGEST} one  type\n"
                                   "wasm3 core/ref 7 9 message unknown local $x\nwasm3 core/if 2 4 message m\n"),
                         {("wasm3", "core/ref"): [importer.Divergence(0, 3, "parity", f"0x1b {DIGEST} one  type"),
                                                  importer.Divergence(7, 9, "message", "unknown local $x")],
                          ("wasm3", "core/if"): [importer.Divergence(2, 4, "message", "m")]})

    def test_malformedRowsFail(self):
        for text in ("wasm3 core/ref 0 3 parity\n", "wasm3 core/ref x 3 message m\n", "wasm3 core/ref 0 x message m\n",
                     "wasm3 core/ref 0 3 skip m\n", "wasm3 core/ref 0 3 parity 1b why\n",
                     "wasm3 core/ref 0 3 parity 0x1B why\n", "wasm3 core/ref 0 3 parity 0x1b\n",
                     # The offset as the runner prints it, then a SHA-256 of 64 lower-case hex digits, then why.
                     "wasm3 core/ref 0 3 parity 0x1b why\n", f"wasm3 core/ref 0 3 parity 0x1B {DIGEST} why\n",
                     f"wasm3 core/ref 0 3 parity 0x01b {DIGEST} why\n", f"wasm3 core/ref 0 3 parity 0x00 {DIGEST} why\n",
                     f"wasm3 core/ref 0 3 parity 0x {DIGEST} why\n", f"wasm3 core/ref 0 3 parity 0x1b {DIGEST}\n",
                     f"wasm3 core/ref 0 3 parity 0x1b {DIGEST[:-1]} why\n",
                     f"wasm3 core/ref 0 3 parity 0x1b {DIGEST}0 why\n",
                     f"wasm3 core/ref 0 3 parity 0x1b {DIGEST[:-1]}A why\n",
                     f"wasm3 core/ref 0 3 parity 0x1b sha1:{DIGEST[7:]} why\n"):
            with self.subTest(text=text), self.assertRaises(SystemExit):
                self.read(text)

    def test_aMissingFileRecordsNothing(self):
        self.assertEqual(importer.readDivergences(pathlib.Path("/nonexistent/divergences.txt")), {})

    def test_divergencesOfAnotherImportFail(self):
        rows = [importer.Row("wasm3", "core/ref", skips=0),
                importer.Row("wasm3", "core/gc/array", exclude="every module needs GC")]
        importer.checkDivergenceRows({("wasm3", "core/ref"): []}, rows)
        for key in (("wasm3", "core/gc/array"), ("wasm3", "core/gone")):
            with self.subTest(key=key), self.assertRaises(SystemExit):
                importer.checkDivergenceRows({key: []}, rows)

    def test_offsetsWithoutLeadingZerosAreRead(self):
        for offset in ("0x0", "0x10", "0xa0"):
            with self.subTest(offset=offset):
                self.assertEqual(self.read(f"wasm3 core/ref 0 3 parity {offset} {DIGEST} why\n"),
                                 {("wasm3", "core/ref"): [importer.Divergence(0, 3, "parity", f"{offset} {DIGEST} why")]})

    def test_oraclesThatDoNotCarryTheRecordFail(self):
        rows = [importer.Row("wasm3", "core/ref", skips=0),
                importer.Row("wasm3", "core/gc/array", exclude="every module needs GC")]
        recorded = {("wasm3", "core/ref"): [importer.Divergence(0, 3, "parity", PARITY)]}
        carrying = f"commands 1\n0 3 wasm ref.0.wasm\n0 3 parity {hexOf(PARITY)}\n"
        with tempfile.TemporaryDirectory() as directory:
            spec = pathlib.Path(directory)
            (spec / "wasm3" / "core" / "ref").mkdir(parents=True)
            oracle = spec / "wasm3" / "core" / "ref" / "ref.oracle"
            oracle.write_text(carrying, encoding="utf-8")
            importer.checkOracleDivergences(spec, recorded, rows)
            for manifest, divergences in (
                    (carrying, {}),
                    ("commands 1\n0 3 wasm ref.0.wasm\n", recorded),
                    (carrying.replace(hexOf(PARITY), hexOf(PARITY.replace("0x1b", "0x1c"))), recorded),
                    (carrying + f"0 3 message {hexOf('m')}\n", recorded)):
                with self.subTest(manifest=manifest, divergences=divergences), self.assertRaises(SystemExit) as failure:
                    oracle.write_text(manifest, encoding="utf-8")
                    importer.checkOracleDivergences(spec, divergences, rows)
                self.assertIn("re-import the file", str(failure.exception.code))

    def test_theRepositoryRecordIsWellFormed(self):
        # Every row names an import, and every import's .oracle carries exactly its rows.
        rows = [row for row in importer.readFiles(SPEC / "files.txt") if row.exclude is None]
        self.assertTrue((SPEC / importer.DIVERGENCES).is_file())
        divergences = importer.readDivergences(SPEC / importer.DIVERGENCES)
        importer.checkDivergenceRows(divergences, rows)
        importer.checkOracleDivergences(SPEC, divergences, rows)


class ReadFilesTest(unittest.TestCase):
    def read(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "files.txt"
            path.write_text(text, encoding="utf-8")
            return importer.readFiles(path)

    def test_rowsWithPinnedSkipsAndExclusions(self):
        self.assertEqual(self.read("# comment\nwasm2 core/address 1\n\nwasm3   core/simd/simd_lane 0\n"
                                   "wasm3 core/gc/array exclude every module needs GC\n"),
                         [importer.Row("wasm2", "core/address", skips=1),
                          importer.Row("wasm3", "core/simd/simd_lane", skips=0),
                          importer.Row("wasm3", "core/gc/array", exclude="every module needs GC")])

    def test_malformedRowsFail(self):
        for text in ("core/address 1\n", "wasm2 core/address\n", "wasm2 core/address x\n",
                     "wasm2 core/address exclude\n", "wasm2 core/address 1 native\n",
                     # The retired .isotest translation's token is no form any more.
                     "wasm2 core/address 1 isotest\n"):
            with self.subTest(text=text), self.assertRaises(SystemExit):
                self.read(text)

    def test_theRepositoryListIsWellFormed(self):
        # An imported row has its verbatim .wast and its .oracle, and no retired .isotest translation; an
        # excluded row none of them.
        rows = importer.readFiles(SPEC / "files.txt")
        self.assertTrue(rows)
        lock = (SCRIPT.parent / "tools.lock").read_text(encoding="utf-8")
        for row in rows:
            with self.subTest(source=row.source, path=row.path):
                self.assertIn(f"source.{row.source}.commit", lock)
                stem = row.directory(SPEC) / pathlib.PurePosixPath(row.path).name
                present = {suffix for suffix in (".isotest", ".wast", ".oracle") if stem.with_suffix(suffix).is_file()}
                self.assertEqual(present, set() if row.exclude else {".wast", ".oracle"})


TESTS = SCRIPT.parent.parent / "tests"

# The Wasmtime component tests imported (plan P11, Task 20), and the two deliberately not.
CURATED_WASMTIME = ("adapter aliasing borrow-transfer-to-owner core-import-func-subtype core-type-out-of-group-deps "
                    "empty-rec-groups enum_discriminant enums import implements instance lazy-lenders linking map-types "
                    "modules nested nested-many-instantiations post-return resources restrictions simple strings "
                    "string-transcode-invalid tags tail-calls exceptions trap types").split()
DROPPED_WASMTIME = ("big-strings", "same-instance-call")


class ComponentSourcesTest(unittest.TestCase):
    """The component suites' sources and their files, as tools.lock and tests/component-files.txt pin them."""

    def test_component_sources_pinned(self):
        lock = importer.loadFetchTools().readLock(SCRIPT.parent / "tools.lock")
        self.assertEqual(lock["source.component-model.repository"], "https://github.com/WebAssembly/component-model")
        self.assertEqual(lock["source.component-model.commit"], "f78ac03c178ad0f51b307852087792f1f164dc34")
        self.assertEqual(lock["source.component-wasm-tools.repository"], "https://github.com/bytecodealliance/wasm-tools")
        self.assertEqual(lock["source.component-wasm-tools.commit"], "7a2fa422cdf9aeeaf488a2acac8bc94a078bfa32")
        self.assertEqual(lock["source.component-wasmtime.repository"], "https://github.com/bytecodealliance/wasmtime")
        self.assertEqual(lock["source.component-wasmtime.commit"], "32656dc4873fcec5c8662aa937bd01acda3b2e85")
        for source in ("component-model", "component-wasm-tools", "component-wasmtime"):
            with self.subTest(source=source):
                self.assertIn(lock[f"source.{source}.features-from"], importer.FEATURE_SOURCES)
                self.assertTrue(lock[f"source.{source}.tests"])

    def test_cut_matches_the_feature_table(self):
        # The component cut switches on exactly the gates Features.hpp implements, every other one off.
        lock = importer.loadFetchTools().readLock(SCRIPT.parent / "tools.lock")
        cut = lock["component.cut.features"].split(",")
        gates = importer.readGates(importer.FEATURE_TABLE.read_text(encoding="utf-8"))
        for gate in gates:
            with self.subTest(gate=gate.name):
                self.assertEqual(gate.name in cut, gate.implemented())
        self.assertEqual(cut[0], "wasm3")  # which resets every gate first

    def test_curated_wasmtime_list_exact(self):
        rows = [row for row in importer.readComponentFiles(TESTS / "component-files.txt")
                if row.source == "component-wasmtime"]
        self.assertEqual(sorted(row.path for row in rows if row.exclude is None), sorted(CURATED_WASMTIME))
        self.assertEqual(sorted(row.path for row in rows if row.exclude is not None), sorted(DROPPED_WASMTIME))
        for row in rows:
            if row.exclude is not None:
                with self.subTest(path=row.path):
                    self.assertRegex(row.exclude, r"definitions\.py:\d+")

    def test_every_row_is_imported(self):
        for row in importer.readComponentFiles(TESTS / "component-files.txt"):
            stem = row.directory(TESTS) / pathlib.PurePosixPath(row.path).name
            with self.subTest(source=row.source, path=row.path):
                present = {suffix for suffix in (".wast", ".oracle") if stem.with_suffix(suffix).is_file()}
                self.assertEqual(present, set() if row.exclude else {".wast", ".oracle"})


class RunLineTest(unittest.TestCase):
    """Where a component file's features come from: wasm-tools' RUN line, Wasmtime's directives."""

    def test_reads_run_line_features(self):
        self.assertEqual(importer.runLineFeatures(";; RUN: wast --assert default --snapshot tests/snapshots % -f cm-async\n"),
                         ({}, ["cm-async"]))
        self.assertEqual(importer.runLineFeatures(";; RUN: wast % -f cm-map,cm-error-context --assert default\n"),
                         ({}, ["cm-map", "cm-error-context"]))
        self.assertEqual(importer.runLineFeatures(";; RUN: wast --features=-exceptions --assert default %\n"),
                         ({}, ["-exceptions"]))
        self.assertEqual(importer.runLineFeatures(
            ";; RUN: WAST_STRICT_COMPONENT_INDICES=0 wast --assert default % -f gc,cm-gc\n"),
            ({"WAST_STRICT_COMPONENT_INDICES": "0"}, ["gc", "cm-gc"]))
        self.assertEqual(importer.runLineFeatures(";; RUN: wast --assert default %\n"), ({}, []))
        # A lenient RUN line is no `wast` run of the file's own.
        self.assertEqual(importer.runLineFeatures(
            ";; RUN[lenient]: WAST_STRICT_COMPONENT_INDICES=0 parse % -o x\n;; RUN: wast % -f cm64\n"), ({}, ["cm64"]))
        with self.assertRaises(importer.Unsupported):
            importer.runLineFeatures("(component)\n")

    def test_reads_wasmtime_directives(self):
        self.assertEqual(importer.directiveFeatures(";;! component_model_async = true\n;;! gc = false\n;;! hogs_memory = true\n"),
                         ({}, ["cm-async", "-gc"]))
        with self.assertRaises(importer.Unsupported):
            importer.directiveFeatures(";;! something_new = true\n")

    def test_runner_features(self):
        gates = importer.readGates(importer.FEATURE_TABLE.read_text(encoding="utf-8"))
        self.assertEqual(importer.componentFeaturesOf([], gates), ("all", "cm-map,cm-implements"))
        self.assertEqual(importer.componentFeaturesOf(["-cm-map", "-exceptions"], gates),
                         ("all,-exception-handling", "cm-implements"))
        with self.assertRaises(SystemExit):
            importer.componentFeaturesOf(["-tail-call"], gates)


class ComponentSkipsTest(unittest.TestCase):
    """The component suites' skips: only SkipReasons forms, recorded ones proved."""

    def reasons(self):
        return (importer.readSkipReasons(importer.SKIP_REASON_TABLE.read_text(encoding="utf-8")),
                importer.readGates(importer.FEATURE_TABLE.read_text(encoding="utf-8")))

    def read(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "skips.txt"
            path.write_text(text, encoding="utf-8")
            return importer.readComponentSkips(path)

    def test_refuses_unknown_skip_reason(self):
        reasons, gates = self.reasons()
        for reason in ("async (P12)", "gated 🧵", "core feature cut gc", "upstream nyi.txt", "legacy text indices",
                       "oracle unavailable since fb27002f", "wasmtime restriction (restrictions.wast:9)",
                       "diverges from CM pin (definitions.py:1432)"):
            with self.subTest(reason=reason):
                self.assertTrue(importer.isSkipReason(reason, reasons, gates))
        for reason in ("because", "gated 🐙", "async", "oracle unavailable", "core feature cut two words",
                       "diverges from CM pin (Binary.md:350)"):  # a parity reason is no skip's
            with self.subTest(reason=reason):
                self.assertFalse(importer.isSkipReason(reason, reasons, gates))
        # A recorded skip gives only a reason something proves.
        with self.assertRaises(SystemExit):
            self.read("component-model values/strings 3 40 upstream nyi.txt\n")
        with self.assertRaises(SystemExit):
            self.read("component-model values/strings 3 40 because it is hard\n")
        self.assertEqual(self.read("component-model values/strings 3 40 oracle unavailable since fb27002f\n"),
                         {("component-model", "values/strings"): [importer.RecordedSkip(3, 40, "oracle unavailable since fb27002f")]})

    def test_divergence_needs_definitions_line(self):
        for reason in ("diverges from CM pin (definitions.py:)", "diverges from CM pin (definitions.py:0)",
                       "diverges from CM pin (definitions.py:12a)", "diverges from CM pin"):
            with self.subTest(reason=reason), self.assertRaises(SystemExit):
                self.read(f"component-wasmtime types 93 524 {reason}\n")
        self.assertTrue(self.read("component-wasmtime types 93 524 diverges from CM pin (definitions.py:1493)\n"))
        # The line must be one the pinned definitions.py has.
        importer.checkDefinitionsLine("diverges from CM pin (definitions.py:1493)", 3000)
        with self.assertRaises(SystemExit):
            importer.checkDefinitionsLine("diverges from CM pin (definitions.py:3001)", 3000)

    def test_restriction_names_wasmtimes_own_assertion(self):
        for reason in ("wasmtime restriction", "wasmtime restriction (restrictions.wast:)",
                       "wasmtime restriction (restrictions.wast:0)", "wasmtime restriction (simple.wast:9)"):
            with self.subTest(reason=reason), self.assertRaises(SystemExit):
                self.read(f"component-wasmtime simple 4 25 {reason}\n")
        self.assertTrue(self.read("component-wasmtime simple 4 25 wasmtime restriction (restrictions.wast:5)\n"))
        restrictions = ["(assert_invalid", "  (component (import \"x\" (component)))",
                        "  \"root-level component imports are not supported\")", "",
                        "(assert_invalid", "  (component (component (export \"x\")))",
                        "  \"exporting a component from the root component is not supported\")"]
        command = {"type": "assert_invalid", "line": 31,
                   "text": "exporting a component from the root component is not supported"}
        check = importer.checkRestriction
        check("w", "component-wasmtime", "wasmtime restriction (restrictions.wast:5)", command, restrictions)
        check("w", "component-wasmtime", "oracle unavailable since fb27002f", command, [])  # not a restriction
        # Its own definition's name, quoted, is no other refusal.
        reexport = ["(assert_invalid", "  (component", "    (import \"f\" (func $f))", "    (export \"f\" (func $f))",
                    "  )", "  \"component export `f` is a reexport of an imported function which is not implemented\")"]
        check("w", "component-wasmtime", "wasmtime restriction (restrictions.wast:1)",
              {**command, "text": "component export `x` is a reexport of an imported function which is not implemented"},
              reexport)
        for source, line, given in (("component-wasmtime", 1, command),  # another refusal
                                    ("component-wasmtime", 6, command),  # inside a directive, not its start
                                    ("component-wasmtime", 9, command),  # past the file
                                    ("component-model", 5, command),     # another suite's
                                    ("component-wasmtime", 5, {**command, "type": "assert_malformed"})):
            with self.subTest(source=source, line=line), self.assertRaises(SystemExit):
                check("w", source, f"wasmtime restriction (restrictions.wast:{line})", given, restrictions)

    def test_restriction_wasm_tools_passes_is_stale(self):
        reasons, gates = self.reasons()
        row = importer.ComponentRow("component-wasmtime", "simple", skips=1, features="all", componentFeatures="none")
        command = {"type": "assert_invalid", "line": 5, "text": "x"}
        recorded = [importer.RecordedSkip(0, 5, "wasmtime restriction (restrictions.wast:1)")]

        def manifest(failures):
            oracle = importer.ComponentOracle(lambda filename, features: None, set(failures))
            poisoning = importer.ComponentPoisoning("x", oracle, "F", "C", [], nyi=False)
            return importer.componentManifest("simple", row, [command], [None], poisoning, None, recorded, reasons,
                                              gates)

        self.assertEqual(manifest(failures=[0])[2], 1)
        with self.assertRaises(SystemExit):
            manifest(failures=[])

    def test_file_skips(self):
        self.assertEqual(importer.fileSkipOf("component-model", "async/forward"), "async (P12)")
        self.assertEqual(importer.fileSkipOf("component-wasm-tools", "gc/gc"), "core feature cut gc")
        self.assertEqual(importer.fileSkipOf("component-wasm-tools", "fixed-length-lists"), "gated 🔧")
        self.assertIsNone(importer.fileSkipOf("component-wasm-tools", "fixed-length-lists-too"))
        self.assertIsNone(importer.fileSkipOf("component-model", "values/strings"))


class ComponentCutTest(unittest.TestCase):
    """A command whose verdict Isoline's cut changes is skipped for what gives the suite's verdict back."""

    def poisoning(self, verdicts, failures=()):
        def validate(filename, features):
            return verdicts.get((filename, features))
        oracle = importer.ComponentOracle(validate, set(failures))
        candidates = [("cm-async", "async (P12)"), ("cm-threading", "gated 🧵"), ("gc", "core feature cut gc")]
        return importer.ComponentPoisoning("x", oracle, "F", "C", candidates, nyi=False)

    def test_gate_found_by_giving_the_verdict_back(self):
        poisoning = self.poisoning({("m.wasm", "C"): "needs threading", ("m.wasm", "C,cm-async"): "needs threading"})
        self.assertEqual(poisoning.skipOf(0, module(1, filename="m.wasm")), "gated 🧵")
        # Its users are skipped for it too.
        self.assertEqual(poisoning.skipOf(1, invoke(2)), "gated 🧵")

    def test_valid_under_the_cut_is_kept(self):
        self.assertIsNone(self.poisoning({}).skipOf(0, module(1, filename="m.wasm")))

    def test_unexplained_rejection_fails(self):
        poisoning = self.poisoning({key: "broken" for key in [("m.wasm", "C"), ("m.wasm", "C,cm-async"),
                                    ("m.wasm", "C,cm-threading"), ("m.wasm", "C,gc"), ("m.wasm", "C,cm-async,cm-threading,gc")]})
        with self.assertRaises(SystemExit):
            poisoning.skipOf(0, module(1, filename="m.wasm"))

    def test_wast_failure_must_be_recorded(self):
        with self.assertRaises(SystemExit):
            self.poisoning({}, failures=[0]).skipOf(0, module(1, filename="m.wasm"))
        self.assertEqual(self.poisoning({}, failures=[0]).skipOf(0, module(1, filename="m.wasm"),
                                                                 "oracle unavailable since 01234567"),
                         "oracle unavailable since 01234567")
        self.assertEqual(self.poisoning({}, failures=[0]).skipOf(0, module(1, filename="m.wasm"),
                                                                 "wasmtime restriction (restrictions.wast:1)"),
                         "wasmtime restriction (restrictions.wast:1)")


if __name__ == "__main__":
    unittest.main()
