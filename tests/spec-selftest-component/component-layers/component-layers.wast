;; The component runner's layer checks (tests/CMakeLists.txt, spec-selftest-component-layers): a value or
;; an action of one layer given to the other fails as the harness's, as does what a component instance
;; does not have; each failure is pinned by its line. A host function it re-exports is called, as the
;; host's.
(module $core (func (export "f") (param i32) (result i32) (local.get 0)))
(register "core-module" $core)
(component $component
  (import "host-return-two" (func $two (result u32)))
  (export "two" (func $two))
  (core module $m (func (export "f") (param i32) (result i32) (local.get 0)))
  (core instance $i (instantiate $m))
  (func (export "f") (param "x" u32) (result u32) (canon lift (core func $i "f"))))
(invoke $core "f" (u32.const 1))
(assert_return (invoke $core "f" (i32.const 1)) (u32.const 1))
(invoke $component "f" (i32.const 1))
(assert_return (invoke $component "f" (u32.const 1)) (i32.const 1))
(assert_return (get $component "g"))
(invoke $component "three")
(register "component" $component)
(assert_exception (invoke $component "f" (u32.const 1)))
(assert_return (invoke $component "f" (u32.const 1)) (u32.const 1))
(assert_return (invoke $component "two") (u32.const 2))
