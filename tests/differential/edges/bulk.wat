;; Bulk operations of many granules: fills, an overlapping copy upwards (granules visited backwards), table
;; fills and copies, and inits from passive segments, each may end part-way at an interrupt or a bound.
(module
  (memory 32)
  (table $t 10000 funcref)
  (elem $passive func $target $target $target)
  (data $bytes "isoline differential")
  (func $target)
  (func (export "fill") (param i32 i32) (memory.fill (local.get 0) (i32.const 0x5a) (local.get 1)))
  (func (export "copyUp") (param i32) (memory.copy (i32.const 7) (i32.const 0) (local.get 0)))
  (func (export "copyDown") (param i32) (memory.copy (i32.const 0) (i32.const 9) (local.get 0)))
  (func (export "init") (param i32 i32) (memory.init $bytes (local.get 0) (i32.const 0) (local.get 1)))
  (func (export "dropData") (data.drop $bytes))
  (func (export "tableFill") (param i32 i32) (table.fill $t (local.get 0) (ref.func $target) (local.get 1)))
  (func (export "tableCopy") (param i32) (table.copy $t $t (i32.const 1) (i32.const 0) (local.get 0)))
  (func (export "tableInit") (param i32 i32) (table.init $t $passive (local.get 0) (i32.const 0) (local.get 1)))
  (func (export "tableGrow") (param i32) (result i32) (table.grow $t (ref.null func) (local.get 0)))
  (func (export "tableGet") (param i32) (result funcref) (table.get $t (local.get 0)))
)
