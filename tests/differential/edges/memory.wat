;; Accesses at the bounds of a 32-bit and a 64-bit memory, with offsets and every width, and growth past the
;; store's memory limit.
(module
  (memory $small 1 2)
  (memory $wide i64 1)
  (data (memory $small) (i32.const 0) "\01\02\03\04\05\06\07\08\09\0a\0b\0c\0d\0e\0f\10")
  (func (export "load64") (param i32) (result i64) (i64.load offset=3 (local.get 0)))
  (func (export "load8") (param i32) (result i32) (i32.load8_s offset=65535 (local.get 0)))
  (func (export "store16") (param i32 i64) (i64.store16 offset=1 (local.get 0) (local.get 1)))
  (func (export "store32") (param i32 i32) (i32.store offset=2 (local.get 0) (local.get 1)))
  (func (export "vector") (param i32) (result v128) (v128.load offset=1 (local.get 0)))
  (func (export "lane") (param i32 v128) (result v128) (v128.load32_lane offset=7 1 (local.get 0) (local.get 1)))
  (func (export "wideLoad") (param i64) (result i32) (i32.load8_u $wide offset=65530 (local.get 0)))
  (func (export "wideStore") (param i64 i64) (i64.store $wide offset=1 (local.get 0) (local.get 1)))
  (func (export "grow") (param i32) (result i32) (memory.grow $small (local.get 0)))
  (func (export "growWide") (param i64) (result i64) (memory.grow $wide (local.get 0)))
  (func (export "size") (result i32 i64) (memory.size $small) (memory.size $wide))
)
