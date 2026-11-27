(module $golden
  (type $binary (func (param $left i32) (param $right i32) (result i32)))
  (type (func))
  (type (func (param i32)))
  (import "env" "log" (func $log (type 2)))
  (import "env" "table" (table $imported 1 10 funcref))
  (import "env" "base" (global $base i32))
  (func $add (type 0) (param $left i32) (param $right i32) (result i32)
    (local $sum i32)
    (local f32 f32)
    (local f64)
    local.get 0
    local.get 1
    i32.add
    local.tee 2
    (@metadata.code.branch_hint "\01") if $nonzero (result i32)
      local.get 2
      i32.const 16
      i32.load8_u offset=4
    else
      i32.const 0
    end
  )
  (func $init (type 1)
    (local $i i32)
    block $done
      loop $again
        local.get 0
        i32.const 3
        i32.ge_u
        (@metadata.code.branch_hint "\00") br_if 1
        local.get 0
        call 0
        local.get 0
        i32.const 1
        i32.add
        local.set 0
        f32.const 0x1.8p+0
        f64.const -0x0.0000000000001p-1022
        drop
        drop
        local.get 0
        br_table 0 1 1
      end
    end
    try_table $handler (catch 0 0)
      i32.const 7
      throw 0
    end
    i32.const 0
    i32.const 0
    i32.const 5
    memory.init 0 0
    data.drop 0
  )
  (table $refs 2 externref)
  (memory $memory 1 2)
  (memory $wide i64 1 (pagesize 1))
  (tag $oops (type 2))
  (global $counter (mut i64) i64.const -1)
  (global $vector v128 v128.const i32x4 0x1 0x2 0x3 0xffffffff)
  (export "add" (func 1))
  (export "memory" (memory 0))
  (start 2)
  (elem $callbacks (table 0) (offset i32.const 0) func 1)
  (elem $declared declare func 2)
  (data $greeting "hello, \"world\"\0a")
  (data (memory 0) (offset i32.const 64) "\00\01\ff")
  (@custom "producers" (after data) "isoline")
)
