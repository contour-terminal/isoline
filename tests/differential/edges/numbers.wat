;; Numbers at their edges: NaN payloads and signs through arithmetic, min and copysign, trapping and
;; saturating conversions, division by zero and overflow, relaxed SIMD in the deterministic profile, and
;; wide arithmetic.
(module
  (func (export "add") (param f32 f32) (result f32) (f32.add (local.get 0) (local.get 1)))
  (func (export "min") (param f64 f64) (result f64) (f64.min (local.get 0) (local.get 1)))
  (func (export "negate") (param f32) (result f32) (f32.neg (local.get 0)))
  (func (export "copysign") (param f64 f64) (result f64) (f64.copysign (local.get 0) (local.get 1)))
  (func (export "truncate") (param f64) (result i32) (i32.trunc_f64_s (local.get 0)))
  (func (export "saturate") (param f32) (result i64) (i64.trunc_sat_f32_u (local.get 0)))
  (func (export "demote") (param f64) (result f32) (f32.demote_f64 (local.get 0)))
  (func (export "divide") (param i32 i32) (result i32) (i32.div_s (local.get 0) (local.get 1)))
  (func (export "remainder") (param i64 i64) (result i64) (i64.rem_u (local.get 0) (local.get 1)))
  (func (export "rotate") (param i64 i64) (result i64) (i64.rotl (local.get 0) (local.get 1)))
  (func (export "madd") (param v128 v128 v128) (result v128)
    (f32x4.relaxed_madd (local.get 0) (local.get 1) (local.get 2)))
  (func (export "vectorMax") (param v128 v128) (result v128) (f64x2.max (local.get 0) (local.get 1)))
  (func (export "swizzle") (param v128 v128) (result v128) (i8x16.relaxed_swizzle (local.get 0) (local.get 1)))
  (func (export "add128") (param i64 i64 i64 i64) (result i64 i64)
    (i64.add128 (local.get 0) (local.get 1) (local.get 2) (local.get 3)))
  (func (export "multiplyWide") (param i64 i64) (result i64 i64) (i64.mul_wide_s (local.get 0) (local.get 1)))
)
