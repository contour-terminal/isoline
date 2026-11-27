;; The native runner's self-test (tests/CMakeLists.txt, spec-native-recorded-parity): a parity
;; divergence the oracle records, where its binary is known to be wrong. The oracle's binary of each of
;; the first three modules returns 2, not 1. The first module's entry records the offset they differ at
;; and the digest of Isoline's encoding, and passes; the second's records another offset, and fails; the
;; third's binary is its own, so the divergence it records does not occur, and it fails too. The oracle's
;; binary of the last two exports "g", not "f": both differ first at the export's name, and both entries
;; record the first module's digest. The fourth module is the first, so its entry passes; the fifth
;; returns 3, a change behind the recorded offset, so its entry is stale and fails. The last module's
;; text does not compile, so there is no encoding of Isoline's to check its entry against: it fails as
;; well, and says so. Each assert_return after a module still runs.
(module (func (export "f") (result i32) (i32.const 1)))
(assert_return (invoke "f") (i32.const 1))
(module (func (export "f") (result i32) (i32.const 1)))
(module (func (export "f") (result i32) (i32.const 1)))
(module (func (export "f") (result i32) (i32.const 1)))
(module (func (export "f") (result i32) (i32.const 3)))
(module (func (export "f") (result i32) (bogus)))
