;; Ways to branch to a loop, each a polled back-edge: br_table to the loop, br_on_null to it, a fused
;; compare-and-branch, and a start function that spends steps before the calls.
(module
  (global $started (mut i32) (i32.const 0))
  (func $start
    (loop $again
      (global.set $started (i32.add (global.get $started) (i32.const 1)))
      (br_if $again (i32.lt_u (global.get $started) (i32.const 300)))))
  (start $start)
  (func (export "brTable") (param i32) (result i32) (local i32)
    (block $out
      (loop $again
        (local.set 1 (i32.add (local.get 1) (i32.const 1)))
        (br_table $out $again (i32.lt_u (local.get 1) (local.get 0)))))
    (local.get 1))
  (func (export "brOnNull") (param i32) (result i32) (local i32)
    (loop $again
      (local.set 1 (i32.add (local.get 1) (i32.const 1)))
      (if (i32.lt_u (local.get 1) (local.get 0))
        (then (br_on_null $again (ref.null func)) (drop))))
    (local.get 1))
  (func (export "started") (result i32) (global.get $started))
)
