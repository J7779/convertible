(module
  (type (;0;) (func (param i32 i32) (result i32)))
  (type (;1;) (func (param i32)))
  (type (;2;) (func (param i64) (result i64)))
  (type (;3;) (func (param i32) (result i32)))
  (type (;4;) (func (param i32 i32 i32) (result i32)))
  (type (;5;) (func (param f32 f64) (result f64)))
  (type (;6;) (func))
  (import "env" "log" (func (;0;) (type 1)))
  (import "env" "mem" (memory (;0;) 1))
  (func (;1;) (type 0) (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add)
  (func (;2;) (type 0) (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.sub)
  (func (;3;) (type 2) (param i64) (result i64)
    (local i64)
    i64.const 1
    local.set 1
    block  ;; label = @1
      loop  ;; label = @2
        local.get 0
        i64.eqz
        br_if 1 (;@1;)
        local.get 1
        local.get 0
        i64.mul
        local.set 1
        local.get 0
        i64.const 1
        i64.sub
        local.set 0
        br 0 (;@2;)
      end
    end
    local.get 1)
  (func (;4;) (type 3) (param i32) (result i32)
    local.get 0
    if (result i32)  ;; label = @1
      i32.const 10
    else
      i32.const -20
    end)
  (func (;5;) (type 4) (param i32 i32 i32) (result i32)
    local.get 1
    local.get 2
    local.get 0
    call_indirect (type 0))
  (func (;6;) (type 3) (param i32) (result i32)
    local.get 0
    i32.const 7
    i32.store offset=4
    local.get 0
    i32.load8_u offset=4
    global.get 0
    i32.add
    call 0
    local.get 0
    i32.load16_s offset=2
    memory.size
    drop)
  (func (;7;) (type 3) (param i32) (result i32)
    block  ;; label = @1
      block  ;; label = @2
        block  ;; label = @3
          local.get 0
          br_table 0 (;@3;) 1 (;@2;) 2 (;@1;)
        end
        i32.const 1
        return
      end
      i32.const 2
      return
    end
    i32.const 3)
  (func (;8;) (type 5) (param f32 f64) (result f64)
    local.get 0
    f64.promote_f32
    local.get 1
    f64.mul
    f64.const -0x1p-1 (;=-0.5;)
    f64.add
    f64.sqrt)
  (func (;9;) (type 4) (param i32 i32 i32) (result i32)
    local.get 0
    local.get 1
    local.get 2
    select)
  (func (;10;) (type 6)
    i32.const 1
    global.set 0)
  (table (;0;) 2 funcref)
  (global (;0;) (mut i32) (i32.const 0))
  (global (;1;) f64 (f64.const 0x1.ap+1 (;=3.25;)))
  (export "fact" (func 3))
  (export "pick" (func 4))
  (export "dispatch" (func 5))
  (export "memory_ops" (func 6))
  (export "switch" (func 7))
  (export "floats" (func 8))
  (export "select" (func 9))
  (export "table" (table 0))
  (start 10)
  (elem (;0;) (i32.const 0) func 1 2)
  (data (;0;) (i32.const 16) "hello\00\ff"))
