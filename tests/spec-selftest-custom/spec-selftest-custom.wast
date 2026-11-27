;; The spec runner's self-test of assert_invalid_custom (tests/CMakeLists.txt, spec-selftest-custom), under
;; 3.0, whose branch hints are the custom section Isoline interprets: a module whose hint names a non-branch
;; (offset 7, its `return`) passes with "invalid target" and fails with "invalid order", a wrong message; a
;; malformed module fails it, as a load failure is never a custom error.
(assert_invalid_custom
  (module
    (func (param i32) (local i32) local.get 1 local.get 0 i32.eq return)
    (memory 1 1)
    (@custom "metadata.code.branch_hint" (after memory) "\01\00\01\07\01\01"))
  "@metadata.code.branch_hint annotation: invalid target")
(assert_invalid_custom
  (module
    (func (param i32) (local i32) local.get 1 local.get 0 i32.eq return)
    (memory 1 1)
    (@custom "metadata.code.branch_hint" (after memory) "\01\00\01\07\01\01"))
  "@metadata.code.branch_hint annotation: invalid order")
(assert_invalid_custom (module binary "\00asX\01\00\00\00") "@metadata.code.branch_hint annotation: invalid target")
