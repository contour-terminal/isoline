;; The text format's sugar, which the reader expands as wast does.
(component
  (import "i" (instance $i
    (export "f" (func (param "x" (list (tuple u8 (option string)))) (result (result u32 (error string)))))
    (export "g" (func))
    (export "nested" (instance (export "h" (func))))))
  (core module $m (export "m-out") (func (export "run")))
  (core instance $ci0 (instantiate $m))
  (core instance $ci (instantiate $m (with "env" (instance (export "run" (func $ci0 "run"))))))
  (alias core export $ci "run" (core func $i-run))
  (func $f (export "f-out") (alias export $i "f"))
  (core func $lowered (canon lower (func $i "g")))
  (core func $deep (canon lower (func $i "nested" "h")))
  (type $r (export "rec") (record (field "a" u32) (field "b" (list (variant (case "x") (case "y" s64))))))
  (instance $bundle (export "bundled") (export "f" (func $f)) (export "r" (type $r)))
  (instance (instantiate 0 (with "a" (instance (export "f" (func $f)))) (with "b" (func $f))))
  (component $c (import "f" (func)))
  (component (import "imported-component") (type 0))
  (export "core-module" (core module $m)))
