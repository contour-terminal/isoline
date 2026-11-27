;; The native runner's self-test (tests/CMakeLists.txt, spec-native-divergence): an error-priority
;; divergence. The reference reports the `if` without its `(then` first, Isoline the unknown local.
;; The oracle records Isoline's message for the first assertion, which then passes; the second, without
;; such an entry, fails; the third records a prefix of Isoline's message only, and fails, as a recorded
;; divergence must be Isoline's message exactly.
(assert_malformed (module quote "(func (if (local.get $x)))") "unexpected token")
(assert_malformed (module quote "(func (if (local.get $x)))") "unexpected token")
(assert_malformed (module quote "(func (if (local.get $x)))") "unexpected token")
