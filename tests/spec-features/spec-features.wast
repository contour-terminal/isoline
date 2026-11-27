;; The feature sets (tests/CMakeLists.txt, spec-features-*): a module of extended constant expressions, a
;; 3.0 feature, instantiates under wasm3 and all, and fails, with the read of its global, under wasm2.
(module (global (export "g") i32 (i32.add (i32.const 1) (i32.const 2))))
(assert_return (get "g") (i32.const 3))
