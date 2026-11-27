;; The native runner's self-test (tests/CMakeLists.txt, spec-native-stages): malformed text that is no
;; custom annotation's is what assert_malformed expects (passes) and not what assert_malformed_custom
;; does (fails); a module given as text reports its error at its place in this script, 8:22.
(assert_malformed (module quote "(func (i32.const x))") "unknown operator")
(assert_malformed_custom (module quote "(func (i32.const x))") "unknown operator")
(module
  (func
    (drop (i32.const x))))
