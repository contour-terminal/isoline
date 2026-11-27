;; The native runner's self-test (tests/CMakeLists.txt, spec-native-shifted): the oracle's entries are
;; each moved onto the command after their own, so the binary of the module lands on an assert_return,
;; and the script fails as a whole before any command runs.
(module
  (func (export "f") (result i32) (i32.const 1)))
(assert_return (invoke "f") (i32.const 1))
