(module $golden
  (type $binary (func (param $left i32) (param $right i32) (result i32)))
  (type (func))
  (import "env" "log" (func $log (param i32)))
  (import "env" "table" (table $imported 1 10 funcref))
  (import "env" "base" (global $base i32))
  (memory $memory 1 2)
  (memory $wide i64 1 (pagesize 1))
  (table $refs 2 externref)
  (tag $oops (param i32))
  (global $counter (mut i64) (i64.const -1))
  (global $vector v128 (v128.const i32x4 1 2 3 0xffffffff))
  (export "add" (func $add))
  (export "memory" (memory $memory))
  (start $init)
  (func $add (type $binary) (param $left i32) (param $right i32) (result i32)
    (local $sum i32) (local f32 f32) (local f64)
    local.get $left
    local.get $right
    i32.add
    local.tee $sum
    (@metadata.code.branch_hint "\01")
    if $nonzero (result i32)
      local.get $sum
      i32.const 0x10
      i32.load8_u offset=4
    else
      i32.const 0
    end)
  (func $init (type 1)
    (local $i i32)
    block $done
      loop $again
        local.get $i
        i32.const 3
        i32.ge_u
        (@metadata.code.branch_hint "\00")
        br_if $done
        local.get $i
        call $log
        local.get $i
        i32.const 1
        i32.add
        local.set $i
        f32.const 1.5
        f64.const -0x1p-1074
        drop
        drop
        local.get $i
        br_table $again $done $done
      end
    end
    try_table $handler (catch $oops 0)
      i32.const 7
      throw $oops
    end
    i32.const 0
    i32.const 0
    i32.const 5
    memory.init $greeting
    data.drop $greeting)
  (elem $callbacks (table $imported) (offset i32.const 0) func $add)
  (elem $declared declare func $init)
  (data $greeting "hello, \"world\"\n")
  (data (memory $memory) (offset i32.const 64) "\00\01\ff")
  (@custom "producers" (after data) "isoline"))
