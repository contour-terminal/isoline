;; The native runner's self-test (tests/CMakeLists.txt, spec-native-parity): the oracle's binary of the
;; first module is wasm-tools' own and agrees; that of the quote module is deliberately another module's,
;; so its parity check fails; the last command is skipped by the oracle and never run.
(module
  (func (export "f") (result i32) (i32.const 1)))
(assert_return (invoke "f") (i32.const 1))
(module quote "(func (export \"g\") (result i32) (i32.const 2))")
(assert_return (invoke "g") (i32.const 3))
