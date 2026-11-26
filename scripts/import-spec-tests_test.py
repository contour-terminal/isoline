#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: Apache-2.0
"""Self-test of import-spec-tests.py: which commands become skip lines, and which fail the import."""

import importlib.util
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


def accepted(filename):
    """The cut of a fixture whose every module is valid: it accepts them all."""
    return None


def lineOf(command, source="x.wast", validate=accepted):
    """The line of one command, translated as the first of its file."""
    return importer.Translator(source, lambda filename: [], validate).lineOf(command)


def skipReason(line):
    kind, reason = line.split(" ")
    assert kind == "skip", line
    return hexText(reason)


def module(line, name=None, text=False, filename=None):
    command = {"type": "module", "line": line, "module_type": "text" if text else "binary",
               "filename": filename or f"m.{line}.{'wat' if text else 'wasm'}"}
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


class TranslateTest(unittest.TestCase):
    def test_moduleDefinition(self):
        self.assertEqual(importer.translate({"type": "module_definition", "line": 3, "name": "M",
                                             "filename": "x.0.wasm", "module_type": "binary"}),
                         ["module_definition", "x.0.wasm", "4d"])
        self.assertEqual(importer.translate({"type": "module_definition", "line": 3, "filename": "x.0.wasm",
                                             "module_type": "binary"}),
                         ["module_definition", "x.0.wasm"])

    def test_moduleInstance(self):
        self.assertEqual(importer.translate({"type": "module_instance", "line": 10, "instance": "I1", "module": "M"}),
                         ["module_instance", "4931", "4d"])
        self.assertEqual(importer.translate({"type": "module_instance", "line": 10, "module": "M"}),
                         ["module_instance", "-", "4d"])

    def test_assertException(self):
        command = {"type": "assert_exception", "line": 39,
                   "action": {"type": "invoke", "field": "throw-if", "args": [{"type": "i32", "value": "10"}]}}
        self.assertEqual(importer.translate(command),
                         ["assert_exception", "invoke", "7468726f772d6966", "-", "i32:a", "->"])

    def test_assertInvalidCustom(self):
        command = {"type": "assert_invalid_custom", "line": 86, "filename": "x.3.wasm", "module_type": "binary",
                   "text": "bad"}
        self.assertEqual(importer.translate(command), ["assert_invalid_custom", "x.3.wasm", "626164"])

    def test_vectorOfIntegerLanes(self):
        lanes = [str(index) for index in range(16)]
        lanes[15] = "-1"  # a signed lane is its bits
        self.assertEqual(importer.valueToken({"type": "v128", "lane_type": "i8", "value": lanes}),
                         "v128:ff0e0d0c0b0a09080706050403020100")
        self.assertEqual(importer.valueToken({"type": "v128", "lane_type": "i64", "value": ["1", "-2"]}),
                         "v128:fffffffffffffffe0000000000000001")

    def test_vectorOfFloatLanes(self):
        exact = {"type": "v128", "lane_type": "f32", "value": ["1065353216", "0", "0", "2143289344"]}
        self.assertEqual(importer.valueToken(exact), "v128:7fc0000000000000000000003f800000")
        withNan = {"type": "v128", "lane_type": "f32", "value": ["nan:canonical", "1065353216", "nan:arithmetic", "0"]}
        self.assertEqual(importer.valueToken(withNan), "v128:f32x4:nan:canonical,3f800000,nan:arithmetic,0")
        wide = {"type": "v128", "lane_type": "f64", "value": ["-9223372036854775808", "nan:canonical"]}
        self.assertEqual(importer.valueToken(wide), "v128:f64x2:8000000000000000,nan:canonical")

    def test_aVectorOfTheWrongLaneCountFails(self):
        with self.assertRaises(importer.Unsupported):
            importer.valueToken({"type": "v128", "lane_type": "i32", "value": ["0", "0"]})

    def test_either(self):
        value = {"type": "either", "values": [{"type": "i32", "value": "1"}, {"type": "i32", "value": "2"}]}
        self.assertEqual(importer.valueToken(value), "either(i32:1;i32:2)")
        nested = {"type": "either", "values": [value]}
        with self.assertRaises(importer.Unsupported):
            importer.valueToken(nested)

    def test_nullOfAnyHeap(self):
        self.assertEqual(importer.valueToken({"type": "refnull"}), "ref.null")

    def test_exceptionReferencesAreUnsupported(self):
        # No imported file has an exnref value yet; one must fail the import, not vanish.
        with self.assertRaises(importer.Unsupported):
            importer.valueToken({"type": "exnref", "value": "null"})
        with self.assertRaises(importer.Unsupported):
            importer.valueToken({"type": "externref"})  # any externref is GC's


class LineOfTest(unittest.TestCase):
    def test_textAssertionsBecomeSkips(self):
        for kind in ("assert_malformed", "assert_invalid", "assert_malformed_custom"):
            with self.subTest(kind=kind):
                command = {"type": kind, "line": 7, "module_type": "text", "filename": "m.wat", "text": "bad"}
                line, isSkip = lineOf(command)
                self.assertTrue(isSkip)
                self.assertEqual(skipReason(line), f"{kind} of a text module (wast line 7)")

    def test_binaryAssertMalformedIsKept(self):
        command = {"type": "assert_malformed", "line": 7, "module_type": "binary", "filename": "m.wasm",
                   "text": "magic header not detected"}
        line, isSkip = lineOf(command, validate=lambda filename: "magic header not detected")
        self.assertFalse(isSkip)
        self.assertTrue(line.startswith("assert_malformed m.wasm "))

    def test_otherTextAssertionsFailTheImport(self):
        for kind in ("assert_unlinkable", "assert_uninstantiable", "assert_invalid_custom"):
            with self.subTest(kind=kind), self.assertRaises(SystemExit) as failure:
                lineOf({"type": kind, "line": 3, "module_type": "text", "filename": "m.wat", "text": "type mismatch"})
            self.assertIn("x.wast:3", str(failure.exception.code))

    def test_binaryAssertMalformedCustomFailsTheImport(self):
        with self.assertRaises(SystemExit):
            lineOf({"type": "assert_malformed_custom", "line": 3, "module_type": "binary", "filename": "m.wasm",
                    "text": "bad"})

    def test_unsupportedValueFailsTheImport(self):
        command = {"type": "assert_return", "line": 12,
                   "action": {"type": "invoke", "field": "f", "args": []},
                   "expected": [{"type": "exnref", "value": "null"}]}
        with self.assertRaises(SystemExit) as failure:
            lineOf(command, "exn.wast")
        self.assertIn("exn.wast:12: value type exnref", str(failure.exception.code))

    def test_unknownCommandFailsTheImport(self):
        with self.assertRaises(SystemExit):
            lineOf({"type": "assert_suspension", "line": 1})

    def test_anInstanceOfTheLatestDefinitionFailsTheImport(self):
        # The format names the definition an instance instantiates; the wast may leave it implicit.
        with self.assertRaises(SystemExit) as failure:
            lineOf({"type": "module_instance", "line": 8, "instance": "I"})
        self.assertIn("x.wast:8: module_instance of the latest definition", str(failure.exception.code))

    def test_aBadImportSectionFailsTheImport(self):
        def readImports(filename):
            return importer.importModuleNames(wasmModule((2, b"\x01\x01a")))  # truncated in the field name

        translator = importer.Translator("x.wast", readImports, accepted)
        translator.lineOf(module(1, text=True))
        translator.lineOf({"type": "register", "line": 2, "as": "t"})
        with self.assertRaises(SystemExit) as failure:
            translator.lineOf(module(3))
        self.assertIn("x.wast:3: a truncated LEB128 number in an import section", str(failure.exception.code))


def translateAll(commands, imports, cut):
    """The lines of commands, translated in order as one file whose module files import from the
    names imports maps them to, and which the cut rejects with the message cut maps them to."""
    translator = importer.Translator("x.wast", lambda filename: imports.get(filename, []),
                                     lambda filename: cut.get(filename))
    return [translator.lineOf(command) for command in commands]


class PoisoningTest(unittest.TestCase):
    """A text module is skipped, and so is every command that uses it, until a module replaces it."""

    def lines(self, commands, imports=None):
        return translateAll(commands, imports or {}, {})

    def test_aTextModuleIsSkipped(self):
        [(line, isSkip)] = self.lines([module(1, text=True)])
        self.assertTrue(isSkip)
        self.assertEqual(skipReason(line), "text: module of a text module (wast line 1)")

    def test_theCurrentModuleIsPoisonedUntilTheNextModule(self):
        results = self.lines([module(1, name="M"), module(2, text=True), invoke(3), invoke(4, module="M"),
                              module(5), invoke(6)])
        self.assertEqual([isSkip for _, isSkip in results], [False, True, True, False, False, False])
        self.assertEqual(skipReason(results[2][0]), "text: module of a text module (root wast line 2; wast line 3)")

    def test_aNamedTextModuleStaysPoisonedByName(self):
        results = self.lines([module(1, name="T", text=True), module(2), invoke(3, module="T"),
                              {"type": "register", "line": 4, "as": "t", "name": "T"},
                              module(5, name="T"), invoke(6, module="T")])
        self.assertEqual([isSkip for _, isSkip in results], [True, False, True, True, False, False])

    def test_registrationsPoisonTheirImporters(self):
        results = self.lines([module(1, text=True), {"type": "register", "line": 2, "as": "t"},
                              module(3, filename="a.wasm"), invoke(4), module(5, filename="b.wasm"),
                              {"type": "assert_unlinkable", "line": 6, "module_type": "binary", "filename": "c.wasm",
                               "text": "unknown import"}],
                             imports={"a.wasm": ["spectest", "t"], "b.wasm": ["spectest"], "c.wasm": ["t"]})
        self.assertEqual([isSkip for _, isSkip in results], [True, True, True, True, False, True])
        self.assertEqual(skipReason(results[3][0]), "text: module of a text module (root wast line 1; wast line 4)")

    def test_aReRegistrationClearsTheName(self):
        results = self.lines([module(1, text=True), {"type": "register", "line": 2, "as": "t"}, module(3),
                              {"type": "register", "line": 4, "as": "t"}, module(5, filename="a.wasm")],
                             imports={"a.wasm": ["t"]})
        self.assertEqual([isSkip for _, isSkip in results], [True, True, False, False, False])

    def test_definitionsAndInstances(self):
        definition = {"type": "module_definition", "line": 1, "name": "D", "module_type": "text",
                      "filename": "d.wat"}
        results = self.lines([module(0), definition, invoke(2),
                              {"type": "module_instance", "line": 3, "instance": "I", "module": "D"},
                              invoke(4), invoke(5, module="I")])
        # A definition is no current module: the unnamed invoke after it still uses module 0.
        self.assertEqual([isSkip for _, isSkip in results], [False, True, False, True, True, True])
        self.assertEqual(skipReason(results[1][0]), "text: module_definition of a text module (wast line 1)")

    def test_anAssertExceptionOfAPoisonedModule(self):
        results = self.lines([module(1, text=True), invoke(2, kind="assert_exception")])
        self.assertEqual([isSkip for _, isSkip in results], [True, True])
        self.assertEqual(skipReason(results[1][0]), "text: module of a text module (root wast line 1; wast line 2)")

    def test_aBinaryDefinitionImportingAPoisonedRegistration(self):
        definition = {"type": "module_definition", "line": 3, "name": "D", "module_type": "binary",
                      "filename": "d.wasm"}
        results = self.lines([module(1, text=True), {"type": "register", "line": 2, "as": "t"}, definition,
                              {"type": "module_instance", "line": 4, "instance": "I", "module": "D"},
                              invoke(5, module="I")],
                             imports={"d.wasm": ["t"]})
        self.assertEqual([isSkip for _, isSkip in results], [True, True, True, True, True])
        self.assertEqual(skipReason(results[4][0]), "text: module of a text module (root wast line 1; wast line 5)")


def assertion(kind, line, filename, text="type mismatch"):
    return {"type": kind, "line": line, "module_type": "binary", "filename": filename, "text": text}


GC_MESSAGE = "rec group usage requires `gc` proposal to be enabled"


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
                [(line, isSkip)] = translateAll([module(1, filename="g.wasm")], {},
                                                {"g.wasm": importer.rejectionMessage(stderr)})
                self.assertTrue(isSkip)
                self.assertTrue(skipReason(line).startswith("gc: "))
        with self.assertRaises(SystemExit) as failure:
            translateAll([module(1, filename="v.wasm")], {},
                         {"v.wasm": importer.rejectionMessage(ORACLE_TYPE_MISMATCH)})
        self.assertIn("not for GC: type mismatch: expected i32, found i64", str(failure.exception.code))


class CutSourcesTest(unittest.TestCase):
    def test_theLockCutsEverySource(self):
        lock = importer.loadFetchTools().readLock(SCRIPT.parent / "tools.lock")
        self.assertNotIn("cut.exempt", lock)
        self.assertIn("-gc", lock["cut.features"].split(","))


class CutTest(unittest.TestCase):
    """The oracle's GC rejections become skips; any other disagreement with the suite fails the import."""

    def lines(self, commands, cut, imports=None):
        return translateAll(commands, imports or {}, cut)

    def test_everyGcMessageIsCut(self):
        for message in ("rec group usage requires `gc` proposal to be enabled",
                        "heap types not supported without the gc feature",
                        "type not valid because the GC proposal is disabled",
                        "gc proposal must be enabled",
                        "constant expression required: global.get of locally defined global"):
            with self.subTest(message=message):
                [(line, isSkip)] = self.lines([module(1, filename="g.wasm")], {"g.wasm": message})
                self.assertTrue(isSkip)
                self.assertEqual(skipReason(line), f"gc: {message} (wast line 1)")

    def test_aCutModulePoisonsItsUses(self):
        results = self.lines([module(1, name="G", filename="g.wasm"), invoke(2), invoke(3, module="G"),
                              {"type": "register", "line": 4, "as": "g", "name": "G"},
                              module(5, filename="a.wasm"), invoke(6), module(7), invoke(8)],
                             {"g.wasm": GC_MESSAGE}, imports={"a.wasm": ["g"]})
        self.assertEqual([isSkip for _, isSkip in results], [True, True, True, True, True, True, False, False])
        self.assertEqual(skipReason(results[5][0]), f"gc: {GC_MESSAGE} (root wast line 1; wast line 6)")

    def test_aCutDefinitionPoisonsItsInstances(self):
        definition = {"type": "module_definition", "line": 1, "name": "D", "module_type": "binary",
                      "filename": "d.wasm"}
        results = self.lines([definition, {"type": "module_instance", "line": 2, "instance": "I", "module": "D"},
                              invoke(3, module="I")],
                             {"d.wasm": GC_MESSAGE})
        self.assertEqual([isSkip for _, isSkip in results], [True, True, True])

    def test_assertionsOfGcModulesAreCut(self):
        for kind in ("assert_invalid", "assert_malformed", "assert_unlinkable", "assert_uninstantiable"):
            with self.subTest(kind=kind):
                [(line, isSkip)] = self.lines([assertion(kind, 9, "g.wasm")], {"g.wasm": GC_MESSAGE})
                self.assertTrue(isSkip)
                self.assertEqual(skipReason(line), f"gc: {GC_MESSAGE} (wast line 9)")

    def test_otherRejectionsOfInvalidModulesAreKept(self):
        for kind in ("assert_invalid", "assert_malformed"):
            with self.subTest(kind=kind):
                [(line, isSkip)] = self.lines([assertion(kind, 9, "i.wasm")], {"i.wasm": "type mismatch: bad"})
                self.assertFalse(isSkip)
                self.assertTrue(line.startswith(f"{kind} i.wasm "))

    def test_anAcceptedInvalidModuleFailsTheImport(self):
        for kind in ("assert_invalid", "assert_malformed"):
            with self.subTest(kind=kind), self.assertRaises(SystemExit) as failure:
                self.lines([assertion(kind, 9, "i.wasm")], {})
            self.assertIn(f"x.wast:9: {kind}: the cut accepts a module", str(failure.exception.code))

    def test_otherRejectionsOfValidModulesFailTheImport(self):
        for command in (module(4, filename="v.wasm"), assertion("assert_unlinkable", 4, "v.wasm"),
                        assertion("assert_uninstantiable", 4, "v.wasm"),
                        {"type": "module_definition", "line": 4, "module_type": "binary", "filename": "v.wasm"}):
            with self.subTest(kind=command["type"]), self.assertRaises(SystemExit) as failure:
                self.lines([command], {"v.wasm": "type mismatch: bad"})
            self.assertIn(f"x.wast:4: {command['type']}: the cut rejects", str(failure.exception.code))

    def test_aSourceWithoutTheCutKeepsEveryModule(self):
        translator = importer.Translator("x.wast", lambda filename: [], None)
        line, isSkip = translator.lineOf(assertion("assert_invalid", 9, "i.wasm"))
        self.assertFalse(isSkip)
        self.assertTrue(line.startswith("assert_invalid i.wasm "))

    def test_aPoisonedModuleIsNotValidated(self):
        # A module importing from a cut registration is skipped as its dependent, whatever the oracle says.
        results = self.lines([module(1, filename="g.wasm"), {"type": "register", "line": 2, "as": "g"},
                              module(3, filename="a.wasm")],
                             {"g.wasm": GC_MESSAGE, "a.wasm": "unknown import"}, imports={"a.wasm": ["g"]})
        self.assertEqual(skipReason(results[2][0]), f"gc: {GC_MESSAGE} (root wast line 1; wast line 3)")


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
                     "wasm2 core/address exclude\n"):
            with self.subTest(text=text), self.assertRaises(SystemExit):
                self.read(text)

    def test_theRepositoryListIsWellFormed(self):
        spec = SCRIPT.parent.parent / "tests" / "spec"
        rows = importer.readFiles(spec / "files.txt")
        self.assertTrue(rows)
        lock = (SCRIPT.parent / "tools.lock").read_text(encoding="utf-8")
        for row in rows:
            with self.subTest(source=row.source, path=row.path):
                self.assertIn(f"source.{row.source}.commit", lock)
                isotest = row.directory(spec) / f"{pathlib.PurePosixPath(row.path).name}.isotest"
                self.assertEqual(isotest.is_file(), row.exclude is None)


if __name__ == "__main__":
    unittest.main()
