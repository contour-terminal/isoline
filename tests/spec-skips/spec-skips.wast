;; The pinned skip count (tests/CMakeLists.txt, spec-skips*): the oracle skips the one command, which would
;; fail if it ran, as no module exports "f".
(assert_return (invoke "f") (i32.const 1))
