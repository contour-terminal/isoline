;; Every kind of name the component-name section records, as wasm-tools 1.263.0 records it.
(component $root
  (core module $m
    (func $f (export "f") (result i32) (i32.const 7))
    (memory $mem (export "mem") 1)
    (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 0)))
  (core instance $i (instantiate $m))
  (alias core export $i "f" (core func $cf))
  (alias core export $i "mem" (core memory $cm))
  (core type $ct (func (param i32)))
  (type $t u32)
  (type $ft (func (result $t)))
  (func $lifted (type $ft) (canon lift (core func $cf)))
  (import "in" (func $imported (type $ft)))
  (import "res" (type $res (sub resource)))
  (canon resource.drop $res (core func $drop))
  (component $inner
    (type $u (tuple u8 u16))
    (import "x" (func $x (param "a" $u)))
    (export "y" (func $x)))
  (instance $made (instantiate $inner (with "x" (func $lifted))))
  (export $e "out" (func $lifted))
  (export "inner" (component $inner))
  ;; A nested component whose core module names itself, and names its function.
  (component $holder
    (core module $self (func $g))))
