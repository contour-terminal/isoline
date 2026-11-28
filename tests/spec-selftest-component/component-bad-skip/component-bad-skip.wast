;; The component suites' skip reasons (tests/CMakeLists.txt, spec-selftest-component-bad-skip): the oracle
;; skips the one command for a reason no SkipReasons row gives, which fails the file before anything runs.
(assert_return (invoke "host-echo-u32" (u32.const 1)) (u32.const 1))
