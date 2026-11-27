;; WastReader.ordinalsMatchWasmTools: the command ordinals of this script are those wasm-tools
;; json-from-wast gives, one per command in source order, whatever lines a command spans or shares.
;; ordinals.commands lists them as `<ordinal> <type>`, the type the JSON's.
(module $first
  (func (export "add") (param i32 i32) (result i32)
    (i32.add (local.get 0) (local.get 1)))
  (global (export "g") i32 (i32.const 7)))
(register "first" $first)
(assert_return
  (invoke $first "add" (i32.const 1) (i32.const 2))
  (i32.const 3))
(assert_return (get "g") (i32.const 7)) (invoke "add" (i32.const 0) (i32.const 0))
(module definition $template (func (export "f")))
(module instance $copy $template)
(assert_invalid
  (module (func (result i32)))
  "type mismatch")
(assert_malformed (module quote "(func") "unexpected end") (assert_malformed (module binary "") "unexpected end")
(assert_trap
  (module (func $start unreachable) (start $start))
  "unreachable")
(assert_unlinkable
  (module (import "first" "missing" (func)))
  "unknown import")
(module binary "\00asm" "\01\00\00\00")
(module quote "(func (export \"h\") (result i32) (i32.const 5))")
(assert_return (invoke "h") (i32.const 5))
(assert_trap (invoke $first "add" (i32.const 1) (i32.const 1)) "never") (assert_exhaustion (invoke "h") "never")
(assert_exception (invoke "h"))
(assert_invalid_custom (module) "never")
(assert_malformed_custom (module quote "(@custom)") "@custom annotation: missing section name")
