;; The component runner's self-test (tests/CMakeLists.txt, spec-selftest-component-wrong): exactly one
;; command fails, the assert_return on line 10, which expects a wrong value.
(component
  (core module $m
    (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1))))
  (core instance $i (instantiate $m))
  (func (export "add") (param "a" u32) (param "b" u32) (result u32) (canon lift (core func $i "add"))))
(assert_return (invoke "add" (u32.const 2) (u32.const 3)) (u32.const 5))
(assert_return (invoke "add" (u32.const 1) (u32.const 1)) (u32.const 2))
(assert_return (invoke "add" (u32.const 2) (u32.const 2)) (u32.const 5))
(assert_return (invoke "add" (u32.const 0) (u32.const 0)) (u32.const 0))
