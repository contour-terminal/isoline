# Conformance

Isoline implements the WebAssembly core language of the 3.0 release without garbage collection, plus
three proposals beyond 3.0. This page pins, for each feature, the proposal and specification revision it
follows and the spec test suite that proves it. The suites are imported by
`scripts/import-spec-tests.py` into `tests/spec/<source>/<path>/` and run by CTest (label `spec`), one
test per file; `tests/spec/files.txt` names every imported file and pins its skip count.

## Suite sources

The suites are pinned in `scripts/tools.lock` as `source.<name>.repository` and
`source.<name>.commit`.

| Source | Repository | Commit |
|---|---|---|
| `wasm3` | [WebAssembly/spec](https://github.com/WebAssembly/spec), tag `wg-3.0` | `9d36019973201a19f9c9ebb0f10828b2fe2374aa` |
| `wide-arithmetic` | [WebAssembly/wide-arithmetic](https://github.com/WebAssembly/wide-arithmetic) | `024d4ce1b28cee7fd006d9c5175e3b7ece52b429` |
| `custom-page-sizes` | [WebAssembly/custom-page-sizes](https://github.com/WebAssembly/custom-page-sizes) | `61c873a1860acad2772539164f284cc5110f1b1b` |
| `compact-imports` | [WebAssembly/compact-import-section](https://github.com/WebAssembly/compact-import-section) | `0624de829e2663da799e708444dec80337a3fe54` |
| `wasm2` | [WebAssembly/spec](https://github.com/WebAssembly/spec), tag `wg-2.0` | `fffc6e12fa454e475455a7b58d3b5dc343980c10` |

The `wasm2` source holds the 2.0 imports of the files whose 3.0 versions are not imported yet; it
goes once every file has moved to `wasm3`.

## Features

Suite paths are relative to the source's `test/` directory.

| Feature | Proposal | Spec revision | Suite source | Suite files |
|---|---|---|---|---|
| Wasm 1.0 (MVP, mutable-global import/export) | — | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/*.wast` |
| Sign-extension operators | [sign-extension-ops](https://github.com/WebAssembly/sign-extension-ops) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/i32.wast`, `core/i64.wast` |
| Non-trapping float-to-int | [nontrapping-float-to-int-conversions](https://github.com/WebAssembly/nontrapping-float-to-int-conversions) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/conversions.wast` |
| Multi-value | [multi-value](https://github.com/WebAssembly/multi-value) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/block.wast`, `core/func.wast`, `core/if.wast`, `core/loop.wast`, … |
| Reference types | [reference-types](https://github.com/WebAssembly/reference-types) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/ref_*.wast`, `core/table*.wast`, `core/select.wast` |
| Bulk memory | [bulk-memory-operations](https://github.com/WebAssembly/bulk-memory-operations) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/bulk-memory/*.wast` |
| 128-bit SIMD | [simd](https://github.com/WebAssembly/simd) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/simd/*.wast` |
| Tail calls | [tail-call](https://github.com/WebAssembly/tail-call) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/return_call*.wast` |
| Extended constant expressions | [extended-const](https://github.com/WebAssembly/extended-const) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/data.wast`, `core/elem.wast`, `core/global.wast` |
| Typed function references | [function-references](https://github.com/WebAssembly/function-references) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/call_ref.wast`, `core/br_on_*null.wast`, `core/ref_as_non_null.wast`, `core/local_init.wast`, `core/type-equivalence.wast`, … |
| Multiple memories | [multi-memory](https://github.com/WebAssembly/multi-memory) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/multi-memory/*.wast` |
| Relaxed SIMD | [relaxed-simd](https://github.com/WebAssembly/relaxed-simd) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/relaxed-simd/*.wast` |
| Exception handling (`exnref`) | [exception-handling](https://github.com/WebAssembly/exception-handling) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/exceptions/*.wast` |
| memory64 and table64 | [memory64](https://github.com/WebAssembly/memory64) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/memory64/*.wast` |
| Branch hinting | [branch-hinting](https://github.com/WebAssembly/branch-hinting) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `custom/metadata.code.branch_hint/branch_hint.wast` |
| Custom annotations (text format) | [annotations](https://github.com/WebAssembly/annotations) | WebAssembly 3.0 (`wg-3.0`) | `wasm3` | `core/annotations.wast`, `custom/custom/custom_annot.wast`, `custom/name/name_annot.wast` |
| Wide arithmetic | [wide-arithmetic](https://github.com/WebAssembly/wide-arithmetic) (phase 4) | the proposal at the pinned commit | `wide-arithmetic` | `core/wide-arithmetic.wast` |
| Custom page sizes | [custom-page-sizes](https://github.com/WebAssembly/custom-page-sizes) (phase 3) | the proposal at the pinned commit | `custom-page-sizes` | `core/custom-page-sizes/*.wast` |
| Compact import section | [compact-import-section](https://github.com/WebAssembly/compact-import-section) (phase 4) | the proposal at the pinned commit | `compact-imports` | `core/compact-import-section/*.wast` |

## The GC cut

Isoline has typed function references but no garbage collection. The language it accepts is exactly
what the pinned oracle accepts:

```sh
wasm-tools validate --features=wasm3,-gc,wide-arithmetic,compact-imports,custom-page-sizes <module.wasm>
```

with `wasm-tools` at the version `scripts/tools.lock` pins, and the features its `cut.features` names.
The importer runs the oracle on the module of every command in the cut sources (`cut.sources`). A
module it rejects with a message of GC's becomes a `gc:` skip line, together with every command that
uses that module, and a whole file is excluded where all of it is about GC. Any other disagreement
between the oracle and the suite fails the import, so the cut never hides a difference that is not
GC's.

## Exclusions

Generated by `scripts/check-conformance.py --update` from the `exclude` rows of
`tests/spec/files.txt` and the `gc:` skip lines of the imported scripts; the CTest test
`conformance-doc-current` fails when it is not current. Each line is an excluded file,
`<source> <path>: <reason>`, or an assertion the cut skips, `<source> <path>:<wast line>: <oracle
message>`; a command skipped because it uses a cut module names its root, `(root wast line N)`.

<!-- exclusions:begin -->
```text
wasm3 core/data:89: constant expression required: global.get of locally defined global
wasm3 core/data:90: constant expression required: global.get of locally defined global
wasm3 core/elem:178: constant expression required: global.get of locally defined global
wasm3 core/elem:182: constant expression required: global.get of locally defined global
wasm3 core/gc/array: GC: the GC proposal's own tests
wasm3 core/gc/array_copy: GC: the GC proposal's own tests
wasm3 core/gc/array_fill: GC: the GC proposal's own tests
wasm3 core/gc/array_init_data: GC: the GC proposal's own tests
wasm3 core/gc/array_init_elem: GC: the GC proposal's own tests
wasm3 core/gc/array_new_data: GC: the GC proposal's own tests
wasm3 core/gc/array_new_elem: GC: the GC proposal's own tests
wasm3 core/gc/binary-gc: GC: the GC proposal's own tests
wasm3 core/gc/br_on_cast: GC: the GC proposal's own tests
wasm3 core/gc/br_on_cast_fail: GC: the GC proposal's own tests
wasm3 core/gc/extern: GC: the GC proposal's own tests
wasm3 core/gc/i31: GC: the GC proposal's own tests
wasm3 core/gc/ref_cast: GC: the GC proposal's own tests
wasm3 core/gc/ref_eq: GC: the GC proposal's own tests
wasm3 core/gc/ref_test: GC: the GC proposal's own tests
wasm3 core/gc/struct: GC: the GC proposal's own tests
wasm3 core/gc/type-subtyping: GC: the GC proposal's own tests
wasm3 core/global:373: constant expression required: global.get of locally defined global
wasm3 core/global:374: constant expression required: global.get of locally defined global
wasm3 core/global:634: constant expression required: global.get of locally defined global
wasm3 core/global:659: constant expression required: global.get of locally defined global (root wast line 634)
wasm3 core/global:660: constant expression required: global.get of locally defined global (root wast line 634)
wasm3 core/global:661: constant expression required: global.get of locally defined global (root wast line 634)
wasm3 core/global:663: constant expression required: global.get of locally defined global (root wast line 634)
wasm3 core/global:664: constant expression required: global.get of locally defined global (root wast line 634)
wasm3 core/ref_null: GC: the nulls of GC's heap types and anyref
wasm3 core/type-canon: GC: canonicalization of recursive type groups
wasm3 core/type-equivalence:30: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:38: unknown type 0: type index out of bounds because the GC proposal is disabled
wasm3 core/type-equivalence:49: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:136: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:156: rec group usage requires `gc` proposal to be enabled (root wast line 136)
wasm3 core/type-equivalence:161: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:188: rec group usage requires `gc` proposal to be enabled (root wast line 161)
wasm3 core/type-equivalence:233: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:237: rec group usage requires `gc` proposal to be enabled (root wast line 233)
wasm3 core/type-equivalence:238: rec group usage requires `gc` proposal to be enabled (root wast line 233)
wasm3 core/type-equivalence:246: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:256: rec group usage requires `gc` proposal to be enabled (root wast line 246)
wasm3 core/type-equivalence:257: rec group usage requires `gc` proposal to be enabled (root wast line 246)
wasm3 core/type-equivalence:268: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:278: rec group usage requires `gc` proposal to be enabled (root wast line 268)
wasm3 core/type-equivalence:279: rec group usage requires `gc` proposal to be enabled (root wast line 268)
wasm3 core/type-equivalence:290: rec group usage requires `gc` proposal to be enabled
wasm3 core/type-equivalence:307: rec group usage requires `gc` proposal to be enabled (root wast line 290)
wasm3 core/type-equivalence:308: rec group usage requires `gc` proposal to be enabled (root wast line 290)
wasm3 core/type-rec: GC: recursive type groups
wasm3 custom/custom/custom_annot: text format (P10)
wasm3 custom/name/name_annot: text format (P10)
```
<!-- exclusions:end -->

## Skips of text modules

Isoline reads no text format yet, so a command whose module the suite gives as text is skipped, and so
is every command that uses such a module (`text:`). Generated and checked the same way, from the other
skip lines of the imported scripts, `<source> <path>:<wast line>: <reason>`; `tests/spec/files.txt`
pins each file's count.

<!-- text-skips:begin -->
```text
custom-page-sizes core/custom-page-sizes/custom-page-sizes:113: assert_malformed of a text module
custom-page-sizes core/custom-page-sizes/custom-page-sizes-invalid:3: assert_malformed of a text module
custom-page-sizes core/custom-page-sizes/custom-page-sizes-invalid:8: assert_malformed of a text module
custom-page-sizes core/custom-page-sizes/memory_max_i64:28: assert_malformed of a text module
wasm2 core/imports:584: assert_malformed of a text module
wasm2 core/imports:588: assert_malformed of a text module
wasm2 core/imports:592: assert_malformed of a text module
wasm2 core/imports:596: assert_malformed of a text module
wasm2 core/imports:601: assert_malformed of a text module
wasm2 core/imports:605: assert_malformed of a text module
wasm2 core/imports:609: assert_malformed of a text module
wasm2 core/imports:613: assert_malformed of a text module
wasm2 core/imports:618: assert_malformed of a text module
wasm2 core/imports:622: assert_malformed of a text module
wasm2 core/imports:626: assert_malformed of a text module
wasm2 core/imports:630: assert_malformed of a text module
wasm2 core/imports:635: assert_malformed of a text module
wasm2 core/imports:639: assert_malformed of a text module
wasm2 core/imports:643: assert_malformed of a text module
wasm2 core/imports:647: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:1: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:2: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:3: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:4: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:5: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:6: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:7: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:8: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:9: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:10: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:11: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:12: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:13: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:14: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:15: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:16: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:17: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:18: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:19: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:20: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:21: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:22: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:23: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:24: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:25: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:26: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:27: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:28: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:29: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:30: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:31: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:32: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:33: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:34: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:35: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:36: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:37: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:38: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:39: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:40: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:41: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:42: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:43: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:44: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:45: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:46: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:47: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:48: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:49: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:50: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:51: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:52: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:53: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:54: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:55: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:56: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:57: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:58: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:59: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:60: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:61: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:62: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:63: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:64: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:65: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:66: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:67: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:68: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:69: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:70: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:71: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:72: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:73: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:74: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:75: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:76: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:77: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:78: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:79: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:80: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:81: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:82: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:83: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:84: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:85: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:86: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:87: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:88: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:89: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:90: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:91: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:92: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:93: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:94: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:95: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:96: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:97: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:98: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:99: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:100: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:101: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:102: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:103: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:104: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:105: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:106: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:107: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:108: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:109: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:110: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:111: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:112: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:113: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:114: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:115: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:116: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:117: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:118: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:119: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:120: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:121: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:122: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:123: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:124: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:125: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:126: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:127: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:128: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:129: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:130: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:131: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:132: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:133: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:134: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:135: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:136: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:137: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:138: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:139: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:140: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:141: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:142: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:143: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:144: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:145: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:146: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:147: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:148: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:149: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:150: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:151: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:152: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:153: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:154: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:155: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:156: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:157: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:158: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:159: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:160: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:161: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:162: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:163: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:164: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:165: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:166: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:167: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:168: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:169: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:170: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:171: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:172: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:173: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:174: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:175: assert_malformed of a text module
wasm2 core/utf8-invalid-encoding:176: assert_malformed of a text module
wasm3 core/address:214: assert_invalid of a text module
wasm3 core/align:28: assert_malformed of a text module
wasm3 core/align:34: assert_malformed of a text module
wasm3 core/align:40: assert_malformed of a text module
wasm3 core/align:46: assert_malformed of a text module
wasm3 core/align:52: assert_malformed of a text module
wasm3 core/align:58: assert_malformed of a text module
wasm3 core/align:64: assert_malformed of a text module
wasm3 core/align:70: assert_malformed of a text module
wasm3 core/align:76: assert_malformed of a text module
wasm3 core/align:82: assert_malformed of a text module
wasm3 core/align:88: assert_malformed of a text module
wasm3 core/align:94: assert_malformed of a text module
wasm3 core/align:100: assert_malformed of a text module
wasm3 core/align:106: assert_malformed of a text module
wasm3 core/align:112: assert_malformed of a text module
wasm3 core/align:118: assert_malformed of a text module
wasm3 core/align:124: assert_malformed of a text module
wasm3 core/align:130: assert_malformed of a text module
wasm3 core/align:136: assert_malformed of a text module
wasm3 core/align:142: assert_malformed of a text module
wasm3 core/align:148: assert_malformed of a text module
wasm3 core/align:154: assert_malformed of a text module
wasm3 core/align:160: assert_malformed of a text module
wasm3 core/align:166: assert_malformed of a text module
wasm3 core/align:172: assert_malformed of a text module
wasm3 core/align:178: assert_malformed of a text module
wasm3 core/align:184: assert_malformed of a text module
wasm3 core/align:190: assert_malformed of a text module
wasm3 core/align:197: assert_malformed of a text module
wasm3 core/align:203: assert_malformed of a text module
wasm3 core/align:209: assert_malformed of a text module
wasm3 core/align:215: assert_malformed of a text module
wasm3 core/align:221: assert_malformed of a text module
wasm3 core/align:227: assert_malformed of a text module
wasm3 core/align:233: assert_malformed of a text module
wasm3 core/align:239: assert_malformed of a text module
wasm3 core/align:245: assert_malformed of a text module
wasm3 core/align:251: assert_malformed of a text module
wasm3 core/align:257: assert_malformed of a text module
wasm3 core/align:263: assert_malformed of a text module
wasm3 core/align:269: assert_malformed of a text module
wasm3 core/align:275: assert_malformed of a text module
wasm3 core/align:281: assert_malformed of a text module
wasm3 core/align:287: assert_malformed of a text module
wasm3 core/align:293: assert_malformed of a text module
wasm3 core/align:299: assert_malformed of a text module
wasm3 core/annotations:23: assert_malformed of a text module
wasm3 core/annotations:24: assert_malformed of a text module
wasm3 core/annotations:25: assert_malformed of a text module
wasm3 core/annotations:26: assert_malformed of a text module
wasm3 core/annotations:27: assert_malformed of a text module
wasm3 core/annotations:28: assert_malformed of a text module
wasm3 core/annotations:29: assert_malformed of a text module
wasm3 core/annotations:30: assert_malformed of a text module
wasm3 core/annotations:31: assert_malformed of a text module
wasm3 core/annotations:32: text: module of a text module
wasm3 core/annotations:33: text: module of a text module
wasm3 core/annotations:34: assert_malformed of a text module
wasm3 core/annotations:35: assert_malformed of a text module
wasm3 core/annotations:36: text: module of a text module
wasm3 core/annotations:37: assert_malformed of a text module
wasm3 core/annotations:38: assert_malformed of a text module
wasm3 core/annotations:39: assert_malformed of a text module
wasm3 core/annotations:40: assert_malformed of a text module
wasm3 core/annotations:41: assert_malformed of a text module
wasm3 core/annotations:42: assert_malformed of a text module
wasm3 core/annotations:43: assert_malformed of a text module
wasm3 core/annotations:44: assert_malformed of a text module
wasm3 core/annotations:45: assert_malformed of a text module
wasm3 core/annotations:46: assert_malformed of a text module
wasm3 core/annotations:47: assert_malformed of a text module
wasm3 core/annotations:48: assert_malformed of a text module
wasm3 core/annotations:49: assert_malformed of a text module
wasm3 core/annotations:50: assert_malformed of a text module
wasm3 core/annotations:51: assert_malformed of a text module
wasm3 core/annotations:52: assert_malformed of a text module
wasm3 core/annotations:53: assert_malformed of a text module
wasm3 core/annotations:54: assert_malformed of a text module
wasm3 core/annotations:55: text: module of a text module
wasm3 core/annotations:56: assert_malformed of a text module
wasm3 core/annotations:57: assert_malformed of a text module
wasm3 core/annotations:58: assert_malformed of a text module
wasm3 core/annotations:59: assert_malformed of a text module
wasm3 core/annotations:60: assert_malformed of a text module
wasm3 core/annotations:61: assert_malformed of a text module
wasm3 core/annotations:62: assert_malformed of a text module
wasm3 core/annotations:63: assert_malformed of a text module
wasm3 core/annotations:64: assert_malformed of a text module
wasm3 core/annotations:65: assert_malformed of a text module
wasm3 core/annotations:66: assert_malformed of a text module
wasm3 core/annotations:67: assert_malformed of a text module
wasm3 core/annotations:68: assert_malformed of a text module
wasm3 core/annotations:70: assert_malformed of a text module
wasm3 core/annotations:72: assert_malformed of a text module
wasm3 core/annotations:73: assert_malformed of a text module
wasm3 core/annotations:74: assert_malformed of a text module
wasm3 core/annotations:75: assert_malformed of a text module
wasm3 core/annotations:76: assert_malformed of a text module
wasm3 core/annotations:77: assert_malformed of a text module
wasm3 core/annotations:78: assert_malformed of a text module
wasm3 core/annotations:79: assert_malformed of a text module
wasm3 core/annotations:81: assert_malformed of a text module
wasm3 core/annotations:82: assert_malformed of a text module
wasm3 core/annotations:83: assert_malformed of a text module
wasm3 core/annotations:84: assert_malformed of a text module
wasm3 core/annotations:86: assert_malformed of a text module
wasm3 core/annotations:87: assert_malformed of a text module
wasm3 core/annotations:88: assert_malformed of a text module
wasm3 core/annotations:89: assert_malformed of a text module
wasm3 core/annotations:91: assert_malformed of a text module
wasm3 core/annotations:92: assert_malformed of a text module
wasm3 core/annotations:94: assert_malformed of a text module
wasm3 core/annotations:95: assert_malformed of a text module
wasm3 core/annotations:96: assert_malformed of a text module
wasm3 core/annotations:206: text: module of a text module
wasm3 core/annotations:207: text: module of a text module
wasm3 core/block:422: assert_malformed of a text module
wasm3 core/block:429: assert_malformed of a text module
wasm3 core/block:436: assert_malformed of a text module
wasm3 core/block:443: assert_malformed of a text module
wasm3 core/block:450: assert_malformed of a text module
wasm3 core/block:457: assert_malformed of a text module
wasm3 core/block:464: assert_malformed of a text module
wasm3 core/block:468: assert_malformed of a text module
wasm3 core/block:475: assert_malformed of a text module
wasm3 core/block:482: assert_malformed of a text module
wasm3 core/block:489: assert_malformed of a text module
wasm3 core/block:1475: assert_malformed of a text module
wasm3 core/block:1479: assert_malformed of a text module
wasm3 core/block:1485: assert_malformed of a text module
wasm3 core/block:1489: assert_malformed of a text module
wasm3 core/call_indirect:669: assert_malformed of a text module
wasm3 core/call_indirect:681: assert_malformed of a text module
wasm3 core/call_indirect:693: assert_malformed of a text module
wasm3 core/call_indirect:705: assert_malformed of a text module
wasm3 core/call_indirect:717: assert_malformed of a text module
wasm3 core/call_indirect:729: assert_malformed of a text module
wasm3 core/call_indirect:739: assert_malformed of a text module
wasm3 core/call_indirect:746: assert_malformed of a text module
wasm3 core/call_indirect:756: assert_malformed of a text module
wasm3 core/call_indirect:766: assert_malformed of a text module
wasm3 core/call_indirect:776: assert_malformed of a text module
wasm3 core/comments:83: text: module of a text module
wasm3 core/comments:104: text: module of a text module (root wast line 83)
wasm3 core/comments:105: text: module of a text module (root wast line 83)
wasm3 core/comments:106: text: module of a text module (root wast line 83)
wasm3 core/const:8: assert_malformed of a text module
wasm3 core/const:12: assert_malformed of a text module
wasm3 core/const:16: assert_malformed of a text module
wasm3 core/const:20: assert_malformed of a text module
wasm3 core/const:27: assert_malformed of a text module
wasm3 core/const:31: assert_malformed of a text module
wasm3 core/const:35: assert_malformed of a text module
wasm3 core/const:39: assert_malformed of a text module
wasm3 core/const:68: assert_malformed of a text module
wasm3 core/const:72: assert_malformed of a text module
wasm3 core/const:76: assert_malformed of a text module
wasm3 core/const:80: assert_malformed of a text module
wasm3 core/const:84: assert_malformed of a text module
wasm3 core/const:88: assert_malformed of a text module
wasm3 core/const:92: assert_malformed of a text module
wasm3 core/const:96: assert_malformed of a text module
wasm3 core/const:100: assert_malformed of a text module
wasm3 core/const:104: assert_malformed of a text module
wasm3 core/const:108: assert_malformed of a text module
wasm3 core/const:112: assert_malformed of a text module
wasm3 core/const:116: assert_malformed of a text module
wasm3 core/const:120: assert_malformed of a text module
wasm3 core/const:124: assert_malformed of a text module
wasm3 core/const:128: assert_malformed of a text module
wasm3 core/const:132: assert_malformed of a text module
wasm3 core/const:136: assert_malformed of a text module
wasm3 core/const:140: assert_malformed of a text module
wasm3 core/const:185: assert_malformed of a text module
wasm3 core/const:189: assert_malformed of a text module
wasm3 core/const:193: assert_malformed of a text module
wasm3 core/const:197: assert_malformed of a text module
wasm3 core/const:201: assert_malformed of a text module
wasm3 core/const:205: assert_malformed of a text module
wasm3 core/const:209: assert_malformed of a text module
wasm3 core/const:213: assert_malformed of a text module
wasm3 core/const:217: assert_malformed of a text module
wasm3 core/const:221: assert_malformed of a text module
wasm3 core/const:225: assert_malformed of a text module
wasm3 core/const:229: assert_malformed of a text module
wasm3 core/const:233: assert_malformed of a text module
wasm3 core/const:237: assert_malformed of a text module
wasm3 core/const:241: assert_malformed of a text module
wasm3 core/const:245: assert_malformed of a text module
wasm3 core/const:249: assert_malformed of a text module
wasm3 core/const:253: assert_malformed of a text module
wasm3 core/const:257: assert_malformed of a text module
wasm3 core/const:267: assert_malformed of a text module
wasm3 core/const:271: assert_malformed of a text module
wasm3 core/const:278: assert_malformed of a text module
wasm3 core/const:282: assert_malformed of a text module
wasm3 core/const:289: assert_malformed of a text module
wasm3 core/const:293: assert_malformed of a text module
wasm3 core/const:300: assert_malformed of a text module
wasm3 core/const:304: assert_malformed of a text module
wasm3 core/const:319: assert_malformed of a text module
wasm3 core/const:323: assert_malformed of a text module
wasm3 core/const:327: assert_malformed of a text module
wasm3 core/const:331: assert_malformed of a text module
wasm3 core/const:338: assert_malformed of a text module
wasm3 core/const:342: assert_malformed of a text module
wasm3 core/const:349: assert_malformed of a text module
wasm3 core/const:353: assert_malformed of a text module
wasm3 core/const:366: assert_malformed of a text module
wasm3 core/const:370: assert_malformed of a text module
wasm3 core/const:374: assert_malformed of a text module
wasm3 core/const:378: assert_malformed of a text module
wasm3 core/const:385: assert_malformed of a text module
wasm3 core/const:389: assert_malformed of a text module
wasm3 core/const:396: assert_malformed of a text module
wasm3 core/const:400: assert_malformed of a text module
wasm3 core/const:410: assert_malformed of a text module
wasm3 core/const:414: assert_malformed of a text module
wasm3 core/const:419: assert_malformed of a text module
wasm3 core/const:423: assert_malformed of a text module
wasm3 core/const:428: assert_malformed of a text module
wasm3 core/const:432: assert_malformed of a text module
wasm3 core/f32:2537: assert_malformed of a text module
wasm3 core/f32:2541: assert_malformed of a text module
wasm3 core/f64:2537: assert_malformed of a text module
wasm3 core/f64:2541: assert_malformed of a text module
wasm3 core/float_literals:236: assert_malformed of a text module
wasm3 core/float_literals:240: assert_malformed of a text module
wasm3 core/float_literals:244: assert_malformed of a text module
wasm3 core/float_literals:248: assert_malformed of a text module
wasm3 core/float_literals:252: assert_malformed of a text module
wasm3 core/float_literals:256: assert_malformed of a text module
wasm3 core/float_literals:260: assert_malformed of a text module
wasm3 core/float_literals:264: assert_malformed of a text module
wasm3 core/float_literals:268: assert_malformed of a text module
wasm3 core/float_literals:272: assert_malformed of a text module
wasm3 core/float_literals:276: assert_malformed of a text module
wasm3 core/float_literals:280: assert_malformed of a text module
wasm3 core/float_literals:284: assert_malformed of a text module
wasm3 core/float_literals:288: assert_malformed of a text module
wasm3 core/float_literals:292: assert_malformed of a text module
wasm3 core/float_literals:296: assert_malformed of a text module
wasm3 core/float_literals:300: assert_malformed of a text module
wasm3 core/float_literals:304: assert_malformed of a text module
wasm3 core/float_literals:308: assert_malformed of a text module
wasm3 core/float_literals:312: assert_malformed of a text module
wasm3 core/float_literals:316: assert_malformed of a text module
wasm3 core/float_literals:320: assert_malformed of a text module
wasm3 core/float_literals:324: assert_malformed of a text module
wasm3 core/float_literals:328: assert_malformed of a text module
wasm3 core/float_literals:332: assert_malformed of a text module
wasm3 core/float_literals:336: assert_malformed of a text module
wasm3 core/float_literals:340: assert_malformed of a text module
wasm3 core/float_literals:344: assert_malformed of a text module
wasm3 core/float_literals:348: assert_malformed of a text module
wasm3 core/float_literals:352: assert_malformed of a text module
wasm3 core/float_literals:356: assert_malformed of a text module
wasm3 core/float_literals:360: assert_malformed of a text module
wasm3 core/float_literals:364: assert_malformed of a text module
wasm3 core/float_literals:368: assert_malformed of a text module
wasm3 core/float_literals:372: assert_malformed of a text module
wasm3 core/float_literals:376: assert_malformed of a text module
wasm3 core/float_literals:380: assert_malformed of a text module
wasm3 core/float_literals:384: assert_malformed of a text module
wasm3 core/float_literals:388: assert_malformed of a text module
wasm3 core/float_literals:393: assert_malformed of a text module
wasm3 core/float_literals:397: assert_malformed of a text module
wasm3 core/float_literals:401: assert_malformed of a text module
wasm3 core/float_literals:405: assert_malformed of a text module
wasm3 core/float_literals:409: assert_malformed of a text module
wasm3 core/float_literals:413: assert_malformed of a text module
wasm3 core/float_literals:417: assert_malformed of a text module
wasm3 core/float_literals:421: assert_malformed of a text module
wasm3 core/float_literals:425: assert_malformed of a text module
wasm3 core/float_literals:429: assert_malformed of a text module
wasm3 core/float_literals:433: assert_malformed of a text module
wasm3 core/float_literals:437: assert_malformed of a text module
wasm3 core/float_literals:441: assert_malformed of a text module
wasm3 core/float_literals:445: assert_malformed of a text module
wasm3 core/float_literals:449: assert_malformed of a text module
wasm3 core/float_literals:453: assert_malformed of a text module
wasm3 core/float_literals:457: assert_malformed of a text module
wasm3 core/float_literals:461: assert_malformed of a text module
wasm3 core/float_literals:465: assert_malformed of a text module
wasm3 core/float_literals:469: assert_malformed of a text module
wasm3 core/float_literals:473: assert_malformed of a text module
wasm3 core/float_literals:477: assert_malformed of a text module
wasm3 core/float_literals:481: assert_malformed of a text module
wasm3 core/float_literals:485: assert_malformed of a text module
wasm3 core/float_literals:489: assert_malformed of a text module
wasm3 core/float_literals:493: assert_malformed of a text module
wasm3 core/float_literals:497: assert_malformed of a text module
wasm3 core/float_literals:501: assert_malformed of a text module
wasm3 core/float_literals:505: assert_malformed of a text module
wasm3 core/float_literals:509: assert_malformed of a text module
wasm3 core/float_literals:513: assert_malformed of a text module
wasm3 core/float_literals:517: assert_malformed of a text module
wasm3 core/float_literals:521: assert_malformed of a text module
wasm3 core/float_literals:525: assert_malformed of a text module
wasm3 core/float_literals:529: assert_malformed of a text module
wasm3 core/float_literals:533: assert_malformed of a text module
wasm3 core/float_literals:537: assert_malformed of a text module
wasm3 core/float_literals:541: assert_malformed of a text module
wasm3 core/float_literals:545: assert_malformed of a text module
wasm3 core/func:448: assert_malformed of a text module
wasm3 core/func:560: assert_malformed of a text module
wasm3 core/func:567: assert_malformed of a text module
wasm3 core/func:574: assert_malformed of a text module
wasm3 core/func:581: assert_malformed of a text module
wasm3 core/func:588: assert_malformed of a text module
wasm3 core/func:595: assert_malformed of a text module
wasm3 core/func:602: assert_malformed of a text module
wasm3 core/func:609: assert_malformed of a text module
wasm3 core/func:616: assert_malformed of a text module
wasm3 core/func:623: assert_malformed of a text module
wasm3 core/func:938: assert_malformed of a text module
wasm3 core/func:942: assert_malformed of a text module
wasm3 core/func:946: assert_malformed of a text module
wasm3 core/func:950: assert_malformed of a text module
wasm3 core/func:954: assert_malformed of a text module
wasm3 core/func:958: assert_malformed of a text module
wasm3 core/func:965: assert_malformed of a text module
wasm3 core/func:969: assert_malformed of a text module
wasm3 core/func:973: assert_malformed of a text module
wasm3 core/func:978: assert_malformed of a text module
wasm3 core/func:982: assert_malformed of a text module
wasm3 core/func:986: assert_malformed of a text module
wasm3 core/global:686: assert_malformed of a text module
wasm3 core/global:693: assert_malformed of a text module
wasm3 core/global:700: assert_malformed of a text module
wasm3 core/i32:979: assert_malformed of a text module
wasm3 core/i32:983: assert_malformed of a text module
wasm3 core/i64:488: assert_malformed of a text module
wasm3 core/i64:492: assert_malformed of a text module
wasm3 core/id:26: assert_malformed of a text module
wasm3 core/id:27: assert_malformed of a text module
wasm3 core/id:28: assert_malformed of a text module
wasm3 core/id:29: assert_malformed of a text module
wasm3 core/id:30: assert_malformed of a text module
wasm3 core/id:31: assert_malformed of a text module
wasm3 core/if:736: assert_malformed of a text module
wasm3 core/if:745: assert_malformed of a text module
wasm3 core/if:754: assert_malformed of a text module
wasm3 core/if:763: assert_malformed of a text module
wasm3 core/if:772: assert_malformed of a text module
wasm3 core/if:781: assert_malformed of a text module
wasm3 core/if:788: assert_malformed of a text module
wasm3 core/if:796: assert_malformed of a text module
wasm3 core/if:806: assert_malformed of a text module
wasm3 core/if:816: assert_malformed of a text module
wasm3 core/if:826: assert_malformed of a text module
wasm3 core/if:1513: assert_malformed of a text module
wasm3 core/if:1517: assert_malformed of a text module
wasm3 core/if:1522: assert_malformed of a text module
wasm3 core/if:1526: assert_malformed of a text module
wasm3 core/if:1530: assert_malformed of a text module
wasm3 core/if:1534: assert_malformed of a text module
wasm3 core/if:1538: assert_malformed of a text module
wasm3 core/if:1542: assert_malformed of a text module
wasm3 core/if:1546: assert_malformed of a text module
wasm3 core/if:1550: assert_malformed of a text module
wasm3 core/if:1554: assert_malformed of a text module
wasm3 core/if:1558: assert_malformed of a text module
wasm3 core/if:1562: assert_malformed of a text module
wasm3 core/int_literals:72: assert_malformed of a text module
wasm3 core/int_literals:76: assert_malformed of a text module
wasm3 core/int_literals:80: assert_malformed of a text module
wasm3 core/int_literals:84: assert_malformed of a text module
wasm3 core/int_literals:88: assert_malformed of a text module
wasm3 core/int_literals:92: assert_malformed of a text module
wasm3 core/int_literals:96: assert_malformed of a text module
wasm3 core/int_literals:100: assert_malformed of a text module
wasm3 core/int_literals:104: assert_malformed of a text module
wasm3 core/int_literals:108: assert_malformed of a text module
wasm3 core/int_literals:113: assert_malformed of a text module
wasm3 core/int_literals:117: assert_malformed of a text module
wasm3 core/int_literals:121: assert_malformed of a text module
wasm3 core/int_literals:125: assert_malformed of a text module
wasm3 core/int_literals:129: assert_malformed of a text module
wasm3 core/int_literals:133: assert_malformed of a text module
wasm3 core/int_literals:137: assert_malformed of a text module
wasm3 core/int_literals:141: assert_malformed of a text module
wasm3 core/int_literals:145: assert_malformed of a text module
wasm3 core/int_literals:149: assert_malformed of a text module
wasm3 core/load:214: assert_malformed of a text module
wasm3 core/load:221: assert_malformed of a text module
wasm3 core/load:228: assert_malformed of a text module
wasm3 core/load:235: assert_malformed of a text module
wasm3 core/load:242: assert_malformed of a text module
wasm3 core/load:249: assert_malformed of a text module
wasm3 core/load:257: assert_malformed of a text module
wasm3 core/load:264: assert_malformed of a text module
wasm3 core/load:271: assert_malformed of a text module
wasm3 core/load:279: assert_malformed of a text module
wasm3 core/load:286: assert_malformed of a text module
wasm3 core/load:294: assert_malformed of a text module
wasm3 core/load:301: assert_malformed of a text module
wasm3 core/loop:526: assert_malformed of a text module
wasm3 core/loop:533: assert_malformed of a text module
wasm3 core/loop:540: assert_malformed of a text module
wasm3 core/loop:547: assert_malformed of a text module
wasm3 core/loop:554: assert_malformed of a text module
wasm3 core/loop:561: assert_malformed of a text module
wasm3 core/loop:568: assert_malformed of a text module
wasm3 core/loop:572: assert_malformed of a text module
wasm3 core/loop:579: assert_malformed of a text module
wasm3 core/loop:586: assert_malformed of a text module
wasm3 core/loop:593: assert_malformed of a text module
wasm3 core/loop:770: assert_malformed of a text module
wasm3 core/loop:774: assert_malformed of a text module
wasm3 core/loop:779: assert_malformed of a text module
wasm3 core/loop:783: assert_malformed of a text module
wasm3 core/memory:242: assert_malformed of a text module
wasm3 core/memory:246: assert_malformed of a text module
wasm3 core/memory:250: assert_malformed of a text module
wasm3 core/memory64/align64:28: assert_malformed of a text module
wasm3 core/memory64/align64:34: assert_malformed of a text module
wasm3 core/memory64/align64:40: assert_malformed of a text module
wasm3 core/memory64/align64:46: assert_malformed of a text module
wasm3 core/memory64/align64:52: assert_malformed of a text module
wasm3 core/memory64/align64:58: assert_malformed of a text module
wasm3 core/memory64/align64:64: assert_malformed of a text module
wasm3 core/memory64/align64:70: assert_malformed of a text module
wasm3 core/memory64/align64:76: assert_malformed of a text module
wasm3 core/memory64/align64:82: assert_malformed of a text module
wasm3 core/memory64/align64:88: assert_malformed of a text module
wasm3 core/memory64/align64:94: assert_malformed of a text module
wasm3 core/memory64/align64:100: assert_malformed of a text module
wasm3 core/memory64/align64:106: assert_malformed of a text module
wasm3 core/memory64/align64:112: assert_malformed of a text module
wasm3 core/memory64/align64:118: assert_malformed of a text module
wasm3 core/memory64/align64:124: assert_malformed of a text module
wasm3 core/memory64/align64:130: assert_malformed of a text module
wasm3 core/memory64/align64:136: assert_malformed of a text module
wasm3 core/memory64/align64:142: assert_malformed of a text module
wasm3 core/memory64/align64:148: assert_malformed of a text module
wasm3 core/memory64/align64:154: assert_malformed of a text module
wasm3 core/memory64/align64:160: assert_malformed of a text module
wasm3 core/memory64/align64:166: assert_malformed of a text module
wasm3 core/memory64/align64:172: assert_malformed of a text module
wasm3 core/memory64/align64:178: assert_malformed of a text module
wasm3 core/memory64/align64:184: assert_malformed of a text module
wasm3 core/memory64/align64:190: assert_malformed of a text module
wasm3 core/memory64/align64:197: assert_malformed of a text module
wasm3 core/memory64/align64:203: assert_malformed of a text module
wasm3 core/memory64/align64:209: assert_malformed of a text module
wasm3 core/memory64/align64:215: assert_malformed of a text module
wasm3 core/memory64/align64:221: assert_malformed of a text module
wasm3 core/memory64/align64:227: assert_malformed of a text module
wasm3 core/memory64/align64:233: assert_malformed of a text module
wasm3 core/memory64/align64:239: assert_malformed of a text module
wasm3 core/memory64/align64:245: assert_malformed of a text module
wasm3 core/memory64/align64:251: assert_malformed of a text module
wasm3 core/memory64/align64:257: assert_malformed of a text module
wasm3 core/memory64/align64:263: assert_malformed of a text module
wasm3 core/memory64/align64:269: assert_malformed of a text module
wasm3 core/memory64/align64:275: assert_malformed of a text module
wasm3 core/memory64/align64:281: assert_malformed of a text module
wasm3 core/memory64/align64:287: assert_malformed of a text module
wasm3 core/memory64/align64:293: assert_malformed of a text module
wasm3 core/memory64/align64:299: assert_malformed of a text module
wasm3 core/memory64/load64:214: assert_malformed of a text module
wasm3 core/memory64/load64:221: assert_malformed of a text module
wasm3 core/memory64/load64:228: assert_malformed of a text module
wasm3 core/memory64/load64:235: assert_malformed of a text module
wasm3 core/memory64/load64:242: assert_malformed of a text module
wasm3 core/memory64/load64:249: assert_malformed of a text module
wasm3 core/memory64/load64:257: assert_malformed of a text module
wasm3 core/memory64/load64:264: assert_malformed of a text module
wasm3 core/memory64/load64:271: assert_malformed of a text module
wasm3 core/memory64/load64:279: assert_malformed of a text module
wasm3 core/memory64/load64:286: assert_malformed of a text module
wasm3 core/memory64/load64:294: assert_malformed of a text module
wasm3 core/memory64/load64:301: assert_malformed of a text module
wasm3 core/obsolete-keywords:3: assert_malformed of a text module
wasm3 core/obsolete-keywords:11: assert_malformed of a text module
wasm3 core/obsolete-keywords:20: assert_malformed of a text module
wasm3 core/obsolete-keywords:27: assert_malformed of a text module
wasm3 core/obsolete-keywords:34: assert_malformed of a text module
wasm3 core/obsolete-keywords:41: assert_malformed of a text module
wasm3 core/obsolete-keywords:48: assert_malformed of a text module
wasm3 core/obsolete-keywords:56: assert_malformed of a text module
wasm3 core/obsolete-keywords:64: assert_malformed of a text module
wasm3 core/obsolete-keywords:71: assert_malformed of a text module
wasm3 core/obsolete-keywords:78: assert_malformed of a text module
wasm3 core/simd/simd_address:113: assert_malformed of a text module
wasm3 core/simd/simd_address:131: assert_malformed of a text module
wasm3 core/simd/simd_address:144: assert_invalid of a text module
wasm3 core/simd/simd_address:152: assert_invalid of a text module
wasm3 core/simd/simd_align:105: assert_malformed of a text module
wasm3 core/simd/simd_align:111: assert_malformed of a text module
wasm3 core/simd/simd_align:117: assert_malformed of a text module
wasm3 core/simd/simd_align:123: assert_malformed of a text module
wasm3 core/simd/simd_align:129: assert_malformed of a text module
wasm3 core/simd/simd_align:135: assert_malformed of a text module
wasm3 core/simd/simd_align:141: assert_malformed of a text module
wasm3 core/simd/simd_align:147: assert_malformed of a text module
wasm3 core/simd/simd_align:153: assert_malformed of a text module
wasm3 core/simd/simd_align:159: assert_malformed of a text module
wasm3 core/simd/simd_align:165: assert_malformed of a text module
wasm3 core/simd/simd_align:171: assert_malformed of a text module
wasm3 core/simd/simd_align:177: assert_malformed of a text module
wasm3 core/simd/simd_align:183: assert_malformed of a text module
wasm3 core/simd/simd_align:189: assert_malformed of a text module
wasm3 core/simd/simd_align:195: assert_malformed of a text module
wasm3 core/simd/simd_align:201: assert_malformed of a text module
wasm3 core/simd/simd_align:207: assert_malformed of a text module
wasm3 core/simd/simd_align:213: assert_malformed of a text module
wasm3 core/simd/simd_align:219: assert_malformed of a text module
wasm3 core/simd/simd_align:225: assert_malformed of a text module
wasm3 core/simd/simd_align:231: assert_malformed of a text module
wasm3 core/simd/simd_align:237: assert_malformed of a text module
wasm3 core/simd/simd_align:243: assert_malformed of a text module
wasm3 core/simd/simd_align:249: assert_malformed of a text module
wasm3 core/simd/simd_align:255: assert_malformed of a text module
wasm3 core/simd/simd_align:261: assert_malformed of a text module
wasm3 core/simd/simd_align:267: assert_malformed of a text module
wasm3 core/simd/simd_align:273: assert_malformed of a text module
wasm3 core/simd/simd_align:279: assert_malformed of a text module
wasm3 core/simd/simd_align:285: assert_malformed of a text module
wasm3 core/simd/simd_align:291: assert_malformed of a text module
wasm3 core/simd/simd_align:297: assert_malformed of a text module
wasm3 core/simd/simd_align:303: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:991: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:992: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:993: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:994: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:995: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:996: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:997: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:998: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:999: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:1000: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:1001: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:1002: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:1003: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:1004: assert_malformed of a text module
wasm3 core/simd/simd_bit_shift:1005: assert_malformed of a text module
wasm3 core/simd/simd_boolean:1004: assert_malformed of a text module
wasm3 core/simd/simd_boolean:1005: assert_malformed of a text module
wasm3 core/simd/simd_boolean:1006: assert_malformed of a text module
wasm3 core/simd/simd_boolean:1007: assert_malformed of a text module
wasm3 core/simd/simd_const:130: assert_malformed of a text module
wasm3 core/simd/simd_const:134: assert_malformed of a text module
wasm3 core/simd/simd_const:138: assert_malformed of a text module
wasm3 core/simd/simd_const:142: assert_malformed of a text module
wasm3 core/simd/simd_const:146: assert_malformed of a text module
wasm3 core/simd/simd_const:150: assert_malformed of a text module
wasm3 core/simd/simd_const:154: assert_malformed of a text module
wasm3 core/simd/simd_const:158: assert_malformed of a text module
wasm3 core/simd/simd_const:162: assert_malformed of a text module
wasm3 core/simd/simd_const:166: assert_malformed of a text module
wasm3 core/simd/simd_const:170: assert_malformed of a text module
wasm3 core/simd/simd_const:174: assert_malformed of a text module
wasm3 core/simd/simd_const:178: assert_malformed of a text module
wasm3 core/simd/simd_const:182: assert_malformed of a text module
wasm3 core/simd/simd_const:186: assert_malformed of a text module
wasm3 core/simd/simd_const:190: assert_malformed of a text module
wasm3 core/simd/simd_const:194: assert_malformed of a text module
wasm3 core/simd/simd_const:198: assert_malformed of a text module
wasm3 core/simd/simd_const:203: assert_malformed of a text module
wasm3 core/simd/simd_const:209: assert_malformed of a text module
wasm3 core/simd/simd_const:214: assert_malformed of a text module
wasm3 core/simd/simd_const:219: assert_malformed of a text module
wasm3 core/simd/simd_const:225: assert_malformed of a text module
wasm3 core/simd/simd_const:231: assert_malformed of a text module
wasm3 core/simd/simd_const:236: assert_malformed of a text module
wasm3 core/simd/simd_const:240: assert_malformed of a text module
wasm3 core/simd/simd_const:244: assert_malformed of a text module
wasm3 core/simd/simd_const:248: assert_malformed of a text module
wasm3 core/simd/simd_const:252: assert_malformed of a text module
wasm3 core/simd/simd_const:257: assert_malformed of a text module
wasm3 core/simd/simd_const:261: assert_malformed of a text module
wasm3 core/simd/simd_const:265: assert_malformed of a text module
wasm3 core/simd/simd_const:269: assert_malformed of a text module
wasm3 core/simd/simd_const:274: assert_malformed of a text module
wasm3 core/simd/simd_const:278: assert_malformed of a text module
wasm3 core/simd/simd_const:282: assert_malformed of a text module
wasm3 core/simd/simd_const:286: assert_malformed of a text module
wasm3 core/simd/simd_const:291: assert_malformed of a text module
wasm3 core/simd/simd_const:295: assert_malformed of a text module
wasm3 core/simd/simd_const:299: assert_malformed of a text module
wasm3 core/simd/simd_const:303: assert_malformed of a text module
wasm3 core/simd/simd_const:308: assert_malformed of a text module
wasm3 core/simd/simd_const:312: assert_malformed of a text module
wasm3 core/simd/simd_const:316: assert_malformed of a text module
wasm3 core/simd/simd_const:320: assert_malformed of a text module
wasm3 core/simd/simd_const:324: assert_malformed of a text module
wasm3 core/simd/simd_const:328: assert_malformed of a text module
wasm3 core/simd/simd_const:332: assert_malformed of a text module
wasm3 core/simd/simd_const:336: assert_malformed of a text module
wasm3 core/simd/simd_const:340: assert_malformed of a text module
wasm3 core/simd/simd_const:344: assert_malformed of a text module
wasm3 core/simd/simd_const:348: assert_malformed of a text module
wasm3 core/simd/simd_const:352: assert_malformed of a text module
wasm3 core/simd/simd_const:356: assert_malformed of a text module
wasm3 core/simd/simd_const:360: assert_malformed of a text module
wasm3 core/simd/simd_const:364: assert_malformed of a text module
wasm3 core/simd/simd_const:368: assert_malformed of a text module
wasm3 core/simd/simd_const:372: assert_malformed of a text module
wasm3 core/simd/simd_const:376: assert_malformed of a text module
wasm3 core/simd/simd_const:380: assert_malformed of a text module
wasm3 core/simd/simd_const:384: assert_malformed of a text module
wasm3 core/simd/simd_const:388: assert_malformed of a text module
wasm3 core/simd/simd_const:393: assert_malformed of a text module
wasm3 core/simd/simd_const:397: assert_malformed of a text module
wasm3 core/simd/simd_const:401: assert_malformed of a text module
wasm3 core/simd/simd_const:405: assert_malformed of a text module
wasm3 core/simd/simd_const:409: assert_malformed of a text module
wasm3 core/simd/simd_const:413: assert_malformed of a text module
wasm3 core/simd/simd_const:417: assert_malformed of a text module
wasm3 core/simd/simd_const:421: assert_malformed of a text module
wasm3 core/simd/simd_const:425: assert_malformed of a text module
wasm3 core/simd/simd_const:429: assert_malformed of a text module
wasm3 core/simd/simd_const:433: assert_malformed of a text module
wasm3 core/simd/simd_const:437: assert_malformed of a text module
wasm3 core/simd/simd_const:441: assert_malformed of a text module
wasm3 core/simd/simd_const:445: assert_malformed of a text module
wasm3 core/simd/simd_const:449: assert_malformed of a text module
wasm3 core/simd/simd_const:453: assert_malformed of a text module
wasm3 core/simd/simd_const:457: assert_malformed of a text module
wasm3 core/simd/simd_const:461: assert_malformed of a text module
wasm3 core/simd/simd_const:465: assert_malformed of a text module
wasm3 core/simd/simd_const:469: assert_malformed of a text module
wasm3 core/simd/simd_const:473: assert_malformed of a text module
wasm3 core/simd/simd_const:480: assert_malformed of a text module
wasm3 core/simd/simd_const:486: assert_malformed of a text module
wasm3 core/simd/simd_const:1135: assert_malformed of a text module
wasm3 core/simd/simd_const:1139: assert_malformed of a text module
wasm3 core/simd/simd_const:1143: assert_malformed of a text module
wasm3 core/simd/simd_const:1147: assert_malformed of a text module
wasm3 core/simd/simd_const:1151: assert_malformed of a text module
wasm3 core/simd/simd_const:1155: assert_malformed of a text module
wasm3 core/simd/simd_const:1159: assert_malformed of a text module
wasm3 core/simd/simd_const:1163: assert_malformed of a text module
wasm3 core/simd/simd_const:1167: assert_malformed of a text module
wasm3 core/simd/simd_const:1171: assert_malformed of a text module
wasm3 core/simd/simd_const:1176: assert_malformed of a text module
wasm3 core/simd/simd_const:1180: assert_malformed of a text module
wasm3 core/simd/simd_const:1184: assert_malformed of a text module
wasm3 core/simd/simd_const:1188: assert_malformed of a text module
wasm3 core/simd/simd_const:1192: assert_malformed of a text module
wasm3 core/simd/simd_const:1196: assert_malformed of a text module
wasm3 core/simd/simd_const:1200: assert_malformed of a text module
wasm3 core/simd/simd_const:1204: assert_malformed of a text module
wasm3 core/simd/simd_const:1208: assert_malformed of a text module
wasm3 core/simd/simd_const:1212: assert_malformed of a text module
wasm3 core/simd/simd_const:1263: assert_malformed of a text module
wasm3 core/simd/simd_const:1267: assert_malformed of a text module
wasm3 core/simd/simd_const:1271: assert_malformed of a text module
wasm3 core/simd/simd_const:1275: assert_malformed of a text module
wasm3 core/simd/simd_const:1279: assert_malformed of a text module
wasm3 core/simd/simd_const:1283: assert_malformed of a text module
wasm3 core/simd/simd_const:1287: assert_malformed of a text module
wasm3 core/simd/simd_const:1291: assert_malformed of a text module
wasm3 core/simd/simd_const:1295: assert_malformed of a text module
wasm3 core/simd/simd_const:1299: assert_malformed of a text module
wasm3 core/simd/simd_const:1303: assert_malformed of a text module
wasm3 core/simd/simd_const:1307: assert_malformed of a text module
wasm3 core/simd/simd_const:1311: assert_malformed of a text module
wasm3 core/simd/simd_const:1315: assert_malformed of a text module
wasm3 core/simd/simd_const:1319: assert_malformed of a text module
wasm3 core/simd/simd_const:1323: assert_malformed of a text module
wasm3 core/simd/simd_const:1327: assert_malformed of a text module
wasm3 core/simd/simd_const:1331: assert_malformed of a text module
wasm3 core/simd/simd_const:1335: assert_malformed of a text module
wasm3 core/simd/simd_const:1339: assert_malformed of a text module
wasm3 core/simd/simd_const:1343: assert_malformed of a text module
wasm3 core/simd/simd_const:1347: assert_malformed of a text module
wasm3 core/simd/simd_const:1351: assert_malformed of a text module
wasm3 core/simd/simd_const:1355: assert_malformed of a text module
wasm3 core/simd/simd_const:1359: assert_malformed of a text module
wasm3 core/simd/simd_const:1363: assert_malformed of a text module
wasm3 core/simd/simd_const:1367: assert_malformed of a text module
wasm3 core/simd/simd_const:1371: assert_malformed of a text module
wasm3 core/simd/simd_const:1375: assert_malformed of a text module
wasm3 core/simd/simd_const:1379: assert_malformed of a text module
wasm3 core/simd/simd_const:1383: assert_malformed of a text module
wasm3 core/simd/simd_const:1387: assert_malformed of a text module
wasm3 core/simd/simd_const:1391: assert_malformed of a text module
wasm3 core/simd/simd_const:1395: assert_malformed of a text module
wasm3 core/simd/simd_const:1399: assert_malformed of a text module
wasm3 core/simd/simd_const:1403: assert_malformed of a text module
wasm3 core/simd/simd_const:1407: assert_malformed of a text module
wasm3 core/simd/simd_const:1411: assert_malformed of a text module
wasm3 core/simd/simd_const:1416: assert_malformed of a text module
wasm3 core/simd/simd_const:1420: assert_malformed of a text module
wasm3 core/simd/simd_const:1424: assert_malformed of a text module
wasm3 core/simd/simd_const:1428: assert_malformed of a text module
wasm3 core/simd/simd_const:1432: assert_malformed of a text module
wasm3 core/simd/simd_const:1436: assert_malformed of a text module
wasm3 core/simd/simd_const:1440: assert_malformed of a text module
wasm3 core/simd/simd_const:1444: assert_malformed of a text module
wasm3 core/simd/simd_const:1448: assert_malformed of a text module
wasm3 core/simd/simd_const:1452: assert_malformed of a text module
wasm3 core/simd/simd_const:1456: assert_malformed of a text module
wasm3 core/simd/simd_const:1460: assert_malformed of a text module
wasm3 core/simd/simd_const:1464: assert_malformed of a text module
wasm3 core/simd/simd_const:1468: assert_malformed of a text module
wasm3 core/simd/simd_const:1472: assert_malformed of a text module
wasm3 core/simd/simd_const:1476: assert_malformed of a text module
wasm3 core/simd/simd_const:1480: assert_malformed of a text module
wasm3 core/simd/simd_const:1484: assert_malformed of a text module
wasm3 core/simd/simd_const:1488: assert_malformed of a text module
wasm3 core/simd/simd_const:1492: assert_malformed of a text module
wasm3 core/simd/simd_const:1496: assert_malformed of a text module
wasm3 core/simd/simd_const:1500: assert_malformed of a text module
wasm3 core/simd/simd_const:1504: assert_malformed of a text module
wasm3 core/simd/simd_const:1508: assert_malformed of a text module
wasm3 core/simd/simd_const:1512: assert_malformed of a text module
wasm3 core/simd/simd_const:1516: assert_malformed of a text module
wasm3 core/simd/simd_const:1520: assert_malformed of a text module
wasm3 core/simd/simd_const:1524: assert_malformed of a text module
wasm3 core/simd/simd_const:1528: assert_malformed of a text module
wasm3 core/simd/simd_const:1532: assert_malformed of a text module
wasm3 core/simd/simd_const:1536: assert_malformed of a text module
wasm3 core/simd/simd_const:1540: assert_malformed of a text module
wasm3 core/simd/simd_const:1544: assert_malformed of a text module
wasm3 core/simd/simd_const:1548: assert_malformed of a text module
wasm3 core/simd/simd_const:1552: assert_malformed of a text module
wasm3 core/simd/simd_const:1556: assert_malformed of a text module
wasm3 core/simd/simd_const:1560: assert_malformed of a text module
wasm3 core/simd/simd_const:1564: assert_malformed of a text module
wasm3 core/simd/simd_conversions:605: assert_malformed of a text module
wasm3 core/simd/simd_conversions:608: assert_malformed of a text module
wasm3 core/simd/simd_conversions:611: assert_malformed of a text module
wasm3 core/simd/simd_conversions:614: assert_malformed of a text module
wasm3 core/simd/simd_conversions:617: assert_malformed of a text module
wasm3 core/simd/simd_conversions:620: assert_malformed of a text module
wasm3 core/simd/simd_conversions:624: assert_malformed of a text module
wasm3 core/simd/simd_conversions:627: assert_malformed of a text module
wasm3 core/simd/simd_conversions:630: assert_malformed of a text module
wasm3 core/simd/simd_conversions:633: assert_malformed of a text module
wasm3 core/simd/simd_conversions:637: assert_malformed of a text module
wasm3 core/simd/simd_conversions:640: assert_malformed of a text module
wasm3 core/simd/simd_conversions:643: assert_malformed of a text module
wasm3 core/simd/simd_conversions:646: assert_malformed of a text module
wasm3 core/simd/simd_conversions:649: assert_malformed of a text module
wasm3 core/simd/simd_conversions:652: assert_malformed of a text module
wasm3 core/simd/simd_conversions:655: assert_malformed of a text module
wasm3 core/simd/simd_conversions:658: assert_malformed of a text module
wasm3 core/simd/simd_conversions:662: assert_malformed of a text module
wasm3 core/simd/simd_conversions:665: assert_malformed of a text module
wasm3 core/simd/simd_conversions:668: assert_malformed of a text module
wasm3 core/simd/simd_conversions:671: assert_malformed of a text module
wasm3 core/simd/simd_conversions:674: assert_malformed of a text module
wasm3 core/simd/simd_conversions:677: assert_malformed of a text module
wasm3 core/simd/simd_conversions:680: assert_malformed of a text module
wasm3 core/simd/simd_conversions:683: assert_malformed of a text module
wasm3 core/simd/simd_conversions:686: assert_malformed of a text module
wasm3 core/simd/simd_conversions:689: assert_malformed of a text module
wasm3 core/simd/simd_conversions:692: assert_malformed of a text module
wasm3 core/simd/simd_conversions:695: assert_malformed of a text module
wasm3 core/simd/simd_f32x4:2325: assert_malformed of a text module
wasm3 core/simd/simd_f32x4:2326: assert_malformed of a text module
wasm3 core/simd/simd_f32x4:2327: assert_malformed of a text module
wasm3 core/simd/simd_f32x4:2328: assert_malformed of a text module
wasm3 core/simd/simd_f32x4:2329: assert_malformed of a text module
wasm3 core/simd/simd_f32x4:2330: assert_malformed of a text module
wasm3 core/simd/simd_f32x4:2331: assert_malformed of a text module
wasm3 core/simd/simd_f32x4:2332: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_cmp:7789: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_cmp:7790: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_cmp:7791: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_cmp:7792: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_cmp:7793: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_cmp:7794: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_pmin_pmax:11629: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_pmin_pmax:11630: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_pmin_pmax:11631: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_pmin_pmax:11632: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_pmin_pmax:11633: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_pmin_pmax:11634: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_pmin_pmax:11635: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_pmin_pmax:11636: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:367: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:368: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:369: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:370: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:371: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:372: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:373: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:374: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:375: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:376: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:377: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:378: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:379: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:380: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:381: assert_malformed of a text module
wasm3 core/simd/simd_f32x4_rounding:382: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_cmp:7954: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_cmp:7955: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_cmp:7956: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_cmp:7957: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_cmp:7958: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_cmp:7959: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_pmin_pmax:11629: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_pmin_pmax:11630: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_pmin_pmax:11631: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_pmin_pmax:11632: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_pmin_pmax:11633: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_pmin_pmax:11634: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_pmin_pmax:11635: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_pmin_pmax:11636: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:367: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:368: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:369: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:370: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:371: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:372: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:373: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:374: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:375: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:376: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:377: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:378: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:379: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:380: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:381: assert_malformed of a text module
wasm3 core/simd/simd_f64x2_rounding:382: assert_malformed of a text module
wasm3 core/simd/simd_i16x8_arith2:337: assert_malformed of a text module
wasm3 core/simd/simd_i16x8_arith2:338: assert_malformed of a text module
wasm3 core/simd/simd_i16x8_sat_arith:609: assert_malformed of a text module
wasm3 core/simd/simd_i16x8_sat_arith:612: assert_malformed of a text module
wasm3 core/simd/simd_i16x8_sat_arith:615: assert_malformed of a text module
wasm3 core/simd/simd_i16x8_sat_arith:618: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:281: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:282: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:283: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:284: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:285: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:286: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:287: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:288: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:289: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:290: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:291: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_arith2:292: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_cmp:1910: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_cmp:1911: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_cmp:1912: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_cmp:1913: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_cmp:1914: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_cmp:1915: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_cmp:1916: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_cmp:1917: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_cmp:1918: assert_malformed of a text module
wasm3 core/simd/simd_i32x4_cmp:1919: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_arith2:378: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_arith2:379: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_arith2:380: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_arith2:381: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_arith2:382: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_arith2:383: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:561: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:564: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:567: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:570: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:573: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:576: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:579: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:582: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:585: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:588: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:591: assert_malformed of a text module
wasm3 core/simd/simd_i8x16_sat_arith:594: assert_malformed of a text module
wasm3 core/simd/simd_lane:398: assert_malformed of a text module
wasm3 core/simd/simd_lane:399: assert_malformed of a text module
wasm3 core/simd/simd_lane:400: assert_malformed of a text module
wasm3 core/simd/simd_lane:401: assert_malformed of a text module
wasm3 core/simd/simd_lane:402: assert_malformed of a text module
wasm3 core/simd/simd_lane:403: assert_malformed of a text module
wasm3 core/simd/simd_lane:404: assert_malformed of a text module
wasm3 core/simd/simd_lane:405: assert_malformed of a text module
wasm3 core/simd/simd_lane:406: assert_malformed of a text module
wasm3 core/simd/simd_lane:407: assert_malformed of a text module
wasm3 core/simd/simd_lane:408: assert_malformed of a text module
wasm3 core/simd/simd_lane:409: assert_malformed of a text module
wasm3 core/simd/simd_lane:410: assert_malformed of a text module
wasm3 core/simd/simd_lane:411: assert_malformed of a text module
wasm3 core/simd/simd_lane:415: assert_malformed of a text module
wasm3 core/simd/simd_lane:416: assert_malformed of a text module
wasm3 core/simd/simd_lane:417: assert_malformed of a text module
wasm3 core/simd/simd_lane:418: assert_malformed of a text module
wasm3 core/simd/simd_lane:419: assert_malformed of a text module
wasm3 core/simd/simd_lane:420: assert_malformed of a text module
wasm3 core/simd/simd_lane:421: assert_malformed of a text module
wasm3 core/simd/simd_lane:422: assert_malformed of a text module
wasm3 core/simd/simd_lane:423: assert_malformed of a text module
wasm3 core/simd/simd_lane:424: assert_malformed of a text module
wasm3 core/simd/simd_lane:425: assert_malformed of a text module
wasm3 core/simd/simd_lane:426: assert_malformed of a text module
wasm3 core/simd/simd_lane:427: assert_malformed of a text module
wasm3 core/simd/simd_lane:428: assert_malformed of a text module
wasm3 core/simd/simd_lane:515: assert_malformed of a text module
wasm3 core/simd/simd_lane:518: assert_malformed of a text module
wasm3 core/simd/simd_lane:521: assert_malformed of a text module
wasm3 core/simd/simd_lane:525: assert_malformed of a text module
wasm3 core/simd/simd_lane:536: assert_malformed of a text module
wasm3 core/simd/simd_lane:537: assert_malformed of a text module
wasm3 core/simd/simd_lane:538: assert_malformed of a text module
wasm3 core/simd/simd_lane:539: assert_malformed of a text module
wasm3 core/simd/simd_lane:540: assert_malformed of a text module
wasm3 core/simd/simd_lane:541: assert_malformed of a text module
wasm3 core/simd/simd_lane:545: assert_malformed of a text module
wasm3 core/simd/simd_lane:549: assert_malformed of a text module
wasm3 core/simd/simd_lane:555: assert_malformed of a text module
wasm3 core/simd/simd_lane:559: assert_malformed of a text module
wasm3 core/simd/simd_lane:570: assert_malformed of a text module
wasm3 core/simd/simd_lane:571: assert_malformed of a text module
wasm3 core/simd/simd_lane:572: assert_malformed of a text module
wasm3 core/simd/simd_lane:573: assert_malformed of a text module
wasm3 core/simd/simd_lane:574: assert_malformed of a text module
wasm3 core/simd/simd_lane:575: assert_malformed of a text module
wasm3 core/simd/simd_lane:576: assert_malformed of a text module
wasm3 core/simd/simd_lane:577: assert_malformed of a text module
wasm3 core/simd/simd_lane:578: assert_malformed of a text module
wasm3 core/simd/simd_lane:579: assert_malformed of a text module
wasm3 core/simd/simd_lane:581: assert_malformed of a text module
wasm3 core/simd/simd_lane:582: assert_malformed of a text module
wasm3 core/simd/simd_lane:583: assert_malformed of a text module
wasm3 core/simd/simd_lane:584: assert_malformed of a text module
wasm3 core/simd/simd_lane:588: assert_malformed of a text module
wasm3 core/simd/simd_lane:589: assert_malformed of a text module
wasm3 core/simd/simd_lane:590: assert_malformed of a text module
wasm3 core/simd/simd_lane:591: assert_malformed of a text module
wasm3 core/simd/simd_lane:592: assert_malformed of a text module
wasm3 core/simd/simd_lane:593: assert_malformed of a text module
wasm3 core/simd/simd_lane:594: assert_malformed of a text module
wasm3 core/simd/simd_lane:595: assert_malformed of a text module
wasm3 core/simd/simd_lane:596: assert_malformed of a text module
wasm3 core/simd/simd_lane:597: assert_malformed of a text module
wasm3 core/simd/simd_lane:600: assert_malformed of a text module
wasm3 core/simd/simd_lane:604: assert_malformed of a text module
wasm3 core/simd/simd_lane:608: assert_malformed of a text module
wasm3 core/simd/simd_lane:612: assert_malformed of a text module
wasm3 core/simd/simd_lane:616: assert_malformed of a text module
wasm3 core/simd/simd_lane:620: assert_malformed of a text module
wasm3 core/simd/simd_lane:877: assert_malformed of a text module
wasm3 core/simd/simd_lane:878: assert_malformed of a text module
wasm3 core/simd/simd_lane:879: assert_malformed of a text module
wasm3 core/simd/simd_lane:880: assert_malformed of a text module
wasm3 core/simd/simd_lane:881: assert_malformed of a text module
wasm3 core/simd/simd_lane:882: assert_malformed of a text module
wasm3 core/simd/simd_lane:883: assert_malformed of a text module
wasm3 core/simd/simd_lane:897: assert_malformed of a text module
wasm3 core/simd/simd_lane:902: assert_malformed of a text module
wasm3 core/simd/simd_lane:918: assert_malformed of a text module
wasm3 core/simd/simd_lane:926: assert_malformed of a text module
wasm3 core/simd/simd_lane:942: assert_malformed of a text module
wasm3 core/simd/simd_lane:950: assert_malformed of a text module
wasm3 core/simd/simd_lane:966: assert_malformed of a text module
wasm3 core/simd/simd_lane:974: assert_malformed of a text module
wasm3 core/simd/simd_lane:990: assert_malformed of a text module
wasm3 core/simd/simd_lane:998: assert_malformed of a text module
wasm3 core/simd/simd_lane:1014: assert_malformed of a text module
wasm3 core/simd/simd_lane:1022: assert_malformed of a text module
wasm3 core/simd/simd_lane:1038: assert_malformed of a text module
wasm3 core/simd/simd_lane:1046: assert_malformed of a text module
wasm3 core/simd/simd_lane:1070: assert_malformed of a text module
wasm3 core/simd/simd_lane:1078: assert_malformed of a text module
wasm3 core/simd/simd_lane:1102: assert_malformed of a text module
wasm3 core/simd/simd_lane:1110: assert_malformed of a text module
wasm3 core/simd/simd_lane:1134: assert_malformed of a text module
wasm3 core/simd/simd_lane:1142: assert_malformed of a text module
wasm3 core/simd/simd_lane:1166: assert_malformed of a text module
wasm3 core/simd/simd_lane:1174: assert_malformed of a text module
wasm3 core/simd/simd_lane:1198: assert_malformed of a text module
wasm3 core/simd/simd_lane:1206: assert_malformed of a text module
wasm3 core/simd/simd_lane:1230: assert_malformed of a text module
wasm3 core/simd/simd_lane:1238: assert_malformed of a text module
wasm3 core/simd/simd_lane:1259: assert_malformed of a text module
wasm3 core/simd/simd_load:141: assert_malformed of a text module
wasm3 core/simd/simd_load:148: assert_malformed of a text module
wasm3 core/simd/simd_load:155: assert_malformed of a text module
wasm3 core/simd/simd_load_extend:301: assert_malformed of a text module
wasm3 core/simd/simd_load_extend:302: assert_malformed of a text module
wasm3 core/simd/simd_load_extend:303: assert_malformed of a text module
wasm3 core/simd/simd_load_extend:304: assert_malformed of a text module
wasm3 core/simd/simd_load_extend:305: assert_malformed of a text module
wasm3 core/simd/simd_load_extend:306: assert_malformed of a text module
wasm3 core/simd/simd_load_splat:222: assert_malformed of a text module
wasm3 core/simd/simd_load_splat:223: assert_malformed of a text module
wasm3 core/simd/simd_load_splat:224: assert_malformed of a text module
wasm3 core/simd/simd_load_splat:225: assert_malformed of a text module
wasm3 core/simd/simd_load_zero:119: assert_malformed of a text module
wasm3 core/simd/simd_load_zero:120: assert_malformed of a text module
wasm3 core/simd/simd_load_zero:121: assert_malformed of a text module
wasm3 core/simd/simd_load_zero:122: assert_malformed of a text module
wasm3 core/simd/simd_load_zero:123: assert_malformed of a text module
wasm3 core/simd/simd_load_zero:124: assert_malformed of a text module
wasm3 core/simd/simd_splat:122: assert_malformed of a text module
wasm3 core/simd/simd_store:103: assert_malformed of a text module
wasm3 core/simd/simd_store:110: assert_malformed of a text module
wasm3 core/simd/simd_store:117: assert_malformed of a text module
wasm3 core/start:103: assert_malformed of a text module
wasm3 core/store:58: assert_malformed of a text module
wasm3 core/store:65: assert_malformed of a text module
wasm3 core/store:73: assert_malformed of a text module
wasm3 core/store:81: assert_malformed of a text module
wasm3 core/store:88: assert_malformed of a text module
wasm3 core/store:96: assert_malformed of a text module
wasm3 core/store:103: assert_malformed of a text module
wasm3 core/table:36: assert_invalid of a text module
wasm3 core/table:40: assert_invalid of a text module
wasm3 core/table:44: assert_invalid of a text module
wasm3 core/table:147: assert_malformed of a text module
wasm3 core/table:154: assert_malformed of a text module
wasm3 core/table:161: assert_malformed of a text module
wasm3 core/token:4: assert_malformed of a text module
wasm3 core/token:8: assert_malformed of a text module
wasm3 core/token:86: assert_malformed of a text module
wasm3 core/token:92: assert_malformed of a text module
wasm3 core/token:102: assert_malformed of a text module
wasm3 core/token:108: assert_malformed of a text module
wasm3 core/token:118: assert_malformed of a text module
wasm3 core/token:124: assert_malformed of a text module
wasm3 core/token:144: assert_malformed of a text module
wasm3 core/token:154: assert_malformed of a text module
wasm3 core/token:164: assert_malformed of a text module
wasm3 core/token:174: assert_malformed of a text module
wasm3 core/token:184: assert_malformed of a text module
wasm3 core/token:194: assert_malformed of a text module
wasm3 core/token:204: assert_malformed of a text module
wasm3 core/token:214: assert_malformed of a text module
wasm3 core/token:224: assert_malformed of a text module
wasm3 core/token:234: assert_malformed of a text module
wasm3 core/token:244: assert_malformed of a text module
wasm3 core/token:254: assert_malformed of a text module
wasm3 core/token:264: assert_malformed of a text module
wasm3 core/token:274: assert_malformed of a text module
wasm3 core/token:282: assert_malformed of a text module
wasm3 core/token:288: assert_malformed of a text module
wasm3 core/token:294: assert_malformed of a text module
wasm3 core/token:300: assert_malformed of a text module
wasm3 core/type:44: assert_malformed of a text module
wasm3 core/type:48: assert_malformed of a text module
```
<!-- text-skips:end -->
