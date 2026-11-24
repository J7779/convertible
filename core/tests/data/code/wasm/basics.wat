(module
  (type $binop (func (param i32 i32) (result i32)))
  (import "env" "log" (func $log (param i32)))
  (import "env" "mem" (memory 1))
  (global $counter (mut i32) (i32.const 0))
  (global $pi f64 (f64.const 3.25))
  (table 2 funcref)
  (elem (i32.const 0) $add $sub)
  (data (i32.const 16) "hello\00\ff")
  (func $add (type $binop) (local.get 0) (local.get 1) (i32.add))
  (func $sub (type $binop) local.get 0 local.get 1 i32.sub)
  (func $fact (export "fact") (param $n i64) (result i64)
    (local $acc i64)
    i64.const 1
    local.set $acc
    block $done
      loop $again
        local.get $n
        i64.eqz
        br_if $done
        local.get $acc
        local.get $n
        i64.mul
        local.set $acc
        local.get $n
        i64.const 1
        i64.sub
        local.set $n
        br $again
      end
    end
    local.get $acc)
  (func (export "pick") (param i32) (result i32)
    (if (result i32) (local.get 0)
      (then (i32.const 10))
      (else (i32.const -20))))
  (func (export "dispatch") (param i32 i32 i32) (result i32)
    local.get 1
    local.get 2
    local.get 0
    call_indirect (type $binop))
  (func (export "memory_ops") (param i32) (result i32)
    local.get 0
    i32.const 7
    i32.store offset=4
    local.get 0
    i32.load8_u offset=4 align=1
    global.get $counter
    i32.add
    call $log
    local.get 0
    i32.load16_s offset=2
    memory.size
    drop)
  (func (export "switch") (param i32) (result i32)
    block
      block
        block
          local.get 0
          br_table 0 1 2
        end
        i32.const 1
        return
      end
      i32.const 2
      return
    end
    i32.const 3)
  (func (export "floats") (param f32 f64) (result f64)
    local.get 0
    f64.promote_f32
    local.get 1
    f64.mul
    f64.const -0.5
    f64.add
    f64.sqrt)
  (func (export "select") (param i32 i32 i32) (result i32)
    local.get 0
    local.get 1
    local.get 2
    select)
  (func $init i32.const 1 global.set $counter)
  (start $init)
  (export "table" (table 0))
)
