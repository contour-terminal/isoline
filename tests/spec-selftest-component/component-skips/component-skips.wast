;; The component suites' skip reasons (tests/CMakeLists.txt, spec-selftest-component-skips): the oracle
;; skips the one command for "async (P12)", a SkipReasons row's, so the file passes with one skip pinned.
(assert_return (invoke "host-echo-u32" (u32.const 1)) (u32.const 1))
