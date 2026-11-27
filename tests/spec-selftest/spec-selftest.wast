;; The spec runner's self-test (tests/CMakeLists.txt, spec-selftest): a script with wrong assertions the
;; runner must report and fail on. The oracle records nothing but the command count.
(module
  (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1)))
  (func (export "div_s") (param i32 i32) (result i32) (i32.div_s (local.get 0) (local.get 1))))
;; Wrong values: 1 + 2 is not 4, and dividing by zero is no overflow.
(assert_return (invoke "add" (i32.const 1) (i32.const 2)) (i32.const 3))
(assert_return (invoke "add" (i32.const 1) (i32.const 2)) (i32.const 4))
(assert_trap (invoke "div_s" (i32.const 1) (i32.const 0)) "integer overflow")
;; Each failure assertion's stage: one module that fails at its stage (passes), and one that fails with the
;; right message at another stage (fails). An assert_trap of a module is the JSON's assert_uninstantiable.
(assert_malformed (module binary "\00asX\01\00\00\00") "magic header not detected")
(assert_invalid (module binary "\00asX\01\00\00\00") "magic header not detected")
(assert_invalid (module (func (result i32))) "type mismatch")
(assert_malformed (module (func (result i32))) "type mismatch")
(assert_unlinkable (module (import "nowhere" "f" (func))) "unknown import")
(assert_trap (module (import "nowhere" "f" (func))) "unknown import")
(assert_trap (module (memory 0) (data (i32.const 0) "a")) "out of bounds memory access")
(assert_unlinkable (module (memory 0) (data (i32.const 0) "a")) "out of bounds memory access")
;; A definition, instantiated under a name and invoked by it and by an either, passes.
(module definition $D
  (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1))))
(module instance $I $D)
(assert_return (invoke $I "add" (i32.const 1) (i32.const 2)) (i32.const 3))
(assert_return (invoke "add" (i32.const 1) (i32.const 2)) (either (i32.const 4) (i32.const 3)))
;; An assert_exception of a function that returns, an assert_invalid_custom of a module without custom
;; sections, and an instance of an unknown definition fail.
(assert_exception (invoke "add" (i32.const 1) (i32.const 2)))
(assert_invalid_custom
  (module (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1))))
  "x")
(module instance $X)
;; An instance of a named module, which names a definition too, passes.
(module $M
  (func (export "add") (param i32 i32) (result i32) (i32.add (local.get 0) (local.get 1))))
(module instance $N $M)
(assert_return (invoke $N "add" (i32.const 1) (i32.const 2)) (i32.const 3))
