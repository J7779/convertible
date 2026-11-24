(module
  (type (;0;) (func (result i32 i32)))
  (type (;1;) (func (param i32) (result i32)))
  (type (;2;) (func (param i32)))
  (type (;3;) (func (param externref) (result i32)))
  (type (;4;) (func (param f32) (result i32)))
  (type (;5;) (func (result i32)))
  (type (;6;) (func (param i64 i64 i32) (result i64)))
  (func (;0;) (type 0) (result i32 i32)
    i32.const 1
    i32.const 2)
  (func (;1;) (type 1) (param i32) (result i32)
    local.get 0
    i32.extend8_s)
  (func (;2;) (type 2) (param i32)
    local.get 0
    i32.const 0
    i32.const 3
    memory.init 0
    data.drop 0
    local.get 0
    i32.const 100
    i32.const 3
    memory.copy
    local.get 0
    i32.const 255
    i32.const 10
    memory.fill
    i32.const 0
    i32.const 0
    i32.const 2
    table.init 1 0
    elem.drop 0)
  (func (;3;) (type 3) (param externref) (result i32)
    i32.const 0
    local.get 0
    table.set 0
    ref.null extern
    ref.is_null
    ref.func 0
    drop
    table.size 0
    i32.add)
  (func (;4;) (type 4) (param f32) (result i32)
    local.get 0
    i32.trunc_sat_f32_s)
  (func (;5;) (type 5) (result i32)
    call 0
    i32.add)
  (func (;6;) (type 6) (param i64 i64 i32) (result i64)
    local.get 0
    local.get 1
    local.get 2
    select (result i64))
  (func (;7;) (type 5) (result i32)
    i32.const 5
    block (param i32) (result i32)  ;; label = @1
      i32.const 1
      i32.add
    end)
  (table (;0;) 4 externref)
  (table (;1;) 3 funcref)
  (memory (;0;) 1)
  (export "bulk" (func 2))
  (export "refs" (func 3))
  (export "sat" (func 4))
  (export "multi" (func 5))
  (export "typed_select" (func 6))
  (export "block_types" (func 7))
  (elem (;0;) func 0 1)
  (data (;0;) "abc"))
