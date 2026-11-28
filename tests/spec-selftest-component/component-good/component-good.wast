;; The component runner's self-test (tests/CMakeLists.txt, spec-selftest-component): every command passes.

;; A component given as text, its exports called with component values and checked against them.
(component $math
  (core module $m
    (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1)))
    (func (export "neg") (param f64) (result f64) (f64.neg (local.get 0)))
    (func (export "sum") (param i32 i32 i32) (result i32)
      (i32.add (i32.add (local.get 0) (local.get 1)) (local.get 2)))
    (func (export "pick") (param i32 i32) (result i32)
      (if (result i32) (local.get 0) (then (local.get 1)) (else (i32.const 99))))
    (func (export "trap") unreachable))
  (core instance $i (instantiate $m))
  (func (export "add") (param "a" u32) (param "b" u32) (result u32) (canon lift (core func $i "add")))
  (func (export "neg") (param "x" f64) (result f64) (canon lift (core func $i "neg")))
  (type $point-definition (record (field "x" u32) (field "y" u32) (field "z" u8)))
  (export $point "point" (type $point-definition))
  (func (export "sum") (param "p" $point) (result u32) (canon lift (core func $i "sum")))
  (func (export "pick") (param "o" (option u32)) (result u32) (canon lift (core func $i "pick")))
  (func (export "trap") (canon lift (core func $i "trap"))))
(assert_return (invoke "add" (u32.const 2) (u32.const 3)) (u32.const 5))
(assert_return (invoke $math "neg" (f64.const 1.5)) (f64.const -1.5))
(assert_return (invoke "sum" (record.const (field "x" u32.const 1) (field "y" u32.const 2) (field "z" u8.const 3)))
               (u32.const 6))
(assert_return (invoke "pick" (option.some (u32.const 7))) (u32.const 7))
(assert_return (invoke "pick" (option.none)) (u32.const 99))
(invoke "add" (u32.const 1) (u32.const 1))
;; A core trap, worded as Wasmtime words it; the instance refuses every call after it.
(assert_trap (invoke "trap") "wasm `unreachable` instruction executed")
(assert_trap (invoke "add" (u32.const 1) (u32.const 1)) "cannot enter component instance")

;; A definition, instantiated twice, each instance in a store of its own.
(component definition $counter
  (core module $m
    (global $count (mut i32) (i32.const 0))
    (func (export "next") (result i32)
      (global.set $count (i32.add (global.get $count) (i32.const 1)))
      (global.get $count)))
  (core instance $i (instantiate $m))
  (func (export "next") (result u32) (canon lift (core func $i "next"))))
(component instance $first $counter)
(assert_return (invoke "next") (u32.const 1))
(assert_return (invoke "next") (u32.const 2))
(component instance $second $counter)
(assert_return (invoke $second "next") (u32.const 1))
(assert_return (invoke $first "next") (u32.const 3))

;; The component spectest's host functions, lowered into a core module and lifted back out.
(component
  (import "host-return-two" (func $two (result u32)))
  (import "host" (instance $host (export "return-three" (func (result u32)))))
  (alias export $host "return-three" (func $three))
  (core func $two (canon lower (func $two)))
  (core func $three (canon lower (func $three)))
  (core module $m
    (import "" "two" (func $two (result i32)))
    (import "" "three" (func $three (result i32)))
    (func (export "five") (result i32) (i32.add (call $two) (call $three))))
  (core instance $i (instantiate $m (with "" (instance (export "two" (func $two)) (export "three" (func $three))))))
  (func (export "five") (result u32) (canon lift (core func $i "five"))))
(assert_return (invoke "five") (u32.const 5))

;; The binary and quote forms.
(component binary "\00asm" "\0d\00\01\00")
(component quote "(core module $m)" "(core instance (instantiate $m))")

;; Components that fail to load, link or instantiate.
(assert_malformed (component binary "\00asm" "\0d\00\01\00" "\ff") "malformed section id")
;; A preamble of no known layer is reported by the component decoder, as the script gives a component.
(assert_malformed (component binary "\00asm" "\0e\00\01\00") "unknown binary version:    0x1000e")
(assert_invalid (component (core instance (instantiate 0))) "unknown module 0")
(assert_unlinkable (component (import "no-such-import" (func))) "was not found")
(assert_trap (component
  (core module $m (func $start unreachable) (start $start))
  (core instance (instantiate $m))) "unreachable")

;; A core module beside them still runs as the core suites run it.
(module (func (export "one") (result i32) (i32.const 1)))
(assert_return (invoke "one") (i32.const 1))
