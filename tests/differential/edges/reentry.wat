;; Host re-entry. An import named "reenter" calls back into one of the caller's exports at every call, so
;; each export recurses through the host until the store's nested invocations run out, and the host
;; swallows the trap; "readBack" reads the memory the host writes. "reenterDeep" calls "deep" at every call,
;; which adds 400 to 463 Wasm frames to each nested invocation: the call depth, which counts the frames of
;; every invocation in progress, runs out in the third.
(module
  (import "env" "reenter" (func $reenter (param i32) (result i32)))
  (import "env" "reenterDeep" (func $reenterDeep (param i32) (result i32)))
  (memory 1)
  (func (export "down") (param i32) (result i32) (call $reenter (local.get 0)))
  (func $descend (param i32) (result i32)
    (if (result i32) (i32.eqz (local.get 0))
      (then (call $reenterDeep (local.get 0)))
      (else (call $descend (i32.sub (local.get 0) (i32.const 1))))))
  (func (export "deep") (param i32) (result i32)
    (call $descend (i32.add (i32.const 400) (i32.and (local.get 0) (i32.const 63)))))
  (func (export "readBack") (param i32) (result i64)
    (drop (call $reenter (local.get 0)))
    (i64.load (i32.and (local.get 0) (i32.const 0xfff8))))
)
